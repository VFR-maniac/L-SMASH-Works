/*****************************************************************************
 * lsmashinput.c:
 *****************************************************************************
 * Copyright (C) 2011 L-SMASH project
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

#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <windows.h>

/* L-SMASH */
#include <lsmash.h>                 /* Demuxer */

/* Libav
 * The binary file will be LGPLed or GPLed. */
#include <libavformat/avformat.h>   /* Codec specific info importer */
#include <libavcodec/avcodec.h>     /* Decoder */
#include <libswscale/swscale.h>     /* Colorspace converter */
#ifdef DEBUG
#include <libavutil/pixdesc.h>
#endif

#include "input.h"

#ifdef DEBUG
#define DEBUG_MESSAGE_BOX_DESKTOP( uType, ... ) \
do \
{ \
    char temp[256]; \
    wsprintf( temp, __VA_ARGS__ ); \
    MessageBox( HWND_DESKTOP, temp, "lsmashinput", uType ); \
} while( 0 )
#else
#define DEBUG_MESSAGE_BOX_DESKTOP( uType, ... )
#endif

#define MPEG4_FILE_EXT  "*.mp4;*.m4v;*.m4a;*.mov;*.qt;*.3gp;*.3g2;*.f4v"

INPUT_PLUGIN_TABLE input_plugin_table =
{
    INPUT_PLUGIN_FLAG_VIDEO,                                    /* INPUT_PLUGIN_FLAG_VIDEO : support images
                                                                 * INPUT_PLUGIN_FLAG_AUDIO : support audio */
    "Libav-SMASH File Reader",                                  /* Name of plugin */
    "MPEG-4 File (" MPEG4_FILE_EXT ")\0" MPEG4_FILE_EXT "\0",   /* Filter for Input file */
    "Libav-SMASH File Reader",                                  /* Information of plugin */
    func_init,                                                  /* Pointer to function called when opening DLL (If NULL, won't be called.) */
    NULL,                                                       /* Pointer to function called when closing DLL (If NULL, won't be called.) */
    func_open,                                                  /* Pointer to function to open input file */
    func_close,                                                 /* Pointer to function to close input file */
    func_info_get,                                              /* Pointer to function to get information of input file */
    func_read_video,                                            /* Pointer to function to read image data */
    func_read_audio,                                            /* Pointer to function to read audio data */
    func_is_keyframe,                                           /* Pointer to function to check if it is a keyframe or not (If NULL, all is keyframe.) */
    NULL,                                                       /* Pointer to function called when configuration dialog is required */
};

EXTERN_C INPUT_PLUGIN_TABLE __declspec(dllexport) * __stdcall GetInputPluginTable( void )
{
    return &input_plugin_table;
}

typedef enum
{
    DECODE_REQUIRE_INITIAL = 0,
    DECODE_INITIALIZING    = 1,
    DECODE_INITIALIZED     = 2
} decode_status_t;

typedef struct
{
    /* L-SMASH's stuff */
    lsmash_root_t     *root;
    uint32_t           track_ID;
    /* Libav's stuff */
    AVCodecContext    *ctx;
    AVFormatContext   *format_ctx;
    struct SwsContext *sws_ctx;
    /* Others */
    uint8_t           *input_buffer;
    BITMAPINFOHEADER   video_format;
    int                framerate_num;
    int                framerate_den;
    uint32_t           sample_count;
    uint32_t           last_sample_number;
    uint32_t           delay_count;
    decode_status_t    decode_status;
} lsmash_handler_t;

BOOL func_init( void )
{
    av_register_all();
    avcodec_register_all();
    return TRUE;
}

static inline void cleanup_handler( lsmash_handler_t *hp )
{
    if( !hp )
        return;
    lsmash_destroy_root( hp->root );
    if( hp->ctx )
        avcodec_close( hp->ctx );
    if( hp->format_ctx )
        av_close_input_file( hp->format_ctx );
    if( hp->sws_ctx )
        sws_freeContext( hp->sws_ctx );
    if( hp->input_buffer )
        av_free( hp->input_buffer );
    free( hp );
}

static INPUT_HANDLE error_out( lsmash_handler_t *hp )
{
    cleanup_handler( hp );
    return NULL;
}

static int decode_video_sample( lsmash_handler_t *hp, AVFrame *picture, int *got_picture, uint32_t sample_number )
{
    lsmash_sample_t *sample = lsmash_get_sample_from_media_timeline( hp->root, hp->track_ID, sample_number );
    if( !sample )
        return 1;
    AVPacket pkt;
    av_init_packet( &pkt );
    pkt.flags = sample->prop.random_access_type == ISOM_SAMPLE_RANDOM_ACCESS_TYPE_NONE ? 0 : AV_PKT_FLAG_KEY;
    /* Note: the input buffer for avcodec_decode_video2 must be FF_INPUT_BUFFER_PADDING_SIZE larger than the actual read bytes. */
    pkt.size  = sample->length;
    pkt.data  = hp->input_buffer;
    memcpy( pkt.data, sample->data, sample->length );
    lsmash_delete_sample( sample );
    if( avcodec_decode_video2( hp->ctx, picture, got_picture, &pkt ) < 0 )
    {
        MessageBox( HWND_DESKTOP, "Failed to decode a video frame.", "lsmashinput", MB_ICONERROR | MB_OK );
        return -1;
    }
    return 0;
}

static int get_picture( lsmash_handler_t *hp, AVFrame *picture, uint32_t current, uint32_t goal )
{
    if( hp->decode_status == DECODE_INITIALIZING )
    {
        if( hp->delay_count > hp->ctx->has_b_frames )
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
        DEBUG_MESSAGE_BOX_DESKTOP( MB_OK, "current frame = %d, decoded frame = %d, delay_count = %d",
                                   goal, current - 1, hp->delay_count );
        if( hp->delay_count > hp->ctx->has_b_frames && hp->decode_status == DECODE_INITIALIZED )
            break;
        if( got_picture && current > goal )
            break;
    } while( 1 );
    /* Flush the last frames. */
    if( current > hp->sample_count && !got_picture && hp->ctx->has_b_frames )
    {
        AVPacket pkt;
        av_init_packet( &pkt );
        pkt.data = NULL;
        pkt.size = 0;
        if( avcodec_decode_video2( hp->ctx, picture, &got_picture, &pkt ) < 0 )
        {
            MessageBox( HWND_DESKTOP, "Failed to decode a video frame.", "lsmashinput", MB_ICONERROR | MB_OK );
            return -1;
        }
    }
    if( hp->decode_status == DECODE_REQUIRE_INITIAL )
        hp->decode_status = DECODE_INITIALIZING;
    return got_picture ? 0 : -1;
}

INPUT_HANDLE func_open( LPSTR file )
{
    lsmash_handler_t *hp = (lsmash_handler_t *)malloc( sizeof(lsmash_handler_t) );
    if( !hp )
        return NULL;
    memset( hp, 0, sizeof(lsmash_handler_t) );
    /* L-SMASH */
    hp->root = lsmash_open_movie( file, LSMASH_FILE_MODE_READ );
    if( !hp->root )
    {
        free( hp );
        return NULL;
    }
    lsmash_movie_parameters_t movie_param;
    lsmash_initialize_movie_parameters( &movie_param );
    lsmash_get_movie_parameters( hp->root, &movie_param );
    uint32_t i;
    for( i = 1; i <= movie_param.number_of_tracks; i++ )
    {
        hp->track_ID = lsmash_get_track_ID( hp->root, i );
        if( hp->track_ID == 0 )
            break;
        lsmash_media_parameters_t media_param;
        lsmash_initialize_media_parameters( &media_param );
        if( lsmash_get_media_parameters( hp->root, hp->track_ID, &media_param ) )
        {
            DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to get media parameters." );
            return error_out( hp );
        }
        if( media_param.handler_type == ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK )
            break;
    }
    if( i > movie_param.number_of_tracks )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "No video tracks found." );
        return error_out( hp );
    }
    if( lsmash_construct_timeline( hp->root, hp->track_ID ) )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to get construct timeline." );
        return error_out( hp );
    }
    lsmash_discard_boxes( hp->root );
    hp->sample_count = lsmash_get_sample_count_in_media_timeline( hp->root, hp->track_ID );
    /* libavformat */
    if( avformat_open_input( &hp->format_ctx, file, NULL, NULL ) )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to avformat_open_input." );
        return error_out( hp );
    }
    if( avformat_find_stream_info( hp->format_ctx, NULL ) < 0 )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to avformat_find_stream_info." );
        return error_out( hp );
    }
    for( i = 0; i < hp->format_ctx->nb_streams && hp->format_ctx->streams[i]->codec->codec_type != AVMEDIA_TYPE_VIDEO; i++ );
    if( i == hp->format_ctx->nb_streams )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to find video stream by libavformat." );
        return error_out( hp );
    }
    AVStream *stream = hp->format_ctx->streams[i];
    hp->framerate_num = stream->r_frame_rate.num;
    hp->framerate_den = stream->r_frame_rate.den;
    /* libavcodec */
    hp->ctx = stream->codec;
    uint32_t codec_id = hp->ctx->codec_id;
    AVCodec *codec = avcodec_find_decoder( codec_id );
    if( !codec )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to find %s decoder.", codec->name );
        return error_out( hp );
    }
    if( avcodec_open2( hp->ctx, codec, NULL ) < 0 )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to avcodec_open2." );
        return error_out( hp );
    }
    /* Threads */
    DWORD ProcessAffinityMask, SystemAffinityMask;
    GetProcessAffinityMask( GetCurrentProcess(), &ProcessAffinityMask, &SystemAffinityMask );
    hp->ctx->thread_count = ProcessAffinityMask > SystemAffinityMask ? SystemAffinityMask : ProcessAffinityMask;
    /* Preparation for decoding samples */
    uint32_t max_sample_size = lsmash_get_max_sample_size_in_media_timeline( hp->root, hp->track_ID );
    if( max_sample_size == 0 )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "No valid sample found." );
        return error_out( hp );
    }
    hp->input_buffer = av_mallocz( max_sample_size + FF_INPUT_BUFFER_PADDING_SIZE );
    if( !hp->input_buffer )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to allocate memory to the input buffer." );
        return error_out( hp );
    }
    AVFrame picture;
    if( get_picture( hp, &picture, 1, 1 ) )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to get the first sample." );
        return error_out( hp );
    }
    hp->last_sample_number = 1;
    /* BITMAPINFOHEADER */
    hp->video_format.biSize        = sizeof( BITMAPINFOHEADER );
    hp->video_format.biWidth       = hp->ctx->width;
    hp->video_format.biHeight      = hp->ctx->height;
    hp->video_format.biPlanes      = 1;
    hp->video_format.biBitCount    = 16;   /* packed YUV 4:2:2 */
    hp->video_format.biCompression = MAKEFOURCC( 'Y', 'U', 'Y', '2' );
    hp->video_format.biSizeImage   = picture.linesize[0] * hp->ctx->height;
    /* swscale */
    hp->sws_ctx = sws_getCachedContext( NULL,
                                        hp->ctx->width, hp->ctx->height, hp->ctx->pix_fmt,
                                        hp->ctx->width, hp->ctx->height, PIX_FMT_YUYV422,   /* packed YUV 4:2:2 */
                                        SWS_POINT, NULL, NULL, NULL );
    if( !hp->sws_ctx )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to get swscale context." );
        return error_out( hp );
    }
    DEBUG_MESSAGE_BOX_DESKTOP( MB_OK, "src_pix_fmt = %s, dst_pix_fmt = %s",
                               av_pix_fmt_descriptors[hp->ctx->pix_fmt].name, av_pix_fmt_descriptors[PIX_FMT_YUYV422].name );
    return hp;
}

BOOL func_close( INPUT_HANDLE ih )
{
    cleanup_handler( (lsmash_handler_t *)ih );
    return TRUE;
}

BOOL func_info_get( INPUT_HANDLE ih, INPUT_INFO *iip )
{
    lsmash_handler_t *hp = (lsmash_handler_t *)ih;
    memset( iip, 0, sizeof(INPUT_INFO) );
    iip->flag        = INPUT_INFO_FLAG_VIDEO;
    iip->rate        = hp->framerate_num;
    iip->scale       = hp->framerate_den;
    iip->n           = hp->sample_count;
    iip->format      = &hp->video_format;
    iip->format_size = hp->video_format.biSize;
    iip->handler     = 0;
    return TRUE;
}

static uint32_t seek_media( lsmash_handler_t *hp, AVFrame *picture, uint32_t sample_number, uint32_t *rap_number )
{
    /* Prepare to decode from random accessible sample. */
    avcodec_flush_buffers( hp->ctx );
    hp->delay_count   = 0;
    hp->decode_status = DECODE_REQUIRE_INITIAL;
    uint32_t distance;
    if( lsmash_get_closest_random_accessible_point_detail_from_media_timeline( hp->root, hp->track_ID, sample_number, rap_number, NULL, NULL, &distance ) )
        *rap_number = 1;
    uint32_t start_number = *rap_number;
    if( distance && *rap_number > distance )
        *rap_number -= distance;
    hp->ctx->skip_frame = AVDISCARD_NONREF;
    int dummy;
    for( uint32_t i = *rap_number; i < sample_number + hp->ctx->has_b_frames; i++ )
    {
        avcodec_get_frame_defaults( picture );
        decode_video_sample( hp, picture, &dummy, i );
    }
    hp->ctx->skip_frame = AVDISCARD_DEFAULT;
    hp->delay_count = hp->ctx->has_b_frames;
    DEBUG_MESSAGE_BOX_DESKTOP( MB_OK, "rap_number = %d, distance = %d", *rap_number, distance );
    return start_number;
}

int func_read_video( INPUT_HANDLE ih, int sample_number, void *buf )
{
    lsmash_handler_t *hp = (lsmash_handler_t *)ih;
    ++sample_number;        /* For L-SMASH, sample_number is 1-origin. */
    AVFrame picture;        /* Decoded video data will be stored here. */
    uint32_t start_number;  /* number of sample, for normal decoding, where decoding starts excluding decoding delay */
    uint32_t rap_number;    /* number of sample, for seeking, where decoding starts excluding decoding delay */
    if( sample_number == hp->last_sample_number + 1 )
        start_number = sample_number;
    else
        /* Require starting to decode from random accessible sample. */
        start_number = seek_media( hp, &picture, sample_number, &rap_number );
    do
    {
        int error = get_picture( hp, &picture, start_number + hp->delay_count, sample_number + hp->delay_count );
        if( error == 0 )
            break;
        else if( error == -1 )
        {
            /* No error of decoding, but couldn't get a picture.
             * Retry to decode from more past random accessible sample. */
            start_number = seek_media( hp, &picture, rap_number - 1, &rap_number );
            if( start_number == 1 )
                return 0;   /* Not found an appropriate random accessible sample */
        }
        return 0;   /* error of decoding */
    } while( 1 );
    /* Colorspace conversion */
    DEBUG_MESSAGE_BOX_DESKTOP( MB_OK, "src_linesize[0] = %d, src_linesize[1] = %d, src_linesize[2] = %d, src_linesize[3] = %d",
                               picture.linesize[0], picture.linesize[1], picture.linesize[2], picture.linesize[3] );
    const int dst_linesize[4] = { picture.linesize[0] + picture.linesize[1] + picture.linesize[2] + picture.linesize[3], 0, 0, 0 };
    uint8_t  *dst_data    [4] = { NULL, NULL, NULL, NULL };
    dst_data[0] = av_mallocz( dst_linesize[0] * hp->ctx->height );
    if( !dst_data[0] )
    {
        MessageBox( HWND_DESKTOP, "Failed to av_malloc.", "lsmashinput", MB_ICONERROR | MB_OK );
        return 0;
    }
    int output_height = sws_scale( hp->sws_ctx, (const uint8_t* const*)picture.data, picture.linesize, 0, hp->ctx->height, dst_data, dst_linesize );
    int buf_linesize  = hp->ctx->width * 2;
    int output_size   = buf_linesize * output_height;
    DEBUG_MESSAGE_BOX_DESKTOP( MB_OK, "dst linesize = %d, output_height = %d, output_size = %d",
                               dst_linesize[0], output_height, output_size );
    uint8_t *dst = dst_data[0];
    while( output_height-- )
    {
        memcpy( buf, dst, buf_linesize );
        buf += buf_linesize;
        dst += dst_linesize[0];
    }
    av_free( dst_data[0] );
    hp->last_sample_number = sample_number;
    return output_size;
}

int func_read_audio( INPUT_HANDLE ih, int start, int length, void *buf )
{
    return 0;
}

BOOL func_is_keyframe( INPUT_HANDLE ih, int sample_number )
{
    lsmash_handler_t *hp = (lsmash_handler_t *)ih;
    uint32_t rap_number;
    if( lsmash_get_closest_random_accessible_point_from_media_timeline( hp->root, hp->track_ID, ++sample_number, &rap_number ) )
        return FALSE;
    return sample_number == rap_number ? TRUE : FALSE;
}
