/*****************************************************************************
 * video_output.c
 *****************************************************************************
 * Copyright (C) 2013 L-SMASH Works project
 *
 * Authors: Yusuke Nakamura <muken.the.vfrmaniac@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *****************************************************************************/

/* This file is available under an ISC license. */

#include <string.h>

/* Libav */
#include <libavcodec/avcodec.h>         /* Decoder */
#include <libswscale/swscale.h>         /* Colorspace converter */
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>

#include "VapourSynth.h"
#include "video_output.h"

static void bit_blt( VSFrameRef *dst, uint8_t *src_data, int src_linesize, int plane, int row_size, int height, const VSAPI *vsapi )
{
    uint8_t *dst_data     = vsapi->getWritePtr( dst, plane );
    int      dst_linesize = vsapi->getStride  ( dst, plane );
    if( src_linesize == dst_linesize && src_linesize == row_size )
    {
        memcpy( dst_data, src_data, src_linesize * height );
        return;
    }
    for( int i = 0; i < height; i++ )
    {
        memcpy( dst_data, src_data, row_size );
        dst_data += dst_linesize;
        src_data += src_linesize;
    }
}

#if 0
static int get_conversion_multiplier( enum AVPixelFormat dst_pix_fmt, enum AVPixelFormat src_pix_fmt, int width )
{
    int src_size = 0;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get( src_pix_fmt );
    int used_plane[4] = { 0, 0, 0, 0 };
    for( int i = 0; i < desc->nb_components; i++ )
    {
        int plane = desc->comp[i].plane;
        if( used_plane[plane] )
            continue;
        src_size += av_image_get_linesize( src_pix_fmt, width, plane );
        used_plane[plane] = 1;
    }
    if( src_size == 0 )
        return 1;
    int dst_size = 0;
    desc = av_pix_fmt_desc_get( dst_pix_fmt );
    used_plane[0] = used_plane[1] = used_plane[2] = used_plane[3] = 0;
    for( int i = 0; i < desc->nb_components; i++ )
    {
        int plane = desc->comp[i].plane;
        if( used_plane[plane] )
            continue;
        dst_size += av_image_get_linesize( dst_pix_fmt, width, plane );
        used_plane[plane] = 1;
    }
    return (dst_size - 1) / src_size + 1;
}
#endif

static void make_black_background_planar_yuv( VSFrameRef *frame, const VSAPI *vsapi )
{
    int shift = vsapi->getFrameFormat( frame )->bitsPerSample - 8;
    for( int i = 0; i < 3; i++ )
        memset( vsapi->getWritePtr( frame, i ), i ? 0x00000080 << shift : 0x00000000, vsapi->getStride( frame, i ) * vsapi->getFrameHeight( frame, i ) );
}

int make_frame_planar_yuv( struct SwsContext *sws_ctx, AVFrame *picture, VSFrameRef *frame, VSFrameContext *frame_ctx, const VSAPI *vsapi )
{
    int abs_dst_linesize = picture->linesize[0] > 0 ? picture->linesize[0] : -picture->linesize[0];
    if( abs_dst_linesize & 15 )
        abs_dst_linesize = (abs_dst_linesize & 0xfffffff0) + 16;  /* Make mod16. */
    uint8_t *dst_data[4];
    dst_data[0] = (uint8_t *)av_mallocz( abs_dst_linesize * picture->height * 3 );
    if( !dst_data[0] )
    {
        if( frame_ctx )
            vsapi->setFilterError( "lsmas: failed to av_mallocz.", frame_ctx );
        return -1;
    }
    for( int i = 1; i < 3; i++ )
        dst_data[i] = dst_data[i - 1] + abs_dst_linesize * picture->height;
    dst_data[3] = NULL;
    const int dst_linesize[4] = { abs_dst_linesize, abs_dst_linesize, abs_dst_linesize, 0 };
    sws_scale( sws_ctx, (const uint8_t* const*)picture->data, picture->linesize, 0, picture->height, dst_data, dst_linesize );
    const VSFormat *format = vsapi->getFrameFormat( frame );
    int row_size_0 = format->bytesPerSample * picture->width;
    for( int i = 0; i < 3; i++ )
    {
        int sub_sampling_w = i ? format->subSamplingW : 0;
        int sub_sampling_h = i ? format->subSamplingH : 0;
        bit_blt( frame, dst_data[i], dst_linesize[i], i, row_size_0 >> sub_sampling_w, picture->height >> sub_sampling_h, vsapi );
    }
    av_free( dst_data[0] );
    return 0;
}

static void avoid_yuv_scale_conversion( enum AVPixelFormat *input_pixel_format )
{
    static const struct
    {
        enum AVPixelFormat full;
        enum AVPixelFormat limited;
    } range_hack_table[] =
        {
            { AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUV420P },
            { AV_PIX_FMT_YUVJ422P, AV_PIX_FMT_YUV422P },
            { AV_PIX_FMT_YUVJ444P, AV_PIX_FMT_YUV444P },
            { AV_PIX_FMT_YUVJ440P, AV_PIX_FMT_YUV440P },
            { AV_PIX_FMT_NONE,     AV_PIX_FMT_NONE    }
        };
    for( int i = 0; range_hack_table[i].full != AV_PIX_FMT_NONE; i++ )
        if( *input_pixel_format == range_hack_table[i].full )
            *input_pixel_format = range_hack_table[i].limited;
}

int determine_colorspace_conversion( video_output_handler_t *vohp, enum AVPixelFormat *input_pixel_format, VSPresetFormat *output_pixel_type )
{
    avoid_yuv_scale_conversion( input_pixel_format );
    static const struct
    {
        enum AVPixelFormat          av_input_pixel_format;
        enum AVPixelFormat          av_output_pixel_format;
        VSPresetFormat              vs_output_pixel_type;
        func_make_black_background *func_make_black_background;
        func_make_frame            *func_make_frame;
    } conversion_table[] =
        {
            { AV_PIX_FMT_YUV420P,     AV_PIX_FMT_YUV420P,     pfYUV420P8,   make_black_background_planar_yuv, make_frame_planar_yuv },
            { AV_PIX_FMT_NV12,        AV_PIX_FMT_YUV420P,     pfYUV420P8,   make_black_background_planar_yuv, make_frame_planar_yuv },
            { AV_PIX_FMT_NV21,        AV_PIX_FMT_YUV420P,     pfYUV420P8,   make_black_background_planar_yuv, make_frame_planar_yuv },
            { AV_PIX_FMT_YUV422P,     AV_PIX_FMT_YUV422P,     pfYUV422P8,   make_black_background_planar_yuv, make_frame_planar_yuv },
            { AV_PIX_FMT_YUV444P,     AV_PIX_FMT_YUV444P,     pfYUV444P8,   make_black_background_planar_yuv, make_frame_planar_yuv },
            { AV_PIX_FMT_YUV420P9LE,  AV_PIX_FMT_YUV420P9LE,  pfYUV420P9,   make_black_background_planar_yuv, make_frame_planar_yuv },
            { AV_PIX_FMT_YUV420P9BE,  AV_PIX_FMT_YUV420P9LE,  pfYUV420P9,   make_black_background_planar_yuv, make_frame_planar_yuv },
            { AV_PIX_FMT_YUV422P9LE,  AV_PIX_FMT_YUV422P9LE,  pfYUV422P9,   make_black_background_planar_yuv, make_frame_planar_yuv },
            { AV_PIX_FMT_YUV422P9BE,  AV_PIX_FMT_YUV422P9LE,  pfYUV422P9,   make_black_background_planar_yuv, make_frame_planar_yuv },
            { AV_PIX_FMT_YUV444P9LE,  AV_PIX_FMT_YUV444P9LE,  pfYUV444P9,   make_black_background_planar_yuv, make_frame_planar_yuv },
            { AV_PIX_FMT_YUV444P9BE,  AV_PIX_FMT_YUV444P9LE,  pfYUV444P9,   make_black_background_planar_yuv, make_frame_planar_yuv },
            { AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_YUV420P10LE, pfYUV420P10,  make_black_background_planar_yuv, make_frame_planar_yuv },
            { AV_PIX_FMT_YUV420P10BE, AV_PIX_FMT_YUV420P10LE, pfYUV420P10,  make_black_background_planar_yuv, make_frame_planar_yuv },
            { AV_PIX_FMT_YUV422P10LE, AV_PIX_FMT_YUV422P10LE, pfYUV422P10,  make_black_background_planar_yuv, make_frame_planar_yuv },
            { AV_PIX_FMT_YUV422P10BE, AV_PIX_FMT_YUV422P10LE, pfYUV422P10,  make_black_background_planar_yuv, make_frame_planar_yuv },
            { AV_PIX_FMT_YUV444P10LE, AV_PIX_FMT_YUV444P10LE, pfYUV444P10,  make_black_background_planar_yuv, make_frame_planar_yuv },
            { AV_PIX_FMT_YUV444P10BE, AV_PIX_FMT_YUV444P10LE, pfYUV444P10,  make_black_background_planar_yuv, make_frame_planar_yuv },
            { AV_PIX_FMT_YUV420P16LE, AV_PIX_FMT_YUV420P16LE, pfYUV420P16,  make_black_background_planar_yuv, make_frame_planar_yuv },
            { AV_PIX_FMT_YUV420P16BE, AV_PIX_FMT_YUV420P16LE, pfYUV420P16,  make_black_background_planar_yuv, make_frame_planar_yuv },
            { AV_PIX_FMT_YUV422P16LE, AV_PIX_FMT_YUV422P16LE, pfYUV422P16,  make_black_background_planar_yuv, make_frame_planar_yuv },
            { AV_PIX_FMT_YUV422P16BE, AV_PIX_FMT_YUV422P16LE, pfYUV422P16,  make_black_background_planar_yuv, make_frame_planar_yuv },
            { AV_PIX_FMT_YUV444P16LE, AV_PIX_FMT_YUV444P16LE, pfYUV444P16,  make_black_background_planar_yuv, make_frame_planar_yuv },
            { AV_PIX_FMT_YUV444P16BE, AV_PIX_FMT_YUV444P16LE, pfYUV444P16,  make_black_background_planar_yuv, make_frame_planar_yuv },
            { AV_PIX_FMT_NONE,        AV_PIX_FMT_NONE,        pfNone,       NULL,                             NULL }
        };
    for( int i = 0; conversion_table[i].vs_output_pixel_type != pfNone; i++ )
        if( *input_pixel_format == conversion_table[i].av_input_pixel_format )
        {
            vohp->output_pixel_format   = conversion_table[i].av_output_pixel_format;
            vohp->make_black_background = conversion_table[i].func_make_black_background;
            vohp->make_frame            = conversion_table[i].func_make_frame;
            *output_pixel_type          = conversion_table[i].vs_output_pixel_type;
            return i + 1;
        }
    vohp->output_pixel_format   = AV_PIX_FMT_NONE;
    vohp->make_black_background = NULL;
    vohp->make_frame            = NULL;
    *output_pixel_type          = pfNone;
    return 0;
}

int make_frame( video_output_handler_t *vohp, AVFrame *picture, VSFrameRef *frame, VSFrameContext *frame_ctx, const VSAPI *vsapi )
{
    /* Convert color space. We don't change the presentation resolution. */
    int64_t width;
    int64_t height;
    int64_t format;
    av_opt_get_int( vohp->sws_ctx, "srcw",       0, &width );
    av_opt_get_int( vohp->sws_ctx, "srch",       0, &height );
    av_opt_get_int( vohp->sws_ctx, "src_format", 0, &format );
    avoid_yuv_scale_conversion( (enum AVPixelFormat *)&picture->format );
    if( !vohp->sws_ctx || picture->width != width || picture->height != height || picture->format != format )
    {
        /* Update scaler. */
        vohp->sws_ctx = sws_getCachedContext( vohp->sws_ctx,
                                              picture->width, picture->height, (enum AVPixelFormat)picture->format,
                                              picture->width, picture->height, vohp->output_pixel_format,
                                              vohp->scaler_flags, NULL, NULL, NULL );
        if( !vohp->sws_ctx )
        {
            if( frame_ctx )
                vsapi->setFilterError( "lsmas: failed to update scaler settings.", frame_ctx );
            return -1;
        }
    }
    if( !vohp->make_frame )
        return -1;
    if( vohp->make_black_background && !vohp->variable_info )
        vohp->make_black_background( frame, vsapi );
    return vohp->make_frame( vohp->sws_ctx, picture, frame, frame_ctx, vsapi );
}
