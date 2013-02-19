/*****************************************************************************
 * lwlibav_source.c
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

/* This file is available under an ISC license.
 * However, when distributing its binary file, it will be under LGPL or GPL. */

#define NO_PROGRESS_HANDLER

/* Libav (LGPL or GPL) */
#include <libavformat/avformat.h>       /* Codec specific info importer */
#include <libavcodec/avcodec.h>         /* Decoder */
#include <libswscale/swscale.h>         /* Colorspace converter */
#include <libavutil/imgutils.h>

#include "lsmashsource.h"
#include "video_output.h"

/* Dummy definitions.
 * Audio resampler/buffer is NOT used at all in this filter. */
typedef void AVAudioResampleContext;
typedef void audio_samples_t;
int flush_resampler_buffers( AVAudioResampleContext *avr ){ return 0; }
int update_resampler_configuration( AVAudioResampleContext *avr,
                                    uint64_t out_channel_layout, int out_sample_rate, enum AVSampleFormat out_sample_fmt,
                                    uint64_t  in_channel_layout, int  in_sample_rate, enum AVSampleFormat  in_sample_fmt,
                                    int *input_planes, int *input_block_align ){ return 0; }
int resample_audio( AVAudioResampleContext *avr, audio_samples_t *out, audio_samples_t *in ){ return 0; }
void avresample_free( AVAudioResampleContext **avr ){ }
#include "../common/audio_output.h"
uint64_t output_pcm_samples_from_buffer
(
    lw_audio_output_handler_t *aohp,
    AVFrame                   *frame_buffer,
    uint8_t                  **output_buffer,
    enum audio_output_flag    *output_flags
)
{
    return 0;
}

uint64_t output_pcm_samples_from_packet
(
    lw_audio_output_handler_t *aohp,
    AVCodecContext            *ctx,
    AVPacket                  *pkt,
    AVFrame                   *frame_buffer,
    uint8_t                  **output_buffer,
    enum audio_output_flag    *output_flags
)
{
    return 0;
}

#include "../common/utils.h"
#include "../common/progress.h"
#include "../common/lwlibav_dec.h"
#include "../common/lwlibav_video.h"
#include "../common/lwlibav_audio.h"
#include "../common/lwindex.h"

typedef lw_video_scaler_handler_t lwlibav_video_scaler_handler_t;
typedef lw_video_output_handler_t lwlibav_video_output_handler_t;

typedef struct
{
    VSVideoInfo                    vi;
    lwlibav_file_handler_t         lwh;
    lwlibav_video_decode_handler_t vdh;
    lwlibav_video_output_handler_t voh;
} lwlibav_handler_t;

static void VS_CC vs_filter_init( VSMap *in, VSMap *out, void **instance_data, VSNode *node, VSCore *core, const VSAPI *vsapi )
{
    lwlibav_handler_t *hp = (lwlibav_handler_t *)*instance_data;
    vsapi->setVideoInfo( &hp->vi, 1, node );
}

static int prepare_video_decoding( lwlibav_handler_t *hp, VSCore *core, const VSAPI *vsapi )
{
    lwlibav_video_decode_handler_t *vdhp  = &hp->vdh;
    lwlibav_video_output_handler_t *vohp  = &hp->voh;
    VSVideoInfo                    *vi    = &hp->vi;
    vs_basic_handler_t             *vsbhp = (vs_basic_handler_t *)vdhp->eh.message_priv;
    /* Import AVIndexEntrys. */
    if( vdhp->index_entries )
    {
        AVStream *video_stream = vdhp->format->streams[ vdhp->stream_index ];
        for( int i = 0; i < vdhp->index_entries_count; i++ )
        {
            AVIndexEntry *ie = &vdhp->index_entries[i];
            if( av_add_index_entry( video_stream, ie->pos, ie->timestamp, ie->size, ie->min_distance, ie->flags ) < 0 )
            {
                set_error( vsbhp, "lsmas: failed to import AVIndexEntrys for video." );
                return -1;
            }
        }
        av_freep( &vdhp->index_entries );
    }
    /* Set up output format. */
    vdhp->ctx->width   = vdhp->initial_width;
    vdhp->ctx->height  = vdhp->initial_height;
    vdhp->ctx->pix_fmt = vdhp->initial_pix_fmt;
    enum AVPixelFormat input_pixel_format = vdhp->ctx->pix_fmt;
    if( determine_colorspace_conversion( vohp, &vdhp->ctx->pix_fmt ) )
    {
        set_error( vsbhp, "lsmas: %s is not supported", av_get_pix_fmt_name( input_pixel_format ) );
        return -1;
    }
    vs_video_output_handler_t *vs_vohp = (vs_video_output_handler_t *)vohp->private_handler;
    vs_vohp->frame_ctx = NULL;
    vs_vohp->core      = core;
    vs_vohp->vsapi     = vsapi;
    vs_vohp->direct_rendering &= !!(vdhp->ctx->codec->capabilities & CODEC_CAP_DR1);
    vs_vohp->direct_rendering &= check_dr_support_format( vdhp->ctx->pix_fmt );
    if( vs_vohp->variable_info )
    {
        vi->format = NULL;
        vi->width  = 0;
        vi->height = 0;
    }
    else
    {
        vi->format = vsapi->getFormatPreset( vs_vohp->vs_output_pixel_format, core );
        vi->width  = vdhp->max_width;
        vi->height = vdhp->max_height;
        if( vs_vohp->direct_rendering )
        {
            /* Align output width and height for direct rendering. */
            int linesize_align[AV_NUM_DATA_POINTERS];
            input_pixel_format = vdhp->ctx->pix_fmt;
            vdhp->ctx->pix_fmt = vohp->scaler.output_pixel_format;
            avcodec_align_dimensions2( vdhp->ctx, &vi->width, &vi->height, linesize_align );
            vdhp->ctx->pix_fmt = input_pixel_format;
        }
        vs_vohp->background_frame = vsapi->newVideoFrame( vi->format, vi->width, vi->height, NULL, core );
        if( !vs_vohp->background_frame )
        {
            set_error( vsbhp, "lsmas: failed to allocate memory for the background black frame data." );
            return -1;
        }
        vs_vohp->make_black_background( vs_vohp->background_frame, vsapi );
    }
    vohp->output_width  = vi->width;
    vohp->output_height = vi->height;
    /* Set up scaler. */
    lwlibav_video_scaler_handler_t *vshp = &vohp->scaler;
    vshp->flags   = SWS_FAST_BILINEAR;
    vshp->sws_ctx = sws_getCachedContext( NULL,
                                          vdhp->ctx->width, vdhp->ctx->height, vdhp->ctx->pix_fmt,
                                          vdhp->ctx->width, vdhp->ctx->height, vshp->output_pixel_format,
                                          vshp->flags, NULL, NULL, NULL );
    if( !vshp->sws_ctx )
    {
        set_error( vsbhp, "lsmas: failed to get swscale context." );
        return -1;
    }
    vshp->input_width        = vdhp->ctx->width;
    vshp->input_height       = vdhp->ctx->height;
    vshp->input_pixel_format = vdhp->ctx->pix_fmt;
    /* Set up custom get_buffer() for direct rendering if available. */
    if( vs_vohp->direct_rendering )
    {
        vdhp->ctx->get_buffer     = vs_video_get_buffer;
        vdhp->ctx->release_buffer = vs_video_release_buffer;
        vdhp->ctx->opaque         = vohp;
        vdhp->ctx->flags         |= CODEC_FLAG_EMU_EDGE;
    }
    /* Find the first valid video frame. */
    vdhp->seek_flags = (vdhp->seek_base & SEEK_FILE_OFFSET_BASED) ? AVSEEK_FLAG_BYTE : vdhp->seek_base == 0 ? AVSEEK_FLAG_FRAME : 0;
    if( vi->numFrames != 1 )
    {
        vdhp->seek_flags |= AVSEEK_FLAG_BACKWARD;
        uint32_t rap_number;
        lwlibav_find_random_accessible_point( vdhp, 1, 0, &rap_number );
        int64_t rap_pos = lwlibav_get_random_accessible_point_position( vdhp, rap_number );
        if( av_seek_frame( vdhp->format, vdhp->stream_index, rap_pos, vdhp->seek_flags ) < 0 )
            av_seek_frame( vdhp->format, vdhp->stream_index, rap_pos, vdhp->seek_flags | AVSEEK_FLAG_ANY );
    }
    for( uint32_t i = 1; i <= vi->numFrames + get_decoder_delay( vdhp->ctx ); i++ )
    {
        AVPacket *pkt = &vdhp->packet;
        lwlibav_get_av_frame( vdhp->format, vdhp->stream_index, pkt );
        avcodec_get_frame_defaults( vdhp->frame_buffer );
        int got_picture;
        if( avcodec_decode_video2( vdhp->ctx, vdhp->frame_buffer, &got_picture, pkt ) >= 0 && got_picture )
        {
            vohp->first_valid_frame_number = i - MIN( get_decoder_delay( vdhp->ctx ), vdhp->delay_count );
            if( vohp->first_valid_frame_number > 1 || vi->numFrames == 1 )
            {
                vohp->first_valid_frame = make_frame( vohp, vdhp->frame_buffer );
                if( !vohp->first_valid_frame )
                {
                    set_error( vsbhp, "lsmas: failed to allocate the first valid video frame." );
                    return -1;
                }
            }
            break;
        }
        else if( pkt->data )
            ++ vdhp->delay_count;
    }
    vdhp->last_frame_number = vi->numFrames + 1;    /* Force seeking at the first reading. */
    return 0;
}

static void set_frame_properties( lwlibav_handler_t *hp, AVFrame *av_frame, VSFrameRef *vs_frame, const VSAPI *vsapi )
{
    lwlibav_video_decode_handler_t *vdhp  = &hp->vdh;
    VSVideoInfo                    *vi    = &hp->vi;
    AVCodecContext                 *ctx   = vdhp->ctx;
    VSMap                          *props = vsapi->getFramePropsRW( vs_frame );
    /* Sample aspect ratio */
    vsapi->propSetInt( props, "_SARNum", av_frame->sample_aspect_ratio.num, paReplace );
    vsapi->propSetInt( props, "_SARDen", av_frame->sample_aspect_ratio.den, paReplace );
    /* Sample duration
     * Variable Frame Rate is not supported yet. */
    vsapi->propSetInt( props, "_DurationNum", vi->fpsDen, paReplace );
    vsapi->propSetInt( props, "_DurationDen", vi->fpsNum, paReplace );
    /* Color format */
    if( ctx )
    {
        vsapi->propSetInt( props, "_ColorRange",  ctx->color_range != AVCOL_RANGE_JPEG, paReplace );
        vsapi->propSetInt( props, "_ColorSpace",  ctx->colorspace,                      paReplace );
        int chroma_loc;
        switch( ctx->chroma_sample_location )
        {
            case AVCHROMA_LOC_LEFT       : chroma_loc = 0;  break;
            case AVCHROMA_LOC_CENTER     : chroma_loc = 1;  break;
            case AVCHROMA_LOC_TOPLEFT    : chroma_loc = 2;  break;
            case AVCHROMA_LOC_TOP        : chroma_loc = 3;  break;
            case AVCHROMA_LOC_BOTTOMLEFT : chroma_loc = 4;  break;
            case AVCHROMA_LOC_BOTTOM     : chroma_loc = 5;  break;
            default                      : chroma_loc = -1; break;
        }
        if( chroma_loc != -1 )
            vsapi->propSetInt( props, "_ChromaLocation", chroma_loc, paReplace );
    }
    /* Picture type */
    char pict_type = av_get_picture_type_char( av_frame->pict_type );
    vsapi->propSetData( props, "_PictType", &pict_type, 1, paReplace );
    /* Progressive or Interlaced */
    vsapi->propSetInt( props, "_FieldBased", !!av_frame->interlaced_frame, paReplace );
}

static const VSFrameRef *VS_CC vs_filter_get_frame( int n, int activation_reason, void **instance_data, void **frame_data, VSFrameContext *frame_ctx, VSCore *core, const VSAPI *vsapi )
{
    if( activation_reason != arInitial )
        return NULL;
    lwlibav_handler_t *hp = (lwlibav_handler_t *)*instance_data;
    VSVideoInfo       *vi = &hp->vi;
    uint32_t frame_number = n + 1;     /* frame_number is 1-origin. */
    if( frame_number > vi->numFrames )
    {
        vsapi->setFilterError( "lsmas: exceeded the number of frames.", frame_ctx );
        return NULL;
    }
    lwlibav_video_decode_handler_t *vdhp = &hp->vdh;
    lwlibav_video_output_handler_t *vohp = &hp->voh;
    if( frame_number < vohp->first_valid_frame_number || vi->numFrames == 1 )
    {
        /* Copy the first valid video frame. */
        vdhp->last_frame_number = vi->numFrames + 1;    /* Force seeking at the next access for valid video sample. */
        return vsapi->cloneFrameRef( vohp->first_valid_frame );
    }
    if( vdhp->eh.error )
        return vsapi->newVideoFrame( vi->format, vi->width, vi->height, NULL, core );
    /* Set up VapourSynth error handler. */
    vs_basic_handler_t vsbh = { 0 };
    vsbh.out       = NULL;
    vsbh.frame_ctx = frame_ctx;
    vsbh.vsapi     = vsapi;
    vdhp->eh.message_priv  = &vsbh;
    vdhp->eh.error_message = set_error;
    /* Get and decode the desired video frame. */
    vs_video_output_handler_t *vs_vohp = (vs_video_output_handler_t *)vohp->private_handler;
    vs_vohp->frame_ctx = frame_ctx;
    vs_vohp->core      = core;
    vs_vohp->vsapi     = vsapi;
    vdhp->ctx->opaque = vohp;
    if( lwlibav_get_video_frame( vdhp, frame_number, vi->numFrames ) )
        return NULL;
    /* Output the video frame. */
    AVFrame    *av_frame = vdhp->frame_buffer;
    VSFrameRef *vs_frame = make_frame( vohp, av_frame );
    if( !vs_frame )
    {
        vsapi->setFilterError( "lsmas: failed to output a video frame.", frame_ctx );
        return vsapi->newVideoFrame( vi->format, vi->width, vi->height, NULL, core );
    }
    set_frame_properties( hp, av_frame, vs_frame, vsapi );
    return vs_frame != av_frame->opaque
         ? vs_frame
         : vsapi->cloneFrameRef( vs_frame );
}

static void VS_CC vs_filter_free( void *instance_data, VSCore *core, const VSAPI *vsapi )
{
    lwlibav_handler_t *hp = (lwlibav_handler_t *)instance_data;
    if( !hp )
        return;
    lwlibav_cleanup_video_decode_handler( &hp->vdh );
    if( hp->voh.first_valid_frame )
        vsapi->freeFrame( hp->voh.first_valid_frame );
    if( hp->voh.scaler.sws_ctx )
        sws_freeContext( hp->voh.scaler.sws_ctx );
    if( hp->voh.free_private_handler && hp->voh.private_handler )
        hp->voh.free_private_handler( hp->voh.private_handler );
    if( hp->lwh.file_path )
        free( hp->lwh.file_path );
    free( hp );
}

void VS_CC vs_lwlibavsource_create( const VSMap *in, VSMap *out, void *user_data, VSCore *core, const VSAPI *vsapi )
{
    /* Get file path. */
    const char *file_path = vsapi->propGetData( in, "source", 0, NULL );
    if( !file_path )
    {
        vsapi->setError( out, "lsmas: failed to get source file name." );
        return;
    }
    /* Allocate the handler of this filter function. */
    lwlibav_handler_t *hp = lw_malloc_zero( sizeof(lwlibav_handler_t) );
    if( !hp )
    {
        vsapi->setError( out, "lsmas: failed to allocate the LW-Libav handler." );
        return;
    }
    lwlibav_file_handler_t         *lwhp = &hp->lwh;
    lwlibav_video_decode_handler_t *vdhp = &hp->vdh;
    lwlibav_video_output_handler_t *vohp = &hp->voh;
    vs_video_output_handler_t *vs_vohp = lw_malloc_zero( sizeof(vs_video_output_handler_t) );
    if( !vs_vohp )
    {
        free( hp );
        vsapi->setError( out, "lsmas: failed to allocate the VapourSynth video output handler." );
        return;
    }
    vohp->private_handler      = vs_vohp;
    vohp->free_private_handler = free;
    /* Set up VapourSynth error handler. */
    vs_basic_handler_t vsbh = { 0 };
    vsbh.out       = out;
    vsbh.frame_ctx = NULL;
    vsbh.vsapi     = vsapi;
    /* Set up LW-Libav error handler. */
    error_handler_t eh = { 0 };
    eh.message_priv  = &vsbh;
    eh.error_message = set_error;
    /* Get options. */
    int64_t stream_index;
    int64_t threads;
    int64_t cache_index;
    int64_t seek_mode;
    int64_t seek_threshold;
    int64_t variable_info;
    int64_t direct_rendering;
    const char *format;
    set_option_int64 ( &stream_index,    -1,    "stream_index",   in, vsapi );
    set_option_int64 ( &threads,          0,    "threads",        in, vsapi );
    set_option_int64 ( &cache_index,      1,    "cache_index",    in, vsapi );
    set_option_int64 ( &seek_mode,        0,    "seek_mode",      in, vsapi );
    set_option_int64 ( &seek_threshold,   10,   "seek_threshold", in, vsapi );
    set_option_int64 ( &variable_info,    0,    "variable",       in, vsapi );
    set_option_int64 ( &direct_rendering, 0,    "dr",             in, vsapi );
    set_option_string( &format,           NULL, "format",         in, vsapi );
    /* Set options. */
    lwlibav_option_t opt;
    opt.file_path         = file_path;
    opt.threads           = threads >= 0 ? threads : 0;
    opt.av_sync           = 0;
    opt.no_create_index   = !cache_index;
    opt.force_video       = (stream_index >= 0);
    opt.force_video_index = stream_index >= 0 ? stream_index : -1;
    opt.force_audio       = 0;
    opt.force_audio_index = -1;
    vdhp->seek_mode                 = CLIP_VALUE( seek_mode,         0, 2 );
    vdhp->forward_seek_threshold    = CLIP_VALUE( seek_threshold,    1, 999 );
    vs_vohp->variable_info          = CLIP_VALUE( variable_info,     0, 1 );
    vs_vohp->direct_rendering       = CLIP_VALUE( direct_rendering,  0, 1 ) && !format;
    vs_vohp->vs_output_pixel_format = vs_vohp->variable_info ? pfNone : get_vs_output_pixel_format( format );
    /* Set up progress indicator. */
    progress_indicator_t indicator;
    indicator.open   = NULL;
    indicator.update = NULL;
    indicator.close  = NULL;
    /* Construct index. */
    lwlibav_audio_decode_handler_t adh = { 0 };
    lwlibav_audio_output_handler_t aoh = { 0 };
    int ret = lwlibav_construct_index( lwhp, vdhp, &adh, &aoh, &eh, &opt, &indicator, NULL );
    lwlibav_cleanup_audio_decode_handler( &adh );
    lwlibav_cleanup_audio_output_handler( &aoh );
    if( ret < 0 )
    {
        vs_filter_free( hp, core, vsapi );
        set_error( &vsbh, "lsmas: failed to construct index." );
        return;
    }
    /* Get the desired video track. */
    vdhp->eh = eh;
    if( lwlibav_get_desired_video_track( lwhp->file_path, vdhp, lwhp->threads ) < 0 )
    {
        vs_filter_free( hp, core, vsapi );
        return;
    }
    /* Set up timestamp info. */
    hp->vi.numFrames = vdhp->frame_count;
    int fps_num;
    int fps_den;
    lwlibav_setup_timestamp_info( vdhp, &fps_num, &fps_den );
    hp->vi.fpsNum = (unsigned int)fps_num;
    hp->vi.fpsDen = (unsigned int)fps_den;
    /* Set up decoders for this stream. */
    if( prepare_video_decoding( hp, core, vsapi ) < 0 )
    {
        vs_filter_free( hp, core, vsapi );
        return;
    }
    vsapi->createFilter( in, out, "LWLibavSource", vs_filter_init, vs_filter_get_frame, vs_filter_free, fmSerial, 0, hp, core );
    return;
}
