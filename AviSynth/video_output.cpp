/*****************************************************************************
 * video_output.cpp
 *****************************************************************************
 * Copyright (C) 2012-2013 L-SMASH Works project
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

/* This file is available under an ISC license.
 * However, when distributing its binary file, it will be under LGPL or GPL. */

#include "avisynth.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
}

#include "video_output.h"

static void make_black_background_yuv420p( PVideoFrame &frame )
{
    memset( frame->GetWritePtr( PLANAR_Y ), 0x00, frame->GetPitch( PLANAR_Y ) * frame->GetHeight( PLANAR_Y ) );
    memset( frame->GetWritePtr( PLANAR_U ), 0x80, frame->GetPitch( PLANAR_U ) * frame->GetHeight( PLANAR_U ) );
    memset( frame->GetWritePtr( PLANAR_V ), 0x80, frame->GetPitch( PLANAR_V ) * frame->GetHeight( PLANAR_V ) );
}

static void make_black_background_yuv422( PVideoFrame &frame )
{
    uint32_t *p = (uint32_t *)frame->GetWritePtr();
    int num_loops = frame->GetPitch() * frame->GetHeight() / 4;
    for( int i = 0; i < num_loops; i++ )
        *p++ = 0x00800080;
}

static void make_black_background_rgba32( PVideoFrame &frame )
{
    memset( frame->GetWritePtr(), 0x00, frame->GetPitch() * frame->GetHeight() );
}

static int make_frame_yuv420p
(
    struct SwsContext  *sws_ctx,
    AVFrame            *picture,
    PVideoFrame        &frame,
    IScriptEnvironment *env
)
{
    uint8_t *dst_data    [4];
    int      dst_linesize[4];
    if( av_image_alloc( dst_data, dst_linesize, picture->width, picture->height, AV_PIX_FMT_YUV420P, 16 ) < 0 )
        return -1;
    sws_scale( sws_ctx, (const uint8_t* const*)picture->data, picture->linesize, 0, picture->height, dst_data, dst_linesize );
    env->BitBlt( frame->GetWritePtr( PLANAR_Y ), frame->GetPitch( PLANAR_Y ), dst_data[0], dst_linesize[0], picture->width,     picture->height ); 
    env->BitBlt( frame->GetWritePtr( PLANAR_U ), frame->GetPitch( PLANAR_U ), dst_data[1], dst_linesize[1], picture->width / 2, picture->height / 2 ); 
    env->BitBlt( frame->GetWritePtr( PLANAR_V ), frame->GetPitch( PLANAR_V ), dst_data[2], dst_linesize[2], picture->width / 2, picture->height / 2 ); 
    av_free( dst_data[0] );
    return 0;
}

static int make_frame_yuv422
(
    struct SwsContext  *sws_ctx,
    AVFrame            *picture,
    PVideoFrame        &frame,
    IScriptEnvironment *env
)
{
    uint8_t *dst_data    [4];
    int      dst_linesize[4];
    if( av_image_alloc( dst_data, dst_linesize, picture->width, picture->height, AV_PIX_FMT_YUYV422, 16 ) < 0 )
        return -1;
    sws_scale( sws_ctx, (const uint8_t* const*)picture->data, picture->linesize, 0, picture->height, dst_data, dst_linesize );
    env->BitBlt( frame->GetWritePtr(), frame->GetPitch(), dst_data[0], dst_linesize[0], picture->width * 2, picture->height );
    av_free( dst_data[0] );
    return 0;
}

static int make_frame_rgba32
(
    struct SwsContext  *sws_ctx,
    AVFrame            *picture,
    PVideoFrame        &frame,
    IScriptEnvironment *env
)
{
    uint8_t *dst_data    [4];
    int      dst_linesize[4];
    if( av_image_alloc( dst_data, dst_linesize, picture->width, picture->height, AV_PIX_FMT_BGRA, 16 ) < 0 )
        return -1;
    sws_scale( sws_ctx, (const uint8_t* const*)picture->data, picture->linesize, 0, picture->height, dst_data, dst_linesize );
    env->BitBlt( frame->GetWritePtr() + frame->GetPitch() * (frame->GetHeight() - 1), -frame->GetPitch(), dst_data[0], dst_linesize[0], picture->width * 4, picture->height ); 
    av_free( dst_data[0] );
    return 0;
}

static inline void avoid_yuv_scale_conversion( enum AVPixelFormat *input_pixel_format )
{
    static const struct
    {
        enum AVPixelFormat full;
        enum AVPixelFormat limited;
    } range_hack_table[]
        = {
            { AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUV420P },
            { AV_PIX_FMT_YUVJ422P, AV_PIX_FMT_YUV422P },
            { AV_PIX_FMT_NONE,     AV_PIX_FMT_NONE    }
          };
    for( int i = 0; range_hack_table[i].full != AV_PIX_FMT_NONE; i++ )
        if( *input_pixel_format == range_hack_table[i].full )
            *input_pixel_format = range_hack_table[i].limited;
}

int determine_colorspace_conversion
(
    video_output_handler_t *vohp,
    enum AVPixelFormat     *input_pixel_format,
    int                    *output_pixel_type
)
{
    avoid_yuv_scale_conversion( input_pixel_format );
    static const struct
    {
        enum AVPixelFormat input_pixel_format;
        enum AVPixelFormat output_pixel_format;
    } conversion_table[] =
        {
            { AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P },
            { AV_PIX_FMT_NV12,    AV_PIX_FMT_YUV420P },
            { AV_PIX_FMT_NV21,    AV_PIX_FMT_YUV420P },
            { AV_PIX_FMT_YUYV422, AV_PIX_FMT_YUYV422 },
            { AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUYV422 },
            { AV_PIX_FMT_UYVY422, AV_PIX_FMT_YUYV422 },
            { AV_PIX_FMT_ARGB,    AV_PIX_FMT_BGRA    },
            { AV_PIX_FMT_RGBA,    AV_PIX_FMT_BGRA    },
            { AV_PIX_FMT_ABGR,    AV_PIX_FMT_BGRA    },
            { AV_PIX_FMT_BGRA,    AV_PIX_FMT_BGRA    },
            { AV_PIX_FMT_RGB24,   AV_PIX_FMT_BGRA    },
            { AV_PIX_FMT_BGR24,   AV_PIX_FMT_BGRA    },
            { AV_PIX_FMT_NONE,    AV_PIX_FMT_NONE    }
        };
    vohp->output_pixel_format = AV_PIX_FMT_NONE;
    for( int i = 0; conversion_table[i].output_pixel_format != AV_PIX_FMT_NONE; i++ )
        if( conversion_table[i].input_pixel_format == *input_pixel_format )
        {
            vohp->output_pixel_format = conversion_table[i].output_pixel_format;
            break;
        }
    switch( vohp->output_pixel_format )
    {
        case AV_PIX_FMT_YUV420P :   /* planar YUV 4:2:0, 12bpp, (1 Cr & Cb sample per 2x2 Y samples) */
            vohp->make_black_background = make_black_background_yuv420p;
            vohp->make_frame            = make_frame_yuv420p;
            *output_pixel_type          = VideoInfo::CS_I420;
            return 0;
        case AV_PIX_FMT_YUYV422 :   /* packed YUV 4:2:2, 16bpp */
            vohp->make_black_background = make_black_background_yuv422;
            vohp->make_frame            = make_frame_yuv422;
            *output_pixel_type          = VideoInfo::CS_YUY2;
            return 0;
        case AV_PIX_FMT_BGRA :      /* packed BGRA 8:8:8:8, 32bpp, BGRABGRA... */
            vohp->make_black_background = make_black_background_rgba32;
            vohp->make_frame            = make_frame_rgba32;
            *output_pixel_type          = VideoInfo::CS_BGR32;
            return 0;
        default :
            vohp->make_black_background = NULL;
            vohp->make_frame            = NULL;
            *output_pixel_type          = VideoInfo::CS_UNKNOWN;
            return -1;
    }
}

int make_frame
(
    video_output_handler_t *vohp,
    AVFrame                *picture,
    PVideoFrame            &frame,
    IScriptEnvironment     *env
)
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
            return -1;
    }
    vohp->make_black_background( frame );
    return vohp->make_frame( vohp->sws_ctx, picture, frame, env );
}
