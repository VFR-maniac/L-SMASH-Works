/*****************************************************************************
 * lsmashinput.c
 *****************************************************************************
 * Copyright (C) 2011 Libav-SMASH project
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
#ifdef DEBUG_VIDEO
#include <libavutil/pixdesc.h>
#endif

#include "input.h"

/* Macros for debug */
#if defined( DEBUG_VIDEO ) || defined( DEBUG_AUDIO )
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

#ifdef DEBUG_VIDEO
#define DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( uType, ... ) DEBUG_MESSAGE_BOX_DESKTOP( uType, __VA_ARGS__ )
#else
#define DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( uType, ... )
#endif

#ifdef DEBUG_AUDIO
#define DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( uType, ... ) DEBUG_MESSAGE_BOX_DESKTOP( uType, __VA_ARGS__ )
#else
#define DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( uType, ... )
#endif

#define MPEG4_FILE_EXT  "*.mp4;*.m4v;*.m4a;*.mov;*.qt;*.3gp;*.3g2;*.f4v"

INPUT_PLUGIN_TABLE input_plugin_table =
{
    INPUT_PLUGIN_FLAG_VIDEO | INPUT_INFO_FLAG_AUDIO,            /* INPUT_PLUGIN_FLAG_VIDEO : support images
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

typedef struct lsmash_handler_tag
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
    BITMAPINFOHEADER   video_format;
    int                full_range;
    int                pixel_size;
    int                framerate_num;
    int                framerate_den;
    uint32_t           video_sample_count;
    uint32_t           last_video_sample_number;
    uint32_t           delay_count;
    decode_status_t    decode_status;
    int (*convert_colorspace)( struct lsmash_handler_tag *, AVFrame *, uint8_t * );
    /* Audio stuff */
    uint8_t           *audio_input_buffer;
    uint32_t           audio_input_buffer_size;
    uint8_t           *audio_output_buffer;
    WAVEFORMATEX       audio_format;
    uint32_t           audio_frame_count;
    uint32_t           audio_pcm_sample_count;
    uint32_t           next_audio_pcm_sample_number;
    uint32_t           last_audio_sample_number;
    uint32_t           last_remainder_size;
} lsmash_handler_t;

BOOL func_init( void )
{
    av_register_all();
    avcodec_register_all();
    return TRUE;
}

static uint32_t open_file( lsmash_handler_t *hp, char *file_name )
{
    /* L-SMASH */
    hp->root = lsmash_open_movie( file_name, LSMASH_FILE_MODE_READ );
    if( !hp->root )
        return 0;
    lsmash_movie_parameters_t movie_param;
    lsmash_initialize_movie_parameters( &movie_param );
    lsmash_get_movie_parameters( hp->root, &movie_param );
    /* libavformat */
    if( avformat_open_input( &hp->format_ctx, file_name, NULL, NULL ) )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to avformat_open_input." );
        return 0;
    }
    if( avformat_find_stream_info( hp->format_ctx, NULL ) < 0 )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to avformat_find_stream_info." );
        return 0;
    }
    return movie_param.number_of_tracks;
}

static int get_first_track_of_type( lsmash_handler_t *hp, uint32_t number_of_tracks, uint32_t type )
{
    /* L-SMASH */
    uint32_t track_ID;
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
        hp->video_sample_count = lsmash_get_sample_count_in_media_timeline( hp->root, track_ID );
    }
    else
    {
        hp->audio_track_ID = track_ID;
        hp->audio_frame_count = lsmash_get_sample_count_in_media_timeline( hp->root, track_ID );
        hp->audio_pcm_sample_count = lsmash_get_media_duration( hp->root, track_ID );
    }
    /* libavformat */
    type = type == ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
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
    {
        hp->video_ctx = stream->codec;
        hp->framerate_num = stream->r_frame_rate.num;
        hp->framerate_den = stream->r_frame_rate.den;
    }
    else
        hp->audio_ctx = stream->codec;
    AVCodec *codec = avcodec_find_decoder( ctx->codec_id );
    if( !codec )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to find %s decoder.", codec->name );
        return -1;
    }
    if( avcodec_open2( ctx, codec, NULL ) < 0 )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to avcodec_open2." );
        return -1;
    }
    return 0;
}

static int decode_video_sample( lsmash_handler_t *hp, AVFrame *picture, int *got_picture, uint32_t sample_number )
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

static int get_picture( lsmash_handler_t *hp, AVFrame *picture, uint32_t current, uint32_t goal )
{
    if( hp->decode_status == DECODE_INITIALIZING )
    {
        if( hp->delay_count > hp->video_ctx->has_b_frames )
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
        if( hp->delay_count > hp->video_ctx->has_b_frames && hp->decode_status == DECODE_INITIALIZED )
            break;
        if( got_picture && current > goal )
            break;
    } while( 1 );
    /* Flush the last frames. */
    if( current > hp->video_sample_count && !got_picture && hp->video_ctx->has_b_frames )
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

static int to_yuv16le_to_yc48( lsmash_handler_t *hp, AVFrame *picture, uint8_t *buf )
{
    const int dst_linesize[4] =
        {
            picture->linesize[0] << (hp->video_ctx->pix_fmt == PIX_FMT_YUV444P || hp->video_ctx->pix_fmt == PIX_FMT_YUVJ444P),
            picture->linesize[0] << (hp->video_ctx->pix_fmt == PIX_FMT_YUV444P || hp->video_ctx->pix_fmt == PIX_FMT_YUVJ444P),
            picture->linesize[0] << (hp->video_ctx->pix_fmt == PIX_FMT_YUV444P || hp->video_ctx->pix_fmt == PIX_FMT_YUVJ444P),
            0
        };
    uint8_t *dst_data[4];
    for( int i = 0; i < 3; i++ )
    {
        dst_data[i] = av_mallocz( dst_linesize[0] * hp->video_ctx->height );
        if( !dst_data[i] )
        {
            MessageBox( HWND_DESKTOP, "Failed to av_malloc.", "lsmashinput", MB_ICONERROR | MB_OK );
            return 0;
        }
    }
    dst_data[3] = NULL;
    int output_height = sws_scale( hp->sws_ctx, (const uint8_t* const*)picture->data, picture->linesize, 0, hp->video_ctx->height, dst_data, dst_linesize );
    int buf_linesize  = hp->video_ctx->width * hp->pixel_size;
    int output_size   = buf_linesize * output_height;
    DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_OK, "dst linesize = %d, output_height = %d, output_size = %d",
                                     dst_linesize[0], output_height, output_size );
    /* Convert planar YUV 4:4:4 48bpp little-endian into YC48. */
    uint32_t offset = 0;
    while( output_height-- )
    {
        uint8_t *p_dst[3] = { dst_data[0] + offset, dst_data[1] + offset, dst_data[2] + offset };
        for( int i = 0; i < buf_linesize; i += hp->pixel_size )
        {
            static const uint32_t y_coef   [2] = {  1197,   4770 };
            static const uint32_t y_shift  [2] = {    14,     16 };
            static const uint32_t uv_coef  [2] = {  4682,   4662 };
            static const uint32_t uv_offset[2] = { 32768, 589824 };
            uint16_t y  = (((int32_t)((p_dst[0][0] | (p_dst[0][1] << 8)) * y_coef[hp->full_range])) >> y_shift[hp->full_range]) - 299;
            uint16_t cb = ((int32_t)(((p_dst[1][0] | (p_dst[1][1] << 8)) - 32768) * uv_coef[hp->full_range] + uv_offset[hp->full_range])) >> 16;
            uint16_t cr = ((int32_t)(((p_dst[2][0] | (p_dst[2][1] << 8)) - 32768) * uv_coef[hp->full_range] + uv_offset[hp->full_range])) >> 16;
            p_dst[0] += 2;
            p_dst[1] += 2;
            p_dst[2] += 2;
            buf[0] = y;
            buf[1] = y >> 8;
            buf[2] = cb;
            buf[3] = cb >> 8;
            buf[4] = cr;
            buf[5] = cr >> 8;
            buf += hp->pixel_size;
        }
        offset += dst_linesize[0];
    }
    av_free( dst_data[0] );
    av_free( dst_data[1] );
    av_free( dst_data[2] );
    return output_size;
}

static int to_rgb24( lsmash_handler_t *hp, AVFrame *picture, uint8_t *buf )
{
    const int dst_linesize[4] = { picture->linesize[0] + picture->linesize[1] + picture->linesize[2] + picture->linesize[3], 0, 0, 0 };
    uint8_t  *dst_data    [4] = { NULL, NULL, NULL, NULL };
    dst_data[0] = av_mallocz( dst_linesize[0] * hp->video_ctx->height );
    if( !dst_data[0] )
    {
        MessageBox( HWND_DESKTOP, "Failed to av_malloc.", "lsmashinput", MB_ICONERROR | MB_OK );
        return 0;
    }
    int ouptut_height = sws_scale( hp->sws_ctx, (const uint8_t* const*)picture->data, picture->linesize, 0, hp->video_ctx->height, dst_data, dst_linesize );
    int buf_linesize  = hp->video_ctx->width * hp->pixel_size;
    int output_size   = buf_linesize * ouptut_height;
    DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_OK, "dst linesize = %d, ouptut_height = %d, output_size = %d",
                                     dst_linesize[0], ouptut_height, output_size );
    uint8_t *dst = dst_data[0] + dst_linesize[0] * ouptut_height;
    while( ouptut_height-- )
    {
        dst -= dst_linesize[0];
        memcpy( buf, dst, buf_linesize );
        buf += buf_linesize;
    }
    av_free( dst_data[0] );
    return output_size;
}

static int to_yuy2( lsmash_handler_t *hp, AVFrame *picture, uint8_t *buf )
{
    const int dst_linesize[4] = { picture->linesize[0] + picture->linesize[1] + picture->linesize[2] + picture->linesize[3], 0, 0, 0 };
    uint8_t  *dst_data    [4] = { NULL, NULL, NULL, NULL };
    dst_data[0] = av_mallocz( dst_linesize[0] * hp->video_ctx->height );
    if( !dst_data[0] )
    {
        MessageBox( HWND_DESKTOP, "Failed to av_malloc.", "lsmashinput", MB_ICONERROR | MB_OK );
        return 0;
    }
    int ouptut_height = sws_scale( hp->sws_ctx, (const uint8_t* const*)picture->data, picture->linesize, 0, hp->video_ctx->height, dst_data, dst_linesize );
    int buf_linesize  = hp->video_ctx->width * hp->pixel_size;
    int output_size   = buf_linesize * ouptut_height;
    DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_OK, "dst linesize = %d, ouptut_height = %d, output_size = %d",
                                     dst_linesize[0], ouptut_height, output_size );
    uint8_t *dst = dst_data[0];
    while( ouptut_height-- )
    {
        memcpy( buf, dst, buf_linesize );
        buf += buf_linesize;
        dst += dst_linesize[0];
    }
    av_free( dst_data[0] );
    return output_size;
}

static int prepare_video_decoding( lsmash_handler_t *hp, int threads )
{
    if( !hp->video_ctx )
        return 0;
    hp->video_ctx->thread_count = threads;
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
    AVFrame picture;
    if( get_picture( hp, &picture, 1, 1 ) )
    {
        DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to get the first video sample." );
        return -1;
    }
    hp->last_video_sample_number = 1;
    /* swscale */
    enum PixelFormat out_pix_fmt;
    uint32_t compression;
    switch( hp->video_ctx->pix_fmt )
    {
        case PIX_FMT_YUV444P :
        case PIX_FMT_YUVJ444P :
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
            hp->pixel_size         = 6;                     /* YC48 */
            out_pix_fmt            = PIX_FMT_YUV444P16LE;   /* planar YUV 4:4:4, 48bpp little-endian -> YC48 */
            compression            = MAKEFOURCC( 'Y', 'C', '4', '8' );
            hp->full_range         = hp->video_ctx->color_range == AVCOL_RANGE_JPEG;
            break;
        case PIX_FMT_RGB24 :
        case PIX_FMT_BGR24 :
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
            hp->convert_colorspace = to_rgb24;
            hp->pixel_size         = 3;                     /* BGR 8:8:8 */
            out_pix_fmt            = PIX_FMT_BGR24;         /* packed RGB 8:8:8, 24bpp, BGRBGR... */
            compression            = 0;
            break;
        default :
            hp->convert_colorspace = to_yuy2;
            hp->pixel_size         = 2;                     /* YUY2 */
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
    hp->video_format.biSize        = sizeof( BITMAPINFOHEADER );
    hp->video_format.biWidth       = hp->video_ctx->width;
    hp->video_format.biHeight      = hp->video_ctx->height;
    hp->video_format.biPlanes      = 1;
    hp->video_format.biBitCount    = hp->pixel_size * 8;
    hp->video_format.biCompression = compression;
    hp->video_format.biSizeImage   = picture.linesize[0] * hp->pixel_size * hp->video_ctx->height;
    return 0;
}

static int prepare_audio_decoding( lsmash_handler_t *hp, int threads )
{
    if( !hp->audio_ctx )
        return 0;
    hp->audio_ctx->thread_count = threads;
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
    hp->audio_format.nChannels       = hp->audio_ctx->channels;
    hp->audio_format.nSamplesPerSec  = hp->audio_ctx->sample_rate;
    hp->audio_format.wBitsPerSample  = hp->audio_ctx->bits_per_raw_sample;
    hp->audio_format.nBlockAlign     = hp->audio_format.nChannels * av_get_bytes_per_sample( hp->audio_ctx->sample_fmt );
    hp->audio_format.nAvgBytesPerSec = hp->audio_format.nSamplesPerSec * hp->audio_format.nBlockAlign;
    hp->audio_format.wFormatTag      = WAVE_FORMAT_PCM;     /* AviUtl doesn't support WAVE_FORMAT_EXTENSIBLE even if the input audio is 24bit PCM. */
    hp->audio_format.cbSize          = 0;
    DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_OK, "channels = %d, sampling_rate = %d, bits_per_sample = %d, block_align = %d, avg_bps = %d",
                                     hp->audio_format.nChannels, hp->audio_format.nSamplesPerSec,
                                     hp->audio_format.wBitsPerSample, hp->audio_format.nBlockAlign, hp->audio_format.nAvgBytesPerSec );
    return 0;
}

static inline void cleanup_handler( lsmash_handler_t *hp )
{
    if( !hp )
        return;
    lsmash_destroy_root( hp->root );
    if( hp->video_ctx )
        avcodec_close( hp->video_ctx );
    if( hp->audio_ctx )
        avcodec_close( hp->audio_ctx );
    if( hp->format_ctx )
        av_close_input_file( hp->format_ctx );
    if( hp->sws_ctx )
        sws_freeContext( hp->sws_ctx );
    if( hp->video_input_buffer )
        av_free( hp->video_input_buffer );
    if( hp->audio_input_buffer )
        av_free( hp->audio_input_buffer );
    if( hp->audio_output_buffer )
        av_free( hp->audio_output_buffer );
    free( hp );
}

static INPUT_HANDLE error_out( lsmash_handler_t *hp )
{
    cleanup_handler( hp );
    return NULL;
}

INPUT_HANDLE func_open( LPSTR file )
{
    lsmash_handler_t *hp = (lsmash_handler_t *)malloc( sizeof(lsmash_handler_t) );
    if( !hp )
        return NULL;
    memset( hp, 0, sizeof(lsmash_handler_t) );
    /* Open file. */
    uint32_t number_of_tracks = open_file( hp, file );
    if( number_of_tracks == 0 )
        return error_out( hp );
    /* Get video track. */
    if( get_first_track_of_type( hp, number_of_tracks, ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK ) )
        return error_out( hp );
    /* Get audio track. If absent, ignore audio track. */
    if( get_first_track_of_type( hp, number_of_tracks, ISOM_MEDIA_HANDLER_TYPE_AUDIO_TRACK ) )
    {
        lsmash_destruct_timeline( hp->root, hp->audio_track_ID );
        if( hp->audio_ctx )
        {
            avcodec_close( hp->audio_ctx );
            hp->audio_ctx = NULL;
        }
        if( hp->audio_input_buffer )
            av_freep( &hp->audio_input_buffer );
        if( hp->audio_output_buffer )
            av_freep( &hp->audio_output_buffer );
    }
    lsmash_discard_boxes( hp->root );
    /* Prepare decoding. */
    DWORD ProcessAffinityMask, SystemAffinityMask;
    GetProcessAffinityMask( GetCurrentProcess(), &ProcessAffinityMask, &SystemAffinityMask );
    int threads = ProcessAffinityMask > SystemAffinityMask ? SystemAffinityMask : ProcessAffinityMask;
    if( prepare_video_decoding( hp, threads )
     || prepare_audio_decoding( hp, threads ) )
        return error_out( hp );
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
    if( hp->video_ctx )
    {
        iip->flag             |= INPUT_INFO_FLAG_VIDEO;
        iip->rate              = hp->framerate_num;
        iip->scale             = hp->framerate_den;
        iip->n                 = hp->video_sample_count;
        iip->format            = &hp->video_format;
        iip->format_size       = hp->video_format.biSize;
        iip->handler           = 0;
    }
    if( hp->audio_ctx )
    {
        iip->flag             |= INPUT_INFO_FLAG_AUDIO;
        iip->audio_n           = hp->audio_pcm_sample_count;
        iip->audio_format      = &hp->audio_format;
        iip->audio_format_size = sizeof( WAVEFORMATEX );
    }
    return TRUE;
}

static uint32_t seek_video( lsmash_handler_t *hp, AVFrame *picture, uint32_t sample_number, uint32_t *rap_number )
{
    /* Prepare to decode from random accessible sample. */
    avcodec_flush_buffers( hp->video_ctx );
    hp->delay_count   = 0;
    hp->decode_status = DECODE_REQUIRE_INITIAL;
    uint32_t distance;
    if( lsmash_get_closest_random_accessible_point_detail_from_media_timeline( hp->root, hp->video_track_ID, sample_number, rap_number, NULL, NULL, &distance ) )
        *rap_number = 1;
    if( distance && *rap_number > distance )
        *rap_number -= distance;
    hp->video_ctx->skip_frame = AVDISCARD_NONREF;
    int dummy;
    uint32_t i;
    for( i = *rap_number; i < sample_number + hp->video_ctx->has_b_frames; i++ )
    {
        avcodec_get_frame_defaults( picture );
        if( decode_video_sample( hp, picture, &dummy, i ) == 1 )
            break;  /* Sample doesn't exist. */
    }
    hp->video_ctx->skip_frame = AVDISCARD_DEFAULT;
    hp->delay_count = hp->video_ctx->has_b_frames;
    DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_OK, "rap_number = %d, distance = %d, seek_position = %d", *rap_number, distance, i );
    return i - hp->delay_count;
}

int func_read_video( INPUT_HANDLE ih, int sample_number, void *buf )
{
    lsmash_handler_t *hp = (lsmash_handler_t *)ih;
    ++sample_number;        /* For L-SMASH, sample_number is 1-origin. */
    AVFrame picture;        /* Decoded video data will be stored here. */
    uint32_t start_number;  /* number of sample, for normal decoding, where decoding starts excluding decoding delay */
    uint32_t rap_number;    /* number of sample, for seeking, where decoding starts excluding decoding delay */
    if( sample_number == hp->last_video_sample_number + 1 )
        start_number = sample_number;
    else
        /* Require starting to decode from random accessible sample. */
        start_number = seek_video( hp, &picture, sample_number, &rap_number );
    do
    {
        int error = get_picture( hp, &picture, start_number + hp->delay_count, sample_number + hp->delay_count );
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
    return hp->convert_colorspace( hp, &picture, buf );
}

int func_read_audio( INPUT_HANDLE ih, int start, int wanted_length, void *buf )
{
    lsmash_handler_t *hp = (lsmash_handler_t *)ih;
    DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_OK, "start = %d, wanted_length = %d", start, wanted_length );
    uint32_t sample_number;
    uint32_t data_offset;
    int      output_length = 0;
    if( start == hp->next_audio_pcm_sample_number )
    {
        if( hp->last_remainder_size )
        {
            int copy_size = min( hp->last_remainder_size, wanted_length * hp->audio_format.nBlockAlign );
            memcpy( buf, hp->audio_output_buffer, copy_size );
            buf                     += copy_size;
            hp->last_remainder_size -= copy_size;
            int copied_length = copy_size / hp->audio_format.nBlockAlign;
            output_length += copied_length;
            wanted_length -= copied_length;
            if( wanted_length <= 0 )
            {
                /* Move unused decoded data to the head of output buffer for the next access. */
                if( hp->last_remainder_size )
                    memmove( hp->audio_output_buffer, hp->audio_output_buffer + copy_size, hp->last_remainder_size );
                sample_number = hp->last_audio_sample_number;   /* We don't get a new audio sample at this time. */
                goto audio_out;
            }
        }
        sample_number = hp->last_audio_sample_number + 1;
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
            if( lsmash_get_dts_from_media_timeline( hp->root, hp->audio_track_ID, sample_number, &dts ) )
                return 0;
            if( start >= dts )
                break;
            sample_number--;
        } while( 1 );
        data_offset = (start - dts) * hp->audio_format.nBlockAlign;
    }
    do
    {
        lsmash_sample_t *sample = lsmash_get_sample_from_media_timeline( hp->root, hp->audio_track_ID, sample_number );
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
            int copy_size;
            if( output_buffer_size > data_offset )
            {
                copy_size = min( output_buffer_size - data_offset, wanted_length * hp->audio_format.nBlockAlign );
                memcpy( buf, hp->audio_output_buffer + data_offset, copy_size );
                int copied_length = copy_size / hp->audio_format.nBlockAlign;
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
            DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_OK, "sample_number = %d, decoded_length = %d", sample_number, output_buffer_size / hp->audio_format.nBlockAlign );
            if( wanted_length <= 0 )
            {
                /* Move unused decoded data to the head of output buffer for the next access. */
                if( output_buffer_size > copy_size )
                {
                    hp->last_remainder_size = output_buffer_size - copy_size;
                    memmove( hp->audio_output_buffer, hp->audio_output_buffer + copy_size, hp->last_remainder_size );
                }
                goto audio_out;
            }
        }
        ++sample_number;
    } while( 1 );
audio_out:
    DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_OK, "output_length = %d, remainder = %d", output_length, hp->last_remainder_size );
    hp->next_audio_pcm_sample_number = start + output_length;
    hp->last_audio_sample_number = sample_number;
    return output_length;
}

BOOL func_is_keyframe( INPUT_HANDLE ih, int sample_number )
{
    lsmash_handler_t *hp = (lsmash_handler_t *)ih;
    uint32_t rap_number;
    if( lsmash_get_closest_random_accessible_point_from_media_timeline( hp->root, hp->video_track_ID, ++sample_number, &rap_number ) )
        return FALSE;
    return sample_number == rap_number ? TRUE : FALSE;
}
