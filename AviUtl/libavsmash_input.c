/*****************************************************************************
 * libavsmash_input.c
 *****************************************************************************
 * Copyright (C) 2011-2013 L-SMASH Works project
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
 * However, when distributing its binary file, it will be under LGPL or GPL.
 * Don't distribute it if its license is GPL. */

/* L-SMASH */
#define LSMASH_DEMUXER_ENABLED
#include <lsmash.h>                 /* Demuxer */

/* Libav
 * The binary file will be LGPLed or GPLed. */
#include <libavformat/avformat.h>       /* Codec specific info importer */
#include <libavcodec/avcodec.h>         /* Decoder */
#include <libswscale/swscale.h>         /* Colorspace converter */
#include <libavresample/avresample.h>   /* Audio resampler */
#include <libavutil/mathematics.h>

#include "lsmashinput.h"
#include "colorspace.h"
#include "video_output.h"
#include "audio_output.h"

#include "../common/libavsmash.h"
#include "../common/libavsmash_video.h"
#include "../common/libavsmash_audio.h"

typedef struct
{
    uint8_t *keyframe_list;
    uint32_t media_timescale;
    uint64_t skip_duration;
    int64_t  start_pts;
} libavsmash_video_info_handler_t;

typedef struct
{
    uint32_t media_timescale;
    int64_t  start_pts;
} libavsmash_audio_info_handler_t;

typedef struct libavsmash_handler_tag
{
    /* Global stuff */
    UINT                              uType;
    lsmash_root_t                    *root;
    lsmash_movie_parameters_t         movie_param;
    uint32_t                          number_of_tracks;
    AVFormatContext                  *format_ctx;
    int                               threads;
    /* Video stuff */
    libavsmash_video_info_handler_t   vih;
    libavsmash_video_decode_handler_t vdh;
    libavsmash_video_output_handler_t voh;
    /* Audio stuff */
    libavsmash_audio_info_handler_t   aih;
    libavsmash_audio_decode_handler_t adh;
    libavsmash_audio_output_handler_t aoh;
    int64_t                           av_gap;
    int                               av_sync;
} libavsmash_handler_t;

static void *open_file( char *file_name, reader_option_t *opt )
{
    libavsmash_handler_t *hp = lw_malloc_zero( sizeof(libavsmash_handler_t) );
    if( !hp )
        return NULL;
    hp->uType = MB_ICONERROR | MB_OK;
    hp->vdh.config.lh.priv     = &hp->uType;
    hp->adh.config.lh.priv     = &hp->uType;
    hp->vdh.config.lh.level    = LW_LOG_WARNING;
    hp->adh.config.lh.level    = LW_LOG_WARNING;
    hp->vdh.config.lh.show_log = au_message_box_desktop;
    hp->adh.config.lh.show_log = au_message_box_desktop;
    /* L-SMASH */
    hp->root = lsmash_open_movie( file_name, LSMASH_FILE_MODE_READ );
    if( !hp->root )
    {
        free( hp );
        return NULL;
    }
    lsmash_movie_parameters_t movie_param;
    lsmash_initialize_movie_parameters( &movie_param );
    lsmash_get_movie_parameters( hp->root, &movie_param );
    if( movie_param.number_of_tracks == 0 )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "The number of tracks equals 0." );
        goto open_fail;
    }
    hp->movie_param = movie_param;
    hp->number_of_tracks = movie_param.number_of_tracks;
    /* libavformat */
    av_register_all();
    avcodec_register_all();
    if( avformat_open_input( &hp->format_ctx, file_name, NULL, NULL ) )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to avformat_open_input." );
        goto open_fail;
    }
    if( avformat_find_stream_info( hp->format_ctx, NULL ) < 0 )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to avformat_find_stream_info." );
        goto open_fail;
    }
    hp->threads = opt->threads;
    hp->av_sync = opt->av_sync;
    return hp;
open_fail:
    if( hp->format_ctx )
        avformat_close_input( &hp->format_ctx );
    lsmash_destroy_root( hp->root );
    free( hp );
    return NULL;
}

static int setup_timestamp_info( lsmash_handler_t *h, uint32_t track_ID )
{
    libavsmash_handler_t *hp = (libavsmash_handler_t *)h->video_private;
    uint64_t media_timescale = hp->vih.media_timescale;
    if( h->video_sample_count == 1 )
    {
        /* Calculate average framerate. */
        uint64_t media_duration = lsmash_get_media_duration_from_media_timeline( hp->root, track_ID );
        if( media_duration == 0 )
            media_duration = INT32_MAX;
        reduce_fraction( &media_timescale, &media_duration );
        h->framerate_num = media_timescale;
        h->framerate_den = media_duration;
        return 0;
    }
    lsmash_media_ts_list_t ts_list;
    if( lsmash_get_media_timestamps( hp->root, track_ID, &ts_list ) )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to get timestamps." );
        return -1;
    }
    if( ts_list.sample_count != h->video_sample_count )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to count number of video samples." );
        return -1;
    }
    uint32_t composition_sample_delay;
    if( lsmash_get_max_sample_delay( &ts_list, &composition_sample_delay ) )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to get composition delay." );
        lsmash_delete_media_timestamps( &ts_list );
        return -1;
    }
    if( composition_sample_delay )
    {
        /* Consider composition order for keyframe detection.
         * Note: sample number for L-SMASH is 1-origin. */
        hp->vdh.order_converter = lw_malloc_zero( (ts_list.sample_count + 1) * sizeof(order_converter_t) );
        if( !hp->vdh.order_converter )
        {
            DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to allocate memory." );
            lsmash_delete_media_timestamps( &ts_list );
            return -1;
        }
        for( uint32_t i = 0; i < ts_list.sample_count; i++ )
            ts_list.timestamp[i].dts = i + 1;
        lsmash_sort_timestamps_composition_order( &ts_list );
        for( uint32_t i = 0; i < ts_list.sample_count; i++ )
            hp->vdh.order_converter[i + 1].composition_to_decoding = ts_list.timestamp[i].dts;
    }
    /* Calculate average framerate. */
    uint64_t largest_cts          = ts_list.timestamp[1].cts;
    uint64_t second_largest_cts   = ts_list.timestamp[0].cts;
    uint64_t composition_timebase = ts_list.timestamp[1].cts - ts_list.timestamp[0].cts;
    for( uint32_t i = 2; i < ts_list.sample_count; i++ )
    {
        if( ts_list.timestamp[i].cts == ts_list.timestamp[i - 1].cts )
        {
            MESSAGE_BOX_DESKTOP( MB_OK, "Detected CTS duplication at frame %"PRIu32, i );
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
    h->framerate_num = (h->video_sample_count * ((double)media_timescale / composition_duration)) * composition_timebase + 0.5;
    h->framerate_den = composition_timebase;
    return 0;
}

static uint64_t get_empty_duration( lsmash_root_t *root, uint32_t track_ID, uint32_t movie_timescale, uint32_t media_timescale )
{
    /* Consider empty duration if the first edit is an empty edit. */
    lsmash_edit_t edit;
    if( lsmash_get_explicit_timeline_map( root, track_ID, 1, &edit ) )
        return 0;
    if( edit.duration && edit.start_time == ISOM_EDIT_MODE_EMPTY )
        return av_rescale_q( edit.duration,
                             (AVRational){ 1, movie_timescale },
                             (AVRational){ 1, media_timescale } );
    return 0;
}

static int64_t get_start_time( lsmash_root_t *root, uint32_t track_ID )
{
    /* Consider start time of this media if any non-empty edit is present. */
    uint32_t edit_count = lsmash_count_explicit_timeline_map( root, track_ID );
    for( uint32_t edit_number = 1; edit_number <= edit_count; edit_number++ )
    {
        lsmash_edit_t edit;
        if( lsmash_get_explicit_timeline_map( root, track_ID, edit_number, &edit ) )
            return 0;
        if( edit.duration == 0 )
            return 0;   /* no edits */
        if( edit.start_time >= 0 )
            return edit.start_time;
    }
    return 0;
}

static int get_first_track_of_type( lsmash_handler_t *h, uint32_t type )
{
    libavsmash_handler_t *hp = (type == ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK)
                             ? (libavsmash_handler_t *)h->video_private
                             : (libavsmash_handler_t *)h->audio_private;
    /* L-SMASH */
    uint32_t track_ID = 0;
    uint32_t i;
    lsmash_media_parameters_t media_param;
    for( i = 1; i <= hp->number_of_tracks; i++ )
    {
        track_ID = lsmash_get_track_ID( hp->root, i );
        if( track_ID == 0 )
            return -1;
        lsmash_initialize_media_parameters( &media_param );
        if( lsmash_get_media_parameters( hp->root, track_ID, &media_param ) )
        {
            DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to get media parameters." );
            return -1;
        }
        if( media_param.handler_type == type )
            break;
    }
    if( i > hp->number_of_tracks )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to find %s track.",
                                   type == ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK ? "video" : "audio" );
        return -1;
    }
    if( lsmash_construct_timeline( hp->root, track_ID ) )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to get construct timeline." );
        return -1;
    }
    uint32_t ctd_shift;
    if( lsmash_get_composition_to_decode_shift_from_media_timeline( hp->root, track_ID, &ctd_shift ) )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to get the timeline shift." );
        return -1;
    }
    if( type == ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK )
    {
        hp->vdh.track_ID        = track_ID;
        hp->vih.media_timescale = media_param.timescale;
        h->video_sample_count = lsmash_get_sample_count_in_media_timeline( hp->root, track_ID );
        if( get_summaries( hp->root, track_ID, &hp->vdh.config ) )
            return -1;
        hp->vdh.config.lh.show_log = au_message_box_desktop;
        if( setup_timestamp_info( h, track_ID ) )
        {
            DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to set up timestamp info." );
            return -1;
        }
        if( hp->av_sync )
        {
            uint32_t min_cts_sample_number = hp->vdh.order_converter ? hp->vdh.order_converter[1].composition_to_decoding : 1;
            uint64_t min_cts;
            if( lsmash_get_cts_from_media_timeline( hp->root, track_ID, min_cts_sample_number, &min_cts ) )
            {
                DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to get the minimum CTS of video stream." );
                return -1;
            }
            hp->vih.start_pts = min_cts + ctd_shift
                              + get_empty_duration( hp->root, track_ID, hp->movie_param.timescale, hp->vih.media_timescale );
            hp->vih.skip_duration = ctd_shift + get_start_time( hp->root, track_ID );
        }
    }
    else
    {
        hp->adh.track_ID          = track_ID;
        hp->aih.media_timescale   = media_param.timescale;
        hp->adh.frame_count       = lsmash_get_sample_count_in_media_timeline( hp->root, track_ID );
        h->audio_pcm_sample_count = lsmash_get_media_duration_from_media_timeline( hp->root, track_ID );
        if( get_summaries( hp->root, track_ID, &hp->adh.config ) )
            return -1;
        hp->adh.config.lh.show_log = au_message_box_desktop;
        if( hp->av_sync )
        {
            uint64_t min_cts;
            if( lsmash_get_cts_from_media_timeline( hp->root, track_ID, 1, &min_cts ) )
            {
                DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to get the minimum CTS of audio stream." );
                return -1;
            }
            hp->aih.start_pts = min_cts + ctd_shift
                              + get_empty_duration( hp->root, track_ID, hp->movie_param.timescale, hp->aih.media_timescale );
            hp->aoh.skip_decoded_samples = ctd_shift + get_start_time( hp->root, track_ID );
        }
    }
    /* libavformat */
    type = (type == ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK) ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
    for( i = 0; i < hp->format_ctx->nb_streams && hp->format_ctx->streams[i]->codec->codec_type != type; i++ );
    if( i == hp->format_ctx->nb_streams )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to find stream by libavformat." );
        return -1;
    }
    /* libavcodec */
    AVStream *stream = hp->format_ctx->streams[i];
    AVCodecContext *ctx = stream->codec;
    if( type == AVMEDIA_TYPE_VIDEO )
        hp->vdh.config.ctx = ctx;
    else
        hp->adh.config.ctx = ctx;
    AVCodec *codec = avcodec_find_decoder( ctx->codec_id );
    if( !codec )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to find %s decoder.", codec->name );
        return -1;
    }
    ctx->thread_count = hp->threads;
    if( avcodec_open2( ctx, codec, NULL ) < 0 )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to avcodec_open2." );
        return -1;
    }
    return 0;
}

static int get_first_video_track( lsmash_handler_t *h )
{
    libavsmash_handler_t *hp = (libavsmash_handler_t *)h->video_private;
    if( !get_first_track_of_type( h, ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK ) )
        return 0;
    lsmash_destruct_timeline( hp->root, hp->vdh.track_ID );
    if( hp->vdh.config.ctx )
    {
        avcodec_close( hp->vdh.config.ctx );
        hp->vdh.config.ctx = NULL;
    }
    return -1;
}

static int get_first_audio_track( lsmash_handler_t *h )
{
    libavsmash_handler_t *hp = (libavsmash_handler_t *)h->audio_private;
    if( !get_first_track_of_type( h, ISOM_MEDIA_HANDLER_TYPE_AUDIO_TRACK ) )
        return 0;
    lsmash_destruct_timeline( hp->root, hp->adh.track_ID );
    if( hp->adh.config.ctx )
    {
        avcodec_close( hp->adh.config.ctx );
        hp->adh.config.ctx = NULL;
    }
    return -1;
}

static void destroy_disposable( void *private_stuff )
{
    libavsmash_handler_t *hp = (libavsmash_handler_t *)private_stuff;
    lsmash_discard_boxes( hp->root );
}

static int create_keyframe_list( libavsmash_handler_t *hp, uint32_t video_sample_count )
{
    libavsmash_video_info_handler_t *vihp = &hp->vih;
    vihp->keyframe_list = lw_malloc_zero( (video_sample_count + 1) * sizeof(uint8_t) );
    if( !vihp->keyframe_list )
        return -1;
    libavsmash_video_decode_handler_t *vdhp = &hp->vdh;
    for( uint32_t composition_sample_number = 1; composition_sample_number <= video_sample_count; composition_sample_number++ )
    {
        uint32_t decoding_sample_number = get_decoding_sample_number( vdhp->order_converter, composition_sample_number );
        uint32_t rap_number;
        if( lsmash_get_closest_random_accessible_point_from_media_timeline( hp->root, vdhp->track_ID, decoding_sample_number, &rap_number ) )
            continue;
        if( decoding_sample_number == rap_number )
            vihp->keyframe_list[composition_sample_number] = 1;
    }
    return 0;
}

static int prepare_video_decoding( lsmash_handler_t *h, video_option_t *opt )
{
    libavsmash_handler_t *hp = (libavsmash_handler_t *)h->video_private;
    libavsmash_video_decode_handler_t *vdhp = &hp->vdh;
    if( !vdhp->config.ctx )
        return 0;
    vdhp->frame_buffer = avcodec_alloc_frame();
    if( !vdhp->frame_buffer )
    {
        DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to allocate video frame buffer." );
        return -1;
    }
    vdhp->seek_mode              = opt->seek_mode;
    vdhp->forward_seek_threshold = opt->forward_seek_threshold;
    if( create_keyframe_list( hp, h->video_sample_count ) )
    {
        DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to create keyframe list." );
        return -1;
    }
    /* Initialize the video decoder configuration. */
    vdhp->root = hp->root;
    codec_configuration_t *config = &vdhp->config;
    if( initialize_decoder_configuration( vdhp->root, vdhp->track_ID, config ) )
    {
        DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to initialize the decoder configuration." );
        return -1;
    }
    /* Set up video rendering. */
    libavsmash_video_output_handler_t *vohp = &hp->voh;
    if( au_setup_video_rendering( vohp, config->ctx, opt, &h->video_format, config->prefer.width, config->prefer.height ) < 0 )
        return -1;
#ifndef DEBUG_VIDEO
    config->lh.level = LW_LOG_FATAL;
#endif
    /* Find the first valid video frame. */
    if( libavsmash_find_first_valid_video_frame( vdhp, h->video_sample_count ) < 0 )
        return -1;
    /* Force seeking at the first reading. */
    vdhp->last_sample_number = h->video_sample_count + 1;
    return 0;
}

static int prepare_audio_decoding( lsmash_handler_t *h, audio_option_t *opt )
{
    libavsmash_handler_t *hp = (libavsmash_handler_t *)h->audio_private;
    libavsmash_audio_decode_handler_t *adhp = &hp->adh;
    if( !adhp->config.ctx )
        return 0;
    adhp->frame_buffer = avcodec_alloc_frame();
    if( !adhp->frame_buffer )
    {
        DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to allocate audio frame buffer." );
        return -1;
    }
    /* Initialize the audio decoder configuration. */
    codec_configuration_t *config = &adhp->config;
    if( initialize_decoder_configuration( hp->root, adhp->track_ID, config ) )
    {
        DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to initialize the decoder configuration." );
        return -1;
    }
    libavsmash_audio_output_handler_t *aohp = &hp->aoh;
    aohp->output_channel_layout  = config->prefer.channel_layout;
    aohp->output_sample_format   = config->prefer.sample_format;
    aohp->output_sample_rate     = config->prefer.sample_rate;
    aohp->output_bits_per_sample = config->prefer.bits_per_sample;
    /* */
    adhp->root = hp->root;
#ifndef DEBUG_AUDIO
    config->lh.level = LW_LOG_FATAL;
#endif
    if( au_setup_audio_rendering( aohp, config->ctx, opt, &h->audio_format.Format ) < 0 )
        return -1;
    /* Count the number of PCM audio samples. */
    h->audio_pcm_sample_count = libavsmash_count_overall_pcm_samples( adhp, aohp->output_sample_rate, &aohp->skip_decoded_samples );
    if( h->audio_pcm_sample_count == 0 )
    {
        DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "No valid audio frame." );
        return -1;
    }
    if( hp->av_sync && hp->vdh.track_ID )
    {
        AVRational audio_sample_base = (AVRational){ 1, aohp->output_sample_rate };
        hp->av_gap = av_rescale_q( hp->aih.start_pts,
                                   (AVRational){ 1, hp->aih.media_timescale }, audio_sample_base )
                   - av_rescale_q( hp->vih.start_pts - hp->vih.skip_duration,
                                   (AVRational){ 1, hp->vih.media_timescale }, audio_sample_base );
        h->audio_pcm_sample_count += hp->av_gap;
    }
    /* Force seeking at the first reading. */
    adhp->next_pcm_sample_number = h->audio_pcm_sample_count + 1;
    return 0;
}

static int read_video( lsmash_handler_t *h, int sample_number, void *buf )
{
    libavsmash_handler_t *hp = (libavsmash_handler_t *)h->video_private;
    libavsmash_video_decode_handler_t *vdhp = &hp->vdh;
    if( vdhp->config.error )
        return 0;
    libavsmash_video_output_handler_t *vohp = &hp->voh;
    ++sample_number;            /* For L-SMASH, sample_number is 1-origin. */
    if( sample_number == 1 )
    {
        au_video_output_handler_t *au_vohp = (au_video_output_handler_t *)vohp->private_handler;
        memcpy( buf, au_vohp->back_ground, vohp->output_frame_size );
    }
    if( libavsmash_get_video_frame( vdhp, sample_number, h->video_sample_count ) < 0 )
        return 0;
    return convert_colorspace( vohp, vdhp->config.ctx, vdhp->frame_buffer, buf );
}

static int read_audio( lsmash_handler_t *h, int start, int wanted_length, void *buf )
{
    libavsmash_handler_t *hp = (libavsmash_handler_t *)h->audio_private;
    return libavsmash_get_pcm_audio_samples( &hp->adh, &hp->aoh, buf, start, wanted_length );
}

static int is_keyframe( lsmash_handler_t *h, int sample_number )
{
    libavsmash_handler_t *hp = (libavsmash_handler_t *)h->video_private;
    return hp->vih.keyframe_list[sample_number + 1];
}

static int delay_audio( lsmash_handler_t *h, int *start, int wanted_length, int audio_delay )
{
    /* Even if start become negative, its absolute value shall be equal to wanted_length or smaller. */
    libavsmash_handler_t *hp = (libavsmash_handler_t *)h->audio_private;
    int end = *start + wanted_length;
    audio_delay += hp->av_gap;
    if( *start < audio_delay && end <= audio_delay )
    {
        hp->adh.next_pcm_sample_number = h->audio_pcm_sample_count + 1;     /* Force seeking at the next access for valid audio frame. */
        return 0;
    }
    *start -= audio_delay;
    return 1;
}

static void video_cleanup( lsmash_handler_t *h )
{
    libavsmash_handler_t *hp = (libavsmash_handler_t *)h->video_private;
    if( !hp )
        return;
    if( hp->vih.keyframe_list )
        free( hp->vih.keyframe_list );
    libavsmash_cleanup_video_decode_handler( &hp->vdh );
    libavsmash_cleanup_video_output_handler( &hp->voh );
}

static void audio_cleanup( lsmash_handler_t *h )
{
    libavsmash_handler_t *hp = (libavsmash_handler_t *)h->audio_private;
    if( !hp )
        return;
    libavsmash_cleanup_audio_decode_handler( &hp->adh );
    libavsmash_cleanup_audio_output_handler( &hp->aoh );
}

static void close_file( void *private_stuff )
{
    libavsmash_handler_t *hp = (libavsmash_handler_t *)private_stuff;
    if( !hp )
        return;
    if( hp->format_ctx )
        avformat_close_input( &hp->format_ctx );
    lsmash_destroy_root( hp->root );
    free( hp );
}

lsmash_reader_t libavsmash_reader =
{
    LIBAVSMASH_READER,
    open_file,
    get_first_video_track,
    get_first_audio_track,
    destroy_disposable,
    prepare_video_decoding,
    prepare_audio_decoding,
    read_video,
    read_audio,
    is_keyframe,
    delay_audio,
    video_cleanup,
    audio_cleanup,
    close_file
};
