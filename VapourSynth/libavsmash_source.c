/*****************************************************************************
 * libavsmash_source.c
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

/* L-SMASH (ISC) */
#define LSMASH_DEMUXER_ENABLED
#include <lsmash.h>                 /* Demuxer */

/* Libav (LGPL or GPL) */
#include <libavformat/avformat.h>       /* Codec specific info importer */
#include <libavcodec/avcodec.h>         /* Decoder */
#include <libswscale/swscale.h>         /* Colorspace converter */
#include <libavutil/imgutils.h>

#include "lsmashsource.h"
#include "video_output.h"

#include "../common/libavsmash.h"
#include "../common/libavsmash_video.h"

typedef struct
{
    VSVideoInfo                       vi;
    libavsmash_video_decode_handler_t vdh;
    libavsmash_video_output_handler_t voh;
    AVFormatContext                  *format_ctx;
    uint32_t                          media_timescale;
} lsmas_handler_t;

static void VS_CC vs_filter_init( VSMap *in, VSMap *out, void **instance_data, VSNode *node, VSCore *core, const VSAPI *vsapi )
{
    lsmas_handler_t *hp = (lsmas_handler_t *)*instance_data;
    vsapi->setVideoInfo( &hp->vi, 1, node );
}

static int get_composition_duration
(
    libavsmash_video_decode_handler_t *vdhp,
    uint32_t                           composition_sample_number,
    uint32_t                           last_sample_number
)
{
    uint32_t decoding_sample_number = get_decoding_sample_number( vdhp->order_converter, composition_sample_number );
    if( composition_sample_number == last_sample_number )
        goto no_composition_duration;
    uint32_t next_decoding_sample_number = get_decoding_sample_number( vdhp->order_converter, composition_sample_number + 1 );
    uint64_t      cts;
    uint64_t next_cts;
    if( lsmash_get_cts_from_media_timeline( vdhp->root, vdhp->track_ID,      decoding_sample_number,      &cts )
     || lsmash_get_cts_from_media_timeline( vdhp->root, vdhp->track_ID, next_decoding_sample_number, &next_cts ) )
        goto no_composition_duration;
    if( next_cts <= cts || (next_cts - cts) > INT_MAX )
        return 0;
    return (int)(next_cts - cts);
no_composition_duration:;
    uint32_t sample_delta;
    if( lsmash_get_sample_delta_from_media_timeline( vdhp->root, vdhp->track_ID, decoding_sample_number, &sample_delta ) )
        return 0;
    return sample_delta <= INT_MAX ? sample_delta : 0;
}

static void set_sample_duration
(
    libavsmash_video_decode_handler_t *vdhp,
    VSVideoInfo                       *vi,
    VSMap                             *props,
    uint32_t                           sample_number,
    const VSAPI                       *vsapi
)
{
    int sample_duration = get_composition_duration( vdhp, sample_number, vi->numFrames );
    if( sample_duration == 0 )
    {
        vsapi->propSetInt( props, "_DurationNum", vi->fpsDen,      paReplace );
        vsapi->propSetInt( props, "_DurationDen", vi->fpsNum,      paReplace );
    }
    else
    {
        uint32_t media_timescale = lsmash_get_media_timescale( vdhp->root, vdhp->track_ID );
        vsapi->propSetInt( props, "_DurationNum", sample_duration, paReplace );
        vsapi->propSetInt( props, "_DurationDen", media_timescale, paReplace );
    }
}

static void set_frame_properties
(
    libavsmash_video_decode_handler_t *vdhp,
    VSVideoInfo                       *vi,
    AVFrame                           *av_frame,
    VSFrameRef                        *vs_frame,
    uint32_t                           sample_number,
    const VSAPI                       *vsapi
)
{
    AVCodecContext *ctx   = vdhp->config.ctx;
    VSMap          *props = vsapi->getFramePropsRW( vs_frame );
    /* Sample duration */
    set_sample_duration( vdhp, vi, props, sample_number, vsapi );
    /* Sample aspect ratio */
    vsapi->propSetInt( props, "_SARNum", av_frame->sample_aspect_ratio.num, paReplace );
    vsapi->propSetInt( props, "_SARDen", av_frame->sample_aspect_ratio.den, paReplace );
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

static int prepare_video_decoding( lsmas_handler_t *hp, VSCore *core, const VSAPI *vsapi )
{
    libavsmash_video_decode_handler_t *vdhp = &hp->vdh;
    libavsmash_video_output_handler_t *vohp = &hp->voh;
    VSVideoInfo                       *vi   = &hp->vi;
    lw_log_handler_t                  *lhp  = &vdhp->config.lh;
    vdhp->frame_buffer = av_frame_alloc();
    if( !vdhp->frame_buffer )
    {
        set_error( lhp, LW_LOG_FATAL, "lsmas: failed to allocate video frame buffer." );
        return -1;
    }
    /* Initialize the video decoder configuration. */
    codec_configuration_t *config = &vdhp->config;
    if( initialize_decoder_configuration( vdhp->root, vdhp->track_ID, config ) )
    {
        set_error( lhp, LW_LOG_FATAL, "lsmas: failed to initialize the decoder configuration." );
        return -1;
    }
    /* Set up output format. */
    if( determine_colorspace_conversion( vohp, config->ctx->pix_fmt ) )
    {
        set_error( lhp, LW_LOG_FATAL, "lsmas: %s is not supported", av_get_pix_fmt_name( config->ctx->pix_fmt ) );
        return -1;
    }
    if( initialize_scaler_handler( &vohp->scaler, config->ctx, vohp->scaler.enabled, SWS_FAST_BILINEAR, vohp->scaler.output_pixel_format ) < 0 )
    {
        set_error( lhp, LW_LOG_FATAL, "lsmas: failed to initialize scaler handler." );
        return -1;
    }
    vs_video_output_handler_t *vs_vohp = (vs_video_output_handler_t *)vohp->private_handler;
    vs_vohp->frame_ctx = NULL;
    vs_vohp->core      = core;
    vs_vohp->vsapi     = vsapi;
    config->get_buffer = setup_video_rendering( vohp, config->ctx, vi, config->prefer.width, config->prefer.height );
    if( !config->get_buffer )
    {
        set_error( lhp, LW_LOG_FATAL, "lsmas: failed to allocate memory for the background black frame data." );
        return -1;
    }
    /* Find the first valid video sample. */
    if( libavsmash_find_first_valid_video_frame( vdhp, vi->numFrames ) < 0 )
    {
        set_error( lhp, LW_LOG_FATAL, "lsmas: failed to allocate the first valid video frame." );
        return -1;
    }
    /* Force seeking at the first reading. */
    vdhp->last_sample_number = vi->numFrames + 1;
    return 0;
}

static const VSFrameRef *VS_CC vs_filter_get_frame( int n, int activation_reason, void **instance_data, void **frame_data, VSFrameContext *frame_ctx, VSCore *core, const VSAPI *vsapi )
{
    if( activation_reason != arInitial )
        return NULL;
    lsmas_handler_t *hp = (lsmas_handler_t *)*instance_data;
    VSVideoInfo     *vi = &hp->vi;
    uint32_t sample_number = MIN( n + 1, vi->numFrames );   /* For L-SMASH, sample_number is 1-origin. */
    libavsmash_video_decode_handler_t *vdhp = &hp->vdh;
    libavsmash_video_output_handler_t *vohp = &hp->voh;
    codec_configuration_t *config = &vdhp->config;
    if( config->error )
        return vsapi->newVideoFrame( vi->format, vi->width, vi->height, NULL, core );
    /* Set up VapourSynth error handler. */
    vs_basic_handler_t vsbh = { 0 };
    vsbh.out       = NULL;
    vsbh.frame_ctx = frame_ctx;
    vsbh.vsapi     = vsapi;
    config->lh.priv     = &vsbh;
    config->lh.show_log = set_error;
    /* Get and decode the desired video frame. */
    vs_video_output_handler_t *vs_vohp = (vs_video_output_handler_t *)vohp->private_handler;
    vs_vohp->frame_ctx = frame_ctx;
    vs_vohp->core      = core;
    vs_vohp->vsapi     = vsapi;
    if( libavsmash_get_video_frame( vdhp, sample_number, vi->numFrames ) < 0 )
        return NULL;
    /* Output video frame. */
    AVFrame    *av_frame = vdhp->frame_buffer;
    VSFrameRef *vs_frame = make_frame( vohp, config->ctx, av_frame );
    if( !vs_frame )
    {
        vsapi->setFilterError( "lsmas: failed to output a video frame.", frame_ctx );
        return vsapi->newVideoFrame( vi->format, vi->width, vi->height, NULL, core );
    }
    set_frame_properties( vdhp, vi, av_frame, vs_frame, sample_number, vsapi );
    return vs_frame;
}

static void VS_CC vs_filter_free( void *instance_data, VSCore *core, const VSAPI *vsapi )
{
    lsmas_handler_t *hp = (lsmas_handler_t *)instance_data;
    if( !hp )
        return;
    libavsmash_cleanup_video_decode_handler( &hp->vdh );
    libavsmash_cleanup_video_output_handler( &hp->voh );
    if( hp->format_ctx )
        avformat_close_input( &hp->format_ctx );
    lsmash_destroy_root( hp->vdh.root );
    free( hp );
}

static uint32_t open_file
(
    lsmas_handler_t *hp,
    const char      *source,
    VSMap           *out,
    const VSAPI     *vsapi
)
{
    /* L-SMASH */
    hp->vdh.root = lsmash_open_movie( source, LSMASH_FILE_MODE_READ );
    if( !hp->vdh.root )
    {
        vsapi->setError( out, "lsmas: failed to lsmash_open_movie." );
        return 0;
    }
    lsmash_movie_parameters_t movie_param;
    lsmash_initialize_movie_parameters( &movie_param );
    lsmash_get_movie_parameters( hp->vdh.root, &movie_param );
    if( movie_param.number_of_tracks == 0 )
    {
        vsapi->setError( out, "lsmas: the number of tracks equals 0." );
        return 0;
    }
    /* libavformat */
    av_register_all();
    avcodec_register_all();
    if( avformat_open_input( &hp->format_ctx, source, NULL, NULL ) )
    {
        vsapi->setError( out, "lsmas: failed to avformat_open_input." );
        return 0;
    }
    if( avformat_find_stream_info( hp->format_ctx, NULL ) < 0 )
    {
        vsapi->setError( out, "lsmas: failed to avformat_find_stream_info." );
        return 0;
    }
    return movie_param.number_of_tracks;
}

static int get_video_track( lsmas_handler_t *hp, uint32_t track_number, int threads, uint32_t number_of_tracks )
{
    libavsmash_video_decode_handler_t *vdhp = &hp->vdh;
    lw_log_handler_t                  *lhp  = &vdhp->config.lh;
    /* L-SMASH */
    uint32_t i;
    lsmash_media_parameters_t media_param;
    if( track_number == 0 )
    {
        /* Get the first video track. */
        for( i = 1; i <= number_of_tracks; i++ )
        {
            vdhp->track_ID = lsmash_get_track_ID( vdhp->root, i );
            if( vdhp->track_ID == 0 )
            {
                set_error( lhp, LW_LOG_FATAL, "lsmas: failed to find video track." );
                return -1;
            }
            lsmash_initialize_media_parameters( &media_param );
            if( lsmash_get_media_parameters( vdhp->root, vdhp->track_ID, &media_param ) )
            {
                set_error( lhp, LW_LOG_FATAL, "lsmas: failed to get media parameters." );
                return -1;
            }
            if( media_param.handler_type == ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK )
                break;
        }
        if( i > number_of_tracks )
        {
            set_error( lhp, LW_LOG_FATAL, "lsmas: failed to find video track." );
            return -1;
        }
    }
    else
    {
        /* Get the desired video track. */
        vdhp->track_ID = lsmash_get_track_ID( vdhp->root, track_number );
        if( vdhp->track_ID == 0 )
        {
            set_error( lhp, LW_LOG_FATAL, "lsmas: failed to find video track %"PRIu32".", track_number );
            return -1;
        }
        lsmash_initialize_media_parameters( &media_param );
        if( lsmash_get_media_parameters( vdhp->root, vdhp->track_ID, &media_param ) )
        {
            set_error( lhp, LW_LOG_FATAL, "lsmas: failed to get media parameters." );
            return -1;
        }
        if( media_param.handler_type != ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK )
        {
            set_error( lhp, LW_LOG_FATAL, "lsmas: the track you specified is not a video track." );
            return -1;
        }
    }
    if( lsmash_construct_timeline( vdhp->root, vdhp->track_ID ) )
    {
        set_error( lhp, LW_LOG_FATAL, "lsmas: failed to get construct timeline." );
        return -1;
    }
    if( get_summaries( vdhp->root, vdhp->track_ID, &vdhp->config ) )
        return -1;
    hp->media_timescale = media_param.timescale;
    hp->vi.numFrames    = lsmash_get_sample_count_in_media_timeline( vdhp->root, vdhp->track_ID );
    hp->vi.fpsNum       = 25;
    hp->vi.fpsDen       = 1;
    libavsmash_setup_timestamp_info( vdhp, &hp->vi.fpsNum, &hp->vi.fpsDen, hp->vi.numFrames );
    /* libavformat */
    for( i = 0; i < hp->format_ctx->nb_streams && hp->format_ctx->streams[i]->codec->codec_type != AVMEDIA_TYPE_VIDEO; i++ );
    if( i == hp->format_ctx->nb_streams )
    {
        set_error( lhp, LW_LOG_FATAL, "lsmas: failed to find stream by libavformat." );
        return -1;
    }
    /* libavcodec */
    AVStream       *stream = hp->format_ctx->streams[i];
    AVCodecContext *ctx    = stream->codec;
    vdhp->config.ctx = ctx;
    AVCodec *codec = libavsmash_find_decoder( &vdhp->config );
    if( !codec )
    {
        set_error( lhp, LW_LOG_FATAL, "lsmas: failed to find %s decoder.", codec->name );
        return -1;
    }
    ctx->thread_count = threads;
    if( avcodec_open2( ctx, codec, NULL ) < 0 )
    {
        set_error( lhp, LW_LOG_FATAL, "lsmas: failed to avcodec_open2." );
        return -1;
    }
    return 0;
}

void VS_CC vs_libavsmashsource_create( const VSMap *in, VSMap *out, void *user_data, VSCore *core, const VSAPI *vsapi )
{
    /* Get file name. */
    const char *file_name = vsapi->propGetData( in, "source", 0, NULL );
    if( !file_name )
    {
        vsapi->setError( out, "lsmas: failed to get source file name." );
        return;
    }
    /* Allocate the handler of this plugin. */
    lsmas_handler_t *hp = lw_malloc_zero( sizeof(lsmas_handler_t) );
    if( !hp )
    {
        vsapi->setError( out, "lsmas: failed to allocate the handler." );
        return;
    }
    libavsmash_video_decode_handler_t *vdhp = &hp->vdh;
    libavsmash_video_output_handler_t *vohp = &hp->voh;
    vs_video_output_handler_t *vs_vohp = vs_allocate_video_output_handler( vohp );
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
    /* Set up log handler. */
    lw_log_handler_t lh = { 0 };
    lh.level    = LW_LOG_FATAL;
    lh.priv     = &vsbh;
    lh.show_log = set_error;
    /* Open source file. */
    uint32_t number_of_tracks = open_file( hp, file_name, out, vsapi );
    if( number_of_tracks == 0 )
    {
        vs_filter_free( hp, core, vsapi );
        return;
    }
    /* Get options. */
    int64_t track_number;
    int64_t threads;
    int64_t seek_mode;
    int64_t seek_threshold;
    int64_t variable_info;
    int64_t direct_rendering;
    const char *format;
    set_option_int64 ( &track_number,     0,    "track",          in, vsapi );
    set_option_int64 ( &threads,          0,    "threads",        in, vsapi );
    set_option_int64 ( &seek_mode,        0,    "seek_mode",      in, vsapi );
    set_option_int64 ( &seek_threshold,   10,   "seek_threshold", in, vsapi );
    set_option_int64 ( &variable_info,    0,    "variable",       in, vsapi );
    set_option_int64 ( &direct_rendering, 0,    "dr",             in, vsapi );
    set_option_string( &format,           NULL, "format",         in, vsapi );
    threads                         = threads >= 0 ? threads : 0;
    vdhp->seek_mode                 = CLIP_VALUE( seek_mode,      0, 2 );
    vdhp->forward_seek_threshold    = CLIP_VALUE( seek_threshold, 1, 999 );
    vs_vohp->variable_info          = CLIP_VALUE( variable_info,  0, 1 );
    vs_vohp->direct_rendering       = CLIP_VALUE( direct_rendering,  0, 1 ) && !format;
    vs_vohp->vs_output_pixel_format = vs_vohp->variable_info ? pfNone : get_vs_output_pixel_format( format );
    if( track_number && track_number > number_of_tracks )
    {
        vs_filter_free( hp, core, vsapi );
        set_error( &lh, LW_LOG_FATAL, "lsmas: the number of tracks equals %"PRIu32".", number_of_tracks );
        return;
    }
    /* Get video track. */
    if( get_video_track( hp, track_number, threads, number_of_tracks ) < 0 )
    {
        vs_filter_free( hp, core, vsapi );
        return;
    }
    /* Set up decoders for this track. */
    lsmash_discard_boxes( vdhp->root );
    vdhp->config.lh = lh;
    if( prepare_video_decoding( hp, core, vsapi ) < 0 )
    {
        vs_filter_free( hp, core, vsapi );
        return;
    }
    vsapi->createFilter( in, out, "LibavSMASHSource", vs_filter_init, vs_filter_get_frame, vs_filter_free, fmSerial, 0, hp, core );
    return;
}
