/*****************************************************************************
 * libav_input.c
 *****************************************************************************
 * Copyright (C) 2012 L-SMASH Works project
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

/* Libav
 * The binary file will be LGPLed or GPLed. */
#include <libavformat/avformat.h>       /* Demuxer */
#include <libavcodec/avcodec.h>         /* Decoder */
#include <libswscale/swscale.h>         /* Colorspace converter */
#include <libavresample/avresample.h>   /* Audio resampler */
#include <libavutil/mathematics.h>      /* Timebase rescaler */
#include <libavutil/pixdesc.h>
#include <libavutil/opt.h>

#include "lsmashinput.h"
#include "colorspace.h"
#include "resample.h"
#include "resource.h"
#include "progress_dlg.h"

#define SEEK_MODE_NORMAL     0
#define SEEK_MODE_UNSAFE     1
#define SEEK_MODE_AGGRESSIVE 2

#define SEEK_DTS_BASED         0x00000001
#define SEEK_PTS_BASED         0x00000002
#define SEEK_FILE_OFFSET_BASED 0x00000004

#define INDEX_FILE_VERSION 3

typedef enum
{
    DECODE_REQUIRE_INITIAL = 0,
    DECODE_INITIALIZING    = 1,
    DECODE_INITIALIZED     = 2
} decode_status_t;

typedef struct
{
    int64_t pts;
    int64_t dts;
} video_timestamp_t;

typedef struct
{
    uint32_t decoding_to_presentation;
} order_converter_t;

typedef struct
{
    uint8_t  keyframe;
    uint8_t  is_leading;
    uint32_t sample_number;     /* unique value in decoding order */
    int64_t  pts;
    int64_t  dts;
    int64_t  file_offset;
} video_frame_info_t;

typedef struct
{
    int      length;
    int      keyframe;
    uint32_t sample_number;
    int64_t  pts;
    int64_t  dts;
    int64_t  file_offset;
} audio_frame_info_t;

typedef struct libav_handler_tag
{
    char                    *file_path;
    char                    *format_name;
    int                      format_flags;
    int                      threads;
    int                      dv_in_avi;     /* 1 = 'DV in AVI Type-1', 0 = otherwise */
    /* Video stuff */
    int                      video_error;
    int                      video_index;
    enum AVCodecID           video_codec_id;
    AVFormatContext         *video_format;
    AVCodecContext          *video_ctx;
    struct SwsContext       *sws_ctx;
    AVIndexEntry            *video_index_entries;
    int                      video_index_entries_count;
    int                      video_width;
    int                      video_height;
    enum PixelFormat         pix_fmt;
    uint8_t                 *video_input_buffer;
    uint32_t                 video_input_buffer_size;
    uint32_t                 video_frame_count;
    uint32_t                 last_video_frame_number;
    uint32_t                 last_rap_number;
    uint32_t                 video_delay_count;
    uint32_t                 first_valid_video_frame_number;
    uint32_t                 first_valid_video_frame_size;
    uint8_t                 *first_valid_video_frame_data;
    decode_status_t          decode_status;
    video_frame_info_t      *video_frame_list;      /* stored in presentation order */
    uint8_t                 *keyframe_list;         /* stored in decoding order */
    order_converter_t       *order_converter;       /* stored in decoding order */
    int                      video_seek_flags;
    int                      video_seek_base;
    int                      seek_mode;
    uint32_t                 forward_seek_threshold;
    func_convert_colorspace *convert_colorspace;
    /* Audio stuff */
    int                      audio_error;
    int                      audio_index;
    enum AVCodecID           audio_codec_id;
    AVFormatContext         *audio_format;
    AVCodecContext          *audio_ctx;
    AVAudioResampleContext  *avr_ctx;
    AVIndexEntry            *audio_index_entries;
    int                      audio_index_entries_count;
    AVFrame                  audio_frame_buffer;
    AVPacket                 audio_packet;
    enum AVSampleFormat      audio_output_sample_format;
    uint8_t                 *audio_input_buffer;
    uint32_t                 audio_input_buffer_size;
    uint8_t                 *audio_resampled_buffer;
    uint32_t                 audio_resampled_buffer_size;
    uint32_t                 audio_frame_count;
    uint32_t                 audio_delay_count;
    uint32_t                 audio_frame_length;
    audio_frame_info_t      *audio_frame_list;
    int                      audio_seek_base;
    uint32_t                 next_audio_pcm_sample_number;
    uint32_t                 last_audio_frame_number;
    uint32_t                 last_remainder_length;
    uint32_t                 last_remainder_sample_offset;
    int64_t                  av_gap;
    int                      audio_planes;
    int                      audio_input_block_align;
    int                      audio_output_block_align;
    int                      audio_s24_output;
} libav_handler_t;

static int lavf_open_file( AVFormatContext **format_ctx, char *file_path )
{
    if( avformat_open_input( format_ctx, file_path, NULL, NULL ) )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to avformat_open_input." );
        return -1;
    }
    if( avformat_find_stream_info( *format_ctx, NULL ) < 0 )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to avformat_find_stream_info." );
        return -1;
    }
    return 0;
}

static void lavf_close_file( AVFormatContext **format_ctx )
{
    for( int index = 0; index < (*format_ctx)->nb_streams; index++ )
        if( avcodec_is_open( (*format_ctx)->streams[index]->codec ) )
            avcodec_close( (*format_ctx)->streams[index]->codec );
    avformat_close_input( format_ctx );
}

static int open_decoder( AVCodecContext *ctx, enum AVCodecID codec_id, int threads )
{
    AVCodec *codec = avcodec_find_decoder( codec_id );
    if( !codec )
        return -1;
    ctx->thread_count = threads;
    return (avcodec_open2( ctx, codec, NULL ) < 0) ? -1 : 0;
}

static inline int check_frame_reordering( video_frame_info_t *info, uint32_t sample_count )
{
    for( uint32_t i = 2; i <= sample_count; i++ )
        if( info[i].pts < info[i - 1].pts )
            return 1;
    return 0;
}

static int compare_pts( const video_frame_info_t *a, const video_frame_info_t *b )
{
    int64_t diff = (int64_t)(a->pts - b->pts);
    return diff > 0 ? 1 : (diff == 0 ? 0 : -1);
}

static int compare_dts( const video_timestamp_t *a, const video_timestamp_t *b )
{
    int64_t diff = (int64_t)(a->dts - b->dts);
    return diff > 0 ? 1 : (diff == 0 ? 0 : -1);
}

static inline void sort_presentation_order( video_frame_info_t *info, uint32_t sample_count )
{
    qsort( info, sample_count, sizeof(video_frame_info_t), (int(*)( const void *, const void * ))compare_pts );
}

static inline void sort_decoding_order( video_timestamp_t *timestamp, uint32_t sample_count )
{
    qsort( timestamp, sample_count, sizeof(video_timestamp_t), (int(*)( const void *, const void * ))compare_dts );
}

static inline int lineup_seek_base_candidates( libav_handler_t *hp )
{
    return !strcmp( hp->format_name, "mpeg" ) || !strcmp( hp->format_name, "mpegts" )
         ? SEEK_DTS_BASED | SEEK_PTS_BASED | SEEK_FILE_OFFSET_BASED
         : SEEK_DTS_BASED | SEEK_PTS_BASED;
}

static int decide_video_seek_method( libav_handler_t *hp, uint32_t sample_count )
{
    hp->video_seek_base = lineup_seek_base_candidates( hp );
    video_frame_info_t *info = hp->video_frame_list;
    for( uint32_t i = 1; i <= sample_count; i++ )
        if( info[i].pts == AV_NOPTS_VALUE )
        {
            hp->video_seek_base &= ~SEEK_PTS_BASED;
            break;
        }
    for( uint32_t i = 1; i <= sample_count; i++ )
        if( info[i].dts == AV_NOPTS_VALUE )
        {
            hp->video_seek_base &= ~SEEK_DTS_BASED;
            break;
        }
    if( hp->video_seek_base & SEEK_FILE_OFFSET_BASED )
    {
        if( hp->format_flags & AVFMT_NO_BYTE_SEEK )
            hp->video_seek_base &= ~SEEK_FILE_OFFSET_BASED;
        else
        {
            uint32_t error_count = 0;
            for( uint32_t i = 1; i <= sample_count; i++ )
                error_count += (info[i].file_offset == -1);
            if( error_count == sample_count )
                hp->video_seek_base &= ~SEEK_FILE_OFFSET_BASED;
        }
    }
    if( (hp->video_seek_base & SEEK_PTS_BASED) && check_frame_reordering( info, sample_count ) )
    {
        /* Consider presentation order for keyframe detection.
         * Note: sample number is 1-origin. */
        hp->order_converter = malloc_zero( (sample_count + 1) * sizeof(order_converter_t) );
        if( !hp->order_converter )
        {
            DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to allocate memory." );
            return -1;
        }
        sort_presentation_order( &info[1], sample_count );
        video_timestamp_t *timestamp = malloc( (sample_count + 1) * sizeof(video_timestamp_t) );
        if( !timestamp )
        {
            DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to allocate memory." );
            return -1;
        }
        for( uint32_t i = 1; i <= sample_count; i++ )
        {
            timestamp[i].pts = i;
            timestamp[i].dts = info[i].sample_number;
        }
        sort_decoding_order( &timestamp[1], sample_count );
        for( uint32_t i = 1; i <= sample_count; i++ )
            hp->order_converter[i].decoding_to_presentation = timestamp[i].pts;
        free( timestamp );
    }
    else if( hp->video_seek_base & SEEK_DTS_BASED )
        for( uint32_t i = 1; i <= sample_count; i++ )
            info[i].pts = info[i].dts;
    /* Treat video frames with unique value as keyframe. */
    if( hp->video_seek_base & SEEK_FILE_OFFSET_BASED )
    {
        info[ info[1].sample_number ].keyframe &= (info[ info[1].sample_number ].file_offset != -1);
        for( uint32_t i = 2; i <= sample_count; i++ )
        {
            uint32_t j = info[i    ].sample_number;
            uint32_t k = info[i - 1].sample_number;
            if( info[j].file_offset == -1 )
                info[j].keyframe = 0;
            else if( info[j].file_offset == info[k].file_offset )
                info[j].keyframe = info[k].keyframe = 0;
        }
    }
    else if( hp->video_seek_base & SEEK_PTS_BASED )
    {
        info[ info[1].sample_number ].keyframe &= (info[ info[1].sample_number ].pts != AV_NOPTS_VALUE);
        for( uint32_t i = 2; i <= sample_count; i++ )
        {
            uint32_t j = info[i    ].sample_number;
            uint32_t k = info[i - 1].sample_number;
            if( info[j].pts == AV_NOPTS_VALUE )
                info[j].keyframe = 0;
            else if( info[j].pts == info[k].pts )
                info[j].keyframe = info[k].keyframe = 0;
        }
    }
    else if( hp->video_seek_base & SEEK_DTS_BASED )
    {
        info[ info[1].sample_number ].keyframe &= (info[ info[1].sample_number ].dts != AV_NOPTS_VALUE);
        for( uint32_t i = 2; i <= sample_count; i++ )
        {
            uint32_t j = info[i    ].sample_number;
            uint32_t k = info[i - 1].sample_number;
            if( info[j].dts == AV_NOPTS_VALUE )
                info[j].keyframe = 0;
            else if( info[j].dts == info[k].dts )
                info[j].keyframe = info[k].keyframe = 0;
        }
    }
    /* Set up keyframe list: presentation order (info) -> decoding order (keyframe_list) */
    for( uint32_t i = 1; i <= sample_count; i++ )
        hp->keyframe_list[ info[i].sample_number ] = info[i].keyframe;
    return 0;
}

static void decide_audio_seek_method( libav_handler_t *hp, uint32_t sample_count )
{
    hp->audio_seek_base = lineup_seek_base_candidates( hp );
    audio_frame_info_t *info = hp->audio_frame_list;
    for( uint32_t i = 1; i <= sample_count; i++ )
        if( info[i].pts == AV_NOPTS_VALUE )
        {
            hp->audio_seek_base &= ~SEEK_PTS_BASED;
            break;
        }
    for( uint32_t i = 1; i <= sample_count; i++ )
        if( info[i].dts == AV_NOPTS_VALUE )
        {
            hp->audio_seek_base &= ~SEEK_DTS_BASED;
            break;
        }
    if( hp->audio_seek_base & SEEK_FILE_OFFSET_BASED )
    {
        if( hp->format_flags & AVFMT_NO_BYTE_SEEK )
            hp->audio_seek_base &= ~SEEK_FILE_OFFSET_BASED;
        else
        {
            uint32_t error_count = 0;
            for( uint32_t i = 1; i <= sample_count; i++ )
                error_count += (info[i].file_offset == -1);
            if( error_count == sample_count )
                hp->audio_seek_base &= ~SEEK_FILE_OFFSET_BASED;
        }
    }
    if( !(hp->audio_seek_base & SEEK_PTS_BASED) && (hp->audio_seek_base & SEEK_DTS_BASED) )
        for( uint32_t i = 1; i <= sample_count; i++ )
            info[i].pts = info[i].dts;
    /* Treat audio frames with unique value as a keyframe. */
    if( hp->audio_seek_base & SEEK_FILE_OFFSET_BASED )
    {
        info[1].keyframe = (info[1].file_offset != -1);
        for( uint32_t i = 2; i <= sample_count; i++ )
            if( info[i].file_offset == -1 )
                info[i].keyframe = 0;
            else if( info[i].file_offset == info[i - 1].file_offset )
                info[i].keyframe = info[i - 1].keyframe = 0;
            else
                info[i].keyframe = 1;
    }
    else if( hp->audio_seek_base & SEEK_PTS_BASED )
    {
        info[1].keyframe = (info[1].pts != AV_NOPTS_VALUE);
        for( uint32_t i = 2; i <= sample_count; i++ )
            if( info[i].pts == AV_NOPTS_VALUE )
                info[i].keyframe = 0;
            else if( info[i].pts == info[i - 1].pts )
                info[i].keyframe = info[i - 1].keyframe = 0;
            else
                info[i].keyframe = 1;
    }
    else if( hp->audio_seek_base & SEEK_DTS_BASED )
    {
        info[1].keyframe = (info[1].dts != AV_NOPTS_VALUE);
        for( uint32_t i = 2; i <= sample_count; i++ )
            if( info[i].dts == AV_NOPTS_VALUE )
                info[i].keyframe = 0;
            else if( info[i].dts == info[i - 1].dts )
                info[i].keyframe = info[i - 1].keyframe = 0;
            else
                info[i].keyframe = 1;
    }
    else
        for( uint32_t i = 1; i <= sample_count; i++ )
            info[i].keyframe = 1;
}

static void calculate_av_gap( libav_handler_t *hp, AVRational video_time_base, AVRational audio_time_base, int sample_rate )
{
    /* Pick the first video timestamp.
     * If invalid, skip A/V gap calculation. */
    int64_t video_ts = (hp->video_seek_base & SEEK_PTS_BASED) ? hp->video_frame_list[1].pts : hp->video_frame_list[1].dts;
    if( video_ts == AV_NOPTS_VALUE )
        return;
    /* Pick the first valid audio timestamp.
     * If not found, skip A/V gap calculation. */
    int64_t  audio_ts        = 0;
    uint32_t audio_ts_number = 0;
    if( hp->audio_seek_base & SEEK_PTS_BASED )
    {
        for( uint32_t i = 1; i <= hp->audio_frame_count; i++ )
            if( hp->audio_frame_list[i].pts != AV_NOPTS_VALUE )
            {
                audio_ts        = hp->audio_frame_list[i].pts;
                audio_ts_number = i;
                break;
            }
    }
    else
        for( uint32_t i = 1; i <= hp->audio_frame_count; i++ )
            if( hp->audio_frame_list[i].dts != AV_NOPTS_VALUE )
            {
                audio_ts        = hp->audio_frame_list[i].dts;
                audio_ts_number = i;
                break;
            }
    if( audio_ts_number == 0 )
        return;
    /* Estimate the first audio timestamp if invalid. */
    AVRational audio_sample_base = (AVRational){ 1, sample_rate };
    for( uint32_t i = 1, delay_count = 0; i < min( audio_ts_number + delay_count, hp->audio_frame_count ); i++ )
        if( hp->audio_frame_list[i].length != -1 )
            audio_ts -= av_rescale_q( hp->audio_frame_list[i].length, audio_sample_base, audio_time_base );
        else
            ++delay_count;
    /* Calculate A/V gap in audio samplerate. */
    if( video_ts || audio_ts )
        hp->av_gap = av_rescale_q( audio_ts, audio_time_base, audio_sample_base )
                   - av_rescale_q( video_ts, video_time_base, audio_sample_base );
}

static void investigate_pix_fmt_by_decoding( AVCodecContext *video_ctx, AVPacket *pkt )
{
    int got_picture;
    AVFrame picture;
    avcodec_get_frame_defaults( &picture );
    avcodec_decode_video2( video_ctx, &picture, &got_picture, pkt );
}

static inline void print_index( FILE *index, const char *format, ... )
{
    if( !index )
        return;
    va_list args;
    va_start( args, format );
    vfprintf( index, format, args );
    va_end( args );
}

static inline void write_av_index_entry( FILE *index, AVIndexEntry *ie )
{
    print_index( index, "POS=%"PRId64",TS=%"PRId64",Flags=%x,Size=%d,Distance=%d\n",
                 ie->pos, ie->timestamp, ie->flags, ie->size, ie->min_distance );
}

static void disable_video_stream( libav_handler_t *hp )
{
    if( hp->video_frame_list )
    {
        free( hp->video_frame_list );
        hp->video_frame_list = NULL;
    }
    if( hp->keyframe_list )
    {
        free( hp->keyframe_list );
        hp->keyframe_list = NULL;
    }
    if( hp->order_converter )
    {
        free( hp->order_converter );
        hp->order_converter = NULL;
    }
    if( hp->video_index_entries )
    {
        av_free( hp->video_index_entries );
        hp->video_index_entries = NULL;
    }
    hp->video_index               = -1;
    hp->video_index_entries_count = 0;
    hp->video_frame_count         = 0;
}

static inline int read_frame( AVFormatContext *format_ctx, AVPacket *pkt )
{
    int ret = av_read_frame( format_ctx, pkt );
    if( ret == AVERROR( EAGAIN ) )
    {
        av_usleep( 10000 );
        return read_frame( format_ctx, pkt );
    }
    return ret;
}

static void create_index( libav_handler_t *hp, AVFormatContext *format_ctx, reader_option_t *opt )
{
    uint32_t video_info_count = 1 << 16;
    uint32_t audio_info_count = 1 << 16;
    video_frame_info_t *video_info = malloc_zero( video_info_count * sizeof(video_frame_info_t) );
    if( !video_info )
        return;
    audio_frame_info_t *audio_info = malloc_zero( audio_info_count * sizeof(audio_frame_info_t) );
    if( !audio_info )
    {
        free( video_info );
        return;
    }
    avcodec_get_frame_defaults( &hp->audio_frame_buffer );
    /*
        # Structure of Libav reader index file
        <LibavReaderIndexFile=3>
        <InputFilePath>foobar.omo</InputFilePath>
        <LibavReaderIndex=0x00000208,marumoska>
        <ActiveVideoStreamIndex>+0000000000</ActiveVideoStreamIndex>
        <ActiveAudioStreamIndex>-0000000001</ActiveAudioStreamIndex>
        Index=0,Type=0,Codec=2,TimeBase=1001/24000,POS=0,PTS=2002,DTS=0
        Key=1,Width=1920,Height=1080,PixelFormat=yuv420p
        </LibavReaderIndex>
        <StreamIndexEntries=0,0,1>
        POS=0,TS=2002,Flags=1,Size=1024,Distance=0
        </StreamIndexEntries>
        </LibavReaderIndexFile>
     */
    char index_path[512] = { 0 };
    sprintf( index_path, "%s.index", hp->file_path );
    FILE *index = !opt->no_create_index ? fopen( index_path, "wb" ) : NULL;
    if( !index && !opt->no_create_index )
    {
        free( video_info );
        free( audio_info );
        return;
    }
    hp->format_name  = (char *)format_ctx->iformat->name;
    hp->dv_in_avi    = !strcmp( hp->format_name, "avi" ) ? -1 : 0;
    hp->format_flags = format_ctx->iformat->flags;
    hp->video_format = format_ctx;
    hp->audio_format = format_ctx;
    int32_t video_index_pos = 0;
    int32_t audio_index_pos = 0;
    if( index )
    {
        /* Write Index file header. */
        fprintf( index, "<LibavReaderIndexFile=%d>\n", INDEX_FILE_VERSION );
        fprintf( index, "<InputFilePath>%s</InputFilePath>\n", hp->file_path );
        fprintf( index, "<LibavReaderIndex=0x%08x,%s>\n", hp->format_flags, hp->format_name );
        video_index_pos = ftell( index );
        fprintf( index, "<ActiveVideoStreamIndex>%+011d</ActiveVideoStreamIndex>\n", -1 );
        audio_index_pos = ftell( index );
        fprintf( index, "<ActiveAudioStreamIndex>%+011d</ActiveAudioStreamIndex>\n", -1 );
    }
    AVPacket pkt;
    av_init_packet( &pkt );
    int       video_resolution      = 0;
    uint32_t  video_sample_count    = 0;
    int64_t   last_keyframe_pts     = AV_NOPTS_VALUE;
    uint32_t  audio_sample_count    = 0;
    int       audio_sample_rate     = 0;
    int       constant_frame_length = 1;
    int       frame_length          = 0;
    uint64_t  audio_duration        = 0;
    int64_t   first_dts             = AV_NOPTS_VALUE;
    int       max_audio_index       = -1;
    uint32_t *audio_delay_count     = NULL;
    progress_dlg_t prg_dlg;
    init_progress_dlg( &prg_dlg, "lsmashinput.aui", IDD_PROGRESS_ABORTABLE );
    while( read_frame( format_ctx, &pkt ) >= 0 )
    {
        AVCodecContext *pkt_ctx = format_ctx->streams[ pkt.stream_index ]->codec;
        if( pkt_ctx->codec_type != AVMEDIA_TYPE_VIDEO
         && pkt_ctx->codec_type != AVMEDIA_TYPE_AUDIO )
            continue;
        if( pkt_ctx->codec_id == AV_CODEC_ID_NONE )
            continue;
        if( !av_codec_is_decoder( pkt_ctx->codec ) && open_decoder( pkt_ctx, pkt_ctx->codec_id, hp->threads ) )
            continue;
        if( pkt_ctx->codec_type == AVMEDIA_TYPE_VIDEO )
        {
            if( pkt_ctx->pix_fmt == PIX_FMT_NONE )
                investigate_pix_fmt_by_decoding( pkt_ctx, &pkt );
            int dv_in_avi_init = 0;
            if( hp->dv_in_avi == -1 && pkt_ctx->codec_id == AV_CODEC_ID_DVVIDEO && hp->video_index == -1 && !opt->force_audio )
            {
                dv_in_avi_init  = 1;
                hp->dv_in_avi   = 1;
                hp->video_index = pkt.stream_index;
            }
            int higher_resoluton = (pkt_ctx->width * pkt_ctx->height > video_resolution);   /* Replace lower resolution stream with higher. */
            if( dv_in_avi_init
             || (!opt->force_video && (hp->video_index == -1 || (pkt.stream_index != hp->video_index && higher_resoluton)))
             || (opt->force_video && hp->video_index == -1 && pkt.stream_index == opt->force_video_index) )
            {
                /* Update active video stream. */
                if( index )
                {
                    int32_t current_pos = ftell( index );
                    fseek( index, video_index_pos, SEEK_SET );
                    fprintf( index, "<ActiveVideoStreamIndex>%+011d</ActiveVideoStreamIndex>\n", pkt.stream_index );
                    fseek( index, current_pos, SEEK_SET );
                }
                memset( video_info, 0, (video_sample_count + 1) * sizeof(video_frame_info_t) );
                hp->video_ctx               = pkt_ctx;
                hp->video_codec_id          = pkt_ctx->codec_id;
                hp->video_index             = pkt.stream_index;
                hp->video_input_buffer_size = 0;
                video_resolution            = pkt_ctx->width * pkt_ctx->height;
                video_sample_count          = 0;
                last_keyframe_pts           = AV_NOPTS_VALUE;
            }
            if( pkt.stream_index == hp->video_index )
            {
                ++video_sample_count;
                video_info[video_sample_count].pts           = pkt.pts;
                video_info[video_sample_count].dts           = pkt.dts;
                video_info[video_sample_count].file_offset   = pkt.pos;
                video_info[video_sample_count].sample_number = video_sample_count;
                if( pkt.pts != AV_NOPTS_VALUE && last_keyframe_pts != AV_NOPTS_VALUE && pkt.pts < last_keyframe_pts )
                    video_info[video_sample_count].is_leading = 1;
                if( pkt.flags & AV_PKT_FLAG_KEY )
                {
                    video_info[video_sample_count].keyframe = 1;
                    last_keyframe_pts = pkt.pts;
                }
                if( video_sample_count + 1 == video_info_count )
                {
                    video_info_count <<= 1;
                    video_frame_info_t *temp = realloc( video_info, video_info_count * sizeof(video_frame_info_t) );
                    if( !temp )
                    {
                        av_free_packet( &pkt );
                        goto fail_index;
                    }
                    video_info = temp;
                }
                hp->video_input_buffer_size = max( hp->video_input_buffer_size, pkt.size );
            }
            /* Write a video packet info to the index file. */
            print_index( index, "Index=%d,Type=%d,Codec=%d,TimeBase=%d/%d,POS=%"PRId64",PTS=%"PRId64",DTS=%"PRId64"\n"
                         "Key=%d,Width=%d,Height=%d,PixelFormat=%s\n",
                         pkt.stream_index, AVMEDIA_TYPE_VIDEO, pkt_ctx->codec_id,
                         format_ctx->streams[ pkt.stream_index ]->time_base.num,
                         format_ctx->streams[ pkt.stream_index ]->time_base.den,
                         pkt.pos, pkt.pts, pkt.dts,
                         !!(pkt.flags & AV_PKT_FLAG_KEY), pkt_ctx->width, pkt_ctx->height,
                         av_get_pix_fmt_name( pkt_ctx->pix_fmt ) ? av_get_pix_fmt_name( pkt_ctx->pix_fmt ) : "none" );
        }
        else
        {
            if( hp->audio_index == -1 && (!opt->force_audio || (opt->force_audio && pkt.stream_index == opt->force_audio_index)) )
            {
                /* Update active audio stream. */
                if( index )
                {
                    int32_t current_pos = ftell( index );
                    fseek( index, audio_index_pos, SEEK_SET );
                    fprintf( index, "<ActiveAudioStreamIndex>%+011d</ActiveAudioStreamIndex>\n", pkt.stream_index );
                    fseek( index, current_pos, SEEK_SET );
                }
                hp->audio_ctx      = pkt_ctx;
                hp->audio_codec_id = pkt_ctx->codec_id;
                hp->audio_index    = pkt.stream_index;
            }
            if( pkt.stream_index > max_audio_index )
            {
                uint32_t *temp = realloc( audio_delay_count, (pkt.stream_index + 1) * sizeof(uint32_t) );
                if( !temp )
                    goto fail_index;
                audio_delay_count = temp;
                memset( audio_delay_count + max_audio_index + 1, 0, (pkt.stream_index - max_audio_index) * sizeof(uint32_t) );
                max_audio_index = pkt.stream_index;
            }
            uint32_t *delay_count = &audio_delay_count[ pkt.stream_index ];
            /* Get frame_length. */
            frame_length = format_ctx->streams[ pkt.stream_index ]->parser
                         ? format_ctx->streams[ pkt.stream_index ]->parser->duration
                         : pkt_ctx->frame_size;
            if( frame_length == 0 )
            {
                AVPacket temp = pkt;
                int output_audio = 0;
                while( temp.size > 0 )
                {
                    int decode_complete;
                    int wasted_data_length = avcodec_decode_audio4( pkt_ctx, &hp->audio_frame_buffer, &decode_complete, &temp );
                    if( wasted_data_length < 0 )
                    {
                        pkt_ctx->channels    = av_get_channel_layout_nb_channels( hp->audio_frame_buffer.channel_layout );
                        pkt_ctx->sample_rate = hp->audio_frame_buffer.sample_rate;
                        break;
                    }
                    temp.size -= wasted_data_length;
                    temp.data += wasted_data_length;
                    if( decode_complete )
                    {
                        frame_length += hp->audio_frame_buffer.nb_samples;
                        output_audio = 1;
                    }
                }
                if( !output_audio )
                {
                    frame_length = -1;
                    ++ (*delay_count);
                }
            }
            if( pkt.stream_index == hp->audio_index )
            {
                if( frame_length != -1 )
                    audio_duration += frame_length;
                if( audio_duration <= INT32_MAX )
                {
                    /* Set up audio frame info. */
                    ++audio_sample_count;
                    audio_info[audio_sample_count].pts           = pkt.pts;
                    audio_info[audio_sample_count].dts           = pkt.dts;
                    audio_info[audio_sample_count].file_offset   = pkt.pos;
                    audio_info[audio_sample_count].sample_number = audio_sample_count;
                    if( frame_length != -1 && audio_sample_count > *delay_count )
                    {
                        uint32_t audio_frame_number = audio_sample_count - *delay_count;
                        audio_info[audio_frame_number].length = frame_length;
                        if( audio_frame_number > 1 && audio_info[audio_frame_number].length != audio_info[audio_frame_number - 1].length )
                            constant_frame_length = 0;
                    }
                    if( audio_sample_rate == 0 )
                        audio_sample_rate = pkt_ctx->sample_rate;
                    if( audio_sample_count + 1 == audio_info_count )
                    {
                        audio_info_count <<= 1;
                        audio_frame_info_t *temp = realloc( audio_info, audio_info_count * sizeof(audio_frame_info_t) );
                        if( !temp )
                        {
                            av_free_packet( &pkt );
                            goto fail_index;
                        }
                        audio_info = temp;
                    }
                    hp->audio_input_buffer_size = max( hp->audio_input_buffer_size, pkt.size );
                }
            }
            /* Write an audio packet info to the index file. */
            print_index( index, "Index=%d,Type=%d,Codec=%d,TimeBase=%d/%d,POS=%"PRId64",PTS=%"PRId64",DTS=%"PRId64"\n"
                         "Channels=%d,SampleRate=%d,BitsPerSample=%d,Length=%d\n",
                         pkt.stream_index, AVMEDIA_TYPE_AUDIO, pkt_ctx->codec_id,
                         format_ctx->streams[ pkt.stream_index ]->time_base.num,
                         format_ctx->streams[ pkt.stream_index ]->time_base.den,
                         pkt.pos, pkt.pts, pkt.dts,
                         pkt_ctx->channels, pkt_ctx->sample_rate,
                         pkt_ctx->bits_per_raw_sample > 0 ? pkt_ctx->bits_per_raw_sample : av_get_bytes_per_sample( pkt_ctx->sample_fmt ) << 3,
                         frame_length );
        }
        /* Update progress dialog if packet's DTS is valid. */
        if( first_dts == AV_NOPTS_VALUE )
            first_dts = pkt.dts;
        int percent = first_dts == AV_NOPTS_VALUE || pkt.dts == AV_NOPTS_VALUE
                    ? 0
                    : 100.0 * (pkt.dts - first_dts)
                    * (format_ctx->streams[ pkt.stream_index ]->time_base.num / (double)format_ctx->streams[ pkt.stream_index ]->time_base.den)
                    / (format_ctx->duration / AV_TIME_BASE) + 0.5;
        int abort = update_progress_dlg( &prg_dlg, index ? "Creating Index file" : "Parsing input file", percent );
        av_free_packet( &pkt );
        if( abort )
            goto fail_index;
    }
    if( hp->video_index >= 0 )
    {
        hp->keyframe_list = malloc( (video_sample_count + 1) * sizeof(uint8_t) );
        if( !hp->keyframe_list )
            goto fail_index;
        for( uint32_t i = 0; i <= video_sample_count; i++ )
            hp->keyframe_list[i] = video_info[i].keyframe;
        hp->video_frame_list  = video_info;
        hp->video_frame_count = video_sample_count;
        hp->video_width       = hp->video_ctx->width;
        hp->video_height      = hp->video_ctx->height;
        hp->pix_fmt           = hp->video_ctx->pix_fmt;
        if( decide_video_seek_method( hp, video_sample_count ) )
            goto fail_index;
    }
    else
    {
        free( video_info );
        video_info = NULL;
    }
    if( audio_delay_count )
    {
        /* Flush if audio decoding is delayed. */
        for( int stream_index = 0; stream_index <= max_audio_index; stream_index++ )
        {
            AVCodecContext *pkt_ctx = format_ctx->streams[stream_index]->codec;
            if( pkt_ctx->codec_type != AVMEDIA_TYPE_AUDIO )
                continue;
            for( uint32_t i = 1; i <= audio_delay_count[stream_index]; i++ )
            {
                AVPacket null_pkt;
                av_init_packet( &null_pkt );
                null_pkt.data = NULL;
                null_pkt.size = 0;
                int decode_complete;
                if( avcodec_decode_audio4( pkt_ctx, &hp->audio_frame_buffer, &decode_complete, &null_pkt ) >= 0 )
                {
                    frame_length = decode_complete ? hp->audio_frame_buffer.nb_samples : 0;
                    if( stream_index == hp->audio_index )
                    {
                        audio_duration += frame_length;
                        if( audio_duration > INT32_MAX )
                            break;
                        uint32_t audio_frame_number = audio_sample_count - audio_delay_count[stream_index] + i;
                        audio_info[audio_frame_number].length = frame_length;
                        if( audio_frame_number > 1 && audio_info[audio_frame_number].length != audio_info[audio_frame_number - 1].length )
                            constant_frame_length = 0;
                    }
                    print_index( index, "Index=%d,Type=%d,Codec=%d,TimeBase=%d/%d,POS=%"PRId64",PTS=%"PRId64",DTS=%"PRId64"\n"
                                 "Channels=%d,SampleRate=%d,BitsPerSample=%d,Length=%d\n",
                                 stream_index, AVMEDIA_TYPE_AUDIO, pkt_ctx->codec_id,
                                 format_ctx->streams[stream_index]->time_base.num,
                                 format_ctx->streams[stream_index]->time_base.den,
                                 -1LL, AV_NOPTS_VALUE, AV_NOPTS_VALUE,
                                 0, 0, 0, frame_length );
                }
            }
        }
        free( audio_delay_count );
    }
    if( hp->audio_index >= 0 )
    {
        if( hp->dv_in_avi == 1 && format_ctx->streams[ hp->audio_index ]->nb_index_entries == 0 )
        {
            /* DV in AVI Type-1 */
            audio_sample_count = video_info ? min( video_sample_count, audio_sample_count ) : 0;
            for( uint32_t i = 1; i <= audio_sample_count; i++ )
            {
                audio_info[i].keyframe      = video_info[i].keyframe;
                audio_info[i].sample_number = video_info[i].sample_number;
                audio_info[i].pts           = video_info[i].pts;
                audio_info[i].dts           = video_info[i].dts;
                audio_info[i].file_offset   = video_info[i].file_offset;
            }
        }
        else
        {
            if( hp->dv_in_avi == 1 && opt->force_video && opt->force_video_index == -1 )
            {
                /* Disable DV video stream. */
                disable_video_stream( hp );
                video_info = NULL;
            }
            hp->dv_in_avi = 0;
        }
        hp->audio_frame_list   = audio_info;
        hp->audio_frame_count  = audio_sample_count;
        hp->audio_frame_length = constant_frame_length ? frame_length : 0;
        decide_audio_seek_method( hp, audio_sample_count );
        if( opt->av_sync && hp->video_index >= 0 )
            calculate_av_gap( hp,
                              format_ctx->streams[ hp->video_index ]->time_base,
                              format_ctx->streams[ hp->audio_index ]->time_base,
                              audio_sample_rate );
    }
    else
    {
        free( audio_info );
        audio_info = NULL;
    }
    print_index( index, "</LibavReaderIndex>\n" );
    for( int stream_index = 0; stream_index < format_ctx->nb_streams; stream_index++ )
    {
        AVStream *stream = format_ctx->streams[stream_index];
        if( stream->codec->codec_type == AVMEDIA_TYPE_VIDEO )
        {
            print_index( index, "<StreamIndexEntries=%d,%d,%d>\n", stream_index, AVMEDIA_TYPE_VIDEO, stream->nb_index_entries );
            if( hp->video_index != stream_index )
                for( int i = 0; i < stream->nb_index_entries; i++ )
                    write_av_index_entry( index, &stream->index_entries[i] );
            else if( stream->nb_index_entries > 0 )
            {
                hp->video_index_entries = av_malloc( stream->index_entries_allocated_size );
                if( !hp->video_index_entries )
                    goto fail_index;
                for( int i = 0; i < stream->nb_index_entries; i++ )
                {
                    AVIndexEntry *ie = &stream->index_entries[i];
                    hp->video_index_entries[i] = *ie;
                    write_av_index_entry( index, ie );
                }
                hp->video_index_entries_count = stream->nb_index_entries;
            }
            print_index( index, "</StreamIndexEntries>\n" );
        }
        else if( stream->codec->codec_type == AVMEDIA_TYPE_AUDIO )
        {
            print_index( index, "<StreamIndexEntries=%d,%d,%d>\n", stream_index, AVMEDIA_TYPE_AUDIO, stream->nb_index_entries );
            if( hp->audio_index != stream_index )
                for( int i = 0; i < stream->nb_index_entries; i++ )
                    write_av_index_entry( index, &stream->index_entries[i] );
            else if( stream->nb_index_entries > 0 )
            {
                /* Audio stream in matroska container requires index_entries for seeking.
                 * This avoids for re-reading the file to create index_entries since the file will be closed once. */
                hp->audio_index_entries = av_malloc( stream->index_entries_allocated_size );
                if( !hp->audio_index_entries )
                    goto fail_index;
                for( int i = 0; i < stream->nb_index_entries; i++ )
                {
                    AVIndexEntry *ie = &stream->index_entries[i];
                    hp->audio_index_entries[i] = *ie;
                    write_av_index_entry( index, ie );
                }
                hp->audio_index_entries_count = stream->nb_index_entries;
            }
            print_index( index, "</StreamIndexEntries>\n" );
        }
    }
    print_index( index, "</LibavReaderIndexFile>\n" );
    if( index )
        fclose( index );
    close_progress_dlg( &prg_dlg );
    hp->video_format = NULL;
    hp->audio_format = NULL;
    return;
fail_index:
    free( video_info );
    free( audio_info );
    if( audio_delay_count )
        free( audio_delay_count );
    if( index )
        fclose( index );
    close_progress_dlg( &prg_dlg );
    hp->video_format = NULL;
    hp->audio_format = NULL;
    return;
}

static int parse_index( libav_handler_t *hp, FILE *index, reader_option_t *opt )
{
    /* Test to open the target file. */
    char file_path[512] = { 0 };
    if( fscanf( index, "<InputFilePath>%[^\n<]</InputFilePath>\n", file_path ) != 1 )
        return -1;
    FILE *target = fopen( file_path, "rb" );
    if( !target )
        return -1;
    fclose( target );
    int file_path_length = strlen( file_path );
    hp->file_path = malloc( file_path_length + 1 );
    if( !hp->file_path )
        return -1;
    memcpy( hp->file_path, file_path, file_path_length );
    hp->file_path[file_path_length] = '\0';
    /* Parse the index file. */
    char format_name[256];
    int active_video_index;
    int active_audio_index;
    if( fscanf( index, "<LibavReaderIndex=0x%x,%[^>]>\n", &hp->format_flags, format_name ) != 2 )
        return -1;
    int32_t active_index_pos = ftell( index );
    if( fscanf( index, "<ActiveVideoStreamIndex>%d</ActiveVideoStreamIndex>\n", &active_video_index ) != 1
     || fscanf( index, "<ActiveAudioStreamIndex>%d</ActiveAudioStreamIndex>\n", &active_audio_index ) != 1 )
        return -1;
    hp->format_name = format_name;
    hp->dv_in_avi = !strcmp( hp->format_name, "avi" ) ? -1 : 0;
    int video_present = (active_video_index >= 0);
    int audio_present = (active_audio_index >= 0);
    hp->video_index = opt->force_video ? opt->force_video_index : active_video_index;
    hp->audio_index = opt->force_audio ? opt->force_audio_index : active_audio_index;
    uint32_t video_info_count = 1 << 16;
    uint32_t audio_info_count = 1 << 16;
    video_frame_info_t *video_info = NULL;
    audio_frame_info_t *audio_info = NULL;
    if( hp->video_index >= 0 )
    {
        video_info = malloc_zero( video_info_count * sizeof(video_frame_info_t) );
        if( !video_info )
            goto fail_parsing;
    }
    if( hp->audio_index >= 0 )
    {
        audio_info = malloc_zero( audio_info_count * sizeof(audio_frame_info_t) );
        if( !audio_info )
            goto fail_parsing;
    }
    hp->video_codec_id = AV_CODEC_ID_NONE;
    hp->audio_codec_id = AV_CODEC_ID_NONE;
    hp->pix_fmt        = PIX_FMT_NONE;
    uint32_t video_sample_count    = 0;
    int64_t  last_keyframe_pts     = AV_NOPTS_VALUE;
    uint32_t audio_sample_count    = 0;
    int      audio_sample_rate     = 0;
    int      constant_frame_length = 1;
    uint64_t audio_duration        = 0;
    AVRational video_time_base     = { 0, 0 };
    AVRational audio_time_base     = { 0, 0 };
    char buf[1024];
    while( fgets( buf, sizeof(buf), index ) )
    {
        int stream_index;
        int codec_type;
        int codec_id;
        AVRational time_base;
        int64_t pos;
        int64_t pts;
        int64_t dts;
        if( sscanf( buf, "Index=%d,Type=%d,Codec=%d,TimeBase=%d/%d,POS=%"SCNd64",PTS=%"SCNd64",DTS=%"SCNd64,
                    &stream_index, &codec_type, &codec_id, &time_base.num, &time_base.den, &pos, &pts, &dts ) != 8 )
            break;
        if( codec_type == AVMEDIA_TYPE_VIDEO )
        {
            if( !fgets( buf, sizeof(buf), index ) )
                goto fail_parsing;
            if( hp->dv_in_avi == -1 && codec_id == AV_CODEC_ID_DVVIDEO && !opt->force_audio )
            {
                hp->dv_in_avi = 1;
                if( hp->video_index == -1 )
                {
                    hp->video_index = stream_index;
                    video_info = malloc_zero( video_info_count * sizeof(video_frame_info_t) );
                    if( !video_info )
                        goto fail_parsing;
                }
            }
            if( stream_index == hp->video_index )
            {
                int key;
                int width;
                int height;
                char pix_fmt[64];
                if( sscanf( buf, "Key=%d,Width=%d,Height=%d,PixelFormat=%s",
                    &key, &width, &height, pix_fmt ) != 4 )
                    goto fail_parsing;
                if( hp->video_codec_id == AV_CODEC_ID_NONE )
                    hp->video_codec_id = codec_id;
                if( hp->video_width == 0 || hp->video_height == 0 )
                {
                    hp->video_width  = width;
                    hp->video_height = height;
                }
                if( hp->pix_fmt == PIX_FMT_NONE )
                    hp->pix_fmt = av_get_pix_fmt( (const char *)pix_fmt );
                if( video_time_base.num == 0 || video_time_base.den == 0 )
                {
                    video_time_base.num = time_base.num;
                    video_time_base.den = time_base.den;
                }
                ++video_sample_count;
                video_info[video_sample_count].pts           = pts;
                video_info[video_sample_count].dts           = dts;
                video_info[video_sample_count].file_offset   = pos;
                video_info[video_sample_count].sample_number = video_sample_count;
                if( pts != AV_NOPTS_VALUE && last_keyframe_pts != AV_NOPTS_VALUE && pts < last_keyframe_pts )
                    video_info[video_sample_count].is_leading = 1;
                if( key )
                {
                    video_info[video_sample_count].keyframe = 1;
                    last_keyframe_pts = pts;
                }
                if( video_sample_count + 1 == video_info_count )
                {
                    video_info_count <<= 1;
                    video_frame_info_t *temp = realloc( video_info, video_info_count * sizeof(video_frame_info_t) );
                    if( !temp )
                        goto fail_parsing;
                    video_info = temp;
                }
            }
        }
        else if( codec_type == AVMEDIA_TYPE_AUDIO )
        {
            if( !fgets( buf, sizeof(buf), index ) )
                goto fail_parsing;
            if( stream_index == hp->audio_index )
            {
                int channels;
                int sample_rate;
                int bps;
                int frame_length;
                if( sscanf( buf, "Channels=%d,SampleRate=%d,BitsPerSample=%d,Length=%d",
                            &channels, &sample_rate, &bps, &frame_length ) != 4 )
                    goto fail_parsing;
                if( hp->audio_codec_id == AV_CODEC_ID_NONE )
                    hp->audio_codec_id = codec_id;
                if( (channels | sample_rate | bps) && audio_duration <= INT32_MAX )
                {
                    if( audio_sample_rate == 0 )
                        audio_sample_rate = sample_rate;
                    if( audio_time_base.num == 0 || audio_time_base.den == 0 )
                    {
                        audio_time_base.num = time_base.num;
                        audio_time_base.den = time_base.den;
                    }
                    ++audio_sample_count;
                    audio_info[audio_sample_count].pts           = pts;
                    audio_info[audio_sample_count].dts           = dts;
                    audio_info[audio_sample_count].file_offset   = pos;
                    audio_info[audio_sample_count].sample_number = audio_sample_count;
                }
                else
                    for( uint32_t i = 1; i <= hp->audio_delay_count; i++ )
                    {
                        uint32_t audio_frame_number = audio_sample_count - hp->audio_delay_count + i;
                        if( audio_frame_number > audio_sample_count )
                            goto fail_parsing;
                        audio_info[audio_frame_number].length = frame_length;
                        if( audio_frame_number > 1 && audio_info[audio_frame_number].length != audio_info[audio_frame_number - 1].length )
                            constant_frame_length = 0;
                        audio_duration += frame_length;
                    }
                if( audio_sample_count + 1 == audio_info_count )
                {
                    audio_info_count <<= 1;
                    audio_frame_info_t *temp = realloc( audio_info, audio_info_count * sizeof(audio_frame_info_t) );
                    if( !temp )
                        goto fail_parsing;
                    audio_info = temp;
                }
                if( frame_length == -1 )
                    ++ hp->audio_delay_count;
                else if( audio_sample_count > hp->audio_delay_count )
                {
                    uint32_t audio_frame_number = audio_sample_count - hp->audio_delay_count;
                    audio_info[audio_frame_number].length = frame_length;
                    if( audio_frame_number > 1 && audio_info[audio_frame_number].length != audio_info[audio_frame_number - 1].length )
                        constant_frame_length = 0;
                    audio_duration += frame_length;
                }
            }
        }
    }
    if( video_present && opt->force_video && opt->force_video_index != -1
     && (video_sample_count == 0 || hp->pix_fmt == PIX_FMT_NONE || hp->video_width == 0 || hp->video_height == 0) )
        goto fail_parsing;  /* Need to re-create the index file. */
    if( audio_present && opt->force_audio && opt->force_audio_index != -1 && (audio_sample_count == 0 || audio_duration == 0) )
        goto fail_parsing;  /* Need to re-create the index file. */
    if( strncmp( buf, "</LibavReaderIndex>", strlen( "</LibavReaderIndex>" ) ) )
        goto fail_parsing;
    /* Parse AVIndexEntry. */
    if( !fgets( buf, sizeof(buf), index ) )
        goto fail_parsing;
    while( !strncmp( buf, "<StreamIndexEntries=", strlen( "<StreamIndexEntries=" ) ) )
    {
        int stream_index;
        int codec_type;
        int index_entries_count;
        if( sscanf( buf, "<StreamIndexEntries=%d,%d,%d>", &stream_index, &codec_type, &index_entries_count ) != 3 )
            goto fail_parsing;
        if( !fgets( buf, sizeof(buf), index ) )
            goto fail_parsing;
        if( index_entries_count > 0 )
        {
            if( stream_index != hp->video_index && stream_index != hp->audio_index )
            {
                for( int i = 0; i < index_entries_count; i++ )
                    if( !fgets( buf, sizeof(buf), index ) )
                        goto fail_parsing;
            }
            else if( codec_type == AVMEDIA_TYPE_VIDEO && stream_index == hp->video_index )
            {
                hp->video_index_entries_count = index_entries_count;
                hp->video_index_entries = av_malloc( hp->video_index_entries_count * sizeof(AVIndexEntry) );
                if( !hp->video_index_entries )
                    goto fail_parsing;
                for( int i = 0; i < hp->video_index_entries_count; i++ )
                {
                    AVIndexEntry ie;
                    int size;
                    int flags;
                    if( sscanf( buf, "POS=%"SCNd64",TS=%"SCNd64",Flags=%x,Size=%d,Distance=%d",
                                &ie.pos, &ie.timestamp, &flags, &size, &ie.min_distance ) != 5 )
                        break;
                    ie.size  = size;
                    ie.flags = flags;
                    hp->video_index_entries[i] = ie;
                    if( !fgets( buf, sizeof(buf), index ) )
                        goto fail_parsing;
                }
            }
            else if( codec_type == AVMEDIA_TYPE_AUDIO && stream_index == hp->audio_index )
            {
                hp->audio_index_entries_count = index_entries_count;
                hp->audio_index_entries = av_malloc( hp->audio_index_entries_count * sizeof(AVIndexEntry) );
                if( !hp->audio_index_entries )
                    goto fail_parsing;
                for( int i = 0; i < hp->audio_index_entries_count; i++ )
                {
                    AVIndexEntry ie;
                    int size;
                    int flags;
                    if( sscanf( buf, "POS=%"SCNd64",TS=%"SCNd64",Flags=%x,Size=%d,Distance=%d",
                                &ie.pos, &ie.timestamp, &flags, &size, &ie.min_distance ) != 5 )
                        break;
                    ie.size  = size;
                    ie.flags = flags;
                    hp->audio_index_entries[i] = ie;
                    if( !fgets( buf, sizeof(buf), index ) )
                        goto fail_parsing;
                }
            }
        }
        if( strncmp( buf, "</StreamIndexEntries>", strlen( "</StreamIndexEntries>" ) ) )
            goto fail_parsing;
        if( !fgets( buf, sizeof(buf), index ) )
            goto fail_parsing;
        ++stream_index;
    }
    if( !strncmp( buf, "</LibavReaderIndexFile>", strlen( "</LibavReaderIndexFile>" ) ) )
    {
        if( hp->video_index >= 0 )
        {
            hp->keyframe_list = malloc( (video_sample_count + 1) * sizeof(uint8_t) );
            if( !hp->keyframe_list )
                goto fail_parsing;
            for( uint32_t i = 0; i <= video_sample_count; i++ )
                hp->keyframe_list[i] = video_info[i].keyframe;
            hp->video_frame_list  = video_info;
            hp->video_frame_count = video_sample_count;
            if( decide_video_seek_method( hp, video_sample_count ) )
                goto fail_parsing;
        }
        if( hp->audio_index >= 0 )
        {
            if( hp->dv_in_avi == 1 && hp->audio_index_entries_count == 0 )
            {
                /* DV in AVI Type-1 */
                audio_sample_count = min( video_sample_count, audio_sample_count );
                for( uint32_t i = 0; i <= audio_sample_count; i++ )
                {
                    audio_info[i].keyframe      = video_info[i].keyframe;
                    audio_info[i].sample_number = video_info[i].sample_number;
                    audio_info[i].pts           = video_info[i].pts;
                    audio_info[i].dts           = video_info[i].dts;
                    audio_info[i].file_offset   = video_info[i].file_offset;
                }
            }
            else
            {
                if( hp->dv_in_avi == 1 && ((!opt->force_video && active_video_index == -1) || (opt->force_video && opt->force_video_index == -1)) )
                {
                    /* Disable DV video stream. */
                    disable_video_stream( hp );
                    video_info = NULL;
                }
                hp->dv_in_avi = 0;
            }
            hp->audio_frame_list   = audio_info;
            hp->audio_frame_count  = audio_sample_count;
            hp->audio_frame_length = constant_frame_length ? audio_info[1].length : 0;
            decide_audio_seek_method( hp, audio_sample_count );
            if( opt->av_sync && hp->video_index >= 0 )
                calculate_av_gap( hp, video_time_base, audio_time_base, audio_sample_rate );
        }
        if( hp->video_index != active_video_index || hp->audio_index != active_audio_index )
        {
            /* Update the active stream indexes when specifying different stream indexes. */
            fseek( index, active_index_pos, SEEK_SET );
            fprintf( index, "<ActiveVideoStreamIndex>%+011d</ActiveVideoStreamIndex>\n", hp->video_index );
            fprintf( index, "<ActiveAudioStreamIndex>%+011d</ActiveAudioStreamIndex>\n", hp->audio_index );
        }
        return 0;
    }
fail_parsing:
    hp->video_frame_list = NULL;
    hp->audio_frame_list = NULL;
    if( video_info )
        free( video_info );
    if( audio_info )
        free( audio_info );
    return -1;
}

static void *open_file( char *file_path, reader_option_t *opt )
{
    libav_handler_t *hp = malloc_zero( sizeof(libav_handler_t) );
    if( !hp )
        return NULL;
    /* Try to open the index file. */
    int file_path_length = strlen( file_path );
    char index_file_path[file_path_length + 7];
    memcpy( index_file_path, file_path, file_path_length );
    char *ext = file_path_length >= 7 ? &file_path[file_path_length - 6] : NULL;
    if( ext && !strncmp( ext, ".index", strlen( ".index" ) ) )
        index_file_path[file_path_length] = '\0';
    else
    {
        memcpy( index_file_path + file_path_length, ".index", strlen( ".index" ) );
        index_file_path[file_path_length + 6] = '\0';
    }
    FILE *index = fopen( index_file_path, (opt->force_video || opt->force_video) ? "r+b" : "rb" );
    if( index )
    {
        int version = 0;
        int ret = fscanf( index, "<LibavReaderIndexFile=%d>\n", &version );
        if( ret == 1 && version == INDEX_FILE_VERSION && !parse_index( hp, index, opt ) )
        {
            /* Opening and parsing the index file succeeded. */
            fclose( index );
            av_register_all();
            avcodec_register_all();
            return hp;
        }
        fclose( index );
    }
    /* Open file and create the index. */
    if( !hp->file_path )
    {
        hp->file_path = malloc( file_path_length + 1 );
        if( !hp->file_path )
        {
            free( hp );
            return NULL;
        }
        memcpy( hp->file_path, file_path, file_path_length );
        hp->file_path[file_path_length] = '\0';
    }
    av_register_all();
    avcodec_register_all();
    AVFormatContext *format_ctx = NULL;
    if( lavf_open_file( &format_ctx, file_path ) )
    {
        if( format_ctx )
            lavf_close_file( &format_ctx );
        free( hp->file_path );
        free( hp );
        return NULL;
    }
    hp->threads     = opt->threads;
    hp->video_index = -1;
    hp->audio_index = -1;
    create_index( hp, format_ctx, opt );
    /* Close file.
     * By opening file for video and audio separately, indecent work about frame reading can be avoidable. */
    lavf_close_file( &format_ctx );
    hp->video_ctx = NULL;
    hp->audio_ctx = NULL;
    return hp;
}

static int get_video_track( lsmash_handler_t *h )
{
    libav_handler_t *hp = (libav_handler_t *)h->video_private;
    int error = hp->video_index < 0
             || hp->video_frame_count == 0
             || lavf_open_file( &hp->video_format, hp->file_path );
    AVCodecContext *ctx = !error ? hp->video_format->streams[ hp->video_index ]->codec : NULL;
    if( error || open_decoder( ctx, hp->video_codec_id, hp->threads ) )
    {
        if( hp->video_index_entries )
        {
            av_free( hp->video_index_entries );
            hp->video_index_entries = NULL;
        }
        if( hp->video_frame_list )
        {
            free( hp->video_frame_list );
            hp->video_frame_list = NULL;
        }
        if( hp->order_converter )
        {
            free( hp->order_converter );
            hp->order_converter = NULL;
        }
        if( hp->keyframe_list )
        {
            free( hp->keyframe_list );
            hp->keyframe_list = NULL;
        }
        if( hp->video_format )
        {
            lavf_close_file( &hp->video_format );
            hp->video_format = NULL;
        }
        return -1;
    }
    hp->video_ctx = ctx;
    return 0;
}

static int get_audio_track( lsmash_handler_t *h )
{
    libav_handler_t *hp = (libav_handler_t *)h->audio_private;
    int error = hp->audio_index < 0
             || hp->audio_frame_count == 0
             || lavf_open_file( &hp->audio_format, hp->file_path );
    AVCodecContext *ctx = !error ? hp->audio_format->streams[ hp->audio_index ]->codec : NULL;
    if( error || open_decoder( ctx, hp->audio_codec_id, hp->threads ) )
    {
        if( hp->audio_index_entries )
        {
            av_free( hp->audio_index_entries );
            hp->audio_index_entries = NULL;
        }
        if( hp->audio_frame_list )
        {
            free( hp->audio_frame_list );
            hp->audio_frame_list = NULL;
        }
        if( hp->audio_format )
        {
            lavf_close_file( &hp->audio_format );
            hp->audio_format = NULL;
        }
        return -1;
    }
    hp->audio_ctx = ctx;
    return 0;
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

static inline double sigexp10( double value, double *exponent )
{
    /* This function separates significand and exp10 from double floating point. */
    *exponent = 1;
    while( value < 1 )
    {
        value *= 10;
        *exponent /= 10;
    }
    while( value >= 10 )
    {
        value /= 10;
        *exponent *= 10;
    }
    return value;
}

static int try_ntsc_framerate( lsmash_handler_t *h, double fps )
{
#define DOUBLE_EPSILON 5e-5
    if( fps == 0 )
        return 0;
    double exponent;
    double fps_sig = sigexp10( fps, &exponent );
    uint64_t fps_den, fps_num;
    int i = 1;
    while( 1 )
    {
        fps_den = i * 1001;
        fps_num = round( fps_den * fps_sig ) * exponent;
        if( fps_num > UINT32_MAX )
            return 0;
        if( fabs( ((double)fps_num / fps_den) / exponent - fps_sig ) < DOUBLE_EPSILON )
            break;
        ++i;
    }
    h->framerate_num = fps_num;
    h->framerate_den = fps_den;
    return 1;
#undef DOUBLE_EPSILON
}

static void setup_timestamp_info( lsmash_handler_t *h )
{
    libav_handler_t *hp = (libav_handler_t *)h->video_private;
    AVStream *video_stream = hp->video_format->streams[ hp->video_index ];
    if( h->video_sample_count == 1 || !(hp->video_seek_base & (SEEK_DTS_BASED | SEEK_PTS_BASED)) )
    {
        h->framerate_num = video_stream->r_frame_rate.num;
        h->framerate_den = video_stream->r_frame_rate.den;
        return;
    }
    video_frame_info_t *info = hp->video_frame_list;
    int64_t  first_ts;
    int64_t  largest_ts;
    int64_t  second_largest_ts;
    uint64_t stream_timebase;
    if( hp->video_seek_base & SEEK_PTS_BASED )
    {
        first_ts          = info[1].pts;
        largest_ts        = info[2].pts;
        second_largest_ts = info[1].pts;
        stream_timebase   = info[2].pts - info[1].pts;
        for( uint32_t i = 3; i <= h->video_sample_count; i++ )
        {
            if( info[i].pts == info[i - 1].pts )
            {
                MESSAGE_BOX_DESKTOP( MB_OK, "Detected PTS %"PRId64" duplication at frame %"PRIu32, info[i].pts, i );
                h->framerate_num = video_stream->avg_frame_rate.num;
                h->framerate_den = video_stream->avg_frame_rate.den;
                return;
            }
            stream_timebase = get_gcd( stream_timebase, info[i].pts - info[i - 1].pts );
            second_largest_ts = largest_ts;
            largest_ts = info[i].pts;
        }
    }
    else
    {
        first_ts          = info[1].dts;
        largest_ts        = info[2].dts;
        second_largest_ts = info[1].dts;
        stream_timebase   = info[2].dts - info[1].dts;
        for( uint32_t i = 3; i <= h->video_sample_count; i++ )
        {
            if( info[i].dts == info[i - 1].dts )
            {
                MESSAGE_BOX_DESKTOP( MB_OK, "Detected DTS %"PRId64" duplication at frame %"PRIu32, info[i].dts, i );
                h->framerate_num = video_stream->avg_frame_rate.num;
                h->framerate_den = video_stream->avg_frame_rate.den;
                return;
            }
            stream_timebase = get_gcd( stream_timebase, info[i].dts - info[i - 1].dts );
            second_largest_ts = largest_ts;
            largest_ts = info[i].dts;
        }
    }
    stream_timebase *= video_stream->time_base.num;
    uint64_t stream_timescale = video_stream->time_base.den;
    uint64_t reduce = reduce_fraction( &stream_timescale, &stream_timebase );
    uint64_t stream_duration = (((largest_ts - first_ts) + (largest_ts - second_largest_ts)) * video_stream->time_base.num) / reduce;
    double stream_framerate = h->video_sample_count * ((double)stream_timescale / stream_duration);
    if( try_ntsc_framerate( h, stream_framerate ) )
        return;
    h->framerate_num = stream_framerate * stream_timebase + 0.5;
    h->framerate_den = stream_timebase;
}

static void find_random_accessible_point( libav_handler_t *hp, uint32_t presentation_sample_number, uint32_t decoding_sample_number, uint32_t *rap_number )
{
    uint8_t is_leading = hp->video_frame_list[presentation_sample_number].is_leading;
    if( decoding_sample_number == 0 )
        decoding_sample_number = hp->video_frame_list[presentation_sample_number].sample_number;
    *rap_number = decoding_sample_number;
    while( *rap_number )
    {
        if( hp->keyframe_list[ *rap_number ] )
        {
            if( !is_leading )
                break;
            /* Shall be decoded from more past random access point. */
            is_leading = 0;
        }
        --(*rap_number);
    }
    if( *rap_number == 0 )
        *rap_number = 1;
}

static int64_t get_random_accessible_point_position( lsmash_handler_t *h, uint32_t rap_number )
{
    libav_handler_t *hp = (libav_handler_t *)h->video_private;
    uint32_t presentation_rap_number = hp->order_converter
                                     ? hp->order_converter[rap_number].decoding_to_presentation
                                     : rap_number;
    return (hp->video_seek_base & SEEK_FILE_OFFSET_BASED) ? hp->video_frame_list[presentation_rap_number].file_offset
         : (hp->video_seek_base & SEEK_PTS_BASED)         ? hp->video_frame_list[presentation_rap_number].pts
         : (hp->video_seek_base & SEEK_DTS_BASED)         ? hp->video_frame_list[presentation_rap_number].dts
         :                                                  hp->video_frame_list[presentation_rap_number].sample_number;
}

static inline uint32_t get_decoder_delay( AVCodecContext *ctx )
{
    return ctx->has_b_frames + ((ctx->active_thread_type & FF_THREAD_FRAME) ? ctx->thread_count - 1 : 0);
}

static int get_sample( AVFormatContext *format_ctx, int stream_index, uint8_t **buffer, uint32_t *buffer_size, AVPacket *pkt )
{
    AVPacket temp;
    av_init_packet( &temp );
    while( read_frame( format_ctx, &temp ) >= 0 )
    {
        if( temp.stream_index != stream_index )
        {
            av_free_packet( &temp );
            continue;
        }
        /* Don't trust the first survey of the maximum packet size. It seems various by seeking. */
        if( temp.size + FF_INPUT_BUFFER_PADDING_SIZE > *buffer_size )
        {
            uint8_t *new_buffer = av_realloc( *buffer, temp.size + FF_INPUT_BUFFER_PADDING_SIZE );
            if( !new_buffer )
            {
                av_free_packet( &temp );
                continue;
            }
            *buffer      = new_buffer;
            *buffer_size = temp.size + FF_INPUT_BUFFER_PADDING_SIZE;
        }
        *pkt = temp;
        pkt->data = *buffer;
        memcpy( pkt->data, temp.data, temp.size );
        av_free_packet( &temp );
        return 0;
    }
    *pkt = temp;
    pkt->data = NULL;
    pkt->size = 0;
    return 1;
}

static int prepare_video_decoding( lsmash_handler_t *h, video_option_t *opt )
{
    libav_handler_t *hp = (libav_handler_t *)h->video_private;
    if( !hp->video_ctx )
        return 0;
    hp->seek_mode              = opt->seek_mode;
    hp->forward_seek_threshold = opt->forward_seek_threshold;
    hp->video_input_buffer_size += FF_INPUT_BUFFER_PADDING_SIZE;
    hp->video_input_buffer = av_mallocz( hp->video_input_buffer_size );
    if( !hp->video_input_buffer )
    {
        DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to allocate memory to the input buffer for video." );
        return -1;
    }
    h->video_sample_count = hp->video_frame_count;
    if( hp->video_index_entries )
    {
        AVStream *video_stream = hp->video_format->streams[ hp->video_index ];
        for( int i = 0; i < hp->video_index_entries_count; i++ )
        {
            AVIndexEntry *ie = &hp->video_index_entries[i];
            if( av_add_index_entry( video_stream, ie->pos, ie->timestamp, ie->size, ie->min_distance, ie->flags ) < 0 )
                return -1;
        }
        av_free( hp->video_index_entries );
        hp->video_index_entries = NULL;
    }
    setup_timestamp_info( h );
    /* swscale */
    hp->video_ctx->width   = hp->video_width;
    hp->video_ctx->height  = hp->video_height;
    hp->video_ctx->pix_fmt = hp->pix_fmt;
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
    /* Find the first valid video frame. */
    hp->video_seek_flags = (hp->video_seek_base & SEEK_FILE_OFFSET_BASED) ? AVSEEK_FLAG_BYTE : hp->video_seek_base == 0 ? AVSEEK_FLAG_FRAME : 0;
    if( h->video_sample_count != 1 )
    {
        hp->video_seek_flags |= AVSEEK_FLAG_BACKWARD;
        uint32_t rap_number;
        find_random_accessible_point( hp, 1, 0, &rap_number );
        int64_t rap_pos = get_random_accessible_point_position( h, rap_number );
        if( av_seek_frame( hp->video_format, hp->video_index, rap_pos, hp->video_seek_flags ) < 0 )
            av_seek_frame( hp->video_format, hp->video_index, rap_pos, hp->video_seek_flags | AVSEEK_FLAG_ANY );
    }
    for( uint32_t i = 1; i <= h->video_sample_count + get_decoder_delay( hp->video_ctx ); i++ )
    {
        AVPacket pkt;
        get_sample( hp->video_format, hp->video_index, &hp->video_input_buffer, &hp->video_input_buffer_size, &pkt );
        AVFrame picture;
        avcodec_get_frame_defaults( &picture );
        int got_picture;
        if( avcodec_decode_video2( hp->video_ctx, &picture, &got_picture, &pkt ) >= 0 && got_picture )
        {
            hp->first_valid_video_frame_number = i - min( get_decoder_delay( hp->video_ctx ), hp->video_delay_count );
            if( hp->first_valid_video_frame_number > 1 || h->video_sample_count == 1 )
            {
                hp->first_valid_video_frame_size = MAKE_AVIUTL_PITCH( h->video_format.biWidth * h->video_format.biBitCount )
                                                 * h->video_format.biHeight;
                hp->first_valid_video_frame_data = malloc( hp->first_valid_video_frame_size );
                if( !hp->first_valid_video_frame_data )
                    return -1;
                if( hp->first_valid_video_frame_size > hp->convert_colorspace( hp->video_ctx, hp->sws_ctx, &picture, hp->first_valid_video_frame_data ) )
                    continue;
            }
            break;
        }
        else if( pkt.data )
            ++ hp->video_delay_count;
    }
    hp->last_video_frame_number = h->video_sample_count + 1;   /* Force seeking at the first reading. */
    return 0;
}

static int prepare_audio_decoding( lsmash_handler_t *h )
{
    libav_handler_t *hp = (libav_handler_t *)h->audio_private;
    if( !hp->audio_ctx )
        return 0;
    hp->audio_input_buffer_size += FF_INPUT_BUFFER_PADDING_SIZE;
    hp->audio_input_buffer = av_mallocz( hp->audio_input_buffer_size );
    if( !hp->audio_input_buffer )
    {
        DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to allocate memory to the input buffer for audio." );
        return -1;
    }
    for( uint32_t i = 1; i <= hp->audio_frame_count; i++ )
        h->audio_pcm_sample_count += hp->audio_frame_list[i].length;
    if( h->audio_pcm_sample_count == 0 )
    {
        DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "No valid audio frame." );
        return -1;
    }
    if( hp->audio_index_entries )
    {
        AVStream *audio_stream = hp->audio_format->streams[ hp->audio_index ];
        for( int i = 0; i < hp->audio_index_entries_count; i++ )
        {
            AVIndexEntry *ie = &hp->audio_index_entries[i];
            if( av_add_index_entry( audio_stream, ie->pos, ie->timestamp, ie->size, ie->min_distance, ie->flags ) < 0 )
                return -1;
        }
        av_free( hp->audio_index_entries );
        hp->audio_index_entries = NULL;
    }
    avcodec_get_frame_defaults( &hp->audio_frame_buffer );
    if( h->audio_pcm_sample_count * 2 <= hp->audio_frame_count * hp->audio_frame_length )
        /* for HE-AAC upsampling */
        h->audio_pcm_sample_count *= 2;
    h->audio_pcm_sample_count += hp->av_gap;
    hp->next_audio_pcm_sample_number = h->audio_pcm_sample_count + 1;   /* Force seeking at the first reading. */
    /* Set up resampler. */
    hp->avr_ctx = avresample_alloc_context();
    if( !hp->avr_ctx )
    {
        DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to avresample_alloc_context." );
        return -1;
    }
    if( hp->audio_ctx->channel_layout == 0 )
        hp->audio_ctx->channel_layout = av_get_default_channel_layout( hp->audio_ctx->channels );
    hp->audio_output_sample_format = decide_audio_output_sample_format( hp->audio_ctx->sample_fmt );
    av_opt_set_int( hp->avr_ctx, "in_channel_layout",   hp->audio_ctx->channel_layout,  0 );
    av_opt_set_int( hp->avr_ctx, "in_sample_fmt",       hp->audio_ctx->sample_fmt,      0 );
    av_opt_set_int( hp->avr_ctx, "in_sample_rate",      hp->audio_ctx->sample_rate,     0 );
    av_opt_set_int( hp->avr_ctx, "out_channel_layout",  hp->audio_ctx->channel_layout,  0 );
    av_opt_set_int( hp->avr_ctx, "out_sample_fmt",      hp->audio_output_sample_format, 0 );
    av_opt_set_int( hp->avr_ctx, "out_sample_rate",     hp->audio_ctx->sample_rate,     0 );
    av_opt_set_int( hp->avr_ctx, "internal_sample_fmt", AV_SAMPLE_FMT_FLTP,             0 );
    if( avresample_open( hp->avr_ctx ) < 0 )
    {
        DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to open resampler." );
        return -1;
    }
    /* Decide output Bits Per Sample. */
    int output_bits_per_sample;
    if( hp->audio_output_sample_format != AV_SAMPLE_FMT_S32 || hp->audio_ctx->bits_per_raw_sample != 24 )
        output_bits_per_sample = av_get_bytes_per_sample( hp->audio_output_sample_format ) * 8;
    else
    {
        /* 24bit signed integer output */
        if( hp->audio_frame_length )
        {
            hp->audio_resampled_buffer_size = get_linesize( hp->audio_ctx->channels, hp->audio_frame_length, hp->audio_output_sample_format );
            hp->audio_resampled_buffer      = av_malloc( hp->audio_resampled_buffer_size );
            if( !hp->audio_resampled_buffer )
            {
                DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to allocate memory for resampling." );
                return -1;
            }
        }
        hp->audio_s24_output   = 1;
        output_bits_per_sample = 24;
    }
    /* WAVEFORMATEXTENSIBLE (WAVEFORMATEX) */
    WAVEFORMATEX *Format = &h->audio_format.Format;
    Format->nChannels       = hp->audio_ctx->channels;
    Format->nSamplesPerSec  = hp->audio_ctx->sample_rate;
    Format->wBitsPerSample  = output_bits_per_sample;
    Format->nBlockAlign     = (Format->nChannels * Format->wBitsPerSample) / 8;
    Format->nAvgBytesPerSec = Format->nSamplesPerSec * Format->nBlockAlign;
    Format->wFormatTag      = Format->wBitsPerSample == 8 || Format->wBitsPerSample == 16 ? WAVE_FORMAT_PCM : WAVE_FORMAT_EXTENSIBLE;
    if( Format->wFormatTag == WAVE_FORMAT_EXTENSIBLE )
    {
        Format->cbSize = sizeof( WAVEFORMATEXTENSIBLE ) - sizeof( WAVEFORMATEX );
        h->audio_format.Samples.wValidBitsPerSample = Format->wBitsPerSample;
        h->audio_format.dwChannelMask               = hp->audio_ctx->channel_layout;
        h->audio_format.SubFormat                   = KSDATAFORMAT_SUBTYPE_PCM;
    }
    else
        Format->cbSize = 0;
    /* Set up the number of planes and the block alignment of decoded and output data. */
    if( av_sample_fmt_is_planar( hp->audio_ctx->sample_fmt ) )
    {
        hp->audio_planes            = Format->nChannels;
        hp->audio_input_block_align = av_get_bytes_per_sample( hp->audio_ctx->sample_fmt );
    }
    else
    {
        hp->audio_planes            = 1;
        hp->audio_input_block_align = av_get_bytes_per_sample( hp->audio_ctx->sample_fmt ) * Format->nChannels;
    }
    hp->audio_output_block_align = Format->nBlockAlign;
    DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_OK, "frame_length = %"PRIu32", channels = %d, sampling_rate = %d, bits_per_sample = %d, block_align = %d, avg_bps = %d",
                                     hp->audio_frame_length, Format->nChannels, Format->nSamplesPerSec,
                                     Format->wBitsPerSample, Format->nBlockAlign, Format->nAvgBytesPerSec );
    return 0;
}

static int decode_video_sample( libav_handler_t *hp, AVFrame *picture, int *got_picture, uint32_t sample_number )
{
    AVPacket pkt;
    if( get_sample( hp->video_format, hp->video_index, &hp->video_input_buffer, &hp->video_input_buffer_size, &pkt ) )
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

static void flush_buffers( AVCodecContext *ctx, int *error )
{
    /* Close and reopen the decoder even if the decoder implements avcodec_flush_buffers().
     * It seems this brings about more stable composition when seeking. */
    const AVCodec *codec = ctx->codec;
    avcodec_close( ctx );
    if( avcodec_open2( ctx, codec, NULL ) < 0 )
    {
        MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to flush buffers.\nIt is recommended you reopen the file." );
        *error = 1;
    }
}

static uint32_t seek_video( libav_handler_t *hp, AVFrame *picture,
                            uint32_t presentation_sample_number, uint32_t rap_number,
                            int64_t rap_pos, int error_ignorance )
{
    /* Prepare to decode from random accessible sample. */
    flush_buffers( hp->video_ctx, &hp->video_error );
    if( hp->video_error )
        return 0;
    if( av_seek_frame( hp->video_format, hp->video_index, rap_pos, hp->video_seek_flags ) < 0 )
        av_seek_frame( hp->video_format, hp->video_index, rap_pos, hp->video_seek_flags | AVSEEK_FLAG_ANY );
    hp->video_delay_count = 0;
    hp->decode_status     = DECODE_REQUIRE_INITIAL;
    int dummy;
    uint32_t i;
    for( i = rap_number; i < presentation_sample_number + get_decoder_delay( hp->video_ctx ); i++ )
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
    hp->video_delay_count = min( get_decoder_delay( hp->video_ctx ), i - rap_number );
    DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_OK, "rap_number = %"PRIu32", seek_position = %"PRIu32" video_delay_count = %"PRIu32,
                                     rap_number, i, hp->video_delay_count );
    return i;
}

static int get_picture( libav_handler_t *hp, AVFrame *picture, uint32_t current, uint32_t goal, uint32_t video_sample_count )
{
    if( hp->decode_status == DECODE_INITIALIZING )
    {
        if( hp->video_delay_count > get_decoder_delay( hp->video_ctx ) )
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
        if( hp->video_delay_count > get_decoder_delay( hp->video_ctx ) && hp->decode_status == DECODE_INITIALIZED )
            break;
    }
    /* Flush the last frames. */
    if( current > video_sample_count && get_decoder_delay( hp->video_ctx ) )
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
    libav_handler_t *hp = (libav_handler_t *)h->video_private;
    if( hp->video_error )
        return 0;
    ++sample_number;            /* sample_number is 1-origin. */
    if( sample_number < hp->first_valid_video_frame_number || h->video_sample_count == 1 )
    {
        /* Copy the first valid video frame data. */
        memcpy( buf, hp->first_valid_video_frame_data, hp->first_valid_video_frame_size );
        hp->last_video_frame_number = h->video_sample_count + 1;   /* Force seeking at the next access for valid video frame. */
        return hp->first_valid_video_frame_size;
    }
    AVFrame picture;            /* Decoded video data will be stored here. */
    uint32_t start_number;      /* number of sample, for normal decoding, where decoding starts excluding decoding delay */
    uint32_t rap_number;        /* number of sample, for seeking, where decoding starts excluding decoding delay */
    int seek_mode = hp->seek_mode;
    int64_t rap_pos = INT64_MIN;
    if( sample_number > hp->last_video_frame_number
     && sample_number <= hp->last_video_frame_number + hp->forward_seek_threshold )
    {
        start_number = hp->last_video_frame_number + 1 + hp->video_delay_count;
        rap_number = hp->last_rap_number;
    }
    else
    {
        find_random_accessible_point( hp, sample_number, 0, &rap_number );
        if( rap_number == hp->last_rap_number && sample_number > hp->last_video_frame_number )
            start_number = hp->last_video_frame_number + 1 + hp->video_delay_count;
        else
        {
            /* Require starting to decode from random accessible sample. */
            rap_pos = get_random_accessible_point_position( h, rap_number );
            hp->last_rap_number = rap_number;
            start_number = seek_video( hp, &picture, sample_number, rap_number, rap_pos, seek_mode != SEEK_MODE_NORMAL );
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
            find_random_accessible_point( hp, sample_number, rap_number - 1, &rap_number );
            rap_pos = get_random_accessible_point_position( h, rap_number );
            hp->last_rap_number = rap_number;
        }
        start_number = seek_video( hp, &picture, sample_number, rap_number, rap_pos, seek_mode != SEEK_MODE_NORMAL );
    }
    hp->last_video_frame_number = sample_number;
    DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_OK, "src_linesize[0] = %d, src_linesize[1] = %d, src_linesize[2] = %d, src_linesize[3] = %d",
                                     picture.linesize[0], picture.linesize[1], picture.linesize[2], picture.linesize[3] );
    return hp->convert_colorspace( hp->video_ctx, hp->sws_ctx, &picture, buf );
video_fail:
    /* fatal error of decoding */
    DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Couldn't read video frame." );
    return 0;
#undef MAX_ERROR_COUNT
}

static int find_start_audio_frame( libav_handler_t *hp, uint64_t start_frame_pos, uint64_t *start_offset )
{
    int frame_number = 1;
    uint64_t next_frame_pos = 0;
    uint64_t frame_length   = 0;
    do
    {
        frame_length = hp->audio_frame_list[frame_number].length;
        next_frame_pos += frame_length;
        if( start_frame_pos < next_frame_pos )
            break;
        ++frame_number;
    } while( frame_number <= hp->audio_frame_count );
    *start_offset = start_frame_pos + frame_length - next_frame_pos;
    return frame_number;
}

static void seek_audio( libav_handler_t *hp, uint32_t frame_number, AVPacket *pkt )
{
    /* Get an unique value of the closest past audio keyframe. */
    uint32_t rap_number = frame_number;
    while( rap_number && !hp->audio_frame_list[rap_number].keyframe )
        --rap_number;
    if( rap_number == 0 )
        rap_number = 1;
    int64_t rap_pos = (hp->audio_seek_base & SEEK_FILE_OFFSET_BASED) ? hp->audio_frame_list[rap_number].file_offset
                    : (hp->audio_seek_base & SEEK_PTS_BASED)         ? hp->audio_frame_list[rap_number].pts
                    : (hp->audio_seek_base & SEEK_DTS_BASED)         ? hp->audio_frame_list[rap_number].dts
                    :                                                  hp->audio_frame_list[rap_number].sample_number;
    /* Seek to audio keyframe.
     * Note: av_seek_frame() for DV in AVI Type-1 requires stream_index = 0. */
    int flags = (hp->audio_seek_base & SEEK_FILE_OFFSET_BASED) ? AVSEEK_FLAG_BYTE : hp->audio_seek_base == 0 ? AVSEEK_FLAG_FRAME : 0;
    int stream_index = hp->dv_in_avi == 1 ? 0 : hp->audio_index;
    if( av_seek_frame( hp->audio_format, stream_index, rap_pos, flags | AVSEEK_FLAG_BACKWARD ) < 0 )
        av_seek_frame( hp->audio_format, stream_index, rap_pos, flags | AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY );
    /* Seek to the target audio frame and get it. */
    for( uint32_t i = rap_number; i <= frame_number; )
    {
        if( get_sample( hp->audio_format, hp->audio_index, &hp->audio_input_buffer, &hp->audio_input_buffer_size, pkt ) )
            break;
        if( i == rap_number
         && (((hp->audio_seek_base & SEEK_FILE_OFFSET_BASED) && (pkt->pos == -1 || hp->audio_frame_list[i].file_offset > pkt->pos))
         ||  ((hp->audio_seek_base & SEEK_PTS_BASED)         && (pkt->pts == AV_NOPTS_VALUE || hp->audio_frame_list[i].pts > pkt->pts))
         ||  ((hp->audio_seek_base & SEEK_DTS_BASED)         && (pkt->dts == AV_NOPTS_VALUE || hp->audio_frame_list[i].dts > pkt->dts))) )
            continue;   /* Seeking was too backward. */
        ++i;
    }
}

static void waste_decoded_audio_samples( libav_handler_t *hp, int wasted_sample_count, uint8_t **out_data, int sample_offset )
{
    /* Input */
    int decoded_data_offset = sample_offset * hp->audio_input_block_align;
    uint8_t *in_data[ hp->audio_planes ];
    for( int i = 0; i < hp->audio_planes; i++ )
        in_data[i] = hp->audio_frame_buffer.extended_data[i] + decoded_data_offset;
    audio_samples_t in;
    in.channel_layout = hp->audio_frame_buffer.channel_layout;
    in.sample_count   = wasted_sample_count;
    in.sample_format  = hp->audio_frame_buffer.format;
    in.data           = in_data;
    /* Output */
    uint8_t *resampled_buffer = NULL;
    int out_channels = get_channel_layout_nb_channels( hp->audio_frame_buffer.channel_layout );
    if( hp->audio_s24_output )
    {
        int out_linesize = get_linesize( out_channels, wasted_sample_count, hp->audio_output_sample_format );
        if( !hp->audio_resampled_buffer || out_linesize > hp->audio_resampled_buffer_size )
        {
            uint8_t *temp = av_realloc( hp->audio_resampled_buffer, out_linesize );
            if( !temp )
                return;
            hp->audio_resampled_buffer_size = out_linesize;
            hp->audio_resampled_buffer      = temp;
        }
        resampled_buffer = hp->audio_resampled_buffer;
    }
    audio_samples_t out;
    out.channel_layout = hp->audio_frame_buffer.channel_layout;
    out.sample_count   = wasted_sample_count;
    out.sample_format  = hp->audio_output_sample_format;
    out.data           = resampled_buffer ? &resampled_buffer : out_data;
    /* Resample */
    int resampled_count = resample_audio( hp->avr_ctx, &out, &in );
    if( resampled_count <= 0 )
        return;
    int resampled_size = resampled_count * av_get_bytes_per_sample( out.sample_format ) * out_channels;
    *out_data += resampled_size;
    if( resampled_buffer )
        resample_s32_to_s24( out_data, hp->audio_resampled_buffer, resampled_size );
}

static inline void waste_remainder_audio_samples( libav_handler_t *hp, int wasted_sample_count, uint8_t **out_data )
{
    waste_decoded_audio_samples( hp, wasted_sample_count, out_data, hp->last_remainder_sample_offset );
    hp->last_remainder_length -= wasted_sample_count;
    if( hp->last_remainder_length == 0 )
        hp->last_remainder_sample_offset = 0;
}

static int read_audio( lsmash_handler_t *h, int start, int wanted_length, void *buf )
{
    DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_OK, "start = %d, wanted_length = %d", start, wanted_length );
    libav_handler_t *hp = (libav_handler_t *)h->audio_private;
    if( hp->audio_error )
        return 0;
    uint32_t frame_number;
    uint64_t seek_offset;
    int      copy_length;
    int      output_length = 0;
    int      already_gotten;
    AVPacket *pkt = &hp->audio_packet;
    if( start > 0 && start == hp->next_audio_pcm_sample_number )
    {
        frame_number = hp->last_audio_frame_number;
        if( hp->last_remainder_length && hp->audio_frame_buffer.extended_data[0] )
        {
            copy_length = min( hp->last_remainder_length, wanted_length );
            waste_remainder_audio_samples( hp, copy_length, (uint8_t **)&buf );
            output_length += copy_length;
            wanted_length -= copy_length;
            if( wanted_length <= 0 )
                goto audio_out;
        }
        if( pkt->size <= 0 )
            ++frame_number;
        seek_offset = 0;
        already_gotten = 0;
    }
    else
    {
        /* Seek audio stream. */
        flush_buffers( hp->audio_ctx, &hp->audio_error );
        if( hp->audio_error )
            return 0;
        hp->audio_delay_count            = 0;
        hp->last_remainder_length        = 0;
        hp->last_remainder_sample_offset = 0;
        hp->next_audio_pcm_sample_number = 0;
        hp->last_audio_frame_number      = 0;
        uint64_t start_frame_pos;
        if( start >= 0 )
            start_frame_pos = start;
        else
        {
            int silence_length = -start;
            put_silence_audio_samples( silence_length * hp->audio_output_block_align, (uint8_t **)&buf );
            output_length += silence_length;
            wanted_length -= silence_length;
            start_frame_pos = 0;
        }
        frame_number = find_start_audio_frame( hp, start_frame_pos, &seek_offset );
        seek_audio( hp, frame_number, pkt );
        already_gotten = 1;
    }
    do
    {
        if( already_gotten )
            already_gotten = 0;
        else if( frame_number > hp->audio_frame_count )
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
            {
                copy_length = 0;
                goto audio_out;
            }
        }
        else if( pkt->size <= 0 )
            get_sample( hp->audio_format, hp->audio_index, &hp->audio_input_buffer, &hp->audio_input_buffer_size, pkt );
        int output_audio = 0;
        do
        {
            hp->last_remainder_length        = 0;
            hp->last_remainder_sample_offset = 0;
            copy_length = 0;
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
            if( decode_complete && hp->audio_frame_buffer.extended_data[0] )
            {
                int decoded_length = hp->audio_frame_buffer.nb_samples;
                if( decoded_length > seek_offset )
                {
                    copy_length = min( decoded_length - seek_offset, wanted_length );
                    waste_decoded_audio_samples( hp, copy_length, (uint8_t **)&buf, seek_offset );
                    output_length += copy_length;
                    wanted_length -= copy_length;
                    seek_offset = 0;
                    if( wanted_length <= 0 )
                    {
                        hp->last_remainder_length = decoded_length - copy_length;
                        goto audio_out;
                    }
                }
                else
                    seek_offset -= decoded_length;
                output_audio = 1;
            }
        } while( pkt->size > 0 );
        if( !output_audio && pkt->data )    /* Count audio frame delay only if feeding non-NULL packet. */
            ++ hp->audio_delay_count;
        DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_OK, "frame_number = %d, decoded_length = %d, copied_length = %d, output_length = %d",
                                         frame_number, hp->audio_frame_buffer.nb_samples, copy_length, output_length );
        ++frame_number;
    } while( 1 );
audio_out:
    DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_OK, "output_length = %d, remainder = %d", output_length, hp->last_remainder_length );
    hp->next_audio_pcm_sample_number = start + output_length;
    hp->last_audio_frame_number = frame_number;
    if( hp->last_remainder_length )
        hp->last_remainder_sample_offset += copy_length;
    return output_length;
}

static int is_keyframe( lsmash_handler_t *h, int sample_number )
{
    libav_handler_t *hp = (libav_handler_t *)h->video_private;
    return hp->video_frame_list[sample_number + 1].keyframe;
}

static int delay_audio( lsmash_handler_t *h, int *start, int wanted_length, int audio_delay )
{
    /* Even if start become negative, its absolute value shall be equal to wanted_length or smaller. */
    libav_handler_t *hp = (libav_handler_t *)h->audio_private;
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
    libav_handler_t *hp = (libav_handler_t *)h->video_private;
    if( !hp )
        return;
    if( hp->video_input_buffer )
        av_free( hp->video_input_buffer );
    if( hp->first_valid_video_frame_data )
        free( hp->first_valid_video_frame_data );
    if( hp->video_frame_list )
        free( hp->video_frame_list );
    if( hp->order_converter )
        free( hp->order_converter );
    if( hp->keyframe_list )
        free( hp->keyframe_list );
    if( hp->video_index_entries )
        av_free( hp->video_index_entries );
    if( hp->sws_ctx )
        sws_freeContext( hp->sws_ctx );
    if( hp->video_format )
        lavf_close_file( &hp->video_format );
}

static void audio_cleanup( lsmash_handler_t *h )
{
    libav_handler_t *hp = (libav_handler_t *)h->audio_private;
    if( !hp )
        return;
    if( hp->audio_input_buffer )
        av_free( hp->audio_input_buffer );
    if( hp->audio_resampled_buffer )
        av_free( hp->audio_resampled_buffer );
    if( hp->audio_index_entries )
        av_free( hp->audio_index_entries );
    if( hp->avr_ctx )
        avresample_free( &hp->avr_ctx );
    if( hp->audio_ctx )
        avcodec_close( hp->audio_ctx );
    if( hp->audio_format )
        lavf_close_file( &hp->audio_format );
}

static void close_file( void *private_stuff )
{
    libav_handler_t *hp = (libav_handler_t *)private_stuff;
    if( !hp )
        return;
    if( hp->file_path )
        free( hp->file_path );
    free( hp );
}

lsmash_reader_t libav_reader =
{
    LIBAV_READER,
    open_file,
    get_video_track,
    get_audio_track,
    NULL,
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
