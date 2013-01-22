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
#include <libavutil/opt.h>

#include "lsmashinput.h"
#include "colorspace.h"
#include "resample.h"

#include "../common/libavsmash.h"

#define SEEK_MODE_NORMAL     0
#define SEEK_MODE_UNSAFE     1
#define SEEK_MODE_AGGRESSIVE 2

typedef struct
{
    uint8_t *keyframe_list;
    uint32_t media_timescale;
    uint64_t skip_duration;
    int64_t  start_pts;
} video_info_handler_t;

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
    AVFrame              *frame_buffer;
    order_converter_t    *order_converter;
    uint32_t              last_sample_number;
    uint32_t              last_rap_number;
} video_decode_handler_t;

typedef struct
{
    struct SwsContext       *sws_ctx;
    int                      scaler_flags;
    enum PixelFormat         output_pixel_format;
    uint32_t                 first_valid_sample_number;
    uint8_t                 *first_valid_sample_data;
    int                      output_linesize;
    uint32_t                 output_sample_size;
    uint8_t                 *back_ground;
    func_convert_colorspace *convert_colorspace;
} video_output_handler_t;

typedef struct
{
    uint32_t media_timescale;
    int64_t  start_pts;
} audio_info_handler_t;

typedef struct
{
    lsmash_root_t        *root;
    uint32_t              track_ID;
    codec_configuration_t config;
    AVFrame              *frame_buffer;
    AVPacket              packet;
    uint32_t              next_pcm_sample_number;
    uint32_t              last_frame_number;
    uint32_t              frame_count;
} audio_decode_handler_t;

typedef struct
{
    AVAudioResampleContext *avr_ctx;
    uint8_t                *resampled_buffer;
    uint32_t                resampled_buffer_size;
    uint64_t                skip_samples;
    int                     input_planes;
    int                     input_block_align;
    uint64_t                output_channel_layout;
    enum AVSampleFormat     output_sample_format;
    int                     output_block_align;
    int                     output_sample_rate;
    int                     output_bits_per_sample;
    int                     s24_output;
} audio_output_handler_t;

typedef struct libavsmash_handler_tag
{
    /* Global stuff */
    UINT                      uType;
    lsmash_root_t            *root;
    lsmash_movie_parameters_t movie_param;
    uint32_t                  number_of_tracks;
    AVFormatContext          *format_ctx;
    int                       threads;
    /* Video stuff */
    video_info_handler_t      vih;
    video_decode_handler_t    vdh;
    video_output_handler_t    voh;
    /* Audio stuff */
    audio_info_handler_t      aih;
    audio_decode_handler_t    adh;
    audio_output_handler_t    aoh;
    int64_t                   av_gap;
    int                       av_sync;
} libavsmash_handler_t;

static void message_box_desktop( void *message_priv, const char *message, ... )
{
    char temp[256];
    va_list args;
    va_start( args, message );
    wvsprintf( temp, message, args );
    va_end( args );
    UINT uType = *(UINT *)message_priv;
    MessageBox( HWND_DESKTOP, temp, "lsmashinput", uType );
}

static void *open_file( char *file_name, reader_option_t *opt )
{
    libavsmash_handler_t *hp = malloc_zero( sizeof(libavsmash_handler_t) );
    if( !hp )
        return NULL;
    hp->uType = MB_ICONERROR | MB_OK;
    hp->vdh.config.message_priv = &hp->uType;
    hp->adh.config.message_priv = &hp->uType;
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
        hp->vdh.order_converter = malloc_zero( (ts_list.sample_count + 1) * sizeof(order_converter_t) );
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
        hp->vdh.config.error_message = message_box_desktop;
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
        hp->adh.config.error_message = message_box_desktop;
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
            hp->aoh.skip_samples = ctd_shift + get_start_time( hp->root, track_ID );
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

static inline uint32_t get_decoding_sample_number( order_converter_t *order_converter, uint32_t composition_sample_number )
{
    return order_converter
         ? order_converter[composition_sample_number].composition_to_decoding
         : composition_sample_number;
}

static int create_keyframe_list( libavsmash_handler_t *hp, uint32_t video_sample_count )
{
    video_info_handler_t *vihp = &hp->vih;
    vihp->keyframe_list = malloc_zero( (video_sample_count + 1) * sizeof(uint8_t) );
    if( !vihp->keyframe_list )
        return -1;
    video_decode_handler_t *vdhp = &hp->vdh;
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

static int convert_colorspace( libavsmash_handler_t *hp, AVFrame *picture, uint8_t *buf )
{
    video_output_handler_t *vohp = &hp->voh;
    /* Convert color space. We don't change the presentation resolution. */
    int64_t width;
    int64_t height;
    int64_t format;
    av_opt_get_int( vohp->sws_ctx, "srcw",       0, &width );
    av_opt_get_int( vohp->sws_ctx, "srch",       0, &height );
    av_opt_get_int( vohp->sws_ctx, "src_format", 0, &format );
    avoid_yuv_scale_conversion( &picture->format );
    if( !vohp->sws_ctx || picture->width != width || picture->height != height || picture->format != format )
    {
        vohp->sws_ctx = sws_getCachedContext( vohp->sws_ctx,
                                              picture->width, picture->height, picture->format,
                                              picture->width, picture->height, vohp->output_pixel_format,
                                              vohp->scaler_flags, NULL, NULL, NULL );
        if( !vohp->sws_ctx )
            return 0;
        memcpy( buf, vohp->back_ground, vohp->output_sample_size );
    }
    if( vohp->convert_colorspace( hp->vdh.config.ctx, vohp->sws_ctx, picture, buf, vohp->output_linesize ) < 0 )
        return 0;
    return vohp->output_sample_size;
}

static int prepare_video_decoding( lsmash_handler_t *h, video_option_t *opt )
{
    libavsmash_handler_t *hp = (libavsmash_handler_t *)h->video_private;
    video_decode_handler_t *vdhp = &hp->vdh;
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
    codec_configuration_t *config = &vdhp->config;
    if( initialize_decoder_configuration( hp->root, vdhp->track_ID, config ) )
    {
        DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to initialize the decoder configuration." );
        return -1;
    }
    /* swscale */
    video_output_handler_t *vohp = &hp->voh;
    output_colorspace_index index = determine_colorspace_conversion( &config->ctx->pix_fmt, &vohp->output_pixel_format );
    static const struct
    {
        func_convert_colorspace *convert_colorspace;
        int                      pixel_size;
        output_colorspace_tag    compression;
    } colorspace_table[4] =
        {
            { to_yuy2,            YUY2_SIZE,  OUTPUT_TAG_YUY2 },
            { to_rgb24,           RGB24_SIZE, OUTPUT_TAG_RGB  },
            { to_rgba,            RGBA_SIZE,  OUTPUT_TAG_RGBA },
            { to_yuv16le_to_yc48, YC48_SIZE,  OUTPUT_TAG_YC48 }
        };
    vohp->scaler_flags = 1 << opt->scaler;
    if( vohp->scaler_flags != SWS_FAST_BILINEAR )
        vohp->scaler_flags |= SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP | SWS_ACCURATE_RND;
    vohp->sws_ctx = sws_getCachedContext( NULL,
                                          config->ctx->width, config->ctx->height, config->ctx->pix_fmt,
                                          config->ctx->width, config->ctx->height, vohp->output_pixel_format,
                                          vohp->scaler_flags, NULL, NULL, NULL );
    if( !vohp->sws_ctx )
    {
        DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to get swscale context." );
        return -1;
    }
    vohp->convert_colorspace = colorspace_table[index].convert_colorspace;
    /* BITMAPINFOHEADER */
    int output_width  = config->prefer.width;
    int output_height = config->prefer.height;
    h->video_format.biSize        = sizeof( BITMAPINFOHEADER );
    h->video_format.biWidth       = output_width;
    h->video_format.biHeight      = output_height;
    h->video_format.biBitCount    = colorspace_table[index].pixel_size << 3;
    h->video_format.biCompression = colorspace_table[index].compression;
    /* Set up a black frame of back ground. */
    vohp->output_linesize = MAKE_AVIUTL_PITCH( output_width * h->video_format.biBitCount );
    vohp->output_sample_size = vohp->output_linesize * output_height;
    vohp->back_ground = vohp->output_sample_size ? malloc( vohp->output_sample_size ) : NULL;
    if( !vohp->back_ground )
        return -1;
    if( h->video_format.biCompression != OUTPUT_TAG_YUY2 )
        memset( vohp->back_ground, 0, vohp->output_sample_size );
    else
    {
        uint8_t *pic = vohp->back_ground;
        for( int i = 0; i < output_height; i++ )
        {
            for( int j = 0; j < vohp->output_linesize; j += 2 )
            {
                pic[j    ] = 0;
                pic[j + 1] = 128;
            }
            pic += vohp->output_linesize;
        }
    }
    /* Find the first valid video sample. */
    for( uint32_t i = 1; i <= h->video_sample_count + get_decoder_delay( config->ctx ); i++ )
    {
        AVPacket pkt = { 0 };
        get_sample( hp->root, vdhp->track_ID, i, config, &pkt );
        AVFrame *picture = vdhp->frame_buffer;
        avcodec_get_frame_defaults( picture );
        int got_picture;
        if( avcodec_decode_video2( config->ctx, picture, &got_picture, &pkt ) >= 0 && got_picture )
        {
            vohp->first_valid_sample_number = i - min( get_decoder_delay( config->ctx ), config->delay_count );
            if( vohp->first_valid_sample_number > 1 || h->video_sample_count == 1 )
            {
                if( !vohp->first_valid_sample_data )
                {
                    vohp->first_valid_sample_data = malloc( vohp->output_sample_size );
                    if( !vohp->first_valid_sample_data )
                        return -1;
                    memcpy( vohp->first_valid_sample_data, vohp->back_ground, vohp->output_sample_size );
                }
                if( vohp->output_sample_size != convert_colorspace( hp, picture, vohp->first_valid_sample_data ) )
                    continue;
            }
            break;
        }
        else if( pkt.data )
            ++ config->delay_count;
    }
    vdhp->last_sample_number = h->video_sample_count + 1;   /* Force seeking at the first reading. */
    vdhp->root = hp->root;
    return 0;
}

static uint32_t count_overall_pcm_samples( libavsmash_handler_t *hp )
{
    audio_output_handler_t *aohp   = &hp->aoh;
    audio_decode_handler_t *adhp   = &hp->adh;
    codec_configuration_t  *config = &adhp->config;
    libavsmash_summary_t   *s      = NULL;
    int      current_sample_rate          = 0;
    uint32_t current_index                = 0;
    uint32_t current_frame_length         = 0;
    uint32_t audio_frame_count            = 0;
    uint64_t pcm_sample_count             = 0;
    uint32_t overall_pcm_sample_count     = 0;
    uint32_t skip_samples                 = 0;
    uint32_t prior_sequences_sample_count = 0;
    for( uint32_t i = 1; i <= adhp->frame_count; i++ )
    {
        /* Get configuration index. */
        lsmash_sample_t sample;
        if( lsmash_get_sample_info_from_media_timeline( hp->root, adhp->track_ID, i, &sample ) )
            continue;
        if( current_index != sample.index )
        {
            s = &config->entries[ sample.index - 1 ];
            current_index = sample.index;
        }
        else if( !s )
            continue;
        /* Get audio frame length. */
        uint32_t frame_length;
        if( s->extended.frame_length )
            frame_length = s->extended.frame_length;
        else if( lsmash_get_sample_delta_from_media_timeline( hp->root, adhp->track_ID, i, &frame_length ) )
            continue;
        /* */
        if( (current_sample_rate != s->extended.sample_rate && s->extended.sample_rate > 0)
         || current_frame_length != frame_length )
        {
            if( current_sample_rate > 0 )
            {
                if( aohp->skip_samples > pcm_sample_count )
                    skip_samples += pcm_sample_count * s->extended.upsampling;
                else if( aohp->skip_samples > prior_sequences_sample_count )
                    skip_samples += (aohp->skip_samples - prior_sequences_sample_count) * s->extended.upsampling;
                prior_sequences_sample_count += pcm_sample_count;
                pcm_sample_count *= s->extended.upsampling;
                uint32_t resampled_sample_count = aohp->output_sample_rate == current_sample_rate || pcm_sample_count == 0
                                                ? pcm_sample_count
                                                : (pcm_sample_count * aohp->output_sample_rate - 1) / current_sample_rate + 1;
                overall_pcm_sample_count += resampled_sample_count;
                audio_frame_count = 0;
                pcm_sample_count  = 0;
            }
            current_sample_rate  = s->extended.sample_rate > 0 ? s->extended.sample_rate : config->ctx->sample_rate;
            current_frame_length = frame_length;
        }
        pcm_sample_count += frame_length;
        ++audio_frame_count;
    }
    if( !s || (pcm_sample_count == 0 && overall_pcm_sample_count == 0) )
        return 0;
    if( aohp->skip_samples > prior_sequences_sample_count )
        skip_samples += (aohp->skip_samples - prior_sequences_sample_count) * s->extended.upsampling;
    pcm_sample_count *= s->extended.upsampling;
    current_sample_rate = s->extended.sample_rate > 0 ? s->extended.sample_rate : config->ctx->sample_rate;
    if( current_sample_rate == aohp->output_sample_rate )
    {
        aohp->skip_samples = skip_samples;
        if( pcm_sample_count )
            overall_pcm_sample_count += pcm_sample_count;
    }
    else
    {
        if( skip_samples )
            aohp->skip_samples = ((uint64_t)skip_samples * aohp->output_sample_rate - 1) / current_sample_rate + 1;
        if( pcm_sample_count )
            overall_pcm_sample_count += (pcm_sample_count * aohp->output_sample_rate - 1) / current_sample_rate + 1;
    }
    return overall_pcm_sample_count - aohp->skip_samples;
}

static int prepare_audio_decoding( lsmash_handler_t *h )
{
    libavsmash_handler_t *hp = (libavsmash_handler_t *)h->audio_private;
    audio_decode_handler_t *adhp = &hp->adh;
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
    audio_output_handler_t *aohp = &hp->aoh;
    aohp->output_channel_layout  = config->prefer.channel_layout;
    aohp->output_sample_format   = config->prefer.sample_format;
    aohp->output_sample_rate     = config->prefer.sample_rate;
    aohp->output_bits_per_sample = config->prefer.bits_per_sample;
    /* */
    h->audio_pcm_sample_count = count_overall_pcm_samples( hp );
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
    adhp->next_pcm_sample_number = h->audio_pcm_sample_count + 1;   /* Force seeking at the first reading. */
    /* Set up resampler. */
    aohp->avr_ctx = avresample_alloc_context();
    if( !aohp->avr_ctx )
    {
        DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to avresample_alloc_context." );
        return -1;
    }
    if( config->ctx->channel_layout == 0 )
        config->ctx->channel_layout = av_get_default_channel_layout( config->ctx->channels );
    aohp->output_sample_format = decide_audio_output_sample_format( aohp->output_sample_format, aohp->output_bits_per_sample );
    av_opt_set_int( aohp->avr_ctx, "in_channel_layout",   config->ctx->channel_layout, 0 );
    av_opt_set_int( aohp->avr_ctx, "in_sample_fmt",       config->ctx->sample_fmt,     0 );
    av_opt_set_int( aohp->avr_ctx, "in_sample_rate",      config->ctx->sample_rate,    0 );
    av_opt_set_int( aohp->avr_ctx, "out_channel_layout",  aohp->output_channel_layout, 0 );
    av_opt_set_int( aohp->avr_ctx, "out_sample_fmt",      aohp->output_sample_format,  0 );
    av_opt_set_int( aohp->avr_ctx, "out_sample_rate",     aohp->output_sample_rate,    0 );
    av_opt_set_int( aohp->avr_ctx, "internal_sample_fmt", AV_SAMPLE_FMT_FLTP,          0 );
    if( avresample_open( aohp->avr_ctx ) < 0 )
    {
        DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to open resampler." );
        return -1;
    }
    /* Decide output Bits Per Sample. */
    int output_channels = av_get_channel_layout_nb_channels( aohp->output_channel_layout );
    if( aohp->output_sample_format == AV_SAMPLE_FMT_S32
     && (aohp->output_bits_per_sample == 0 || aohp->output_bits_per_sample == 24) )
    {
        /* 24bit signed integer output */
        if( config->ctx->frame_size )
        {
            aohp->resampled_buffer_size = get_linesize( output_channels, config->ctx->frame_size, aohp->output_sample_format );
            aohp->resampled_buffer      = av_malloc( aohp->resampled_buffer_size );
            if( !aohp->resampled_buffer )
            {
                DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to allocate memory for resampling." );
                return -1;
            }
        }
        aohp->s24_output             = 1;
        aohp->output_bits_per_sample = 24;
    }
    else
        aohp->output_bits_per_sample = av_get_bytes_per_sample( aohp->output_sample_format ) * 8;
    /* Support of WAVEFORMATEXTENSIBLE is much restrictive on AviUtl, so we always use WAVEFORMATEX instead. */
    WAVEFORMATEX *Format = &h->audio_format.Format;
    Format->nChannels       = output_channels;
    Format->nSamplesPerSec  = aohp->output_sample_rate;
    Format->wBitsPerSample  = aohp->output_bits_per_sample;
    Format->nBlockAlign     = (Format->nChannels * Format->wBitsPerSample) / 8;
    Format->nAvgBytesPerSec = Format->nSamplesPerSec * Format->nBlockAlign;
    Format->wFormatTag      = WAVE_FORMAT_PCM;
    Format->cbSize          = 0;
    /* Set up the number of planes and the block alignment of decoded and output data. */
    int input_channels = av_get_channel_layout_nb_channels( config->ctx->channel_layout );
    if( av_sample_fmt_is_planar( config->ctx->sample_fmt ) )
    {
        aohp->input_planes      = input_channels;
        aohp->input_block_align = av_get_bytes_per_sample( config->ctx->sample_fmt );
    }
    else
    {
        aohp->input_planes      = 1;
        aohp->input_block_align = av_get_bytes_per_sample( config->ctx->sample_fmt ) * input_channels;
    }
    aohp->output_block_align = Format->nBlockAlign;
    adhp->root = hp->root;
    return 0;
}

static int decode_video_sample( video_decode_handler_t *vdhp, AVFrame *picture, int *got_picture, uint32_t sample_number )
{
    AVPacket pkt = { 0 };
    int ret = get_sample( vdhp->root, vdhp->track_ID, sample_number, &vdhp->config, &pkt );
    if( ret )
        return ret;
    if( pkt.flags != ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE )
    {
        pkt.flags = AV_PKT_FLAG_KEY;
        vdhp->last_rap_number = sample_number;
    }
    else
        pkt.flags = 0;
    avcodec_get_frame_defaults( picture );
    uint64_t cts = pkt.pts;
    ret = avcodec_decode_video2( vdhp->config.ctx, picture, got_picture, &pkt );
    picture->pts = cts;
    if( ret < 0 )
    {
        DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_OK, "Failed to decode a video frame." );
        return -1;
    }
    return 0;
}

static int find_random_accessible_point( video_decode_handler_t *vdhp, uint32_t composition_sample_number, uint32_t decoding_sample_number, uint32_t *rap_number )
{
    if( decoding_sample_number == 0 )
        decoding_sample_number = get_decoding_sample_number( vdhp->order_converter, composition_sample_number );
    lsmash_random_access_flag ra_flags;
    uint32_t distance;  /* distance from the closest random accessible point to the previous. */
    uint32_t number_of_leadings;
    if( lsmash_get_closest_random_accessible_point_detail_from_media_timeline( vdhp->root, vdhp->track_ID, decoding_sample_number,
                                                                               rap_number, &ra_flags, &number_of_leadings, &distance ) )
        *rap_number = 1;
    int roll_recovery = !!(ra_flags & ISOM_SAMPLE_RANDOM_ACCESS_FLAG_GDR);
    int is_leading    = number_of_leadings && (decoding_sample_number - *rap_number <= number_of_leadings);
    if( (roll_recovery || is_leading) && *rap_number > distance )
        *rap_number -= distance;
    /* Check whether random accessible point has the same decoder configuration or not. */
    decoding_sample_number = get_decoding_sample_number( vdhp->order_converter, composition_sample_number );
    do
    {
        lsmash_sample_t sample;
        lsmash_sample_t rap_sample;
        if( lsmash_get_sample_info_from_media_timeline( vdhp->root, vdhp->track_ID, decoding_sample_number, &sample )
         || lsmash_get_sample_info_from_media_timeline( vdhp->root, vdhp->track_ID, *rap_number, &rap_sample ) )
        {
            /* Fatal error. */
            *rap_number = vdhp->last_rap_number;
            return 0;
        }
        if( sample.index == rap_sample.index )
            break;
        uint32_t sample_index = sample.index;
        for( uint32_t i = decoding_sample_number - 1; i; i-- )
        {
            if( lsmash_get_sample_info_from_media_timeline( vdhp->root, vdhp->track_ID, i, &sample ) )
            {
                /* Fatal error. */
                *rap_number = vdhp->last_rap_number;
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

/* This function returns the number of the next sample. */
static uint32_t seek_video( video_decode_handler_t *vdhp, AVFrame *picture, uint32_t composition_sample_number, uint32_t rap_number, int error_ignorance )
{
    /* Prepare to decode from random accessible sample. */
    codec_configuration_t *config = &vdhp->config;
    if( config->update_pending )
        /* Update the decoder configuration. */
        update_configuration( vdhp->root, vdhp->track_ID, config );
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
            config->delay_count = min( decoder_delay, i - rap_number );
        int ret = decode_video_sample( vdhp, picture, &dummy, i );
        /* Some decoders return -1 when feeding a leading sample.
         * We don't consider as an error if the return value -1 is caused by a leading sample since it's not fatal at all. */
        if( i == vdhp->last_rap_number )
            rap_cts = picture->pts;
        if( ret == -1 && picture->pts >= rap_cts && !error_ignorance )
        {
            DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_OK, "Failed to decode a video frame." );
            return 0;
        }
        else if( ret >= 1 )
            /* No decoding occurs. */
            break;
    }
    if( config->index == config->queue.index )
        config->delay_count = min( decoder_delay, i - rap_number );
    return i;
}

static int get_picture( video_decode_handler_t *vdhp, AVFrame *picture, uint32_t current, uint32_t goal, uint32_t video_sample_count )
{
    codec_configuration_t *config = &vdhp->config;
    int got_picture = (current > goal);
    while( current <= goal )
    {
        int ret = decode_video_sample( vdhp, picture, &got_picture, current );
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
    if( current > video_sample_count && get_decoder_delay( config->ctx ) )
        while( current <= goal )
        {
            AVPacket pkt = { 0 };
            av_init_packet( &pkt );
            pkt.data = NULL;
            pkt.size = 0;
            avcodec_get_frame_defaults( picture );
            if( avcodec_decode_video2( config->ctx, picture, &got_picture, &pkt ) < 0 )
            {
                DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_OK, "Failed to decode and flush a video frame." );
                return -1;
            }
            ++current;
        }
    return got_picture ? 0 : -1;
}

static int read_video( lsmash_handler_t *h, int sample_number, void *buf )
{
#define MAX_ERROR_COUNT 3       /* arbitrary */
    libavsmash_handler_t   *hp     = (libavsmash_handler_t *)h->video_private;
    video_decode_handler_t *vdhp   = &hp->vdh;
    codec_configuration_t  *config = &vdhp->config;
    if( config->error )
        return 0;
    video_output_handler_t *vohp = &hp->voh;
    ++sample_number;            /* For L-SMASH, sample_number is 1-origin. */
    if( sample_number == 1 )
        memcpy( buf, vohp->back_ground, vohp->output_sample_size );
    if( sample_number < vohp->first_valid_sample_number || h->video_sample_count == 1 )
    {
        /* Copy the first valid video sample data. */
        memcpy( buf, vohp->first_valid_sample_data, vohp->output_sample_size );
        vdhp->last_sample_number = h->video_sample_count + 1;   /* Force seeking at the next access for valid video sample. */
        return vohp->output_sample_size;
    }
    AVFrame *picture = vdhp->frame_buffer;
    uint32_t start_number;  /* number of sample, for normal decoding, where decoding starts excluding decoding delay */
    uint32_t rap_number;    /* number of sample, for seeking, where decoding starts excluding decoding delay */
    int seek_mode = vdhp->seek_mode;
    int roll_recovery = 0;
    if( sample_number > vdhp->last_sample_number
     && sample_number <= vdhp->last_sample_number + vdhp->forward_seek_threshold )
    {
        start_number = vdhp->last_sample_number + 1 + config->delay_count;
        rap_number   = vdhp->last_rap_number;
    }
    else
    {
        roll_recovery = find_random_accessible_point( vdhp, sample_number, 0, &rap_number );
        if( rap_number == vdhp->last_rap_number && sample_number > vdhp->last_sample_number )
        {
            roll_recovery = 0;
            start_number  = vdhp->last_sample_number + 1 + config->delay_count;
        }
        else
        {
            /* Require starting to decode from random accessible sample. */
            vdhp->last_rap_number = rap_number;
            start_number = seek_video( vdhp, picture, sample_number, rap_number, roll_recovery || seek_mode != SEEK_MODE_NORMAL );
        }
    }
    /* Get the desired picture. */
    int error_count = 0;
    while( start_number == 0    /* Failed to seek. */
     || config->update_pending  /* Need to update the decoder configuration to decode pictures. */
     || get_picture( vdhp, picture, start_number, sample_number + config->delay_count, h->video_sample_count ) )
    {
        if( config->update_pending )
        {
            roll_recovery = find_random_accessible_point( vdhp, sample_number, 0, &rap_number );
            vdhp->last_rap_number = rap_number;
        }
        else
        {
            /* Failed to get the desired picture. */
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
    return convert_colorspace( hp, picture, buf );
video_fail:
    /* fatal error of decoding */
    DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Couldn't read video frame." );
    return 0;
#undef MAX_ERROR_COUNT
}

static inline int get_frame_length( audio_decode_handler_t *adhp, uint32_t frame_number, uint32_t *frame_length, libavsmash_summary_t **sp )
{
    lsmash_sample_t sample;
    if( lsmash_get_sample_info_from_media_timeline( adhp->root, adhp->track_ID, frame_number, &sample ) )
        return -1;
    *sp = &adhp->config.entries[ sample.index - 1 ];
    libavsmash_summary_t *s = *sp;
    if( s->extended.frame_length == 0 )
    {
        /* variable frame length
         * Guess the frame length from sample duration. */
        if( lsmash_get_sample_delta_from_media_timeline( adhp->root, adhp->track_ID, frame_number, frame_length ) )
            return -1;
        *frame_length *= s->extended.upsampling;
    }
    else
        /* constant frame length */
        *frame_length = s->extended.frame_length;
    return 0;
}

static uint32_t get_preroll_samples( audio_decode_handler_t *adhp, uint32_t *frame_number )
{
    /* Some audio CODEC requires pre-roll for correct composition. */
    lsmash_sample_property_t prop;
    if( lsmash_get_sample_property_from_media_timeline( adhp->root, adhp->track_ID, *frame_number, &prop ) )
        return 0;
    if( prop.pre_roll.distance == 0 )
        /* No need to pre-roll or no pre-roll indication. */
        return 0;
    /* Get the number of pre-roll PCM samples. */
    uint32_t preroll_samples = 0;
    for( uint32_t i = 0; i < prop.pre_roll.distance; i++ )
    {
        if( *frame_number > 1 )
            --(*frame_number);
        else
            break;
        libavsmash_summary_t *dummy = NULL;
        uint32_t frame_length;
        if( get_frame_length( adhp, *frame_number, &frame_length, &dummy ) )
            break;
        preroll_samples += frame_length;
    }
    return preroll_samples;
}

static int find_start_audio_frame( audio_decode_handler_t *adhp, int output_sample_rate, uint64_t start_frame_pos, uint64_t *start_offset )
{
    uint32_t frame_number                    = 1;
    uint64_t current_frame_pos               = 0;
    uint64_t next_frame_pos                  = 0;
    int      current_sample_rate             = 0;
    uint32_t current_frame_length            = 0;
    uint64_t pcm_sample_count                = 0;   /* the number of accumulated PCM samples before resampling per sequence */
    uint64_t resampled_sample_count          = 0;   /* the number of accumulated PCM samples after resampling per sequence */
    uint64_t prior_sequences_resampled_count = 0;   /* the number of accumulated PCM samples of all prior sequences */
    do
    {
        current_frame_pos = next_frame_pos;
        libavsmash_summary_t *s = NULL;
        uint32_t frame_length;
        if( get_frame_length( adhp, frame_number, &frame_length, &s ) )
        {
            ++frame_number;
            continue;
        }
        if( (current_sample_rate != s->extended.sample_rate && s->extended.sample_rate > 0)
         || current_frame_length != frame_length )
        {
            /* Encountered a new sequence. */
            prior_sequences_resampled_count += resampled_sample_count;
            pcm_sample_count = 0;
            current_sample_rate  = s->extended.sample_rate > 0 ? s->extended.sample_rate : adhp->config.ctx->sample_rate;
            current_frame_length = frame_length;
        }
        pcm_sample_count += frame_length;
        resampled_sample_count = output_sample_rate == current_sample_rate || pcm_sample_count == 0
                               ? pcm_sample_count
                               : (pcm_sample_count * output_sample_rate - 1) / current_sample_rate + 1;
        next_frame_pos = prior_sequences_resampled_count + resampled_sample_count;
        if( start_frame_pos < next_frame_pos )
            break;
        ++frame_number;
    } while( frame_number <= adhp->frame_count );
    *start_offset = start_frame_pos - current_frame_pos;
    if( *start_offset && current_sample_rate != output_sample_rate )
        *start_offset = (*start_offset * current_sample_rate - 1) / output_sample_rate + 1;
    *start_offset += get_preroll_samples( adhp, &frame_number );
    return frame_number;
}

static int consume_decoded_audio_samples( audio_output_handler_t *aohp, AVFrame *frame,
                                          int input_sample_count, int wanted_sample_count,
                                          uint8_t **out_data, int sample_offset )
{
    /* Input */
    uint8_t *in_data[AVRESAMPLE_MAX_CHANNELS];
    int decoded_data_offset = sample_offset * aohp->input_block_align;
    for( int i = 0; i < aohp->input_planes; i++ )
        in_data[i] = frame->extended_data[i] + decoded_data_offset;
    audio_samples_t in;
    in.channel_layout = frame->channel_layout;
    in.sample_count   = input_sample_count;
    in.sample_format  = frame->format;
    in.data           = in_data;
    /* Output */
    uint8_t *resampled_buffer = NULL;
    if( aohp->s24_output )
    {
        int out_channels = get_channel_layout_nb_channels( aohp->output_channel_layout );
        int out_linesize = get_linesize( out_channels, wanted_sample_count, aohp->output_sample_format );
        if( !aohp->resampled_buffer || out_linesize > aohp->resampled_buffer_size )
        {
            uint8_t *temp = av_realloc( aohp->resampled_buffer, out_linesize );
            if( !temp )
                return 0;
            aohp->resampled_buffer_size = out_linesize;
            aohp->resampled_buffer      = temp;
        }
        resampled_buffer = aohp->resampled_buffer;
    }
    audio_samples_t out;
    out.channel_layout = aohp->output_channel_layout;
    out.sample_count   = wanted_sample_count;
    out.sample_format  = aohp->output_sample_format;
    out.data           = resampled_buffer ? &resampled_buffer : out_data;
    /* Resample */
    int resampled_size = resample_audio( aohp->avr_ctx, &out, &in );
    if( resampled_buffer && resampled_size > 0 )
        resampled_size = resample_s32_to_s24( out_data, aohp->resampled_buffer, resampled_size );
    return resampled_size > 0 ? resampled_size / aohp->output_block_align : 0;
}

static int read_audio( lsmash_handler_t *h, int start, int wanted_length, void *buf )
{
    DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_OK, "start = %d, wanted_length = %d", start, wanted_length );
    libavsmash_handler_t   *hp     = (libavsmash_handler_t *)h->audio_private;
    audio_decode_handler_t *adhp   = &hp->adh;
    codec_configuration_t  *config = &adhp->config;
    if( config->error )
        return 0;
    audio_output_handler_t *aohp = &hp->aoh;
    uint32_t frame_number;
    uint64_t seek_offset;
    int      output_length = 0;
    if( start > 0 && start == adhp->next_pcm_sample_number )
    {
        frame_number = adhp->last_frame_number;
        if( adhp->frame_buffer->extended_data
         && adhp->frame_buffer->extended_data[0] )
        {
            /* Flush remaing audio samples. */
            int resampled_length = consume_decoded_audio_samples( aohp, adhp->frame_buffer, 0, wanted_length, (uint8_t **)&buf, 0 );
            output_length += resampled_length;
            wanted_length -= resampled_length;
            if( wanted_length <= 0 )
                goto audio_out;
        }
        if( adhp->packet.size <= 0 )
            ++frame_number;
        seek_offset = 0;
    }
    else
    {
        /* Seek audio stream. */
        if( flush_resampler_buffers( aohp->avr_ctx ) < 0 )
        {
            MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to flush resampler buffers.\nIt is recommended you reopen the file." );
            config->error = 1;
            return 0;
        }
        flush_buffers( config );
        if( config->error )
            return 0;
        adhp->next_pcm_sample_number = 0;
        adhp->last_frame_number      = 0;
        uint64_t start_frame_pos;
        if( start >= 0 )
            start_frame_pos = start;
        else
        {
            int silence_length = -start;
            put_silence_audio_samples( silence_length * aohp->output_block_align, aohp->output_bits_per_sample == 8, (uint8_t **)&buf );
            output_length += silence_length;
            wanted_length -= silence_length;
            start_frame_pos = 0;
        }
        start_frame_pos += aohp->skip_samples;
        frame_number = find_start_audio_frame( adhp, aohp->output_sample_rate, start_frame_pos, &seek_offset );
    }
    do
    {
        AVPacket *pkt = &adhp->packet;
        if( frame_number > adhp->frame_count )
        {
            if( config->delay_count )
            {
                /* Null packet */
                av_init_packet( pkt );
                pkt->data = NULL;
                pkt->size = 0;
                -- config->delay_count;
            }
            else
                goto audio_out;
        }
        else if( pkt->size <= 0 )
            /* Getting a sample must be after flushing all remaining samples in resampler's FIFO buffer. */
            while( get_sample( adhp->root, adhp->track_ID, frame_number, config, pkt ) == 2 )
                if( config->update_pending )
                    /* Update the decoder configuration. */
                    update_configuration( adhp->root, adhp->track_ID, config );
        int output_audio = 0;
        do
        {
            avcodec_get_frame_defaults( adhp->frame_buffer );
            int decode_complete;
            int wasted_data_length = avcodec_decode_audio4( config->ctx, adhp->frame_buffer, &decode_complete, pkt );
            if( wasted_data_length < 0 )
            {
                pkt->size = 0;  /* Force to get the next sample. */
                break;
            }
            if( pkt->data )
            {
                pkt->size -= wasted_data_length;
                pkt->data += wasted_data_length;
            }
            else if( !decode_complete )
                goto audio_out;
            if( decode_complete
             && adhp->frame_buffer->extended_data
             && adhp->frame_buffer->extended_data[0] )
            {
                /* Check channel layout, sample rate and sample format of decoded audio samples. */
                int64_t channel_layout;
                int64_t sample_rate;
                int64_t sample_format;
                av_opt_get_int( aohp->avr_ctx, "in_channel_layout", 0, &channel_layout );
                av_opt_get_int( aohp->avr_ctx, "in_sample_rate",    0, &sample_rate );
                av_opt_get_int( aohp->avr_ctx, "in_sample_fmt",     0, &sample_format );
                if( adhp->frame_buffer->channel_layout == 0 )
                    adhp->frame_buffer->channel_layout = av_get_default_channel_layout( config->ctx->channels );
                if( adhp->frame_buffer->channel_layout != channel_layout
                 || adhp->frame_buffer->sample_rate    != sample_rate
                 || adhp->frame_buffer->format         != sample_format )
                {
                    /* Detected a change of channel layout, sample rate or sample format.
                     * Reconfigure audio resampler. */
                    if( update_resampler_configuration( aohp->avr_ctx,
                                                        aohp->output_channel_layout,
                                                        aohp->output_sample_rate,
                                                        aohp->output_sample_format,
                                                        adhp->frame_buffer->channel_layout,
                                                        adhp->frame_buffer->sample_rate,
                                                        adhp->frame_buffer->format,
                                                        &aohp->input_planes,
                                                        &aohp->input_block_align ) < 0 )
                    {
                        MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to reconfigure resampler.\nIt is recommended you reopen the file." );
                        config->error = 1;
                        goto audio_out;
                    }
                }
                /* Process decoded audio samples. */
                int decoded_length = adhp->frame_buffer->nb_samples;
                if( decoded_length > seek_offset )
                {
                    /* Send decoded audio data to resampler and get desired resampled audio as you want as much as possible. */
                    int useful_length = decoded_length - seek_offset;
                    int resampled_length = consume_decoded_audio_samples( aohp, adhp->frame_buffer, useful_length, wanted_length, (uint8_t **)&buf, seek_offset );
                    output_length += resampled_length;
                    wanted_length -= resampled_length;
                    seek_offset = 0;
                    if( wanted_length <= 0 )
                        goto audio_out;
                }
                else
                    seek_offset -= decoded_length;
                output_audio = 1;
            }
            DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_OK, "frame_number = %d, wasted_data_length = %d,"
                                             "decode_complete = %d, decoded_length = %d, output_length = %d",
                                             frame_number, wasted_data_length, decode_complete,
                                             hp->audio_frame_buffer->nb_samples, output_length );
        } while( pkt->size > 0 );
        if( !output_audio && pkt->data )    /* Count audio frame delay only if feeding non-NULL packet. */
            ++ config->delay_count;
        ++frame_number;
    } while( 1 );
audio_out:
    DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_OK, "output_length = %d", output_length );
    adhp->next_pcm_sample_number = start + output_length;
    adhp->last_frame_number = frame_number;
    return output_length;
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
    if( hp->voh.back_ground )
        free( hp->voh.back_ground );
    if( hp->voh.first_valid_sample_data )
        free( hp->voh.first_valid_sample_data );
    if( hp->voh.sws_ctx )
        sws_freeContext( hp->voh.sws_ctx );
    if( hp->vdh.order_converter )
        free( hp->vdh.order_converter );
    if( hp->vdh.frame_buffer )
        avcodec_free_frame( &hp->vdh.frame_buffer );
    cleanup_configuration( &hp->vdh.config );
}

static void audio_cleanup( lsmash_handler_t *h )
{
    libavsmash_handler_t *hp = (libavsmash_handler_t *)h->audio_private;
    if( !hp )
        return;
    if( hp->aoh.resampled_buffer )
        av_free( hp->aoh.resampled_buffer );
    if( hp->aoh.avr_ctx )
        avresample_free( &hp->aoh.avr_ctx );
    if( hp->adh.frame_buffer )
        avcodec_free_frame( &hp->adh.frame_buffer );
    cleanup_configuration( &hp->adh.config );
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
