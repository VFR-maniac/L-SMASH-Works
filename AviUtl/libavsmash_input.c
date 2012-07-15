/*****************************************************************************
 * libavsmash_input.c
 *****************************************************************************
 * Copyright (C) 2011-2012 L-SMASH Works project
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
#include <libavformat/avformat.h>   /* Codec specific info importer */
#include <libavcodec/avcodec.h>     /* Decoder */
#include <libswscale/swscale.h>     /* Colorspace converter */

#include "lsmashinput.h"
#include "colorspace.h"

#define DECODER_DELAY( ctx ) (ctx->has_b_frames + ((ctx->active_thread_type & FF_THREAD_FRAME) ? ctx->thread_count - 1 : 0))

#define SEEK_MODE_NORMAL     0
#define SEEK_MODE_UNSAFE     1
#define SEEK_MODE_AGGRESSIVE 2

typedef enum
{
    DECODE_REQUIRE_INITIAL = 0,
    DECODE_INITIALIZING    = 1,
    DECODE_INITIALIZED     = 2
} decode_status_t;

typedef struct
{
    uint32_t composition_to_decoding;
} order_converter_t;

typedef struct libavsmash_handler_tag
{
    /* L-SMASH's stuff */
    lsmash_root_t            *root;
    lsmash_movie_parameters_t movie_param;
    uint32_t                  number_of_tracks;
    uint32_t                  video_track_ID;
    uint32_t                  audio_track_ID;
    /* Libav's stuff */
    AVCodecContext           *video_ctx;
    AVCodecContext           *audio_ctx;
    AVFormatContext          *format_ctx;
    struct SwsContext        *sws_ctx;
    int                       threads;
    /* Video stuff */
    int                       video_error;
    uint8_t                  *video_input_buffer;
    uint32_t                  last_video_sample_number;
    uint32_t                  last_rap_number;
    uint32_t                  video_delay_count;
    uint32_t                  first_valid_video_sample_number;
    uint32_t                  first_valid_video_sample_size;
    uint8_t                  *first_valid_video_sample_data;
    decode_status_t           decode_status;
    order_converter_t        *order_converter;
    uint8_t                  *keyframe_list;
    int                       seek_mode;
    uint32_t                  forward_seek_threshold;
    func_convert_colorspace  *convert_colorspace;
    uint32_t                  video_media_timescale;
    uint64_t                  video_skip_duration;
    int64_t                   video_start_pts;
    /* Audio stuff */
    int                       audio_error;
    uint8_t                  *audio_input_buffer;
    AVFrame                   audio_frame_buffer;
    AVPacket                  audio_packet;
    uint32_t                  audio_frame_count;
    uint32_t                  audio_delay_count;
    uint32_t                  audio_frame_length;
    uint32_t                  next_audio_pcm_sample_number;
    uint32_t                  last_audio_frame_number;
    uint32_t                  last_remainder_size;
    uint32_t                  audio_media_timescale;
    uint64_t                  audio_skip_samples;
    int64_t                   audio_start_pts;
    int64_t                   av_gap;
    int                       av_sync;
    int                       audio_upsampling;
} libavsmash_handler_t;

static void *open_file( char *file_name, reader_option_t *opt )
{
    libavsmash_handler_t *hp = malloc_zero( sizeof(libavsmash_handler_t) );
    if( !hp )
        return NULL;
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
    uint64_t media_timescale = hp->video_media_timescale;
    if( h->video_sample_count == 1 )
    {
        /* Calculate average framerate. */
        uint64_t media_duration = lsmash_get_media_duration( hp->root, track_ID );
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
        hp->order_converter = malloc_zero( (ts_list.sample_count + 1) * sizeof(order_converter_t) );
        if( !hp->order_converter )
        {
            DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to allocate memory." );
            lsmash_delete_media_timestamps( &ts_list );
            return -1;
        }
        for( uint32_t i = 0; i < ts_list.sample_count; i++ )
            ts_list.timestamp[i].dts = i + 1;
        lsmash_sort_timestamps_composition_order( &ts_list );
        for( uint32_t i = 0; i < ts_list.sample_count; i++ )
            hp->order_converter[i + 1].composition_to_decoding = ts_list.timestamp[i].dts;
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

static uint64_t get_empty_duration( libavsmash_handler_t *hp, uint32_t track_ID, uint32_t media_timescale )
{
    /* Consider empty duration if the first edit is an empty edit. */
    lsmash_edit_t edit;
    if( lsmash_get_explicit_timeline_map( hp->root, track_ID, 1, &edit ) )
        return 0;
    if( edit.duration && edit.start_time == ISOM_EDIT_MODE_EMPTY )
        return av_rescale_q( edit.duration,
                             (AVRational){ 1, hp->movie_param.timescale },
                             (AVRational){ 1, media_timescale } );
    return 0;
}

static int64_t get_start_time( libavsmash_handler_t *hp, uint32_t track_ID )
{
    /* Consider start time of this media if any non-empty edit is present. */
    uint32_t edit_count = lsmash_count_explicit_timeline_map( hp->root, track_ID );
    for( uint32_t edit_number = 1; edit_number <= edit_count; edit_number++ )
    {
        lsmash_edit_t edit;
        if( lsmash_get_explicit_timeline_map( hp->root, track_ID, edit_number, &edit ) )
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
    if( type == ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK )
    {
        hp->video_track_ID = track_ID;
        hp->video_media_timescale = media_param.timescale;
        h->video_sample_count = lsmash_get_sample_count_in_media_timeline( hp->root, track_ID );
        if( setup_timestamp_info( h, track_ID ) )
        {
            DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to set up timestamp info." );
            return -1;
        }
        if( hp->av_sync )
        {
            uint32_t min_cts_sample_number = hp->order_converter ? hp->order_converter[1].composition_to_decoding : 1;
            uint64_t min_cts;
            if( lsmash_get_cts_from_media_timeline( hp->root, track_ID, min_cts_sample_number, &min_cts ) )
            {
                DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to get the minimum CTS of video stream." );
                return -1;
            }
            uint64_t empty_duration = get_empty_duration( hp, track_ID, hp->video_media_timescale );
            hp->video_skip_duration = get_start_time( hp, track_ID );
            hp->video_start_pts = min_cts + empty_duration;
        }
    }
    else
    {
        hp->audio_track_ID = track_ID;
        hp->audio_media_timescale = media_param.timescale;
        hp->audio_frame_count = lsmash_get_sample_count_in_media_timeline( hp->root, track_ID );
        h->audio_pcm_sample_count = lsmash_get_media_duration( hp->root, track_ID );
        uint64_t min_cts;
        if( lsmash_get_cts_from_media_timeline( hp->root, track_ID, 1, &min_cts ) )
        {
            DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to get the minimum CTS of audio stream." );
            return -1;
        }
        if( hp->av_sync )
        {
            uint64_t empty_duration = get_empty_duration( hp, track_ID, hp->audio_media_timescale );
            hp->audio_skip_samples = get_start_time( hp, track_ID );
            hp->audio_start_pts = min_cts + empty_duration;
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
        hp->video_ctx = ctx;
    else
        hp->audio_ctx = ctx;
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
    lsmash_destruct_timeline( hp->root, hp->video_track_ID );
    if( hp->video_ctx )
    {
        avcodec_close( hp->video_ctx );
        hp->video_ctx = NULL;
    }
    return -1;
}

static int get_first_audio_track( lsmash_handler_t *h )
{
    libavsmash_handler_t *hp = (libavsmash_handler_t *)h->audio_private;
    if( !get_first_track_of_type( h, ISOM_MEDIA_HANDLER_TYPE_AUDIO_TRACK ) )
        return 0;
    lsmash_destruct_timeline( hp->root, hp->audio_track_ID );
    if( hp->audio_ctx )
    {
        avcodec_close( hp->audio_ctx );
        hp->audio_ctx = NULL;
    }
    return -1;
}

static void destroy_disposable( void *private_stuff )
{
    libavsmash_handler_t *hp = (libavsmash_handler_t *)private_stuff;
    lsmash_discard_boxes( hp->root );
}

static inline uint32_t get_decoding_sample_number( libavsmash_handler_t *hp, uint32_t composition_sample_number )
{
    return hp->order_converter
         ? hp->order_converter[composition_sample_number].composition_to_decoding
         : composition_sample_number;
}

static int create_keyframe_list( libavsmash_handler_t *hp, uint32_t video_sample_count )
{
    hp->keyframe_list = malloc_zero( (video_sample_count + 1) * sizeof(uint8_t) );
    if( !hp->keyframe_list )
        return -1;
    for( uint32_t composition_sample_number = 1; composition_sample_number <= video_sample_count; composition_sample_number++ )
    {
        uint32_t decoding_sample_number = get_decoding_sample_number( hp, composition_sample_number );
        uint32_t rap_number;
        if( lsmash_get_closest_random_accessible_point_from_media_timeline( hp->root, hp->video_track_ID, decoding_sample_number, &rap_number ) )
            continue;
        if( decoding_sample_number == rap_number )
            hp->keyframe_list[composition_sample_number] = 1;
    }
    return 0;
}

static int get_sample( lsmash_root_t *root, uint32_t track_ID, uint32_t sample_number, uint8_t *buffer, AVPacket *pkt )
{
    av_init_packet( pkt );
    lsmash_sample_t *sample = lsmash_get_sample_from_media_timeline( root, track_ID, sample_number );
    if( !sample )
    {
        pkt->data = NULL;
        pkt->size = 0;
        return 1;
    }
    pkt->flags = sample->prop.random_access_type == ISOM_SAMPLE_RANDOM_ACCESS_TYPE_NONE ? 0 : AV_PKT_FLAG_KEY;
    pkt->size  = sample->length;
    pkt->data  = buffer;
    memcpy( pkt->data, sample->data, sample->length );
    lsmash_delete_sample( sample );
    return 0;
}

static int prepare_video_decoding( lsmash_handler_t *h, video_option_t *opt )
{
    libavsmash_handler_t *hp = (libavsmash_handler_t *)h->video_private;
    if( !hp->video_ctx )
        return 0;
    hp->seek_mode              = opt->seek_mode;
    hp->forward_seek_threshold = opt->forward_seek_threshold;
    /* Note: the input buffer for avcodec_decode_video2 must be FF_INPUT_BUFFER_PADDING_SIZE larger than the actual read bytes. */
    uint32_t video_input_buffer_size = lsmash_get_max_sample_size_in_media_timeline( hp->root, hp->video_track_ID );
    if( video_input_buffer_size == 0 )
    {
        DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "No valid video sample found." );
        return -1;
    }
    hp->video_input_buffer = av_mallocz( video_input_buffer_size + FF_INPUT_BUFFER_PADDING_SIZE );
    if( !hp->video_input_buffer )
    {
        DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to allocate memory to the input buffer for video." );
        return -1;
    }
    if( create_keyframe_list( hp, h->video_sample_count ) )
    {
        DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to create keyframe list." );
        return -1;
    }
    /* swscale */
    int output_pixel_format;
    output_colorspace_index index = determine_colorspace_conversion( &hp->video_ctx->pix_fmt, &output_pixel_format );
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
    int flags = 1 << opt->scaler;
    if( flags != SWS_FAST_BILINEAR )
        flags |= SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP | SWS_ACCURATE_RND;
    hp->sws_ctx = sws_getCachedContext( NULL,
                                        hp->video_ctx->width, hp->video_ctx->height, hp->video_ctx->pix_fmt,
                                        hp->video_ctx->width, hp->video_ctx->height, output_pixel_format,
                                        flags, NULL, NULL, NULL );
    if( !hp->sws_ctx )
    {
        DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to get swscale context." );
        return -1;
    }
    hp->convert_colorspace = colorspace_table[index].convert_colorspace;
    /* BITMAPINFOHEADER */
    h->video_format.biSize        = sizeof( BITMAPINFOHEADER );
    h->video_format.biWidth       = hp->video_ctx->width;
    h->video_format.biHeight      = hp->video_ctx->height;
    h->video_format.biBitCount    = colorspace_table[index].pixel_size << 3;
    h->video_format.biCompression = colorspace_table[index].compression;
    /* Find the first valid video sample. */
    for( uint32_t i = 1; i <= h->video_sample_count + DECODER_DELAY( hp->video_ctx ); i++ )
    {
        AVPacket pkt;
        get_sample( hp->root, hp->video_track_ID, i, hp->video_input_buffer, &pkt );
        AVFrame picture;
        avcodec_get_frame_defaults( &picture );
        int got_picture;
        if( avcodec_decode_video2( hp->video_ctx, &picture, &got_picture, &pkt ) >= 0 && got_picture )
        {
            hp->first_valid_video_sample_number = i - min( DECODER_DELAY( hp->video_ctx ), hp->video_delay_count );
            if( hp->first_valid_video_sample_number > 1 || h->video_sample_count == 1 )
            {
                hp->first_valid_video_sample_size = MAKE_AVIUTL_PITCH( h->video_format.biWidth * h->video_format.biBitCount )
                                                  * h->video_format.biHeight;
                hp->first_valid_video_sample_data = malloc( hp->first_valid_video_sample_size );
                if( !hp->first_valid_video_sample_data )
                    return -1;
                if( hp->first_valid_video_sample_size > hp->convert_colorspace( hp->video_ctx, hp->sws_ctx, &picture, hp->first_valid_video_sample_data ) )
                    continue;
            }
            break;
        }
        else if( pkt.data )
            ++ hp->video_delay_count;
    }
    hp->last_video_sample_number = h->video_sample_count + 1;   /* Force seeking at the first reading. */
    return 0;
}

static int prepare_audio_decoding( lsmash_handler_t *h )
{
    libavsmash_handler_t *hp = (libavsmash_handler_t *)h->audio_private;
    if( !hp->audio_ctx )
        return 0;
    /* Note: the input buffer for avcodec_decode_audio3 must be FF_INPUT_BUFFER_PADDING_SIZE larger than the actual read bytes. */
    uint32_t audio_input_buffer_size = lsmash_get_max_sample_size_in_media_timeline( hp->root, hp->audio_track_ID );
    if( audio_input_buffer_size == 0 )
    {
        DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "No valid audio sample found." );
        return -1;
    }
    hp->audio_input_buffer = av_mallocz( audio_input_buffer_size + FF_INPUT_BUFFER_PADDING_SIZE );
    if( !hp->audio_input_buffer )
    {
        DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to allocate memory to the input buffer for audio." );
        return -1;
    }
    avcodec_get_frame_defaults( &hp->audio_frame_buffer );
    if( hp->av_sync && hp->video_track_ID )
    {
        AVRational audio_sample_base = (AVRational){ 1, hp->audio_ctx->sample_rate };
        hp->av_gap = av_rescale_q( hp->audio_start_pts,
                                   (AVRational){ 1, hp->audio_media_timescale }, audio_sample_base )
                   - av_rescale_q( hp->video_start_pts - hp->video_skip_duration,
                                   (AVRational){ 1, hp->video_media_timescale }, audio_sample_base );
    }
    hp->audio_frame_length = hp->audio_ctx->frame_size;
    if( h->audio_pcm_sample_count * 2 <= hp->audio_frame_count * hp->audio_frame_length )
    {
        /* for HE-AAC upsampling */
        hp->audio_upsampling = 2;
        hp->audio_skip_samples    *= hp->audio_upsampling;
        h->audio_pcm_sample_count *= hp->audio_upsampling;
    }
    else
        hp->audio_upsampling = 1;
    h->audio_pcm_sample_count += hp->av_gap - hp->audio_skip_samples;
    hp->next_audio_pcm_sample_number = h->audio_pcm_sample_count + 1;   /* Force seeking at the first reading. */
    /* WAVEFORMATEXTENSIBLE (WAVEFORMATEX) */
    WAVEFORMATEX *Format = &h->audio_format.Format;
    Format->nChannels       = hp->audio_ctx->channels;
    Format->nSamplesPerSec  = hp->audio_ctx->sample_rate;
    Format->wBitsPerSample  = av_get_bytes_per_sample( hp->audio_ctx->sample_fmt ) * 8;
    Format->nBlockAlign     = (Format->nChannels * Format->wBitsPerSample) / 8;
    Format->nAvgBytesPerSec = Format->nSamplesPerSec * Format->nBlockAlign;
    Format->wFormatTag      = Format->wBitsPerSample == 8 || Format->wBitsPerSample == 16 ? WAVE_FORMAT_PCM : WAVE_FORMAT_EXTENSIBLE;
    if( Format->wFormatTag == WAVE_FORMAT_EXTENSIBLE )
    {
        Format->cbSize = 22;
        h->audio_format.Samples.wValidBitsPerSample = hp->audio_ctx->bits_per_raw_sample;
        h->audio_format.SubFormat                   = KSDATAFORMAT_SUBTYPE_PCM;
    }
    else
        Format->cbSize = 0;
    DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_OK, "frame_length = %"PRIu32", channels = %d, sampling_rate = %d, bits_per_sample = %d, block_align = %d, avg_bps = %d",
                                     hp->audio_frame_length, Format->nChannels, Format->nSamplesPerSec,
                                     Format->wBitsPerSample, Format->nBlockAlign, Format->nAvgBytesPerSec );
    return 0;
}

static int decode_video_sample( libavsmash_handler_t *hp, AVFrame *picture, int *got_picture, uint32_t sample_number )
{
    AVPacket pkt;
    if( get_sample( hp->root, hp->video_track_ID, sample_number, hp->video_input_buffer, &pkt ) )
        return 1;
    if( pkt.flags == AV_PKT_FLAG_KEY )
        hp->last_rap_number = sample_number;
    avcodec_get_frame_defaults( picture );
    if( avcodec_decode_video2( hp->video_ctx, picture, got_picture, &pkt ) < 0 )
    {
        DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_OK, "Failed to decode a video frame." );
        return -1;
    }
    return 0;
}

static int find_random_accessible_point( libavsmash_handler_t *hp, uint32_t composition_sample_number, uint32_t decoding_sample_number, uint32_t *rap_number )
{
    if( decoding_sample_number == 0 )
        decoding_sample_number = get_decoding_sample_number( hp, composition_sample_number );
    lsmash_random_access_type rap_type;
    uint32_t distance;  /* distance from the closest random accessible point to the previous. */
    uint32_t number_of_leadings;
    if( lsmash_get_closest_random_accessible_point_detail_from_media_timeline( hp->root, hp->video_track_ID, decoding_sample_number,
                                                                               rap_number, &rap_type, &number_of_leadings, &distance ) )
        *rap_number = 1;
    int roll_recovery = (rap_type == ISOM_SAMPLE_RANDOM_ACCESS_TYPE_POST_ROLL || rap_type == ISOM_SAMPLE_RANDOM_ACCESS_TYPE_PRE_ROLL);
    int is_leading    = number_of_leadings && (decoding_sample_number - *rap_number <= number_of_leadings);
    if( (roll_recovery || is_leading) && *rap_number > distance )
        *rap_number -= distance;
    return roll_recovery;
}

static void flush_buffers( AVCodecContext *ctx, int *error )
{
    /* Close and reopen the decoder even if the decoder implements avcodec_flush_buffers().
     * It seems this brings about more stable composition when seeking. */
    AVCodec *codec = ctx->codec;
    avcodec_close( ctx );
    if( avcodec_open2( ctx, codec, NULL ) < 0 )
    {
        MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to flush buffers.\nIt is recommended you reopen the file." );
        *error = 1;
    }
}

static uint32_t seek_video( libavsmash_handler_t *hp, AVFrame *picture, uint32_t composition_sample_number, uint32_t rap_number, int error_ignorance )
{
    /* Prepare to decode from random accessible sample. */
    flush_buffers( hp->video_ctx, &hp->video_error );
    if( hp->video_error )
        return 0;
    hp->video_delay_count = 0;
    hp->decode_status = DECODE_REQUIRE_INITIAL;
    int dummy;
    uint32_t i;
    for( i = rap_number; i < composition_sample_number + DECODER_DELAY( hp->video_ctx ); i++ )
    {
        int ret = decode_video_sample( hp, picture, &dummy, i );
        if( ret == -1 && !error_ignorance )
        {
            DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_OK, "Failed to decode a video frame." );
            return 0;
        }
        else if( ret == 1 )
            break;      /* Sample doesn't exist. */
    }
    hp->video_delay_count = min( DECODER_DELAY( hp->video_ctx ), i - rap_number );
    DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_OK, "rap_number = %"PRIu32", seek_position = %"PRIu32", video_delay_count = %"PRIu32,
                                     rap_number, i, hp->video_delay_count );
    return i;
}

static int get_picture( libavsmash_handler_t *hp, AVFrame *picture, uint32_t current, uint32_t goal, uint32_t video_sample_count )
{
    if( hp->decode_status == DECODE_INITIALIZING )
    {
        if( hp->video_delay_count > DECODER_DELAY( hp->video_ctx ) )
            -- hp->video_delay_count;
        else
            hp->decode_status = DECODE_INITIALIZED;
    }
    int got_picture = 0;
    while( current <= goal )
    {
        int ret = decode_video_sample( hp, picture, &got_picture, current );
        if( ret == -1 )
            return -1;
        else if( ret == 1 )
            break;      /* Sample doesn't exist. */
        ++current;
        if( !got_picture )
            ++ hp->video_delay_count;
        DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_OK, "current frame = %d, decoded frame = %d, got_picture = %d, delay_count = %d",
                                         goal, current - 1, got_picture, hp->video_delay_count );
        if( hp->video_delay_count > DECODER_DELAY( hp->video_ctx ) && hp->decode_status == DECODE_INITIALIZED )
            break;
    }
    /* Flush the last frames. */
    if( current > video_sample_count && DECODER_DELAY( hp->video_ctx ) )
        while( current <= goal )
        {
            AVPacket pkt;
            av_init_packet( &pkt );
            pkt.data = NULL;
            pkt.size = 0;
            avcodec_get_frame_defaults( picture );
            if( avcodec_decode_video2( hp->video_ctx, picture, &got_picture, &pkt ) < 0 )
            {
                DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_OK, "Failed to decode and flush a video frame." );
                return -1;
            }
            ++current;
            DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_OK, "current frame = %d, decoded frame = %d, got_picture = %d, delay_count = %d",
                                             goal, current - 1, got_picture, hp->video_delay_count );
        }
    if( hp->decode_status == DECODE_REQUIRE_INITIAL )
        hp->decode_status = DECODE_INITIALIZING;
    return got_picture ? 0 : -1;
}

static int read_video( lsmash_handler_t *h, int sample_number, void *buf )
{
#define MAX_ERROR_COUNT 3       /* arbitrary */
    libavsmash_handler_t *hp = (libavsmash_handler_t *)h->video_private;
    if( hp->video_error )
        return 0;
    ++sample_number;            /* For L-SMASH, sample_number is 1-origin. */
    if( sample_number < hp->first_valid_video_sample_number || h->video_sample_count == 1 )
    {
        /* Copy the first valid video sample data. */
        memcpy( buf, hp->first_valid_video_sample_data, hp->first_valid_video_sample_size );
        hp->last_video_sample_number = h->video_sample_count + 1;   /* Force seeking at the next access for valid video sample. */
        return hp->first_valid_video_sample_size;
    }
    AVFrame picture;            /* Decoded video data will be stored here. */
    uint32_t start_number;      /* number of sample, for normal decoding, where decoding starts excluding decoding delay */
    uint32_t rap_number;        /* number of sample, for seeking, where decoding starts excluding decoding delay */
    int seek_mode = hp->seek_mode;
    int roll_recovery = 0;
    if( sample_number > hp->last_video_sample_number
     && sample_number <= hp->last_video_sample_number + hp->forward_seek_threshold )
    {
        start_number = hp->last_video_sample_number + 1 + hp->video_delay_count;
        rap_number = hp->last_rap_number;
    }
    else
    {
        roll_recovery = find_random_accessible_point( hp, sample_number, 0, &rap_number );
        if( rap_number == hp->last_rap_number && sample_number > hp->last_video_sample_number )
        {
            roll_recovery = 0;
            start_number = hp->last_video_sample_number + 1 + hp->video_delay_count;
        }
        else
        {
            /* Require starting to decode from random accessible sample. */
            hp->last_rap_number = rap_number;
            start_number = seek_video( hp, &picture, sample_number, rap_number, roll_recovery || seek_mode != SEEK_MODE_NORMAL );
        }
    }
    /* Get desired picture. */
    int error_count = 0;
    while( start_number == 0 || get_picture( hp, &picture, start_number, sample_number + hp->video_delay_count, h->video_sample_count ) )
    {
        /* Failed to get desired picture. */
        if( hp->video_error || seek_mode == SEEK_MODE_AGGRESSIVE )
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
            roll_recovery = find_random_accessible_point( hp, sample_number, rap_number - 1, &rap_number );
            hp->last_rap_number = rap_number;
        }
        start_number = seek_video( hp, &picture, sample_number, rap_number, roll_recovery || seek_mode != SEEK_MODE_NORMAL );
    }
    hp->last_video_sample_number = sample_number;
    DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_OK, "src_linesize[0] = %d, src_linesize[1] = %d, src_linesize[2] = %d, src_linesize[3] = %d",
                                     picture.linesize[0], picture.linesize[1], picture.linesize[2], picture.linesize[3] );
    return hp->convert_colorspace( hp->video_ctx, hp->sws_ctx, &picture, buf );
video_fail:
    /* fatal error of decoding */
    DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Couldn't read video frame." );
    return 0;
#undef MAX_ERROR_COUNT
}

static inline int get_frame_length( libavsmash_handler_t *hp, uint32_t frame_number, uint32_t *frame_length )
{
    if( hp->audio_frame_length == 0 )
    {
        /* variable frame length
         * Guess the frame length from sample duration. */
        if( lsmash_get_sample_delta_from_media_timeline( hp->root, hp->audio_track_ID, frame_number, frame_length ) )
            return -1;
    }
    else
        /* constant frame length */
        *frame_length = hp->audio_frame_length;
    return 0;
}

static uint32_t get_preroll_samples( libavsmash_handler_t *hp, uint32_t *frame_number )
{
    /* Some audio CODEC requires pre-roll for correct composition. */
    lsmash_sample_property_t prop;
    if( lsmash_get_sample_property_from_media_timeline( hp->root, hp->audio_track_ID, *frame_number, &prop ) )
        return 0;
    if( prop.pre_roll.distance == 0 )
        return 0;
    uint32_t preroll_samples = 0;
    for( uint32_t i = 0; i < prop.pre_roll.distance; i++ )
    {
        if( *frame_number > 1 )
            --(*frame_number);
        else
            break;
        uint32_t frame_length;
        if( get_frame_length( hp, *frame_number, &frame_length ) )
            break;
        preroll_samples += frame_length;
    }
    return preroll_samples;
}

static int read_audio( lsmash_handler_t *h, int start, int wanted_length, void *buf )
{
    DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_OK, "start = %d, wanted_length = %d", start, wanted_length );
    libavsmash_handler_t *hp = (libavsmash_handler_t *)h->audio_private;
    if( hp->audio_error )
        return 0;
    uint32_t frame_number;
    uint64_t data_offset;
    int      copy_size;
    int      output_length = 0;
    int      block_align = h->audio_format.Format.nBlockAlign;
    if( start > 0 && start == hp->next_audio_pcm_sample_number )
    {
        frame_number = hp->last_audio_frame_number;
        if( hp->last_remainder_size && hp->audio_frame_buffer.data[0] )
        {
            copy_size = min( hp->last_remainder_size, wanted_length * block_align );
            memcpy( buf, hp->audio_frame_buffer.data[0], copy_size );
            buf                     += copy_size;
            hp->last_remainder_size -= copy_size;
            int copied_length = copy_size / block_align;
            output_length += copied_length;
            wanted_length -= copied_length;
            if( wanted_length <= 0 )
                goto audio_out;
        }
        if( hp->audio_packet.size <= 0 )
            ++frame_number;
        data_offset = 0;
    }
    else
    {
        /* Seek audio stream. */
        flush_buffers( hp->audio_ctx, &hp->audio_error );
        if( hp->audio_error )
            return 0;
        hp->audio_delay_count            = 0;
        hp->last_remainder_size          = 0;
        hp->next_audio_pcm_sample_number = 0;
        hp->last_audio_frame_number      = 0;
        uint64_t start_frame_pos;
        if( start >= 0 )
            start_frame_pos = start;
        else
        {
            int silent_length = -start;
            copy_size = silent_length * block_align;
            memset( buf, 0, copy_size );
            buf += copy_size;
            output_length += silent_length;
            wanted_length -= silent_length;
            start_frame_pos = 0;
        }
        start_frame_pos += hp->audio_skip_samples;
        frame_number = 1;
        uint64_t next_frame_pos = 0;
        uint32_t frame_length   = 0;
        do
        {
            if( get_frame_length( hp, frame_number, &frame_length ) )
                break;
            next_frame_pos += (uint64_t)frame_length * hp->audio_upsampling;
            if( start_frame_pos < next_frame_pos )
                break;
            ++frame_number;
        } while( frame_number <= hp->audio_frame_count );
        uint32_t preroll_samples = get_preroll_samples( hp, &frame_number );
        data_offset = (start_frame_pos + (preroll_samples + frame_length) * hp->audio_upsampling - next_frame_pos) * block_align;
    }
    do
    {
        copy_size = 0;
        AVPacket *pkt = &hp->audio_packet;
        if( frame_number > hp->audio_frame_count )
        {
            if( hp->audio_delay_count )
            {
                /* Null packet */
                av_init_packet( pkt );
                pkt->data = NULL;
                pkt->size = 0;
                -- hp->audio_delay_count;
            }
            else
                goto audio_out;
        }
        else if( pkt->size <= 0 )
            get_sample( hp->root, hp->audio_track_ID, frame_number, hp->audio_input_buffer, pkt );
        int output_audio = 0;
        do
        {
            int decode_complete;
            int wasted_data_length = avcodec_decode_audio4( hp->audio_ctx, &hp->audio_frame_buffer, &decode_complete, pkt );
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
            if( decode_complete && hp->audio_frame_buffer.data[0] )
            {
                int decoded_data_size = hp->audio_frame_buffer.nb_samples * block_align;
                if( decoded_data_size > data_offset )
                {
                    copy_size = min( decoded_data_size - data_offset, wanted_length * block_align );
                    memcpy( buf, hp->audio_frame_buffer.data[0] + data_offset, copy_size );
                    int copied_length = copy_size / block_align;
                    output_length += copied_length;
                    wanted_length -= copied_length;
                    buf           += copy_size;
                    data_offset = 0;
                    if( wanted_length <= 0 )
                    {
                        hp->last_remainder_size = decoded_data_size - copy_size;
                        goto audio_out;
                    }
                }
                else
                {
                    copy_size = 0;
                    data_offset -= decoded_data_size;
                }
                output_audio = 1;
            }
            DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_OK, "frame_number = %d, wasted_data_length = %d,"
                                             "decode_complete = %d, decoded_length = %d, copied_length = %d, output_length = %d",
                                             frame_number, wasted_data_length, decode_complete,
                                             hp->audio_frame_buffer.nb_samples, copy_size / block_align, output_length );
        } while( pkt->size > 0 );
        if( !output_audio && pkt->data )    /* Count audio frame delay only if feeding non-NULL packet. */
            ++ hp->audio_delay_count;
        ++frame_number;
    } while( 1 );
audio_out:
    DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_OK, "output_length = %d, remainder = %d", output_length, hp->last_remainder_size );
    if( hp->last_remainder_size && copy_size != 0 && hp->audio_frame_buffer.data[0] )
        /* Move unused decoded data to the head of output buffer for the next access. */
        memmove( hp->audio_frame_buffer.data[0], hp->audio_frame_buffer.data[0] + copy_size, hp->last_remainder_size );
    hp->next_audio_pcm_sample_number = start + output_length;
    hp->last_audio_frame_number = frame_number;
    return output_length;
}

static int is_keyframe( lsmash_handler_t *h, int sample_number )
{
    libavsmash_handler_t *hp = (libavsmash_handler_t *)h->video_private;
    return hp->keyframe_list[sample_number + 1];
}

static int delay_audio( lsmash_handler_t *h, int *start, int wanted_length, int audio_delay )
{
    /* Even if start become negative, its absolute value shall be equal to wanted_length or smaller. */
    libavsmash_handler_t *hp = (libavsmash_handler_t *)h->audio_private;
    int end = *start + wanted_length;
    audio_delay += hp->av_gap;
    if( *start < audio_delay && end <= audio_delay )
    {
        hp->next_audio_pcm_sample_number = h->audio_pcm_sample_count + 1;   /* Force seeking at the next access for valid audio frame. */
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
    if( hp->order_converter )
        free( hp->order_converter );
    if( hp->keyframe_list )
        free( hp->keyframe_list );
    if( hp->video_input_buffer )
        av_free( hp->video_input_buffer );
    if( hp->first_valid_video_sample_data )
        free( hp->first_valid_video_sample_data );
    if( hp->sws_ctx )
        sws_freeContext( hp->sws_ctx );
    if( hp->video_ctx )
        avcodec_close( hp->video_ctx );
}

static void audio_cleanup( lsmash_handler_t *h )
{
    libavsmash_handler_t *hp = (libavsmash_handler_t *)h->audio_private;
    if( !hp )
        return;
    if( hp->audio_input_buffer )
        av_free( hp->audio_input_buffer );
    if( hp->audio_ctx )
        avcodec_close( hp->audio_ctx );
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
