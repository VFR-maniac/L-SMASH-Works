/*****************************************************************************
 * lsmashsource.c
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

#include "VapourSynth.h"
#include "video_output.h"

#include "../common/libavsmash.h"

#define MIN( a, b ) ((a) > (b) ? (b) : (a))
#define CLIP_VALUE( value, min, max ) ((value) > (max) ? (max) : (value) < (min) ? (min) : (value))

#define SEEK_MODE_NORMAL     0
#define SEEK_MODE_UNSAFE     1
#define SEEK_MODE_AGGRESSIVE 2

typedef struct
{
    uint32_t composition_to_decoding;
} order_converter_t;

typedef struct
{
    lsmash_root_t        *root;
    uint32_t              track_ID;
    uint32_t              forward_seek_threshold;
    int                   seek_mode;
    codec_configuration_t config;
    AVFormatContext      *format_ctx;
    AVFrame              *frame_buffer;
    order_converter_t    *order_converter;
    uint32_t              last_sample_number;
    uint32_t              last_rap_number;
    uint32_t              media_timescale;
} video_decode_handler_t;

typedef struct
{
    VSMap          *out;
    VSFrameContext *frame_ctx;
    const VSAPI    *vsapi;
} vs_basic_handler_t;

typedef struct
{
    VSVideoInfo            vi;
    video_decode_handler_t vdh;
    video_output_handler_t voh;
    vs_basic_handler_t     eh;
} lsmas_handler_t;

static void set_error( void *message_priv, const char *message, ... )
{
    vs_basic_handler_t *eh = (vs_basic_handler_t *)message_priv;
    if( !eh || !eh->vsapi )
        return;
    char temp[256];
    va_list args;
    va_start( args, message );
    vsprintf( temp, message, args );
    va_end( args );
    if( eh->out )
        eh->vsapi->setError( eh->out, (const char *)temp );
    else if( eh->frame_ctx )
        eh->vsapi->setFilterError( (const char *)temp, eh->frame_ctx );
}

static void set_option_int64( int64_t *opt, int64_t default_value, const char *arg, const VSMap *in, const VSAPI *vsapi )
{
    int e;
    *opt = vsapi->propGetInt( in, arg, 0, &e );
    if( e )
        *opt = default_value;
}

static void set_option_string( const char **opt, const char *default_value, const char *arg, const VSMap *in, const VSAPI *vsapi )
{
    int e;
    *opt = vsapi->propGetData( in, arg, 0, &e );
    if( e )
        *opt = default_value;
}

static void VS_CC vs_filter_init( VSMap *in, VSMap *out, void **instance_data, VSNode *node, VSCore *core, const VSAPI *vsapi )
{
    lsmas_handler_t *hp = (lsmas_handler_t *)*instance_data;
    hp->eh.out   = out;
    hp->eh.vsapi = vsapi;
    vsapi->setVideoInfo( &hp->vi, 1, node );
}

static int prepare_video_decoding( lsmas_handler_t *hp, VSCore *core )
{
    video_decode_handler_t *vdhp = &hp->vdh;
    video_output_handler_t *vohp = &hp->voh;
    VSVideoInfo            *vi   = &hp->vi;
    vs_basic_handler_t      eh   = hp->eh;
    vdhp->frame_buffer = avcodec_alloc_frame();
    if( !vdhp->frame_buffer )
    {
        set_error( &eh, "lsmas: failed to allocate video frame buffer." );
        return -1;
    }
    /* Initialize the video decoder configuration. */
    codec_configuration_t *config = &vdhp->config;
    config->message_priv  = &hp->eh;
    config->error_message = set_error;
    if( initialize_decoder_configuration( vdhp->root, vdhp->track_ID, config ) )
    {
        set_error( &eh, "lsmas: failed to initialize the decoder configuration." );
        return -1;
    }
    /* Set up output format. */
    enum AVPixelFormat input_pixel_format = config->ctx->pix_fmt;
    if( determine_colorspace_conversion( vohp, &config->ctx->pix_fmt ) )
    {
        set_error( &eh, "lsmas: %s is not supported", av_get_pix_fmt_name( input_pixel_format ) );
        return -1;
    }
    const VSAPI *vsapi = eh.vsapi;
    if( vohp->variable_info )
    {
        vi->format = NULL;
        vi->width  = 0;
        vi->height = 0;
    }
    else
    {
        vi->format = vsapi->getFormatPreset( vohp->vs_output_pixel_format, core );
        vi->width  = config->prefer.width;
        vi->height = config->prefer.height;
    }
    vohp->scaler_flags = SWS_FAST_BILINEAR;
    vohp->sws_ctx = sws_getCachedContext( NULL,
                                          config->ctx->width, config->ctx->height, config->ctx->pix_fmt,
                                          config->ctx->width, config->ctx->height, vohp->av_output_pixel_format,
                                          vohp->scaler_flags, NULL, NULL, NULL );
    if( !vohp->sws_ctx )
    {
        set_error( &eh, "lsmas: failed to get swscale context." );
        return -1;
    }
    /* Find the first valid video sample. */
    for( uint32_t i = 1; i <= vi->numFrames + get_decoder_delay( config->ctx ); i++ )
    {
        AVPacket pkt = { 0 };
        get_sample( vdhp->root, vdhp->track_ID, i, &vdhp->config, &pkt );
        AVFrame *picture = vdhp->frame_buffer;
        avcodec_get_frame_defaults( picture );
        int got_picture;
        if( avcodec_decode_video2( config->ctx, picture, &got_picture, &pkt ) >= 0 && got_picture )
        {
            vohp->first_valid_frame_number = i - MIN( get_decoder_delay( config->ctx ), config->delay_count );
            if( vohp->first_valid_frame_number > 1 || vi->numFrames == 1 )
            {
                vohp->first_valid_frame = vsapi->newVideoFrame( vi->format, vi->width, vi->height, NULL, core );
                if( !vohp->first_valid_frame )
                {
                    set_error( &eh, "lsmas: failed to allocate memory for the first valid video frame data." );
                    return -1;
                }
                if( make_frame( vohp, picture, vohp->first_valid_frame, NULL, vsapi ) )
                {
                    vsapi->freeFrame( vohp->first_valid_frame );
                    vohp->first_valid_frame = NULL;
                    continue;
                }
            }
            break;
        }
        else if( pkt.data )
            ++ config->delay_count;
    }
    vdhp->last_sample_number = vi->numFrames + 1;   /* Force seeking at the first reading. */
    return 0;
}

static int decode_video_sample( video_decode_handler_t *hp, AVFrame *picture, int *got_picture, uint32_t sample_number )
{
    AVPacket pkt = { 0 };
    int ret = get_sample( hp->root, hp->track_ID, sample_number, &hp->config, &pkt );
    if( ret )
        return ret;
    if( pkt.flags != ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE )
    {
        pkt.flags = AV_PKT_FLAG_KEY;
        hp->last_rap_number = sample_number;
    }
    else
        pkt.flags = 0;
    avcodec_get_frame_defaults( picture );
    uint64_t cts = pkt.pts;
    ret = avcodec_decode_video2( hp->config.ctx, picture, got_picture, &pkt );
    picture->pts = cts;
    return ret < 0 ? -1 : 0;
}

static inline uint32_t get_decoding_sample_number( order_converter_t *order_converter, uint32_t composition_sample_number )
{
    return order_converter
         ? order_converter[composition_sample_number].composition_to_decoding
         : composition_sample_number;
}

static int find_random_accessible_point( video_decode_handler_t *hp, uint32_t composition_sample_number, uint32_t decoding_sample_number, uint32_t *rap_number )
{
    if( decoding_sample_number == 0 )
        decoding_sample_number = get_decoding_sample_number( hp->order_converter, composition_sample_number );
    lsmash_random_access_flag ra_flags;
    uint32_t distance;  /* distance from the closest random accessible point to the previous. */
    uint32_t number_of_leadings;
    if( lsmash_get_closest_random_accessible_point_detail_from_media_timeline( hp->root, hp->track_ID, decoding_sample_number,
                                                                               rap_number, &ra_flags, &number_of_leadings, &distance ) )
        *rap_number = 1;
    int roll_recovery = !!(ra_flags & ISOM_SAMPLE_RANDOM_ACCESS_FLAG_GDR);
    int is_leading    = number_of_leadings && (decoding_sample_number - *rap_number <= number_of_leadings);
    if( (roll_recovery || is_leading) && *rap_number > distance )
        *rap_number -= distance;
    /* Check whether random accessible point has the same decoder configuration or not. */
    decoding_sample_number = get_decoding_sample_number( hp->order_converter, composition_sample_number );
    do
    {
        lsmash_sample_t sample;
        lsmash_sample_t rap_sample;
        if( lsmash_get_sample_info_from_media_timeline( hp->root, hp->track_ID, decoding_sample_number, &sample )
         || lsmash_get_sample_info_from_media_timeline( hp->root, hp->track_ID, *rap_number, &rap_sample ) )
        {
            /* Fatal error. */
            *rap_number = hp->last_rap_number;
            return 0;
        }
        if( sample.index == rap_sample.index )
            break;
        uint32_t sample_index = sample.index;
        for( uint32_t i = decoding_sample_number - 1; i; i-- )
        {
            if( lsmash_get_sample_info_from_media_timeline( hp->root, hp->track_ID, i, &sample ) )
            {
                /* Fatal error. */
                *rap_number = hp->last_rap_number;
                return 0;
            }
            if( sample.index != sample_index )
            {
                if( distance )
                {
                    *rap_number += distance;
                    distance = 0;
                    continue;
                }
                else
                    *rap_number = i + 1;
            }
        }
        break;
    } while( 1 );
    return roll_recovery;
}

static uint32_t seek_video( video_decode_handler_t *hp, AVFrame *picture, uint32_t composition_sample_number, uint32_t rap_number, int error_ignorance )
{
    /* Prepare to decode from random accessible sample. */
    codec_configuration_t *config = &hp->config;
    if( config->update_pending )
        /* Update the decoder configuration. */
        update_configuration( hp->root, hp->track_ID, config );
    else
        flush_buffers( config );
    if( config->error )
        return 0;
    int dummy;
    uint64_t rap_cts = 0;
    uint32_t i;
    uint32_t decoder_delay = get_decoder_delay( config->ctx );
    for( i = rap_number; i < composition_sample_number + decoder_delay; i++ )
    {
        if( config->index == config->queue.index )
            config->delay_count = MIN( decoder_delay, i - rap_number );
        int ret = decode_video_sample( hp, picture, &dummy, i );
        /* Some decoders return -1 when feeding a leading sample.
         * We don't consider as an error if the return value -1 is caused by a leading sample since it's not fatal at all. */
        if( i == hp->last_rap_number )
            rap_cts = picture->pts;
        if( ret == -1 && (uint64_t)picture->pts >= rap_cts && !error_ignorance )
            return 0;
        else if( ret >= 1 )
            /* No decoding occurs. */
            break;
    }
    if( config->index == config->queue.index )
        config->delay_count = MIN( decoder_delay, i - rap_number );
    return i;
}

static int get_picture( video_decode_handler_t *hp, AVFrame *picture, uint32_t current, uint32_t goal, uint32_t sample_count )
{
    codec_configuration_t *config = &hp->config;
    int got_picture = (current > goal);
    while( current <= goal )
    {
        int ret = decode_video_sample( hp, picture, &got_picture, current );
        if( ret == -1 )
            return -1;
        else if( ret == 1 )
            /* Sample doesn't exist. */
            break;
        ++current;
        if( config->update_pending )
            /* A new decoder configuration is needed. Anyway, stop getting picture. */
            break;
        if( !got_picture )
            ++ config->delay_count;
    }
    /* Flush the last frames. */
    if( current > sample_count && get_decoder_delay( config->ctx ) )
        while( current <= goal )
        {
            AVPacket pkt = { 0 };
            av_init_packet( &pkt );
            pkt.data = NULL;
            pkt.size = 0;
            avcodec_get_frame_defaults( picture );
            if( avcodec_decode_video2( config->ctx, picture, &got_picture, &pkt ) < 0 )
                return -1;
            ++current;
        }
    return got_picture ? 0 : -1;
}

static int get_composition_duration( video_decode_handler_t *hp, uint32_t composition_sample_number, uint32_t last_sample_number )
{
    uint32_t decoding_sample_number = get_decoding_sample_number( hp->order_converter, composition_sample_number );
    if( composition_sample_number == last_sample_number )
        goto no_composition_duration;
    uint32_t next_decoding_sample_number = get_decoding_sample_number( hp->order_converter, composition_sample_number + 1 );
    uint64_t      cts;
    uint64_t next_cts;
    if( lsmash_get_cts_from_media_timeline( hp->root, hp->track_ID,      decoding_sample_number,      &cts )
     || lsmash_get_cts_from_media_timeline( hp->root, hp->track_ID, next_decoding_sample_number, &next_cts ) )
        goto no_composition_duration;
    if( next_cts <= cts || (next_cts - cts) > INT_MAX )
        return 0;
    return (int)(next_cts - cts);
no_composition_duration:;
    uint32_t sample_delta;
    if( lsmash_get_sample_delta_from_media_timeline( hp->root, hp->track_ID, decoding_sample_number, &sample_delta ) )
        return 0;
    return sample_delta <= INT_MAX ? sample_delta : 0;
}

static void set_frame_properties( lsmas_handler_t *hp, AVFrame *picture, VSFrameRef *frame, uint32_t sample_number, const VSAPI *vsapi )
{
    video_decode_handler_t *vdhp  = &hp->vdh;
    VSVideoInfo            *vi    = &hp->vi;
    AVCodecContext         *ctx   = vdhp->config.ctx;
    VSMap                  *props = vsapi->getFramePropsRW( frame );
    /* Sample aspect ratio */
    vsapi->propSetInt( props, "_SARNum", picture->sample_aspect_ratio.num, paReplace );
    vsapi->propSetInt( props, "_SARDen", picture->sample_aspect_ratio.den, paReplace );
    /* Sample duration */
    int sample_duration = get_composition_duration( vdhp, sample_number, vi->numFrames );
    if( sample_duration == 0 )
    {
        vsapi->propSetInt( props, "_DurationNum", vi->fpsDen,            paReplace );
        vsapi->propSetInt( props, "_DurationDen", vi->fpsNum,            paReplace );
    }
    else
    {
        vsapi->propSetInt( props, "_DurationNum", sample_duration,       paReplace );
        vsapi->propSetInt( props, "_DurationDen", vdhp->media_timescale, paReplace );
    }
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
    char pict_type = av_get_picture_type_char( picture->pict_type );
    vsapi->propSetData( props, "_PictType", &pict_type, 1, paReplace );
    /* Progressive or Interlaced */
    vsapi->propSetInt( props, "_FieldBased", !!picture->interlaced_frame, paReplace );
}

static const VSFrameRef *VS_CC vs_filter_get_frame( int n, int activation_reason, void **instance_data, void **frame_data, VSFrameContext *frame_ctx, VSCore *core, const VSAPI *vsapi )
{
#define MAX_ERROR_COUNT 3       /* arbitrary */
    if( activation_reason != arInitial )
        return NULL;
    lsmas_handler_t *hp = (lsmas_handler_t *)*instance_data;
    VSVideoInfo     *vi = &hp->vi;
    uint32_t sample_number = n + 1;     /* For L-SMASH, sample_number is 1-origin. */
    if( sample_number > vi->numFrames )
    {
        vsapi->setFilterError( "lsmas: exceeded the number of frames.", frame_ctx );
        return NULL;
    }
    video_decode_handler_t *vdhp = &hp->vdh;
    video_output_handler_t *vohp = &hp->voh;
    if( sample_number < vohp->first_valid_frame_number || vi->numFrames == 1 )
    {
        /* Copy the first valid video frame. */
        vdhp->last_sample_number = vi->numFrames + 1;   /* Force seeking at the next access for valid video sample. */
        return vsapi->copyFrame( vohp->first_valid_frame, core );
    }
    codec_configuration_t *config = &vdhp->config;
    if( config->error )
        return vsapi->newVideoFrame( vi->format, vi->width, vi->height, NULL, core );
    config->message_priv = &hp->eh;
    hp->eh.out       = NULL;
    hp->eh.frame_ctx = frame_ctx;
    hp->eh.vsapi     = vsapi;
    AVFrame *picture = vdhp->frame_buffer;
    uint32_t start_number;  /* number of sample, for normal decoding, where decoding starts excluding decoding delay */
    uint32_t rap_number;    /* number of sample, for seeking, where decoding starts excluding decoding delay */
    int seek_mode = vdhp->seek_mode;
    int roll_recovery = 0;
    if( sample_number > vdhp->last_sample_number
     && sample_number <= vdhp->last_sample_number + vdhp->forward_seek_threshold )
    {
        start_number = vdhp->last_sample_number + 1 + config->delay_count;
        rap_number = vdhp->last_rap_number;
    }
    else
    {
        roll_recovery = find_random_accessible_point( vdhp, sample_number, 0, &rap_number );
        if( rap_number == vdhp->last_rap_number && sample_number > vdhp->last_sample_number )
        {
            roll_recovery = 0;
            start_number = vdhp->last_sample_number + 1 + config->delay_count;
        }
        else
        {
            /* Require starting to decode from random accessible sample. */
            vdhp->last_rap_number = rap_number;
            start_number = seek_video( vdhp, picture, sample_number, rap_number, roll_recovery || seek_mode != SEEK_MODE_NORMAL );
        }
    }
    /* Get desired picture. */
    int error_count = 0;
    while( start_number == 0    /* Failed to seek. */
     || config->update_pending  /* Need to update the decoder configuration to decode pictures. */
     || get_picture( vdhp, picture, start_number, sample_number + config->delay_count, vi->numFrames ) )
    {
        if( config->update_pending )
        {
            roll_recovery = find_random_accessible_point( vdhp, sample_number, 0, &rap_number );
            vdhp->last_rap_number = rap_number;
        }
        else
        {
            /* Failed to get desired picture. */
            if( config->error || seek_mode == SEEK_MODE_AGGRESSIVE )
                 goto video_fail;
            if( ++error_count > MAX_ERROR_COUNT || rap_number <= 1 )
            {
                if( seek_mode == SEEK_MODE_UNSAFE )
                    goto video_fail;
                /* Retry to decode from the same random accessible sample with error ignorance. */
                seek_mode = SEEK_MODE_AGGRESSIVE;
            }
            else
            {
                /* Retry to decode from more past random accessible sample. */
                roll_recovery = find_random_accessible_point( vdhp, sample_number, rap_number - 1, &rap_number );
                if( vdhp->last_rap_number == rap_number )
                    goto video_fail;
                vdhp->last_rap_number = rap_number;
            }
        }
        start_number = seek_video( vdhp, picture, sample_number, rap_number, roll_recovery || seek_mode != SEEK_MODE_NORMAL );
    }
    vdhp->last_sample_number = sample_number;
    /* Output frame. */
    const VSFormat *vs_format;
    int             vs_width;
    int             vs_height;
    if( vohp->variable_info )
    {
        if( determine_colorspace_conversion( vohp, &config->ctx->pix_fmt ) )
        {
            vsapi->setFilterError( "lsmas: failed to determin output format.", frame_ctx );
            return NULL;
        }
        vs_format = vsapi->getFormatPreset( vohp->vs_output_pixel_format, core );
        vs_width  = picture->width;
        vs_height = picture->height;
    }
    else
    {
        vs_format = vi->format;
        vs_width  = vi->width;
        vs_height = vi->height;
    }
    VSFrameRef *frame = vsapi->newVideoFrame( vs_format, vs_width, vs_height, NULL, core );
    set_frame_properties( hp, picture, frame, sample_number, vsapi );
    if( make_frame( vohp, picture, frame, frame_ctx, vsapi ) )
    {
        vsapi->setFilterError( "lsmas: failed to output a frame.", frame_ctx );
        return frame;
    }
    return frame;
video_fail:
    vsapi->setFilterError( "lsmas: fatal error of decoding.", frame_ctx );
    return NULL;
#undef MAX_ERROR_COUNT
}

static void VS_CC vs_filter_free( void *instance_data, VSCore *core, const VSAPI *vsapi )
{
    lsmas_handler_t *hp = (lsmas_handler_t *)instance_data;
    if( !hp )
        return;
    if( hp->voh.first_valid_frame )
        vsapi->freeFrame( hp->voh.first_valid_frame );
    if( hp->vdh.order_converter )
        free( hp->vdh.order_converter );
    if( hp->vdh.frame_buffer )
        avcodec_free_frame( &hp->vdh.frame_buffer );
    if( hp->voh.sws_ctx )
        sws_freeContext( hp->voh.sws_ctx );
    cleanup_configuration( &hp->vdh.config );
    if( hp->vdh.format_ctx )
        avformat_close_input( &hp->vdh.format_ctx );
    lsmash_destroy_root( hp->vdh.root );
    free( hp );
}

static uint32_t open_file( lsmas_handler_t *hp, const char *source )
{
    VSMap       *out   = hp->eh.out;
    const VSAPI *vsapi = hp->eh.vsapi;
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
    if( avformat_open_input( &hp->vdh.format_ctx, source, NULL, NULL ) )
    {
        vsapi->setError( out, "lsmas: failed to avformat_open_input." );
        return 0;
    }
    if( avformat_find_stream_info( hp->vdh.format_ctx, NULL ) < 0 )
    {
        vsapi->setError( out, "lsmas: failed to avformat_find_stream_info." );
        return 0;
    }
    return movie_param.number_of_tracks;
}

static inline uint64_t get_gcd( uint64_t a, uint64_t b )
{
    if( !b )
        return a;
    while( 1 )
    {
        uint64_t c = a % b;
        if( !c )
            return b;
        a = b;
        b = c;
    }
}

static inline uint64_t reduce_fraction( uint64_t *a, uint64_t *b )
{
    uint64_t reduce = get_gcd( *a, *b );
    *a /= reduce;
    *b /= reduce;
    return reduce;
}

static int setup_timestamp_info( lsmas_handler_t *hp, uint64_t media_timescale )
{
    video_decode_handler_t *vdhp = &hp->vdh;
    VSVideoInfo            *vi   = &hp->vi;
    vs_basic_handler_t      eh   = hp->eh;
    if( vi->numFrames == 1 )
    {
        /* Calculate average framerate. */
        uint64_t media_duration = lsmash_get_media_duration_from_media_timeline( vdhp->root, vdhp->track_ID );
        if( media_duration == 0 )
            media_duration = UINT32_MAX;
        reduce_fraction( &media_timescale, &media_duration );
        vi->fpsNum = (int64_t)media_timescale;
        vi->fpsDen = (int64_t)media_duration;
        return 0;
    }
    lsmash_media_ts_list_t ts_list;
    if( lsmash_get_media_timestamps( vdhp->root, vdhp->track_ID, &ts_list ) )
    {
        set_error( &eh, "lsmas: failed to get timestamps." );
        return -1;
    }
    if( ts_list.sample_count != vi->numFrames )
    {
        set_error( &eh, "lsmas: failed to count number of video samples." );
        return -1;
    }
    uint32_t composition_sample_delay;
    if( lsmash_get_max_sample_delay( &ts_list, &composition_sample_delay ) )
    {
        lsmash_delete_media_timestamps( &ts_list );
        set_error( &eh, "lsmas: failed to get composition delay." );
        return -1;
    }
    if( composition_sample_delay )
    {
        /* Consider composition order for keyframe detection.
         * Note: sample number for L-SMASH is 1-origin. */
        vdhp->order_converter = malloc( (ts_list.sample_count + 1) * sizeof(order_converter_t) );
        if( !vdhp->order_converter )
        {
            lsmash_delete_media_timestamps( &ts_list );
            set_error( &eh, "lsmas: failed to allocate memory." );
            return -1;
        }
        for( uint32_t i = 0; i < ts_list.sample_count; i++ )
            ts_list.timestamp[i].dts = i + 1;
        lsmash_sort_timestamps_composition_order( &ts_list );
        for( uint32_t i = 0; i < ts_list.sample_count; i++ )
            vdhp->order_converter[i + 1].composition_to_decoding = (uint32_t)ts_list.timestamp[i].dts;
    }
    /* Calculate average framerate. */
    uint64_t largest_cts          = ts_list.timestamp[1].cts;
    uint64_t second_largest_cts   = ts_list.timestamp[0].cts;
    uint64_t composition_timebase = ts_list.timestamp[1].cts - ts_list.timestamp[0].cts;
    for( uint32_t i = 2; i < ts_list.sample_count; i++ )
    {
        if( ts_list.timestamp[i].cts == ts_list.timestamp[i - 1].cts )
        {
            lsmash_delete_media_timestamps( &ts_list );
            return 0;
        }
        composition_timebase = get_gcd( composition_timebase, ts_list.timestamp[i].cts - ts_list.timestamp[i - 1].cts );
        second_largest_cts = largest_cts;
        largest_cts = ts_list.timestamp[i].cts;
    }
    uint64_t reduce = reduce_fraction( &media_timescale, &composition_timebase );
    uint64_t composition_duration = ((largest_cts - ts_list.timestamp[0].cts) + (largest_cts - second_largest_cts)) / reduce;
    lsmash_delete_media_timestamps( &ts_list );
    vi->fpsNum = (int64_t)((vi->numFrames * ((double)media_timescale / composition_duration)) * composition_timebase + 0.5);
    vi->fpsDen = (int64_t)composition_timebase;
    return 0;
}

static int get_video_track( lsmas_handler_t *hp, uint32_t track_number, int threads, uint32_t number_of_tracks )
{
    video_decode_handler_t *vdhp = &hp->vdh;
    vs_basic_handler_t      eh   = hp->eh;
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
                set_error( &eh, "lsmas: failed to find video track." );
                return -1;
            }
            lsmash_initialize_media_parameters( &media_param );
            if( lsmash_get_media_parameters( vdhp->root, vdhp->track_ID, &media_param ) )
            {
                set_error( &eh, "lsmas: failed to get media parameters." );
                return -1;
            }
            if( media_param.handler_type == ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK )
                break;
        }
        if( i > number_of_tracks )
        {
            set_error( &eh, "lsmas: failed to find video track." );
            return -1;
        }
    }
    else
    {
        /* Get the desired video track. */
        vdhp->track_ID = lsmash_get_track_ID( vdhp->root, track_number );
        if( vdhp->track_ID == 0 )
        {
            set_error( &eh, "lsmas: failed to find video track %"PRIu32".", track_number );
            return -1;
        }
        lsmash_initialize_media_parameters( &media_param );
        if( lsmash_get_media_parameters( vdhp->root, vdhp->track_ID, &media_param ) )
        {
            set_error( &eh, "lsmas: failed to get media parameters." );
            return -1;
        }
        if( media_param.handler_type != ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK )
        {
            set_error( &eh, "lsmas: the track you specified is not a video track." );
            return -1;
        }
    }
    if( lsmash_construct_timeline( vdhp->root, vdhp->track_ID ) )
    {
        set_error( &eh, "lsmas: failed to get construct timeline." );
        return -1;
    }
    if( get_summaries( vdhp->root, vdhp->track_ID, &vdhp->config ) )
        return -1;
    hp->vi.numFrames = lsmash_get_sample_count_in_media_timeline( vdhp->root, vdhp->track_ID );
    hp->vdh.media_timescale = media_param.timescale;
    if( setup_timestamp_info( hp, hp->vdh.media_timescale ) )
        return -1;
    /* libavformat */
    for( i = 0; i < vdhp->format_ctx->nb_streams && vdhp->format_ctx->streams[i]->codec->codec_type != AVMEDIA_TYPE_VIDEO; i++ );
    if( i == vdhp->format_ctx->nb_streams )
    {
        set_error( &eh, "lsmas: failed to find stream by libavformat." );
        return -1;
    }
    /* libavcodec */
    AVStream *stream = vdhp->format_ctx->streams[i];
    AVCodecContext *ctx = stream->codec;
    vdhp->config.ctx = ctx;
    AVCodec *codec = avcodec_find_decoder( ctx->codec_id );
    if( !codec )
    {
        set_error( &eh, "lsmas: failed to find %s decoder.", codec->name );
        return -1;
    }
    ctx->thread_count = threads;
    if( avcodec_open2( ctx, codec, NULL ) < 0 )
    {
        set_error( &eh, "lsmas: failed to avcodec_open2." );
        return -1;
    }
    return 0;
}

static void VS_CC vs_filter_create( const VSMap *in, VSMap *out, void *user_data, VSCore *core, const VSAPI *vsapi )
{
    /* Get file name. */
    const char *file_name = vsapi->propGetData( in, "source", 0, NULL );
    if( !file_name )
    {
        vsapi->setError( out, "lsmas: failed to get source file name." );
        return;
    }
    /* Allocate the handler of this plugin. */
    lsmas_handler_t *hp = malloc( sizeof(lsmas_handler_t) );
    if( !hp )
    {
        vsapi->setError( out, "lsmas: failed to allocate the handler." );
        return;
    }
    memset( hp, 0, sizeof(lsmas_handler_t) );
    hp->eh.out       = out;
    hp->eh.frame_ctx = NULL;
    hp->eh.vsapi     = vsapi;
    vs_basic_handler_t eh = hp->eh;
    /* Open source file. */
    uint32_t number_of_tracks = open_file( hp, file_name );
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
    const char *format;
    set_option_int64 ( &track_number,   0,    "track",          in, vsapi );
    set_option_int64 ( &threads,        0,    "threads",        in, vsapi );
    set_option_int64 ( &seek_mode,      0,    "seek_mode",      in, vsapi );
    set_option_int64 ( &seek_threshold, 10,   "seek_threshold", in, vsapi );
    set_option_int64 ( &variable_info,  0,    "variable",       in, vsapi );
    set_option_string( &format,         NULL, "format",         in, vsapi );
    threads                        = threads >= 0 ? threads : 0;
    hp->vdh.seek_mode              = CLIP_VALUE( seek_mode,      0, 2 );
    hp->vdh.forward_seek_threshold = CLIP_VALUE( seek_threshold, 1, 999 );
    hp->voh.variable_info          = CLIP_VALUE( variable_info,  0, 1 );
    hp->voh.vs_output_pixel_format = hp->voh.variable_info ? pfNone : get_vs_output_pixel_format( format );
    if( track_number && track_number > number_of_tracks )
    {
        vs_filter_free( hp, core, vsapi );
        set_error( &eh, "lsmas: the number of tracks equals %"PRIu32".", number_of_tracks );
        return;
    }
    /* Get video track. */
    if( get_video_track( hp, track_number, threads, number_of_tracks ) < 0 )
    {
        vs_filter_free( hp, core, vsapi );
        return;
    }
    /* Set up decoders for this track. */
    lsmash_discard_boxes( hp->vdh.root );
    if( prepare_video_decoding( hp, core ) < 0 )
    {
        vs_filter_free( hp, core, vsapi );
        return;
    }
    vsapi->createFilter( in, out, "Source", vs_filter_init, vs_filter_get_frame, vs_filter_free, fmSerial, 0, hp, core );
    return;
}

VS_EXTERNAL_API(void) VapourSynthPluginInit( VSConfigPlugin config_func, VSRegisterFunction register_func, VSPlugin *plugin )
{
    config_func
    (
        "systems.innocent.lsmas",
        "lsmas",
        "LSMASHSource for VapourSynth",
        VAPOURSYNTH_API_VERSION,
        1,
        plugin
    );
    register_func
    (
        "Source",
        "source:data;track:int:opt;threads:int:opt;seek_mode:int:opt;seek_threshold:int:opt;variable:int:opt;format:data:opt;",
        vs_filter_create,
        NULL,
        plugin
    );
}
