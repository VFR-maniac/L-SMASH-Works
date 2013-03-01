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

#include "lsmashsource.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
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

/* This source filter always uses lines aligned to an address dividable by 32.
 * Furthermore it seems Avisynth bulit-in BitBlt is slow.
 * So, I think it's OK that we always use swscale instead. */
static inline int convert_av_pixel_format
(
    struct SwsContext *sws_ctx,
    AVFrame           *av_frame,
    AVPicture         *av_picture
)
{
    int ret = sws_scale( sws_ctx,
                         (const uint8_t * const *)av_frame->data, av_frame->linesize,
                         0, av_frame->height,
                         av_picture->data, av_picture->linesize );
    return ret > 0 ? ret : -1;
}

static int make_frame_yuv420p
(
    struct SwsContext  *sws_ctx,
    AVFrame            *av_frame,
    PVideoFrame        &as_frame,
    IScriptEnvironment *env
)
{
    AVPicture av_picture = { { { NULL } } };
    av_picture.data    [0] = as_frame->GetWritePtr( PLANAR_Y );
    av_picture.data    [1] = as_frame->GetWritePtr( PLANAR_U );
    av_picture.data    [2] = as_frame->GetWritePtr( PLANAR_V );
    av_picture.linesize[0] = as_frame->GetPitch   ( PLANAR_Y );
    av_picture.linesize[1] = as_frame->GetPitch   ( PLANAR_U );
    av_picture.linesize[2] = as_frame->GetPitch   ( PLANAR_V );
    return convert_av_pixel_format( sws_ctx, av_frame, &av_picture );
}

static int make_frame_yuv422
(
    struct SwsContext  *sws_ctx,
    AVFrame            *av_frame,
    PVideoFrame        &as_frame,
    IScriptEnvironment *env
)
{
    AVPicture av_picture = { { { NULL } } };
    av_picture.data    [0] = as_frame->GetWritePtr();
    av_picture.linesize[0] = as_frame->GetPitch   ();
    return convert_av_pixel_format( sws_ctx, av_frame, &av_picture );
}

static int make_frame_rgba32
(
    struct SwsContext  *sws_ctx,
    AVFrame            *av_frame,
    PVideoFrame        &as_frame,
    IScriptEnvironment *env
)
{
    AVPicture av_picture = { { { NULL } } };
    av_picture.data    [0] = as_frame->GetWritePtr() + as_frame->GetPitch() * (as_frame->GetHeight() - 1);
    av_picture.linesize[0] = -as_frame->GetPitch();
    return convert_av_pixel_format( sws_ctx, av_frame, &av_picture );
}

int determine_colorspace_conversion
(
    lw_video_output_handler_t *vohp,
    enum AVPixelFormat         input_pixel_format,
    int                       *output_pixel_type
)
{
    avoid_yuv_scale_conversion( &input_pixel_format );
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
    vohp->scaler.output_pixel_format = AV_PIX_FMT_NONE;
    for( int i = 0; conversion_table[i].output_pixel_format != AV_PIX_FMT_NONE; i++ )
        if( conversion_table[i].input_pixel_format == input_pixel_format )
        {
            vohp->scaler.output_pixel_format = conversion_table[i].output_pixel_format;
            break;
        }
    as_video_output_handler_t *as_vohp = (as_video_output_handler_t *)vohp->private_handler;
    switch( vohp->scaler.output_pixel_format )
    {
        case AV_PIX_FMT_YUV420P :   /* planar YUV 4:2:0, 12bpp, (1 Cr & Cb sample per 2x2 Y samples) */
            as_vohp->make_black_background = make_black_background_yuv420p;
            as_vohp->make_frame            = make_frame_yuv420p;
            *output_pixel_type             = VideoInfo::CS_I420;
            return 0;
        case AV_PIX_FMT_YUYV422 :   /* packed YUV 4:2:2, 16bpp */
            as_vohp->make_black_background = make_black_background_yuv422;
            as_vohp->make_frame            = make_frame_yuv422;
            *output_pixel_type             = VideoInfo::CS_YUY2;
            return 0;
        case AV_PIX_FMT_BGRA :      /* packed BGRA 8:8:8:8, 32bpp, BGRABGRA... */
            as_vohp->make_black_background = make_black_background_rgba32;
            as_vohp->make_frame            = make_frame_rgba32;
            *output_pixel_type             = VideoInfo::CS_BGR32;
            return 0;
        default :
            as_vohp->make_black_background = NULL;
            as_vohp->make_frame            = NULL;
            *output_pixel_type             = VideoInfo::CS_UNKNOWN;
            return -1;
    }
}

int make_frame
(
    lw_video_output_handler_t *vohp,
    AVFrame                   *av_frame,
    PVideoFrame               &as_frame,
    enum AVColorSpace          colorspace,
    IScriptEnvironment        *env
)
{
    if( !vohp->scaler.enabled )
    {
        /* Render a video frame from the decoder directly. */
        as_frame = ((as_video_buffer_handler_t *)av_frame->opaque)->as_frame_buffer;
        return 0;
    }
    /* Convert pixel format. We don't change the presentation resolution. */
    enum AVPixelFormat *input_pixel_format = (enum AVPixelFormat *)&av_frame->format;
    int yuv_range = avoid_yuv_scale_conversion( input_pixel_format );
    lw_video_scaler_handler_t *vshp = &vohp->scaler;
    if( !vshp->sws_ctx
     || vshp->input_width        != av_frame->width
     || vshp->input_height       != av_frame->height
     || vshp->input_pixel_format != *input_pixel_format
     || vshp->input_colorspace   != colorspace
     || vshp->input_yuv_range    != yuv_range )
    {
        /* Update scaler. */
        vshp->sws_ctx = update_scaler_configuration( vshp->sws_ctx, vshp->flags,
                                                     av_frame->width, av_frame->height,
                                                     *input_pixel_format, vshp->output_pixel_format,
                                                     colorspace, yuv_range );
        if( !vshp->sws_ctx )
            return -1;
        vshp->input_width        = av_frame->width;
        vshp->input_height       = av_frame->height;
        vshp->input_pixel_format = *input_pixel_format;
        vshp->input_colorspace   = colorspace;
        vshp->input_yuv_range    = yuv_range;
    }
    /* Render a video frame through the scaler from the decoder. */
    as_video_output_handler_t *as_vohp = (as_video_output_handler_t *)vohp->private_handler;
    as_frame = env->NewVideoFrame( *as_vohp->vi, 32 );
    if( vohp->output_width != av_frame->width || vohp->output_height != av_frame->height )
        as_vohp->make_black_background( as_frame );
    return as_vohp->make_frame( vshp->sws_ctx, av_frame, as_frame, env );
}

int as_check_dr_available
(
    AVCodecContext    *ctx,
    enum AVPixelFormat pixel_format
)
{
    if( !(ctx->codec->capabilities & CODEC_CAP_DR1) )
        return 0;
    static enum AVPixelFormat dr_support_pix_fmt[] =
        {
            AV_PIX_FMT_YUV420P,
            AV_PIX_FMT_YUV422P,
            AV_PIX_FMT_BGRA,
            AV_PIX_FMT_NONE
        };
    for( int i = 0; dr_support_pix_fmt[i] != AV_PIX_FMT_NONE; i++ )
        if( dr_support_pix_fmt[i] == pixel_format )
            return 1;
    return 0;
}

void setup_direct_rendering
(
    lw_video_output_handler_t *vohp,
    AVCodecContext            *ctx,
    int                       *width,
    int                       *height
)
{
    /* Align output width and height for direct rendering. */
    int linesize_align[AV_NUM_DATA_POINTERS];
    enum AVPixelFormat input_pixel_format = ctx->pix_fmt;
    ctx->pix_fmt = vohp->scaler.output_pixel_format;
    avcodec_align_dimensions2( ctx, width, height, linesize_align );
    ctx->pix_fmt = input_pixel_format;
    /* Set up custom get_buffer() for direct rendering if available. */
    ctx->get_buffer     = as_video_get_buffer;
    ctx->release_buffer = as_video_release_buffer;
    ctx->opaque         = vohp;
    ctx->flags         |= CODEC_FLAG_EMU_EDGE;
}

int as_video_get_buffer
(
    AVCodecContext *ctx,
    AVFrame        *av_frame
)
{
    lw_video_output_handler_t *lw_vohp = (lw_video_output_handler_t *)ctx->opaque;
    lw_vohp->scaler.enabled = 0;
    enum AVPixelFormat pix_fmt = ctx->pix_fmt;
    avoid_yuv_scale_conversion( &pix_fmt );
    if( lw_vohp->scaler.input_pixel_format != pix_fmt
     || !as_check_dr_available( ctx, pix_fmt ) )
        lw_vohp->scaler.enabled = 1;
    as_video_output_handler_t *as_vohp = (as_video_output_handler_t *)lw_vohp->private_handler;
    if( lw_vohp->scaler.enabled )
        return avcodec_default_get_buffer( ctx, av_frame );
    /* New AviSynth video frame buffer. */
    as_video_buffer_handler_t *as_vbhp = new as_video_buffer_handler_t;
    if( !as_vbhp )
        return -1;
    av_frame->opaque = as_vbhp;
    as_vbhp->as_frame_buffer = as_vohp->env->NewVideoFrame( *as_vohp->vi, 32 );
    int aligned_width  = ctx->width;
    int aligned_height = ctx->height;
    avcodec_align_dimensions2( ctx, &aligned_width, &aligned_height, av_frame->linesize );
    if( lw_vohp->output_width != aligned_width || lw_vohp->output_height != aligned_height )
        as_vohp->make_black_background( as_vbhp->as_frame_buffer );
    /* Set data address and linesize. */
    if( as_vohp->vi->pixel_type == VideoInfo::CS_BGR32 )
    {
        av_frame->base    [0] = as_vbhp->as_frame_buffer->GetWritePtr();
        av_frame->data    [0] = av_frame->base[0];
        av_frame->linesize[0] = as_vbhp->as_frame_buffer->GetPitch   ();
    }
    else
        for( int i = 0; i < 3; i++ )
        {
            static const int as_plane[3] = { PLANAR_Y, PLANAR_U, PLANAR_V };
            av_frame->base    [i] = as_vbhp->as_frame_buffer->GetWritePtr( as_plane[i] );
            av_frame->data    [i] = av_frame->base[i];
            av_frame->linesize[i] = as_vbhp->as_frame_buffer->GetPitch   ( as_plane[i] );
        }
    /* Don't use extended_data. */
    av_frame->extended_data       = av_frame->data;
    /* Set fundamental fields. */
    av_frame->type                = FF_BUFFER_TYPE_USER;
    av_frame->pkt_pts             = ctx->pkt ? ctx->pkt->pts : AV_NOPTS_VALUE;
    av_frame->width               = ctx->width;
    av_frame->height              = ctx->height;
    av_frame->format              = ctx->pix_fmt;
    av_frame->sample_aspect_ratio = ctx->sample_aspect_ratio;
    return 0;
}

void as_video_release_buffer
(
    AVCodecContext *ctx,
    AVFrame        *av_frame
)
{
    /* Delete AviSynth video frame buffer. */
    if( av_frame->type == FF_BUFFER_TYPE_USER )
    {
        as_video_buffer_handler_t *as_vbhp = (as_video_buffer_handler_t *)av_frame->opaque;
        if( as_vbhp )
        {
            delete as_vbhp;
            av_frame->opaque = NULL;
        }
        lw_video_output_handler_t *lw_vohp = (lw_video_output_handler_t *)ctx->opaque;
        if( !lw_vohp )
            return;
        as_video_output_handler_t *as_vohp = (as_video_output_handler_t *)lw_vohp->private_handler;
        if( !as_vohp )
            return;
        if( as_vohp->vi->pixel_type == VideoInfo::CS_BGR32 )
        {
            av_frame->base    [0] = NULL;
            av_frame->data    [0] = NULL;
            av_frame->linesize[0] = 0;
        }
        else
            for( int i = 0; i < 3; i++ )
            {
                av_frame->base    [i] = NULL;
                av_frame->data    [i] = NULL;
                av_frame->linesize[i] = 0;
            }
        return;
    }
    avcodec_default_release_buffer( ctx, av_frame );
}
