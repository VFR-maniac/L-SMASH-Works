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
#include <libavformat/avformat.h>   /* Demuxer */
#include <libavcodec/avcodec.h>     /* Decoder */
#include <libswscale/swscale.h>     /* Colorspace converter */
#include <libavutil/mathematics.h>  /* Timebase rescaler */
#include <libavutil/pixdesc.h>

#include "lsmashinput.h"
#include "colorspace.h"
#include "progress_dlg.h"

#define DECODER_DELAY( ctx ) (ctx->has_b_frames + ((ctx->active_thread_type & FF_THREAD_FRAME) ? ctx->thread_count - 1 : 0))

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
    uint32_t sample_number;
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
    /* Video stuff */
    int                      video_error;
    int                      video_index;
    enum CodecID             video_codec_id;
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
    int                      reordering_present;
    int                      video_seek_flags;
    int                      video_seek_base;
    int                      seek_mode;
    uint32_t                 forward_seek_threshold;
    func_convert_colorspace *convert_colorspace;
    /* Audio stuff */
    int                      audio_error;
    int                      audio_index;
    enum CodecID             audio_codec_id;
    AVFormatContext         *audio_format;
    AVCodecContext          *audio_ctx;
    AVCodecParserContext    *audio_parser;
    AVIndexEntry            *audio_index_entries;
    int                      audio_index_entries_count;
    AVFrame                  audio_frame_buffer;
    AVPacket                 audio_packet;
    uint8_t                 *audio_input_buffer;
    uint32_t                 audio_input_buffer_size;
    uint32_t                 audio_frame_count;
    uint32_t                 audio_delay_count;
    uint32_t                 audio_frame_length;
    audio_frame_info_t      *audio_frame_list;
    int                      audio_seek_base;
    uint32_t                 next_audio_pcm_sample_number;
    uint32_t                 last_audio_frame_number;
    uint32_t                 last_remainder_size;
    int64_t                  av_gap;
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

static int open_decoder( AVCodecContext *ctx, enum CodecID codec_id, int threads )
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

static int decide_video_seek_method( libav_handler_t *hp, uint32_t sample_count )
{
    hp->video_seek_base = !strcmp( hp->format_name, "mpeg" ) || !strcmp( hp->format_name, "mpegts" )
                        ? SEEK_DTS_BASED | SEEK_PTS_BASED | SEEK_FILE_OFFSET_BASED
                        : SEEK_DTS_BASED | SEEK_PTS_BASED;
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
            for( uint32_t i = 1; i <= sample_count; i++ )
                if( info[i].file_offset == -1 )
                {
                    hp->video_seek_base &= ~SEEK_FILE_OFFSET_BASED;
                    break;
                }
    }
    if( hp->video_seek_base & SEEK_PTS_BASED )
    {
        if( check_frame_reordering( info, sample_count ) )
        {
            /* Consider presentation order for keyframe detection.
             * Note: sample number is 1-origin. */
            sort_presentation_order( &info[1], sample_count );
            hp->reordering_present = 1;
            if( hp->video_seek_base & SEEK_DTS_BASED )
            {
                hp->order_converter = malloc_zero( (sample_count + 1) * sizeof(order_converter_t) );
                if( !hp->order_converter )
                {
                    DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to allocate memory." );
                    return -1;
                }
                video_timestamp_t timestamp[sample_count + 1];
                for( uint32_t i = 1; i <= sample_count; i++ )
                {
                    timestamp[i].pts = i;
                    timestamp[i].dts = info[i].dts;
                }
                sort_decoding_order( &timestamp[1], sample_count );
                for( uint32_t i = 1; i <= sample_count; i++ )
                    hp->order_converter[i].decoding_to_presentation = timestamp[i].pts;
            }
        }
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
    if( hp->order_converter || !hp->reordering_present )
        for( uint32_t i = 1; i <= sample_count; i++ )
        {
            uint32_t presentation_number = hp->order_converter
                                         ? hp->order_converter[i].decoding_to_presentation
                                         : i;
            hp->keyframe_list[ info[presentation_number].sample_number ] = info[presentation_number].keyframe;
        }
    return 0;
}

static void decide_audio_seek_method( libav_handler_t *hp, uint32_t sample_count )
{
    hp->audio_seek_base = !strcmp( hp->format_name, "mpeg" ) || !strcmp( hp->format_name, "mpegts" )
                        ? SEEK_DTS_BASED | SEEK_PTS_BASED | SEEK_FILE_OFFSET_BASED
                        : SEEK_DTS_BASED | SEEK_PTS_BASED;
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

static void calculate_av_gap( libav_handler_t *hp, video_frame_info_t *video_info, audio_frame_info_t *audio_info,
                              AVRational video_time_base, AVRational audio_time_base, int sample_rate )
{
    int64_t video_ts = (hp->video_seek_base & SEEK_PTS_BASED) ? video_info[1].pts
                     : (hp->video_seek_base & SEEK_DTS_BASED) ? video_info[1].dts
                     :                                          0;
    int64_t audio_ts = (hp->audio_seek_base & SEEK_PTS_BASED) ? audio_info[1].pts
                     : (hp->audio_seek_base & SEEK_DTS_BASED) ? audio_info[1].dts
                     :                                          0;
    if( video_ts || audio_ts )
    {
        AVRational audio_sample_base = (AVRational){ 1, sample_rate };
        hp->av_gap = av_rescale_q( audio_ts, audio_time_base, audio_sample_base )
                   - av_rescale_q( video_ts, video_time_base, audio_sample_base );
    }
}

static void investigate_pix_fmt_by_decoding( AVCodecContext *video_ctx, AVPacket *pkt )
{
    int got_picture;
    AVFrame picture;
    avcodec_get_frame_defaults( &picture );
    avcodec_decode_video2( video_ctx, &picture, &got_picture, pkt );
}

static inline void write_av_index_entry( FILE *index, AVIndexEntry *ie )
{
    fprintf( index, "POS=%"PRId64",TS=%"PRId64",Flags=%x,Size=%d,Distance=%d\n",
             ie->pos, ie->timestamp, ie->flags, ie->size, ie->min_distance );
}

static void create_index( libav_handler_t *hp, AVFormatContext *format_ctx, reader_option_t *opt,
                          int video_present, int audio_present )
{
    uint32_t video_info_count = 1 << 16;
    uint32_t audio_info_count = 1 << 16;
    video_frame_info_t *video_info = NULL;
    audio_frame_info_t *audio_info = NULL;
    if( video_present )
    {
        video_info = malloc_zero( video_info_count * sizeof(video_frame_info_t) );
        if( !video_info )
            return;
    }
    if( audio_present )
    {
        audio_info = malloc_zero( audio_info_count * sizeof(audio_frame_info_t) );
        if( !audio_info )
        {
            if( video_info )
                free( video_info );
            return;
        }
        avcodec_get_frame_defaults( &hp->audio_frame_buffer );
        if( hp->audio_ctx && hp->audio_ctx->frame_size == 0 )
        {
            hp->audio_parser = av_parser_init( hp->audio_ctx->codec_id );
            if( hp->audio_parser )
                hp->audio_parser->flags = PARSER_FLAG_COMPLETE_FRAMES;
        }
    }
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
    FILE *index = fopen( index_path, "wb" );
    if( !index )
        goto fail_index;
    fprintf( index, "<LibavReaderIndexFile=%d>\n", INDEX_FILE_VERSION );
    fprintf( index, "<InputFilePath>%s</InputFilePath>\n", hp->file_path );
    hp->format_name  = (char *)format_ctx->iformat->name;
    hp->format_flags = format_ctx->iformat->flags;
    fprintf( index, "<LibavReaderIndex=0x%08x,%s>\n", hp->format_flags, hp->format_name );
    int32_t video_index_pos = ftell( index );
    fprintf( index, "<ActiveVideoStreamIndex>%+011d</ActiveVideoStreamIndex>\n", hp->video_index );
    fprintf( index, "<ActiveAudioStreamIndex>%+011d</ActiveAudioStreamIndex>\n", hp->audio_index );
    AVPacket pkt;
    av_init_packet( &pkt );
    hp->video_format = format_ctx;
    hp->audio_format = format_ctx;
    int      video_resolution      = hp->video_ctx ? hp->video_ctx->width * hp->video_ctx->height : 0;
    uint32_t video_sample_count    = 0;
    int64_t  last_keyframe_pts     = AV_NOPTS_VALUE;
    uint32_t audio_sample_count    = 0;
    int      constant_frame_length = 1;
    int      frame_length          = 0;
    uint64_t audio_duration        = 0;
    int64_t  first_dts             = AV_NOPTS_VALUE;
    progress_dlg_t prg_dlg;
    init_progress_dlg( &prg_dlg );
    while( av_read_frame( format_ctx, &pkt ) >= 0 )
    {
        frame_length = 0;
        AVCodecContext *pkt_ctx = format_ctx->streams[ pkt.stream_index ]->codec;
        if( video_present )
        {
            if( pkt.stream_index == hp->video_index
             || (!opt->force_video
             &&  pkt_ctx->codec_type == AVMEDIA_TYPE_VIDEO
             &&  pkt_ctx->width * pkt_ctx->height > video_resolution) )
            {
                if( pkt_ctx->pix_fmt == PIX_FMT_NONE && pkt_ctx->codec )
                    investigate_pix_fmt_by_decoding( pkt_ctx, &pkt );
                if( !opt->force_video
                 && pkt.stream_index != hp->video_index
                 && !open_decoder( pkt_ctx, pkt_ctx->codec_id, hp->threads ) )
                {
                    /* Replace lower resolution stream with higher. */
                    if( pkt_ctx->pix_fmt == PIX_FMT_NONE )
                        investigate_pix_fmt_by_decoding( pkt_ctx, &pkt );
                    avcodec_close( hp->video_ctx );
                    hp->video_ctx               = pkt_ctx;
                    hp->video_codec_id          = pkt_ctx->codec_id;
                    hp->video_index             = pkt.stream_index;
                    hp->video_input_buffer_size = 0;
                    video_resolution            = pkt_ctx->width * pkt_ctx->height;
                    video_sample_count          = 0;
                    last_keyframe_pts           = AV_NOPTS_VALUE;
                    int32_t current_pos = ftell( index );
                    fseek( index, video_index_pos, SEEK_SET );
                    fprintf( index, "<ActiveVideoStreamIndex>%+011d</ActiveVideoStreamIndex>\n", hp->video_index );
                    fseek( index, current_pos, SEEK_SET );
                }
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
        }
        if( audio_present && pkt.stream_index == hp->audio_index && audio_duration <= INT32_MAX )
        {
            /* Get frame_length. */
            int output_audio = 0;
            if( hp->audio_parser )
            {
                uint8_t *out_buffer;
                int out_buffer_size;
                AVPacket temp = pkt;
                while( temp.size > 0 )
                {
                    int wasted_data_length = av_parser_parse2( hp->audio_parser, hp->audio_ctx,
                                                               &out_buffer, &out_buffer_size,
                                                               temp.data, temp.size, temp.pts, temp.dts, temp.pos );
                    if( wasted_data_length < 0 )
                        break;
                    temp.size -= wasted_data_length;
                    temp.data += wasted_data_length;
                    if( out_buffer_size )
                    {
                        frame_length += hp->audio_parser->duration;
                        output_audio = 1;
                    }
                }
                if( !output_audio )
                    ++ hp->audio_delay_count;
            }
            if( frame_length == 0 )
                frame_length = hp->audio_ctx->frame_size;
            if( frame_length == 0 )
            {
                if( hp->audio_parser && !output_audio )
                    -- hp->audio_delay_count;
                AVPacket temp = pkt;
                uint8_t *data = pkt.data;
                int decode_complete;
                while( temp.size > 0 )
                {
                    int wasted_data_length = avcodec_decode_audio4( hp->audio_ctx, &hp->audio_frame_buffer, &decode_complete, &temp );
                    if( wasted_data_length < 0 )
                        break;
                    temp.size -= wasted_data_length;
                    temp.data += wasted_data_length;
                    if( decode_complete )
                    {
                        frame_length += hp->audio_frame_buffer.nb_samples;
                        output_audio = 1;
                    }
                }
                if( !output_audio )
                    ++ hp->audio_delay_count;
                pkt      = temp;
                pkt.data = data;
            }
            audio_duration += frame_length;
            if( audio_duration <= INT32_MAX )
            {
                /* Set up audio frame info. */
                ++audio_sample_count;
                audio_info[audio_sample_count].pts           = pkt.pts;
                audio_info[audio_sample_count].dts           = pkt.dts;
                audio_info[audio_sample_count].file_offset   = pkt.pos;
                audio_info[audio_sample_count].sample_number = audio_sample_count;
                if( audio_sample_count > hp->audio_delay_count )
                {
                    uint32_t audio_frame_number = audio_sample_count - hp->audio_delay_count;
                    audio_info[audio_frame_number].length = frame_length;
                    if( audio_frame_number > 1 && audio_info[audio_frame_number].length != audio_info[audio_frame_number - 1].length )
                        constant_frame_length = 0;
                }
                else
                    frame_length = -1;
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
        /* Write a packet info to the index file. */
        if( pkt_ctx->codec_type == AVMEDIA_TYPE_VIDEO )
            fprintf( index, "Index=%d,Type=%d,Codec=%d,TimeBase=%d/%d,POS=%"PRId64",PTS=%"PRId64",DTS=%"PRId64"\n"
                     "Key=%d,Width=%d,Height=%d,PixelFormat=%s\n",
                     pkt.stream_index, AVMEDIA_TYPE_VIDEO, pkt_ctx->codec_id,
                     format_ctx->streams[ pkt.stream_index ]->time_base.num,
                     format_ctx->streams[ pkt.stream_index ]->time_base.den,
                     pkt.pos, pkt.pts, pkt.dts,
                     !!(pkt.flags & AV_PKT_FLAG_KEY), pkt_ctx->width, pkt_ctx->height,
                     av_get_pix_fmt_name( pkt_ctx->pix_fmt ) ? av_get_pix_fmt_name( pkt_ctx->pix_fmt ) : "none" );
        else if( pkt_ctx->codec_type == AVMEDIA_TYPE_AUDIO )
        {
            fprintf( index, "Index=%d,Type=%d,Codec=%d,TimeBase=%d/%d,POS=%"PRId64",PTS=%"PRId64",DTS=%"PRId64"\n"
                     "Channels=%d,SampleRate=%d,BitsPerSample=%d,Length=%d\n",
                     pkt.stream_index, AVMEDIA_TYPE_AUDIO, pkt_ctx->codec_id,
                     format_ctx->streams[ pkt.stream_index ]->time_base.num,
                     format_ctx->streams[ pkt.stream_index ]->time_base.den,
                     pkt.pos, pkt.pts, pkt.dts,
                     pkt_ctx->channels, pkt_ctx->sample_rate,
                     pkt_ctx->bits_per_raw_sample > 0 ? pkt_ctx->bits_per_raw_sample : av_get_bytes_per_sample( pkt_ctx->sample_fmt ) << 3,
                     pkt.stream_index == hp->audio_index ? frame_length : pkt_ctx->frame_size );
        }
        /* Update progress dialog if packet's DTS is valid. */
        if( first_dts == AV_NOPTS_VALUE )
            first_dts = pkt.dts;
        int percent = first_dts == AV_NOPTS_VALUE || pkt.dts == AV_NOPTS_VALUE
                    ? 0
                    : 100.0 * (pkt.dts - first_dts)
                    * (format_ctx->streams[ pkt.stream_index ]->time_base.num / (double)format_ctx->streams[ pkt.stream_index ]->time_base.den)
                    / (format_ctx->duration / AV_TIME_BASE) + 0.5;
        int abort = update_progress_dlg( &prg_dlg, "Creating Index file", percent );
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
    if( hp->audio_index >= 0 )
    {
        for( uint32_t i = 1; i <= hp->audio_delay_count; i++ )
        {
            AVPacket null_pkt;
            av_init_packet( &null_pkt );
            null_pkt.data = NULL;
            null_pkt.size = 0;
            int decode_complete;
            if( avcodec_decode_audio4( hp->audio_ctx, &hp->audio_frame_buffer, &decode_complete, &null_pkt ) >= 0 && decode_complete )
            {
                frame_length = hp->audio_frame_buffer.nb_samples;
                audio_duration += frame_length;
                if( audio_duration > INT32_MAX )
                    break;
                uint32_t audio_frame_number = audio_sample_count - hp->audio_delay_count + i;
                audio_info[audio_frame_number].length = frame_length;
                if( audio_frame_number > 1 && audio_info[audio_frame_number].length != audio_info[audio_frame_number - 1].length )
                    constant_frame_length = 0;
                fprintf( index, "Index=%d,Type=%d,Codec=%d,TimeBase=%d/%d,POS=%"PRId64",PTS=%"PRId64",DTS=%"PRId64"\n"
                         "Channels=%d,SampleRate=%d,BitsPerSample=%d,Length=%d\n",
                         hp->audio_index, AVMEDIA_TYPE_AUDIO, hp->audio_ctx->codec_id,
                         format_ctx->streams[ hp->audio_index ]->time_base.num,
                         format_ctx->streams[ hp->audio_index ]->time_base.den,
                         -1LL, AV_NOPTS_VALUE, AV_NOPTS_VALUE,
                         0, 0, 0, frame_length );
            }
        }
        hp->audio_frame_length = constant_frame_length ? frame_length : 0;
        hp->audio_frame_list   = audio_info;
        hp->audio_frame_count  = audio_sample_count;
        decide_audio_seek_method( hp, audio_sample_count );
        if( hp->video_index >= 0 )
        {
            AVStream *audio_stream = format_ctx->streams[ hp->audio_index ];
            calculate_av_gap( hp, video_info, audio_info,
                              format_ctx->streams[ hp->video_index ]->time_base, audio_stream->time_base,
                              audio_stream->codec->sample_rate );
        }
    }
    fprintf( index, "</LibavReaderIndex>\n" );
    for( int stream_index = 0; stream_index < format_ctx->nb_streams; stream_index++ )
    {
        AVStream *stream = format_ctx->streams[stream_index];
        if( stream->codec->codec_type == AVMEDIA_TYPE_VIDEO )
        {
            fprintf( index, "<StreamIndexEntries=%d,%d,%d>\n", stream_index, AVMEDIA_TYPE_VIDEO, stream->nb_index_entries );
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
            fprintf( index, "</StreamIndexEntries>\n" );
        }
        else if( stream->codec->codec_type == AVMEDIA_TYPE_AUDIO )
        {
            fprintf( index, "<StreamIndexEntries=%d,%d,%d>\n", stream_index, AVMEDIA_TYPE_AUDIO, stream->nb_index_entries );
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
            fprintf( index, "</StreamIndexEntries>\n" );
        }
    }
    hp->video_format = NULL;
    hp->audio_format = NULL;
    fprintf( index, "</LibavReaderIndexFile>\n" );
    fclose( index );
    close_progress_dlg( &prg_dlg );
    return;
fail_index:
    if( video_info )
        free( video_info );
    if( audio_info )
        free( audio_info );
    hp->video_format = NULL;
    hp->audio_format = NULL;
    fclose( index );
    close_progress_dlg( &prg_dlg );
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
    hp->video_codec_id = CODEC_ID_NONE;
    hp->audio_codec_id = CODEC_ID_NONE;
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
            if( stream_index == hp->video_index )
            {
                int key;
                int width;
                int height;
                char pix_fmt[64];
                if( sscanf( buf, "Key=%d,Width=%d,Height=%d,PixelFormat=%s",
                    &key, &width, &height, pix_fmt ) != 4 )
                    goto fail_parsing;
                if( hp->video_codec_id == CODEC_ID_NONE )
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
                if( hp->audio_codec_id == CODEC_ID_NONE )
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
                if( audio_sample_count > hp->audio_delay_count )
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
        if( codec_type == AVMEDIA_TYPE_VIDEO && stream_index == hp->video_index )
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
        else if( codec_type == AVMEDIA_TYPE_AUDIO && stream_index == hp->audio_index )
        {
            hp->audio_frame_length = constant_frame_length ? audio_info[1].length : 0;
            hp->audio_frame_list   = audio_info;
            hp->audio_frame_count  = audio_sample_count;
            decide_audio_seek_method( hp, audio_sample_count );
        }
        ++stream_index;
    }
    if( !strncmp( buf, "</LibavReaderIndexFile>", strlen( "</LibavReaderIndexFile>" ) ) )
    {
        if( hp->video_index >= 0 && hp->audio_index >= 0 )
            calculate_av_gap( hp, video_info, audio_info,
                              video_time_base, audio_time_base,
                              audio_sample_rate );
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
            avformat_close_input( &format_ctx );
        free( hp->file_path );
        free( hp );
        return NULL;
    }
    hp->threads     = opt->threads;
    hp->video_index = -1;
    hp->audio_index = -1;
    int video_resolution = 0;
    int video_present = 0;
    int audio_present = 0;
    for( int index = 0; index < format_ctx->nb_streams; index++ )
    {
        AVCodecContext *ctx = format_ctx->streams[index]->codec;
        if( ctx->codec_type == AVMEDIA_TYPE_VIDEO )
        {
            if( open_decoder( ctx, ctx->codec_id, hp->threads ) )
                continue;
            video_present = 1;
            int is_forced_index = (opt->force_video && index == opt->force_video_index);
            if( is_forced_index || (!opt->force_video && ctx->width * ctx->height > video_resolution) )
            {
                if( hp->video_ctx )
                    avcodec_close( hp->video_ctx );
                hp->video_ctx      = ctx;
                hp->video_codec_id = ctx->codec_id;
                hp->video_index    = index;
                if( is_forced_index )
                    break;
                else
                    video_resolution = ctx->width * ctx->height;
            }
            else
                avcodec_close( ctx );
        }
    }
    for( int index = 0; index < format_ctx->nb_streams; index++ )
    {
        AVCodecContext *ctx = format_ctx->streams[index]->codec;
        if( ctx->codec_type == AVMEDIA_TYPE_AUDIO )
        {
            if( open_decoder( ctx, ctx->codec_id, hp->threads ) )
                continue;
            audio_present = 1;
            if( !opt->force_audio || index == opt->force_audio_index )
            {
                hp->audio_ctx      = ctx;
                hp->audio_codec_id = ctx->codec_id;
                hp->audio_index    = index;
                break;
            }
            else
                avcodec_close( ctx );
        }
    }
    if( !video_present && !audio_present )
    {
        if( hp->video_ctx )
            avcodec_close( hp->video_ctx );
        if( hp->audio_ctx )
            avcodec_close( hp->audio_ctx );
        avformat_close_input( &format_ctx );
        free( hp->file_path );
        free( hp );
        return NULL;
    }
    create_index( hp, format_ctx, opt, video_present, audio_present );
    /* Close file.
     * By opening file for video and audio separately, indecent work about frame reading can be avoidable. */
    if( hp->video_ctx )
    {
        avcodec_close( hp->video_ctx );
        hp->video_ctx = NULL;
    }
    if( hp->audio_ctx )
    {
        avcodec_close( hp->audio_ctx );
        hp->audio_ctx = NULL;
    }
    avformat_close_input( &format_ctx );
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
        if( hp->video_ctx )
        {
            avcodec_close( hp->video_ctx );
            hp->video_ctx = NULL;
        }
        if( hp->video_format )
        {
            avformat_close_input( &hp->video_format );
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
        if( hp->audio_parser )
        {
            av_parser_close( hp->audio_parser );
            hp->audio_parser = NULL;
        }
        if( hp->audio_ctx )
        {
            avcodec_close( hp->audio_ctx );
            hp->audio_ctx = NULL;
        }
        if( hp->audio_format )
        {
            avformat_close_input( &hp->audio_format );
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
    if( hp->order_converter || !hp->reordering_present )
    {
        uint32_t presentation_rap_number = hp->order_converter
                                         ? hp->order_converter[rap_number].decoding_to_presentation
                                         : rap_number;
        return (hp->video_seek_base & SEEK_FILE_OFFSET_BASED) ? hp->video_frame_list[presentation_rap_number].file_offset
             : (hp->video_seek_base & SEEK_PTS_BASED)         ? hp->video_frame_list[presentation_rap_number].pts
             : (hp->video_seek_base & SEEK_DTS_BASED)         ? hp->video_frame_list[presentation_rap_number].dts
             :                                                  hp->video_frame_list[presentation_rap_number].sample_number;
    }
    int64_t rap_pos = INT64_MIN;
    for( uint32_t i = 1; i <= h->video_sample_count; i++ )
        if( rap_number == hp->video_frame_list[i].sample_number )
        {
            rap_pos = (hp->video_seek_base & SEEK_FILE_OFFSET_BASED) ? hp->video_frame_list[i].file_offset
                    : (hp->video_seek_base & SEEK_PTS_BASED)         ? hp->video_frame_list[i].pts
                    : (hp->video_seek_base & SEEK_DTS_BASED)         ? hp->video_frame_list[i].dts
                    :                                                  hp->video_frame_list[i].sample_number;
            break;
        }
    return rap_pos;
}

static int get_sample( AVFormatContext *format_ctx, int stream_index, uint8_t **buffer, uint32_t *buffer_size, AVPacket *pkt )
{
    AVPacket temp;
    av_init_packet( &temp );
    while( av_read_frame( format_ctx, &temp ) >= 0 )
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
    for( uint32_t i = 1; i <= h->video_sample_count + DECODER_DELAY( hp->video_ctx ); i++ )
    {
        AVPacket pkt;
        get_sample( hp->video_format, hp->video_index, &hp->video_input_buffer, &hp->video_input_buffer_size, &pkt );
        AVFrame picture;
        avcodec_get_frame_defaults( &picture );
        int got_picture;
        if( avcodec_decode_video2( hp->video_ctx, &picture, &got_picture, &pkt ) >= 0 && got_picture )
        {
            hp->first_valid_video_frame_number = i - min( DECODER_DELAY( hp->video_ctx ), hp->video_delay_count );
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
    AVCodec *codec = ctx->codec;
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
    for( i = rap_number; i < presentation_sample_number + DECODER_DELAY( hp->video_ctx ); i++ )
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
    DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_OK, "rap_number = %"PRIu32", seek_position = %"PRIu32" video_delay_count = %"PRIu32,
                                     rap_number, i, hp->video_delay_count );
    return i;
}

static int get_picture( libav_handler_t *hp, AVFrame *picture, uint32_t current, uint32_t goal, uint32_t video_sample_count )
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

static int read_audio( lsmash_handler_t *h, int start, int wanted_length, void *buf )
{
    DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_OK, "start = %d, wanted_length = %d", start, wanted_length );
    libav_handler_t *hp = (libav_handler_t *)h->audio_private;
    if( hp->audio_error )
        return 0;
    uint32_t frame_number;
    uint64_t data_offset;
    int      copy_size;
    int      output_length = 0;
    int      block_align = h->audio_format.Format.nBlockAlign;
    int      already_gotten;
    AVPacket *pkt = &hp->audio_packet;
    if( start == hp->next_audio_pcm_sample_number )
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
        if( pkt->size <= 0 )
            ++frame_number;
        data_offset = 0;
        already_gotten = 0;
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
        frame_number = 1;
        uint64_t next_frame_pos = 0;
        uint32_t frame_length   = 0;
        do
        {
            frame_length = hp->audio_frame_list[frame_number].length;
            next_frame_pos += (uint64_t)frame_length;
            if( start < next_frame_pos )
                break;
            ++frame_number;
        } while( frame_number <= hp->audio_frame_count );
        data_offset = (start + frame_length - next_frame_pos) * block_align;
        uint32_t rap_number = frame_number;
        while( rap_number && !hp->audio_frame_list[rap_number].keyframe )
            --rap_number;
        if( rap_number == 0 )
            rap_number = 1;
        int64_t rap_pos = (hp->audio_seek_base & SEEK_FILE_OFFSET_BASED) ? hp->audio_frame_list[rap_number].file_offset
                        : (hp->audio_seek_base & SEEK_PTS_BASED)         ? hp->audio_frame_list[rap_number].pts
                        : (hp->audio_seek_base & SEEK_DTS_BASED)         ? hp->audio_frame_list[rap_number].dts
                        :                                                  hp->audio_frame_list[rap_number].sample_number;
        int flags = (hp->audio_seek_base & SEEK_FILE_OFFSET_BASED) ? AVSEEK_FLAG_BYTE : hp->audio_seek_base == 0 ? AVSEEK_FLAG_FRAME : 0;
        if( av_seek_frame( hp->audio_format, hp->audio_index, rap_pos, flags | AVSEEK_FLAG_BACKWARD ) < 0 )
            av_seek_frame( hp->audio_format, hp->audio_index, rap_pos, flags | AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY );
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
        already_gotten = 1;
    }
    do
    {
        copy_size = 0;
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
                goto audio_out;
        }
        else if( pkt->size <= 0 )
            get_sample( hp->audio_format, hp->audio_index, &hp->audio_input_buffer, &hp->audio_input_buffer_size, pkt );
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
        } while( pkt->size > 0 );
        if( !output_audio && pkt->data )    /* Count audio frame delay only if feeding non-NULL packet. */
            ++ hp->audio_delay_count;
        DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_OK, "frame_number = %d, decoded_length = %d, copied_length = %d, output_length = %d",
                                         frame_number, hp->audio_frame_buffer.nb_samples, copy_size / block_align, output_length );
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
    libav_handler_t *hp = (libav_handler_t *)h->video_private;
    return hp->video_frame_list[sample_number + 1].keyframe;
}

static int delay_audio( lsmash_handler_t *h, int *start, int wanted_length, int audio_delay )
{
    libav_handler_t *hp = (libav_handler_t *)h->audio_private;
    audio_delay += hp->av_gap;
    if( *start < audio_delay )
    {
        if( *start + wanted_length < audio_delay )
        {
            hp->next_audio_pcm_sample_number = h->audio_pcm_sample_count + 1;   /* Force seeking at the next access for valid audio frame. */
            return 0;
        }
        *start = audio_delay - *start;
    }
    else
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
    if( hp->video_ctx )
        avcodec_close( hp->video_ctx );
    if( hp->video_format )
        avformat_close_input( &hp->video_format );
}

static void audio_cleanup( lsmash_handler_t *h )
{
    libav_handler_t *hp = (libav_handler_t *)h->audio_private;
    if( !hp )
        return;
    if( hp->audio_input_buffer )
        av_free( hp->audio_input_buffer );
    if( hp->audio_index_entries )
        av_free( hp->audio_index_entries );
    if( hp->audio_parser )
        av_parser_close( hp->audio_parser );
    if( hp->audio_ctx )
        avcodec_close( hp->audio_ctx );
    if( hp->audio_format )
        avformat_close_input( &hp->audio_format );
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
