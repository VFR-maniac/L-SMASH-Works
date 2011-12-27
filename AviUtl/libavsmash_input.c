/*****************************************************************************
 * libavsmash_input.c
 *****************************************************************************
 * Copyright (C) 2011 L-SMASH Works project
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

#include "lsmashinput.h"

/* L-SMASH */
#define LSMASH_DEMUXER_ENABLED
#include <lsmash.h>                 /* Demuxer */

/* Libav
 * The binary file will be LGPLed or GPLed. */
#include <libavformat/avformat.h>   /* Codec specific info importer */
#include <libavcodec/avcodec.h>     /* Decoder */
#include <libswscale/swscale.h>     /* Colorspace converter */
#ifdef DEBUG_VIDEO
#include <libavutil/pixdesc.h>
#endif

#define DECODER_DELAY( ctx ) (ctx->has_b_frames + (ctx->thread_type == FF_THREAD_FRAME ? ctx->thread_count - 1 : 0))

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
    lsmash_root_t     *root;
    uint32_t           video_track_ID;
    uint32_t           audio_track_ID;
    /* Libav's stuff */
    AVCodecContext    *video_ctx;
    AVCodecContext    *audio_ctx;
    AVFormatContext   *format_ctx;
    struct SwsContext *sws_ctx;
    /* Video stuff */
    uint8_t           *video_input_buffer;
    uint32_t           video_input_buffer_size;
    uint32_t           last_video_sample_number;
    uint32_t           delay_count;
    decode_status_t    decode_status;
    order_converter_t *order_converter;
    uint8_t           *keyframe_list;
    int (*convert_colorspace)( AVCodecContext *, struct SwsContext *, AVFrame *, uint8_t * );
    /* Audio stuff */
    uint8_t           *audio_input_buffer;
    uint32_t           audio_input_buffer_size;
    uint8_t           *audio_output_buffer;
    uint32_t           audio_frame_count;
    uint32_t           next_audio_pcm_sample_number;
    uint32_t           last_audio_sample_number;
    uint32_t           last_remainder_size;
} libavsmash_handler_t;

/* Colorspace converters */
int to_yuv16le_to_yc48( AVCodecContext *video_ctx, struct SwsContext *sws_ctx, AVFrame *picture, uint8_t *buf );
int to_rgb24( AVCodecContext *video_ctx, struct SwsContext *sws_ctx, AVFrame *picture, uint8_t *buf );
int to_yuy2( AVCodecContext *video_ctx, struct SwsContext *sws_ctx, AVFrame *picture, uint8_t *buf );

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
    libavsmash_handler_t *hp = (libavsmash_handler_t *)h->reader->private_stuff;
    uint64_t media_timescale = lsmash_get_media_timescale( hp->root, track_ID );
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
            MESSAGE_BOX_DESKTOP( MB_OK, "Detect CTS duplication at frame %"PRIu32, i );
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

static int get_first_track_of_type( lsmash_handler_t *h, uint32_t number_of_tracks, uint32_t type, int threads )
{
    libavsmash_handler_t *hp = (libavsmash_handler_t *)h->reader->private_stuff;
    /* L-SMASH */
    uint32_t track_ID = 0;
    uint32_t i;
    for( i = 1; i <= number_of_tracks; i++ )
    {
        track_ID = lsmash_get_track_ID( hp->root, i );
        if( track_ID == 0 )
            return -1;
        lsmash_media_parameters_t media_param;
        lsmash_initialize_media_parameters( &media_param );
        if( lsmash_get_media_parameters( hp->root, track_ID, &media_param ) )
        {
            DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to get media parameters." );
            return -1;
        }
        if( media_param.handler_type == type )
            break;
    }
    if( i > number_of_tracks )
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
        h->video_sample_count = lsmash_get_sample_count_in_media_timeline( hp->root, track_ID );
        if( setup_timestamp_info( h, track_ID ) )
        {
            DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to set up timestamp info." );
            return -1;
        }
    }
    else
    {
        hp->audio_track_ID = track_ID;
        hp->audio_frame_count = lsmash_get_sample_count_in_media_timeline( hp->root, track_ID );
        h->audio_pcm_sample_count = lsmash_get_media_duration( hp->root, track_ID );
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
    ctx->thread_count = threads;
    if( avcodec_open2( ctx, codec, NULL ) < 0 )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to avcodec_open2." );
        return -1;
    }
    return 0;
}

static int decode_video_sample( libavsmash_handler_t *hp, AVFrame *picture, int *got_picture, uint32_t sample_number )
{
    lsmash_sample_t *sample = lsmash_get_sample_from_media_timeline( hp->root, hp->video_track_ID, sample_number );
    if( !sample )
        return 1;
    AVPacket pkt;
    av_init_packet( &pkt );
    pkt.flags = sample->prop.random_access_type == ISOM_SAMPLE_RANDOM_ACCESS_TYPE_NONE ? 0 : AV_PKT_FLAG_KEY;
    /* Note: the input buffer for avcodec_decode_video2 must be FF_INPUT_BUFFER_PADDING_SIZE larger than the actual read bytes. */
    pkt.size  = sample->length;
    pkt.data  = hp->video_input_buffer;
    memset( pkt.data, 0, hp->video_input_buffer_size );
    memcpy( pkt.data, sample->data, sample->length );
    lsmash_delete_sample( sample );
    if( avcodec_decode_video2( hp->video_ctx, picture, got_picture, &pkt ) < 0 )
    {
        MessageBox( HWND_DESKTOP, "Failed to decode a video frame.", "lsmashinput", MB_ICONERROR | MB_OK );
        return -1;
    }
    return 0;
}

static int get_picture( libavsmash_handler_t *hp, AVFrame *picture, uint32_t current, uint32_t goal, uint32_t video_sample_count )
{
    if( hp->decode_status == DECODE_INITIALIZING )
    {
        if( hp->delay_count > DECODER_DELAY( hp->video_ctx ) )
            -- hp->delay_count;
        else
            hp->decode_status = DECODE_INITIALIZED;
    }
    avcodec_get_frame_defaults( picture );
    int got_picture = 0;
    do
    {
        int ret = decode_video_sample( hp, picture, &got_picture, current );
        if( ret == -1 )
            return -2;
        else if( ret == 1 )
            break;  /* Sample doesn't exist. */
        ++current;
        if( !got_picture )
            ++ hp->delay_count;
        DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_OK, "current frame = %d, decoded frame = %d, delay_count = %d",
                                         goal, current - 1, hp->delay_count );
        if( hp->delay_count > DECODER_DELAY( hp->video_ctx ) && hp->decode_status == DECODE_INITIALIZED )
            break;
        if( got_picture && current > goal )
            break;
    } while( 1 );
    /* Flush the last frames. */
    if( current > video_sample_count && !got_picture && DECODER_DELAY( hp->video_ctx ) )
    {
        AVPacket pkt;
        av_init_packet( &pkt );
        pkt.data = NULL;
        pkt.size = 0;
        if( avcodec_decode_video2( hp->video_ctx, picture, &got_picture, &pkt ) < 0 )
        {
            MessageBox( HWND_DESKTOP, "Failed to decode a video frame.", "lsmashinput", MB_ICONERROR | MB_OK );
            return -1;
        }
    }
    if( hp->decode_status == DECODE_REQUIRE_INITIAL )
        hp->decode_status = DECODE_INITIALIZING;
    return got_picture ? 0 : -1;
}

static int create_keyframe_list( libavsmash_handler_t *hp, uint32_t video_sample_count )
{
    hp->keyframe_list = malloc_zero( (video_sample_count + 1) * sizeof(uint8_t) );
    if( !hp->keyframe_list )
        return -1;
    for( uint32_t composition_sample_number = 1; composition_sample_number <= video_sample_count; composition_sample_number++ )
    {
        uint32_t decoding_sample_number = hp->order_converter
                                        ? hp->order_converter[composition_sample_number].composition_to_decoding
                                        : composition_sample_number;
        uint32_t rap_number;
        if( lsmash_get_closest_random_accessible_point_from_media_timeline( hp->root, hp->video_track_ID, decoding_sample_number, &rap_number ) )
            continue;
        if( decoding_sample_number == rap_number )
            hp->keyframe_list[composition_sample_number] = 1;
    }
    return 0;
}

static int prepare_video_decoding( lsmash_handler_t *h )
{
    libavsmash_handler_t *hp = (libavsmash_handler_t *)h->reader->private_stuff;
    if( !hp->video_ctx )
        return 0;
    hp->video_input_buffer_size = lsmash_get_max_sample_size_in_media_timeline( hp->root, hp->video_track_ID );
    if( hp->video_input_buffer_size == 0 )
    {
        DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "No valid video sample found." );
        return -1;
    }
    hp->video_input_buffer_size += FF_INPUT_BUFFER_PADDING_SIZE;
    hp->video_input_buffer = av_mallocz( hp->video_input_buffer_size );
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
    AVFrame picture;
    if( get_picture( hp, &picture, 1, 1, h->video_sample_count ) )
    {
        DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to get the first video sample." );
        return -1;
    }
    hp->last_video_sample_number = 1;
    /* Hack for avoiding YUV-scale conversion */
    static const struct
    {
        enum PixelFormat full;
        enum PixelFormat limited;
    } range_hack_table[]
        = {
            { PIX_FMT_YUVJ420P, PIX_FMT_YUV420P },
            { PIX_FMT_YUVJ422P, PIX_FMT_YUV422P },
            { PIX_FMT_YUVJ444P, PIX_FMT_YUV444P },
            { PIX_FMT_YUVJ440P, PIX_FMT_YUV440P },
            { PIX_FMT_NONE,     PIX_FMT_NONE    }
          };
    for( int i = 0; range_hack_table[i].full != PIX_FMT_NONE; i++ )
        if( hp->video_ctx->pix_fmt == range_hack_table[i].full )
            hp->video_ctx->pix_fmt = range_hack_table[i].limited;
    /* swscale */
    enum PixelFormat out_pix_fmt;
    uint32_t pixel_size;
    uint32_t compression;
    switch( hp->video_ctx->pix_fmt )
    {
        case PIX_FMT_YUV444P :
        case PIX_FMT_YUV440P :
        case PIX_FMT_YUV420P9LE :
        case PIX_FMT_YUV420P9BE :
        case PIX_FMT_YUV422P9LE :
        case PIX_FMT_YUV422P9BE :
        case PIX_FMT_YUV444P9LE :
        case PIX_FMT_YUV444P9BE :
        case PIX_FMT_YUV420P10LE :
        case PIX_FMT_YUV420P10BE :
        case PIX_FMT_YUV422P10LE :
        case PIX_FMT_YUV422P10BE :
        case PIX_FMT_YUV444P10LE :
        case PIX_FMT_YUV444P10BE :
        case PIX_FMT_YUV420P16LE :
        case PIX_FMT_YUV420P16BE :
        case PIX_FMT_YUV422P16LE :
        case PIX_FMT_YUV422P16BE :
        case PIX_FMT_YUV444P16LE :
        case PIX_FMT_YUV444P16BE :
        case PIX_FMT_RGB48LE :
        case PIX_FMT_RGB48BE :
        case PIX_FMT_BGR48LE :
        case PIX_FMT_BGR48BE :
        case PIX_FMT_GBRP9LE :
        case PIX_FMT_GBRP9BE :
        case PIX_FMT_GBRP10LE :
        case PIX_FMT_GBRP10BE :
        case PIX_FMT_GBRP16LE :
        case PIX_FMT_GBRP16BE :
            hp->convert_colorspace = to_yuv16le_to_yc48;
            pixel_size             = YC48_SIZE;
            out_pix_fmt            = PIX_FMT_YUV444P16LE;   /* planar YUV 4:4:4, 48bpp little-endian -> YC48 */
            compression            = MAKEFOURCC( 'Y', 'C', '4', '8' );
            break;
        case PIX_FMT_RGB24 :
        case PIX_FMT_BGR24 :
        case PIX_FMT_ARGB :
        case PIX_FMT_RGBA :
        case PIX_FMT_ABGR :
        case PIX_FMT_BGRA :
        case PIX_FMT_BGR8 :
        case PIX_FMT_BGR4 :
        case PIX_FMT_BGR4_BYTE :
        case PIX_FMT_RGB8 :
        case PIX_FMT_RGB4 :
        case PIX_FMT_RGB4_BYTE :
        case PIX_FMT_RGB565LE :
        case PIX_FMT_RGB565BE :
        case PIX_FMT_RGB555LE :
        case PIX_FMT_RGB555BE :
        case PIX_FMT_BGR565LE :
        case PIX_FMT_BGR565BE :
        case PIX_FMT_BGR555LE :
        case PIX_FMT_BGR555BE :
        case PIX_FMT_RGB444LE :
        case PIX_FMT_RGB444BE :
        case PIX_FMT_BGR444LE :
        case PIX_FMT_BGR444BE :
        case PIX_FMT_GBRP :
            hp->convert_colorspace = to_rgb24;
            pixel_size             = RGB24_SIZE;
            out_pix_fmt            = PIX_FMT_BGR24;         /* packed RGB 8:8:8, 24bpp, BGRBGR... */
            compression            = 0;
            break;
        default :
            hp->convert_colorspace = to_yuy2;
            pixel_size             = YUY2_SIZE;
            out_pix_fmt            = PIX_FMT_YUYV422;       /* packed YUV 4:2:2, 16bpp */
            compression            = MAKEFOURCC( 'Y', 'U', 'Y', '2' );
            break;
    }
    hp->sws_ctx = sws_getCachedContext( NULL,
                                        hp->video_ctx->width, hp->video_ctx->height, hp->video_ctx->pix_fmt,
                                        hp->video_ctx->width, hp->video_ctx->height, out_pix_fmt,
                                        SWS_POINT, NULL, NULL, NULL );
    if( !hp->sws_ctx )
    {
        DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to get swscale context." );
        return -1;
    }
    DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_OK, "src_pix_fmt = %s, dst_pix_fmt = %s",
                                     av_pix_fmt_descriptors[hp->video_ctx->pix_fmt].name, av_pix_fmt_descriptors[out_pix_fmt].name );
    /* BITMAPINFOHEADER */
    h->video_format.biSize        = sizeof( BITMAPINFOHEADER );
    h->video_format.biWidth       = hp->video_ctx->width;
    h->video_format.biHeight      = hp->video_ctx->height;
    h->video_format.biPlanes      = 1;
    h->video_format.biBitCount    = pixel_size * 8;
    h->video_format.biCompression = compression;
    h->video_format.biSizeImage   = picture.linesize[0] * pixel_size * hp->video_ctx->height;
    return 0;
}

static int prepare_audio_decoding( lsmash_handler_t *h )
{
    libavsmash_handler_t *hp = (libavsmash_handler_t *)h->reader->private_stuff;
    if( !hp->audio_ctx )
        return 0;
    hp->audio_input_buffer_size = lsmash_get_max_sample_size_in_media_timeline( hp->root, hp->audio_track_ID );
    if( hp->audio_input_buffer_size == 0 )
    {
        DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "No valid audio sample found." );
        return -1;
    }
    hp->audio_input_buffer_size += FF_INPUT_BUFFER_PADDING_SIZE;
    hp->audio_input_buffer = av_mallocz( hp->audio_input_buffer_size );
    if( !hp->audio_input_buffer )
    {
        DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to allocate memory to the input buffer for audio." );
        return -1;
    }
    hp->audio_output_buffer = av_mallocz( AVCODEC_MAX_AUDIO_FRAME_SIZE );
    if( !hp->audio_output_buffer )
    {
        DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to allocate memory to the output buffer for audio." );
        return -1;
    }
    /* WAVEFORMATEX */
    h->audio_format.nChannels       = hp->audio_ctx->channels;
    h->audio_format.nSamplesPerSec  = hp->audio_ctx->sample_rate;
    h->audio_format.wBitsPerSample  = hp->audio_ctx->bits_per_raw_sample;
    h->audio_format.nBlockAlign     = h->audio_format.nChannels * av_get_bytes_per_sample( hp->audio_ctx->sample_fmt );
    h->audio_format.nAvgBytesPerSec = h->audio_format.nSamplesPerSec * h->audio_format.nBlockAlign;
    h->audio_format.wFormatTag      = WAVE_FORMAT_PCM;      /* AviUtl doesn't support WAVE_FORMAT_EXTENSIBLE even if the input audio is 24bit PCM. */
    h->audio_format.cbSize          = 0;
    DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_OK, "channels = %d, sampling_rate = %d, bits_per_sample = %d, block_align = %d, avg_bps = %d",
                                     h->audio_format.nChannels, h->audio_format.nSamplesPerSec,
                                     h->audio_format.wBitsPerSample, h->audio_format.nBlockAlign, h->audio_format.nAvgBytesPerSec );
    return 0;
}

static void cleanup( lsmash_handler_t *h )
{
    libavsmash_handler_t *hp = (libavsmash_handler_t *)h->reader->private_stuff;
    if( !hp )
        return;
    lsmash_destroy_root( hp->root );
    if( hp->video_ctx )
        avcodec_close( hp->video_ctx );
    if( hp->audio_ctx )
        avcodec_close( hp->audio_ctx );
    if( hp->format_ctx )
        avformat_close_input( &hp->format_ctx );
    if( hp->sws_ctx )
        sws_freeContext( hp->sws_ctx );
    if( hp->video_input_buffer )
        av_free( hp->video_input_buffer );
    if( hp->order_converter )
        free( hp->order_converter );
    if( hp->keyframe_list )
        free( hp->keyframe_list );
    if( hp->audio_input_buffer )
        av_free( hp->audio_input_buffer );
    if( hp->audio_output_buffer )
        av_free( hp->audio_output_buffer );
    free( hp );
}

static BOOL open_file( lsmash_handler_t *h, char *file_name, int threads )
{
    libavsmash_handler_t *hp = malloc_zero( sizeof(libavsmash_handler_t) );
    if( !hp )
        return FALSE;
    h->reader->private_stuff = hp;
    av_register_all();
    avcodec_register_all();
    /* L-SMASH */
    hp->root = lsmash_open_movie( file_name, LSMASH_FILE_MODE_READ );
    if( !hp->root )
        return FALSE;
    lsmash_movie_parameters_t movie_param;
    lsmash_initialize_movie_parameters( &movie_param );
    lsmash_get_movie_parameters( hp->root, &movie_param );
    /* libavformat */
    if( avformat_open_input( &hp->format_ctx, file_name, NULL, NULL ) )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to avformat_open_input." );
        return FALSE;
    }
    if( avformat_find_stream_info( hp->format_ctx, NULL ) < 0 )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to avformat_find_stream_info." );
        return FALSE;
    }
    uint32_t number_of_tracks = movie_param.number_of_tracks;
    if( number_of_tracks == 0 )
        return FALSE;
    /* Get video track. If absent, ignore video track. */
    if( get_first_track_of_type( h, number_of_tracks, ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK, threads ) )
    {
        lsmash_destruct_timeline( hp->root, hp->video_track_ID );
        if( hp->video_ctx )
        {
            avcodec_close( hp->video_ctx );
            hp->video_ctx = NULL;
        }
    }
    /* Get audio track. If absent, ignore audio track. */
    if( get_first_track_of_type( h, number_of_tracks, ISOM_MEDIA_HANDLER_TYPE_AUDIO_TRACK, threads ) )
    {
        lsmash_destruct_timeline( hp->root, hp->audio_track_ID );
        if( hp->audio_ctx )
        {
            avcodec_close( hp->audio_ctx );
            hp->audio_ctx = NULL;
        }
    }
    lsmash_discard_boxes( hp->root );
    if( !hp->video_ctx && !hp->audio_ctx )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "No readable video and/or audio streams." );
        return FALSE;
    }
    /* Prepare decoding. */
    if( prepare_video_decoding( h )
     || prepare_audio_decoding( h ) )
        return FALSE;
    return TRUE;
}

static uint32_t seek_video( libavsmash_handler_t *hp, AVFrame *picture, uint32_t composition_sample_number, uint32_t *rap_number )
{
    /* Prepare to decode from random accessible sample. */
    avcodec_flush_buffers( hp->video_ctx );
    hp->delay_count   = 0;
    hp->decode_status = DECODE_REQUIRE_INITIAL;
    uint32_t decoding_sample_number = hp->order_converter
                                    ? hp->order_converter[composition_sample_number].composition_to_decoding
                                    : composition_sample_number;
    uint32_t distance;
    if( lsmash_get_closest_random_accessible_point_detail_from_media_timeline( hp->root, hp->video_track_ID, decoding_sample_number, rap_number, NULL, NULL, &distance ) )
        *rap_number = 1;
    if( distance && *rap_number > distance )
        *rap_number -= distance;
    hp->video_ctx->skip_frame = AVDISCARD_NONREF;
    int dummy;
    uint32_t i;
    for( i = *rap_number; i < composition_sample_number + DECODER_DELAY( hp->video_ctx ); i++ )
    {
        if( i >= composition_sample_number )
            hp->video_ctx->skip_frame = AVDISCARD_DEFAULT;
        avcodec_get_frame_defaults( picture );
        if( decode_video_sample( hp, picture, &dummy, i ) == 1 )
        {
            /* Sample doesn't exist. */
            hp->video_ctx->skip_frame = AVDISCARD_DEFAULT;
            break;
        }
    }
    hp->delay_count = DECODER_DELAY( hp->video_ctx );
    DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_OK, "rap_number = %d, distance = %d, seek_position = %d", *rap_number, distance, i );
    return i - hp->delay_count;
}

static int read_video( lsmash_handler_t *h, int sample_number, void *buf )
{
    libavsmash_handler_t *hp = (libavsmash_handler_t *)h->reader->private_stuff;
    ++sample_number;            /* For L-SMASH, sample_number is 1-origin. */
    AVFrame picture;            /* Decoded video data will be stored here. */
    uint32_t start_number;      /* number of sample, for normal decoding, where decoding starts excluding decoding delay */
    uint32_t rap_number;        /* number of sample, for seeking, where decoding starts excluding decoding delay */
    if( sample_number == hp->last_video_sample_number + 1 )
        start_number = rap_number = sample_number;
    else
        /* Require starting to decode from random accessible sample. */
        start_number = seek_video( hp, &picture, sample_number, &rap_number );
    do
    {
        int error = get_picture( hp, &picture, start_number + hp->delay_count, sample_number + hp->delay_count, h->video_sample_count );
        if( error == 0 )
            break;
        else if( error == -1 )
        {
            /* No error of decoding, but couldn't get a picture.
             * Retry to decode from more past random accessible sample. */
            start_number = seek_video( hp, &picture, rap_number - 1, &rap_number );
            if( start_number == 1 )
                return 0;   /* Not found an appropriate random accessible sample */
        }
        return 0;   /* error of decoding */
    } while( 1 );
    hp->last_video_sample_number = sample_number;
    DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_OK, "src_linesize[0] = %d, src_linesize[1] = %d, src_linesize[2] = %d, src_linesize[3] = %d",
                                     picture.linesize[0], picture.linesize[1], picture.linesize[2], picture.linesize[3] );
    return hp->convert_colorspace( hp->video_ctx, hp->sws_ctx, &picture, buf );
}

static int read_audio( lsmash_handler_t *h, int start, int wanted_length, void *buf )
{
    DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_OK, "start = %d, wanted_length = %d", start, wanted_length );
    libavsmash_handler_t *hp = (libavsmash_handler_t *)h->reader->private_stuff;
    uint32_t sample_number = hp->last_audio_sample_number;
    uint32_t data_offset;
    int      copy_size;
    int      output_length = 0;
    int      block_align = h->audio_format.nBlockAlign;
    if( start == hp->next_audio_pcm_sample_number )
    {
        if( hp->last_remainder_size )
        {
            copy_size = min( hp->last_remainder_size, wanted_length * block_align );
            memcpy( buf, hp->audio_output_buffer, copy_size );
            buf                     += copy_size;
            hp->last_remainder_size -= copy_size;
            int copied_length = copy_size / block_align;
            output_length += copied_length;
            wanted_length -= copied_length;
            if( wanted_length <= 0 )
                goto audio_out;
        }
        data_offset = 0;
    }
    else
    {
        /* Seek audio stream. */
        avcodec_flush_buffers( hp->audio_ctx );
        hp->last_remainder_size = 0;
        sample_number = hp->audio_frame_count;
        uint64_t dts;
        do
        {
            if( lsmash_get_dts_from_media_timeline( hp->root, hp->audio_track_ID, sample_number--, &dts ) )
                return 0;
            if( start >= dts )
                break;
        } while( 1 );
        data_offset = (start - dts) * block_align;
    }
    do
    {
        copy_size = 0;
        lsmash_sample_t *sample = lsmash_get_sample_from_media_timeline( hp->root, hp->audio_track_ID, ++sample_number );
        if( !sample )
            goto audio_out;
        AVPacket pkt;
        av_init_packet( &pkt );
        pkt.size = sample->length;
        pkt.data = hp->audio_input_buffer;
        memset( pkt.data, 0, hp->audio_input_buffer_size );
        memcpy( pkt.data, sample->data, sample->length );
        lsmash_delete_sample( sample );
        while( pkt.size > 0 )
        {
            int output_buffer_size = AVCODEC_MAX_AUDIO_FRAME_SIZE;
            int wasted_data_length = avcodec_decode_audio3( hp->audio_ctx, (int16_t *)hp->audio_output_buffer, &output_buffer_size, &pkt );
            if( wasted_data_length < 0 )
            {
                MessageBox( HWND_DESKTOP, "Failed to decode a audio frame.", "lsmashinput", MB_ICONERROR | MB_OK );
                goto audio_out;
            }
            pkt.size -= wasted_data_length;
            pkt.data += wasted_data_length;
            if( output_buffer_size > data_offset )
            {
                copy_size = min( output_buffer_size - data_offset, wanted_length * block_align );
                memcpy( buf, hp->audio_output_buffer + data_offset, copy_size );
                int copied_length = copy_size / block_align;
                output_length += copied_length;
                wanted_length -= copied_length;
                buf           += copy_size;
                data_offset = 0;
            }
            else
            {
                copy_size = 0;
                data_offset -= output_buffer_size;
            }
            DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_OK, "sample_number = %d, decoded_length = %d", sample_number, output_buffer_size / h->audio_format.nBlockAlign );
            if( wanted_length <= 0 )
            {
                hp->last_remainder_size = output_buffer_size - copy_size;
                goto audio_out;
            }
        }
    } while( 1 );
audio_out:
    DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_OK, "output_length = %d, remainder = %d", output_length, hp->last_remainder_size );
    if( hp->last_remainder_size && copy_size != 0 )
        /* Move unused decoded data to the head of output buffer for the next access. */
        memmove( hp->audio_output_buffer, hp->audio_output_buffer + copy_size, hp->last_remainder_size );
    hp->next_audio_pcm_sample_number = start + output_length;
    hp->last_audio_sample_number = sample_number;
    return output_length;
}

static BOOL is_keyframe( lsmash_handler_t *h, int sample_number )
{
    libavsmash_handler_t *hp = (libavsmash_handler_t *)h->reader->private_stuff;
    return hp->keyframe_list[sample_number + 1] ? TRUE : FALSE;
}

lsmash_reader_t libavsmash_reader =
{
    0,
    NULL,
    open_file,
    read_video,
    read_audio,
    is_keyframe,
    cleanup
};
