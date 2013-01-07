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

static void make_black_background_yuv420p( VSFrameRef *frame, const VSAPI *vsapi )
{
    memset( vsapi->getWritePtr( frame, 0 ), 0x00, vsapi->getStride( frame, 0 ) * vsapi->getFrameHeight( frame, 0 ) );
    memset( vsapi->getWritePtr( frame, 1 ), 0x80, vsapi->getStride( frame, 1 ) * vsapi->getFrameHeight( frame, 1 ) );
    memset( vsapi->getWritePtr( frame, 2 ), 0x80, vsapi->getStride( frame, 2 ) * vsapi->getFrameHeight( frame, 2 ) );
}

int make_frame_yuv420p( struct SwsContext *sws_ctx, AVFrame *picture, VSFrameRef *frame, VSFrameContext *frame_ctx, const VSAPI *vsapi )
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
    bit_blt( frame, dst_data[0], dst_linesize[0], 0, picture->width,     picture->height,     vsapi );  /* Y */
    bit_blt( frame, dst_data[1], dst_linesize[1], 1, picture->width / 2, picture->height / 2, vsapi );  /* U */
    bit_blt( frame, dst_data[2], dst_linesize[2], 2, picture->width / 2, picture->height / 2, vsapi );  /* V */
    av_free( dst_data[0] );
    return 0;
}

static void avoid_yuv_scale_conversion( enum AVPixelFormat *input_pixel_format )
{
    static const struct
    {
        enum AVPixelFormat full;
        enum AVPixelFormat limited;
    } range_hack_table[]
        = {
            { AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUV420P },
            //{ AV_PIX_FMT_YUVJ422P, AV_PIX_FMT_YUV422P },
            //{ AV_PIX_FMT_YUVJ444P, AV_PIX_FMT_YUV444P },
            //{ AV_PIX_FMT_YUVJ440P, AV_PIX_FMT_YUV440P },
            { AV_PIX_FMT_NONE,     AV_PIX_FMT_NONE    }
          };
    for( int i = 0; range_hack_table[i].full != AV_PIX_FMT_NONE; i++ )
        if( *input_pixel_format == range_hack_table[i].full )
            *input_pixel_format = range_hack_table[i].limited;
}

int determine_colorspace_conversion( video_output_handler_t *vohp, enum AVPixelFormat *input_pixel_format, VSPresetFormat *output_pixel_type )
{
    avoid_yuv_scale_conversion( input_pixel_format );
    switch( *input_pixel_format )
    {
        case AV_PIX_FMT_YUV420P :
        case AV_PIX_FMT_NV12 :
        case AV_PIX_FMT_NV21 :
            vohp->output_pixel_format   = AV_PIX_FMT_YUV420P;   /* planar YUV 4:2:0, 12bpp, (1 Cr & Cb sample per 2x2 Y samples) */
            vohp->make_black_background = make_black_background_yuv420p;
            vohp->make_frame            = make_frame_yuv420p;
            *output_pixel_type          = pfYUV420P8;
            return 1;
        default :
            vohp->output_pixel_format   = AV_PIX_FMT_NONE;
            vohp->make_black_background = NULL;
            vohp->make_frame            = NULL;
            *output_pixel_type          = pfNone;
            return 0;
    }
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
