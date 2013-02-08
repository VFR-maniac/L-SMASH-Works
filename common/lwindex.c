/*****************************************************************************
 * lwindex.c / lwindex.cpp
 *****************************************************************************
 * Copyright (C) 2012-2013 L-SMASH Works project
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

/* This file is available under an ISC license. */

#include "cpp_compat.h"

#ifdef __cplusplus
extern "C"
{
#endif  /* __cplusplus */
#include <libavformat/avformat.h>       /* Demuxer */
#include <libavcodec/avcodec.h>         /* Decoder */
#include <libavresample/avresample.h>   /* Resampler/Buffer */
#include <libavutil/mathematics.h>      /* Timebase rescaler */
#include <libavutil/pixdesc.h>
#ifdef __cplusplus
}
#endif  /* __cplusplus */

#include "lwlibav_dec.h"
#include "lwlibav_video.h"
#include "lwlibav_audio.h"
#include "progress.h"
#include "lwindex.h"
#include "utils.h"

typedef struct
{
    int64_t pts;
    int64_t dts;
} video_timestamp_t;

static inline int check_frame_reordering
(
    video_frame_info_t *info,
    uint32_t            sample_count
)
{
    for( uint32_t i = 2; i <= sample_count; i++ )
        if( info[i].pts < info[i - 1].pts )
            return 1;
    return 0;
}

static int compare_pts
(
    const video_frame_info_t *a,
    const video_frame_info_t *b
)
{
    int64_t diff = (int64_t)(a->pts - b->pts);
    return diff > 0 ? 1 : (diff == 0 ? 0 : -1);
}

static int compare_dts
(
    const video_timestamp_t *a,
    const video_timestamp_t *b
)
{
    int64_t diff = (int64_t)(a->dts - b->dts);
    return diff > 0 ? 1 : (diff == 0 ? 0 : -1);
}

static inline void sort_presentation_order
(
    video_frame_info_t *info,
    uint32_t            sample_count
)
{
    qsort( info, sample_count, sizeof(video_frame_info_t), (int(*)( const void *, const void * ))compare_pts );
}

static inline void sort_decoding_order
(
    video_timestamp_t *timestamp,
    uint32_t           sample_count
)
{
    qsort( timestamp, sample_count, sizeof(video_timestamp_t), (int(*)( const void *, const void * ))compare_dts );
}

static inline int lineup_seek_base_candidates( lwlibav_file_handler_t *lwhp )
{
    return !strcmp( lwhp->format_name, "mpeg" ) || !strcmp( lwhp->format_name, "mpegts" )
         ? SEEK_DTS_BASED | SEEK_PTS_BASED | SEEK_FILE_OFFSET_BASED
         : SEEK_DTS_BASED | SEEK_PTS_BASED;
}

static int decide_video_seek_method
(
    lwlibav_file_handler_t *lwhp,
    video_decode_handler_t *vdhp,
    uint32_t                sample_count
)
{
    vdhp->seek_base = lineup_seek_base_candidates( lwhp );
    video_frame_info_t *info = vdhp->frame_list;
    for( uint32_t i = 1; i <= sample_count; i++ )
        if( info[i].pts == AV_NOPTS_VALUE )
        {
            vdhp->seek_base &= ~SEEK_PTS_BASED;
            break;
        }
    for( uint32_t i = 1; i <= sample_count; i++ )
        if( info[i].dts == AV_NOPTS_VALUE )
        {
            vdhp->seek_base &= ~SEEK_DTS_BASED;
            break;
        }
    if( vdhp->seek_base & SEEK_FILE_OFFSET_BASED )
    {
        if( lwhp->format_flags & AVFMT_NO_BYTE_SEEK )
            vdhp->seek_base &= ~SEEK_FILE_OFFSET_BASED;
        else
        {
            uint32_t error_count = 0;
            for( uint32_t i = 1; i <= sample_count; i++ )
                error_count += (info[i].file_offset == -1);
            if( error_count == sample_count )
                vdhp->seek_base &= ~SEEK_FILE_OFFSET_BASED;
        }
    }
    if( (vdhp->seek_base & SEEK_PTS_BASED) && check_frame_reordering( info, sample_count ) )
    {
        /* Consider presentation order for keyframe detection.
         * Note: sample number is 1-origin. */
        vdhp->order_converter = (order_converter_t *)lw_malloc_zero( (sample_count + 1) * sizeof(order_converter_t) );
        if( !vdhp->order_converter )
        {
#ifdef DEBUG_VIDEO
            if( vdhp->eh.error_message )
                vdhp->eh.error_message( vdhp->eh.message_priv, "Failed to allocate memory." );
#endif
            return -1;
        }
        sort_presentation_order( &info[1], sample_count );
        video_timestamp_t *timestamp = (video_timestamp_t *)lw_malloc_zero( (sample_count + 1) * sizeof(video_timestamp_t) );
        if( !timestamp )
        {
#ifdef DEBUG_VIDEO
            if( vdhp->eh.error_message )
                vdhp->eh.error_message( vdhp->eh.message_priv, "Failed to allocate memory of video timestamps." );
#endif
            return -1;
        }
        for( uint32_t i = 1; i <= sample_count; i++ )
        {
            timestamp[i].pts = (int64_t)i;
            timestamp[i].dts = (int64_t)info[i].sample_number;
        }
        sort_decoding_order( &timestamp[1], sample_count );
        for( uint32_t i = 1; i <= sample_count; i++ )
            vdhp->order_converter[i].decoding_to_presentation = (uint32_t)timestamp[i].pts;
        free( timestamp );
    }
    else if( vdhp->seek_base & SEEK_DTS_BASED )
        for( uint32_t i = 1; i <= sample_count; i++ )
            info[i].pts = info[i].dts;
    /* Treat video frames with unique value as keyframe. */
    if( vdhp->seek_base & SEEK_FILE_OFFSET_BASED )
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
    else if( vdhp->seek_base & SEEK_PTS_BASED )
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
    else if( vdhp->seek_base & SEEK_DTS_BASED )
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
        vdhp->keyframe_list[ info[i].sample_number ] = info[i].keyframe;
    return 0;
}

static void decide_audio_seek_method
(
    lwlibav_file_handler_t *lwhp,
    audio_decode_handler_t *adhp,
    uint32_t                sample_count
)
{
    adhp->seek_base = lineup_seek_base_candidates( lwhp );
    audio_frame_info_t *info = adhp->frame_list;
    for( uint32_t i = 1; i <= sample_count; i++ )
        if( info[i].pts == AV_NOPTS_VALUE )
        {
            adhp->seek_base &= ~SEEK_PTS_BASED;
            break;
        }
    for( uint32_t i = 1; i <= sample_count; i++ )
        if( info[i].dts == AV_NOPTS_VALUE )
        {
            adhp->seek_base &= ~SEEK_DTS_BASED;
            break;
        }
    if( adhp->seek_base & SEEK_FILE_OFFSET_BASED )
    {
        if( lwhp->format_flags & AVFMT_NO_BYTE_SEEK )
            adhp->seek_base &= ~SEEK_FILE_OFFSET_BASED;
        else
        {
            uint32_t error_count = 0;
            for( uint32_t i = 1; i <= sample_count; i++ )
                error_count += (info[i].file_offset == -1);
            if( error_count == sample_count )
                adhp->seek_base &= ~SEEK_FILE_OFFSET_BASED;
        }
    }
    if( !(adhp->seek_base & SEEK_PTS_BASED) && (adhp->seek_base & SEEK_DTS_BASED) )
        for( uint32_t i = 1; i <= sample_count; i++ )
            info[i].pts = info[i].dts;
    /* Treat audio frames with unique value as a keyframe. */
    if( adhp->seek_base & SEEK_FILE_OFFSET_BASED )
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
    else if( adhp->seek_base & SEEK_PTS_BASED )
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
    else if( adhp->seek_base & SEEK_DTS_BASED )
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

static int64_t calculate_av_gap
(
    video_decode_handler_t *vdhp,
    audio_decode_handler_t *adhp,
    AVRational              video_time_base,
    AVRational              audio_time_base,
    int                     sample_rate
)
{
    /* Pick the first video timestamp.
     * If invalid, skip A/V gap calculation. */
    int64_t video_ts = (vdhp->seek_base & SEEK_PTS_BASED) ? vdhp->frame_list[1].pts : vdhp->frame_list[1].dts;
    if( video_ts == AV_NOPTS_VALUE )
        return 0;
    /* Pick the first valid audio timestamp.
     * If not found, skip A/V gap calculation. */
    int64_t  audio_ts        = 0;
    uint32_t audio_ts_number = 0;
    if( adhp->seek_base & SEEK_PTS_BASED )
    {
        for( uint32_t i = 1; i <= adhp->frame_count; i++ )
            if( adhp->frame_list[i].pts != AV_NOPTS_VALUE )
            {
                audio_ts        = adhp->frame_list[i].pts;
                audio_ts_number = i;
                break;
            }
    }
    else
        for( uint32_t i = 1; i <= adhp->frame_count; i++ )
            if( adhp->frame_list[i].dts != AV_NOPTS_VALUE )
            {
                audio_ts        = adhp->frame_list[i].dts;
                audio_ts_number = i;
                break;
            }
    if( audio_ts_number == 0 )
        return 0;
    /* Estimate the first audio timestamp if invalid. */
    AVRational audio_sample_base = { 1, sample_rate };
    for( uint32_t i = 1, delay_count = 0; i < MIN( audio_ts_number + delay_count, adhp->frame_count ); i++ )
        if( adhp->frame_list[i].length != -1 )
            audio_ts -= av_rescale_q( adhp->frame_list[i].length, audio_sample_base, audio_time_base );
        else
            ++delay_count;
    /* Calculate A/V gap in audio samplerate. */
    if( video_ts || audio_ts )
        return av_rescale_q( audio_ts, audio_time_base, audio_sample_base )
             - av_rescale_q( video_ts, video_time_base, audio_sample_base );
    return 0;
}

static void investigate_pix_fmt_by_decoding
(
    AVCodecContext *video_ctx,
    AVPacket       *pkt,
    AVFrame        *picture
)
{
    int got_picture;
    avcodec_get_frame_defaults( picture );
    avcodec_decode_video2( video_ctx, picture, &got_picture, pkt );
}

static enum AVSampleFormat select_better_sample_format
(
    enum AVSampleFormat a,
    enum AVSampleFormat b
)
{
    switch( a )
    {
        case AV_SAMPLE_FMT_NONE :
            if( b != AV_SAMPLE_FMT_NONE )
                a = b;
            break;
        case AV_SAMPLE_FMT_U8 :
        case AV_SAMPLE_FMT_U8P :
            if( b != AV_SAMPLE_FMT_U8 && b != AV_SAMPLE_FMT_U8P )
                a = b;
            break;
        case AV_SAMPLE_FMT_S16 :
        case AV_SAMPLE_FMT_S16P :
            if( b != AV_SAMPLE_FMT_U8  && b != AV_SAMPLE_FMT_U8P
             && b != AV_SAMPLE_FMT_S16 && b != AV_SAMPLE_FMT_S16P )
                a = b;
            break;
        case AV_SAMPLE_FMT_S32 :
        case AV_SAMPLE_FMT_S32P :
            if( b != AV_SAMPLE_FMT_U8  && b != AV_SAMPLE_FMT_U8P
             && b != AV_SAMPLE_FMT_S16 && b != AV_SAMPLE_FMT_S16P
             && b != AV_SAMPLE_FMT_S32 && b != AV_SAMPLE_FMT_S32P )
                a = b;
            break;
        case AV_SAMPLE_FMT_FLT :
        case AV_SAMPLE_FMT_FLTP :
            if( b == AV_SAMPLE_FMT_DBL || b == AV_SAMPLE_FMT_DBLP )
                a = b;
            break;
        default :
            break;
    }
    return a;
}

static inline void print_index
(
    FILE       *index,
    const char *format,
    ...
)
{
    if( !index )
        return;
    va_list args;
    va_start( args, format );
    vfprintf( index, format, args );
    va_end( args );
}

static inline void write_av_index_entry
(
    FILE         *index,
    AVIndexEntry *ie
)
{
    print_index( index, "POS=%"PRId64",TS=%"PRId64",Flags=%x,Size=%d,Distance=%d\n",
                 ie->pos, ie->timestamp, ie->flags, ie->size, ie->min_distance );
}

static void disable_video_stream( video_decode_handler_t *vdhp )
{
    if( vdhp->frame_list )
        lw_freep( &vdhp->frame_list );
    if( vdhp->keyframe_list )
        lw_freep( &vdhp->keyframe_list );
    if( vdhp->order_converter )
        lw_freep( &vdhp->order_converter );
    if( vdhp->index_entries )
        av_freep( &vdhp->index_entries );
    vdhp->stream_index        = -1;
    vdhp->index_entries_count = 0;
    vdhp->frame_count         = 0;
}

static void create_index
(
    lwlibav_file_handler_t *lwhp,
    video_decode_handler_t *vdhp,
    audio_decode_handler_t *adhp,
    audio_output_handler_t *aohp,
    AVFormatContext        *format_ctx,
    lwlibav_option_t       *opt,
    progress_indicator_t   *indicator,
    progress_handler_t     *php
)
{
    uint32_t video_info_count = 1 << 16;
    uint32_t audio_info_count = 1 << 16;
    video_frame_info_t *video_info = (video_frame_info_t *)lw_malloc_zero( video_info_count * sizeof(video_frame_info_t) );
    if( !video_info )
        return;
    audio_frame_info_t *audio_info = (audio_frame_info_t *)lw_malloc_zero( audio_info_count * sizeof(audio_frame_info_t) );
    if( !audio_info )
    {
        free( video_info );
        return;
    }
    avcodec_get_frame_defaults( adhp->frame_buffer );
    /*
        # Structure of Libav reader index file
        <LibavReaderIndexFile=5>
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
    sprintf( index_path, "%s.lwi", lwhp->file_path );
    FILE *index = !opt->no_create_index ? fopen( index_path, "wb" ) : NULL;
    if( !index && !opt->no_create_index )
    {
        free( video_info );
        free( audio_info );
        return;
    }
    lwhp->format_name  = (char *)format_ctx->iformat->name;
    lwhp->format_flags = format_ctx->iformat->flags;
    vdhp->format       = format_ctx;
    adhp->format       = format_ctx;
    adhp->dv_in_avi    = !strcmp( lwhp->format_name, "avi" ) ? -1 : 0;
    int32_t video_index_pos = 0;
    int32_t audio_index_pos = 0;
    if( index )
    {
        /* Write Index file header. */
        fprintf( index, "<LibavReaderIndexFile=%d>\n", INDEX_FILE_VERSION );
        fprintf( index, "<InputFilePath>%s</InputFilePath>\n", lwhp->file_path );
        fprintf( index, "<LibavReaderIndex=0x%08x,%s>\n", lwhp->format_flags, lwhp->format_name );
        video_index_pos = ftell( index );
        fprintf( index, "<ActiveVideoStreamIndex>%+011d</ActiveVideoStreamIndex>\n", -1 );
        audio_index_pos = ftell( index );
        fprintf( index, "<ActiveAudioStreamIndex>%+011d</ActiveAudioStreamIndex>\n", -1 );
    }
    AVPacket pkt = { 0 };
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
    if( indicator->open )
        indicator->open( php );
    /* Start to read frames and write the index file. */
    while( read_av_frame( format_ctx, &pkt ) >= 0 )
    {
        AVCodecContext *pkt_ctx = format_ctx->streams[ pkt.stream_index ]->codec;
        if( pkt_ctx->codec_type != AVMEDIA_TYPE_VIDEO
         && pkt_ctx->codec_type != AVMEDIA_TYPE_AUDIO )
            continue;
        if( pkt_ctx->codec_id == AV_CODEC_ID_NONE )
            continue;
        if( !av_codec_is_decoder( pkt_ctx->codec ) && open_decoder( pkt_ctx, pkt_ctx->codec_id, lwhp->threads ) )
            continue;
        if( pkt_ctx->codec_type == AVMEDIA_TYPE_VIDEO )
        {
            if( pkt_ctx->pix_fmt == AV_PIX_FMT_NONE )
                investigate_pix_fmt_by_decoding( pkt_ctx, &pkt, vdhp->frame_buffer );
            int dv_in_avi_init = 0;
            if( adhp->dv_in_avi    == -1
             && vdhp->stream_index == -1
             && pkt_ctx->codec_id  == AV_CODEC_ID_DVVIDEO
             && opt->force_audio   == 0 )
            {
                dv_in_avi_init     = 1;
                adhp->dv_in_avi    = 1;
                vdhp->stream_index = pkt.stream_index;
            }
            int higher_resoluton = (pkt_ctx->width * pkt_ctx->height > video_resolution);   /* Replace lower resolution stream with higher. */
            if( dv_in_avi_init
             || (!opt->force_video && (vdhp->stream_index == -1 || (pkt.stream_index != vdhp->stream_index && higher_resoluton)))
             || (opt->force_video && vdhp->stream_index == -1 && pkt.stream_index == opt->force_video_index) )
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
                vdhp->ctx               = pkt_ctx;
                vdhp->codec_id          = pkt_ctx->codec_id;
                vdhp->stream_index      = pkt.stream_index;
                vdhp->input_buffer_size = 0;
                video_resolution        = pkt_ctx->width * pkt_ctx->height;
                video_sample_count      = 0;
                last_keyframe_pts       = AV_NOPTS_VALUE;
                vdhp->initial_width     = pkt_ctx->width;
                vdhp->initial_height    = pkt_ctx->height;
                vdhp->max_width         = pkt_ctx->width;
                vdhp->max_height        = pkt_ctx->height;
            }
            if( pkt.stream_index == vdhp->stream_index )
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
                if( vdhp->max_width  < pkt_ctx->width )
                    vdhp->max_width  = pkt_ctx->width;
                if( vdhp->max_height < pkt_ctx->height )
                    vdhp->max_height = pkt_ctx->height;
                if( video_sample_count + 1 == video_info_count )
                {
                    video_info_count <<= 1;
                    video_frame_info_t *temp = (video_frame_info_t *)realloc( video_info, video_info_count * sizeof(video_frame_info_t) );
                    if( !temp )
                    {
                        av_free_packet( &pkt );
                        goto fail_index;
                    }
                    video_info = temp;
                }
                vdhp->input_buffer_size = MAX( vdhp->input_buffer_size, (uint32_t)pkt.size );
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
            if( adhp->stream_index == -1 && (!opt->force_audio || (opt->force_audio && pkt.stream_index == opt->force_audio_index)) )
            {
                /* Update active audio stream. */
                if( index )
                {
                    int32_t current_pos = ftell( index );
                    fseek( index, audio_index_pos, SEEK_SET );
                    fprintf( index, "<ActiveAudioStreamIndex>%+011d</ActiveAudioStreamIndex>\n", pkt.stream_index );
                    fseek( index, current_pos, SEEK_SET );
                }
                adhp->ctx          = pkt_ctx;
                adhp->codec_id     = pkt_ctx->codec_id;
                adhp->stream_index = pkt.stream_index;
            }
            if( pkt.stream_index > max_audio_index )
            {
                uint32_t *temp = (uint32_t *)realloc( audio_delay_count, (pkt.stream_index + 1) * sizeof(uint32_t) );
                if( !temp )
                    goto fail_index;
                audio_delay_count = temp;
                memset( audio_delay_count + max_audio_index + 1, 0, (pkt.stream_index - max_audio_index) * sizeof(uint32_t) );
                max_audio_index = pkt.stream_index;
            }
            int bits_per_sample = pkt_ctx->bits_per_raw_sample   > 0 ? pkt_ctx->bits_per_raw_sample
                                : pkt_ctx->bits_per_coded_sample > 0 ? pkt_ctx->bits_per_coded_sample
                                : av_get_bytes_per_sample( pkt_ctx->sample_fmt ) << 3;
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
                    int wasted_data_length = avcodec_decode_audio4( pkt_ctx, adhp->frame_buffer, &decode_complete, &temp );
                    if( wasted_data_length < 0 )
                    {
                        pkt_ctx->channels    = av_get_channel_layout_nb_channels( adhp->frame_buffer->channel_layout );
                        pkt_ctx->sample_rate = adhp->frame_buffer->sample_rate;
                        break;
                    }
                    temp.size -= wasted_data_length;
                    temp.data += wasted_data_length;
                    if( decode_complete )
                    {
                        frame_length += adhp->frame_buffer->nb_samples;
                        output_audio = 1;
                    }
                }
                if( !output_audio )
                {
                    frame_length = -1;
                    ++ (*delay_count);
                }
            }
            if( pkt.stream_index == adhp->stream_index )
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
                    audio_info[audio_sample_count].sample_rate   = pkt_ctx->sample_rate;
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
                        audio_frame_info_t *temp = (audio_frame_info_t *)realloc( audio_info, audio_info_count * sizeof(audio_frame_info_t) );
                        if( !temp )
                        {
                            av_free_packet( &pkt );
                            goto fail_index;
                        }
                        audio_info = temp;
                    }
                    if( pkt_ctx->channel_layout == 0 )
                        pkt_ctx->channel_layout = av_get_default_channel_layout( pkt_ctx->channels );
                    if( av_get_channel_layout_nb_channels( pkt_ctx->channel_layout )
                      > av_get_channel_layout_nb_channels( aohp->output_channel_layout ) )
                        aohp->output_channel_layout = pkt_ctx->channel_layout;
                    aohp->output_sample_format   = select_better_sample_format( aohp->output_sample_format, pkt_ctx->sample_fmt );
                    aohp->output_sample_rate     = MAX( aohp->output_sample_rate, audio_sample_rate );
                    aohp->output_bits_per_sample = MAX( aohp->output_bits_per_sample, bits_per_sample );
                    adhp->input_buffer_size      = MAX( adhp->input_buffer_size, (uint32_t)pkt.size );
                }
            }
            /* Write an audio packet info to the index file. */
            print_index( index, "Index=%d,Type=%d,Codec=%d,TimeBase=%d/%d,POS=%"PRId64",PTS=%"PRId64",DTS=%"PRId64"\n"
                         "Channels=%d:0x%"PRIx64",SampleRate=%d,SampleFormat=%s,BitsPerSample=%d,Length=%d\n",
                         pkt.stream_index, AVMEDIA_TYPE_AUDIO, pkt_ctx->codec_id,
                         format_ctx->streams[ pkt.stream_index ]->time_base.num,
                         format_ctx->streams[ pkt.stream_index ]->time_base.den,
                         pkt.pos, pkt.pts, pkt.dts,
                         pkt_ctx->channels, pkt_ctx->channel_layout, pkt_ctx->sample_rate,
                         av_get_sample_fmt_name( pkt_ctx->sample_fmt ) ? av_get_sample_fmt_name( pkt_ctx->sample_fmt ) : "none",
                         bits_per_sample, frame_length );
        }
        if( indicator->update )
        {
            /* Update progress dialog if packet's DTS is valid. */
            if( first_dts == AV_NOPTS_VALUE )
                first_dts = pkt.dts;
            const char *message = index ? "Creating Index file" : "Parsing input file";
            int percent = first_dts == AV_NOPTS_VALUE || pkt.dts == AV_NOPTS_VALUE
                        ? 0
                        : (int)(100.0 * (pkt.dts - first_dts)
                             * (format_ctx->streams[ pkt.stream_index ]->time_base.num / (double)format_ctx->streams[ pkt.stream_index ]->time_base.den)
                             / (format_ctx->duration / AV_TIME_BASE) + 0.5);
            int abort = indicator->update( php, message, percent );
            av_free_packet( &pkt );
            if( abort )
                goto fail_index;
        }
        else
            av_free_packet( &pkt );
    }
    if( vdhp->stream_index >= 0 )
    {
        vdhp->keyframe_list = (uint8_t *)lw_malloc_zero( (video_sample_count + 1) * sizeof(uint8_t) );
        if( !vdhp->keyframe_list )
            goto fail_index;
        for( uint32_t i = 0; i <= video_sample_count; i++ )
            vdhp->keyframe_list[i] = video_info[i].keyframe;
        vdhp->frame_list      = video_info;
        vdhp->frame_count     = video_sample_count;
        vdhp->initial_pix_fmt = vdhp->ctx->pix_fmt;
        if( decide_video_seek_method( lwhp, vdhp, video_sample_count ) )
            goto fail_index;
    }
    else
        lw_freep( &video_info );
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
                AVPacket null_pkt = { 0 };
                av_init_packet( &null_pkt );
                null_pkt.data = NULL;
                null_pkt.size = 0;
                int decode_complete;
                if( avcodec_decode_audio4( pkt_ctx, adhp->frame_buffer, &decode_complete, &null_pkt ) >= 0 )
                {
                    frame_length = decode_complete ? adhp->frame_buffer->nb_samples : 0;
                    if( stream_index == adhp->stream_index )
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
                                 "Channels=%d:0x%"PRIx64",SampleRate=%d,SampleFormat=%s,BitsPerSample=%d,Length=%d\n",
                                 stream_index, AVMEDIA_TYPE_AUDIO, pkt_ctx->codec_id,
                                 format_ctx->streams[stream_index]->time_base.num,
                                 format_ctx->streams[stream_index]->time_base.den,
                                 -1LL, AV_NOPTS_VALUE, AV_NOPTS_VALUE,
                                 0, 0, 0, "none", 0, frame_length );
                }
            }
        }
        free( audio_delay_count );
    }
    if( adhp->stream_index >= 0 )
    {
        if( adhp->dv_in_avi == 1 && format_ctx->streams[ adhp->stream_index ]->nb_index_entries == 0 )
        {
            /* DV in AVI Type-1 */
            audio_sample_count = video_info ? MIN( video_sample_count, audio_sample_count ) : 0;
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
            if( adhp->dv_in_avi == 1 && opt->force_video && opt->force_video_index == -1 )
            {
                /* Disable DV video stream. */
                disable_video_stream( vdhp );
                video_info = NULL;
            }
            adhp->dv_in_avi = 0;
        }
        adhp->frame_list   = audio_info;
        adhp->frame_count  = audio_sample_count;
        adhp->frame_length = constant_frame_length ? frame_length : 0;
        decide_audio_seek_method( lwhp, adhp, audio_sample_count );
        if( opt->av_sync && vdhp->stream_index >= 0 )
            lwhp->av_gap = calculate_av_gap( vdhp,
                                             adhp,
                                             format_ctx->streams[ vdhp->stream_index ]->time_base,
                                             format_ctx->streams[ adhp->stream_index ]->time_base,
                                             audio_sample_rate );
    }
    else
        lw_freep( &audio_info );
    print_index( index, "</LibavReaderIndex>\n" );
    for( unsigned int stream_index = 0; stream_index < format_ctx->nb_streams; stream_index++ )
    {
        AVStream *stream = format_ctx->streams[stream_index];
        if( stream->codec->codec_type == AVMEDIA_TYPE_VIDEO )
        {
            print_index( index, "<StreamIndexEntries=%d,%d,%d>\n", stream_index, AVMEDIA_TYPE_VIDEO, stream->nb_index_entries );
            if( vdhp->stream_index != stream_index )
                for( int i = 0; i < stream->nb_index_entries; i++ )
                    write_av_index_entry( index, &stream->index_entries[i] );
            else if( stream->nb_index_entries > 0 )
            {
                vdhp->index_entries = (AVIndexEntry *)av_malloc( stream->index_entries_allocated_size );
                if( !vdhp->index_entries )
                    goto fail_index;
                for( int i = 0; i < stream->nb_index_entries; i++ )
                {
                    AVIndexEntry *ie = &stream->index_entries[i];
                    vdhp->index_entries[i] = *ie;
                    write_av_index_entry( index, ie );
                }
                vdhp->index_entries_count = stream->nb_index_entries;
            }
            print_index( index, "</StreamIndexEntries>\n" );
        }
        else if( stream->codec->codec_type == AVMEDIA_TYPE_AUDIO )
        {
            print_index( index, "<StreamIndexEntries=%d,%d,%d>\n", stream_index, AVMEDIA_TYPE_AUDIO, stream->nb_index_entries );
            if( adhp->stream_index != stream_index )
                for( int i = 0; i < stream->nb_index_entries; i++ )
                    write_av_index_entry( index, &stream->index_entries[i] );
            else if( stream->nb_index_entries > 0 )
            {
                /* Audio stream in matroska container requires index_entries for seeking.
                 * This avoids for re-reading the file to create index_entries since the file will be closed once. */
                adhp->index_entries = (AVIndexEntry *)av_malloc( stream->index_entries_allocated_size );
                if( !adhp->index_entries )
                    goto fail_index;
                for( int i = 0; i < stream->nb_index_entries; i++ )
                {
                    AVIndexEntry *ie = &stream->index_entries[i];
                    adhp->index_entries[i] = *ie;
                    write_av_index_entry( index, ie );
                }
                adhp->index_entries_count = stream->nb_index_entries;
            }
            print_index( index, "</StreamIndexEntries>\n" );
        }
    }
    print_index( index, "</LibavReaderIndexFile>\n" );
    if( index )
        fclose( index );
    if( indicator->close )
        indicator->close( php );
    vdhp->format = NULL;
    adhp->format = NULL;
    return;
fail_index:
    free( video_info );
    free( audio_info );
    if( audio_delay_count )
        free( audio_delay_count );
    if( index )
        fclose( index );
    if( indicator->close )
        indicator->close( php );
    vdhp->format = NULL;
    adhp->format = NULL;
    return;
}

static int parse_index
(
    lwlibav_file_handler_t *lwhp,
    video_decode_handler_t *vdhp,
    audio_decode_handler_t *adhp,
    audio_output_handler_t *aohp,
    lwlibav_option_t       *opt,
    FILE                   *index
)
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
    lwhp->file_path = (char *)lw_malloc_zero( file_path_length + 1 );
    if( !lwhp->file_path )
        return -1;
    memcpy( lwhp->file_path, file_path, file_path_length );
    /* Parse the index file. */
    char format_name[256];
    int active_video_index;
    int active_audio_index;
    if( fscanf( index, "<LibavReaderIndex=0x%x,%[^>]>\n", &lwhp->format_flags, format_name ) != 2 )
        return -1;
    int32_t active_index_pos = ftell( index );
    if( fscanf( index, "<ActiveVideoStreamIndex>%d</ActiveVideoStreamIndex>\n", &active_video_index ) != 1
     || fscanf( index, "<ActiveAudioStreamIndex>%d</ActiveAudioStreamIndex>\n", &active_audio_index ) != 1 )
        return -1;
    lwhp->format_name = format_name;
    adhp->dv_in_avi = !strcmp( lwhp->format_name, "avi" ) ? -1 : 0;
    int video_present = (active_video_index >= 0);
    int audio_present = (active_audio_index >= 0);
    vdhp->stream_index = opt->force_video ? opt->force_video_index : active_video_index;
    adhp->stream_index = opt->force_audio ? opt->force_audio_index : active_audio_index;
    uint32_t video_info_count = 1 << 16;
    uint32_t audio_info_count = 1 << 16;
    video_frame_info_t *video_info = NULL;
    audio_frame_info_t *audio_info = NULL;
    if( vdhp->stream_index >= 0 )
    {
        video_info = (video_frame_info_t *)lw_malloc_zero( video_info_count * sizeof(video_frame_info_t) );
        if( !video_info )
            goto fail_parsing;
    }
    if( adhp->stream_index >= 0 )
    {
        audio_info = (audio_frame_info_t *)lw_malloc_zero( audio_info_count * sizeof(audio_frame_info_t) );
        if( !audio_info )
            goto fail_parsing;
    }
    vdhp->codec_id             = AV_CODEC_ID_NONE;
    adhp->codec_id             = AV_CODEC_ID_NONE;
    vdhp->initial_pix_fmt      = AV_PIX_FMT_NONE;
    aohp->output_sample_format = AV_SAMPLE_FMT_NONE;
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
            if( adhp->dv_in_avi == -1 && codec_id == AV_CODEC_ID_DVVIDEO && !opt->force_audio )
            {
                adhp->dv_in_avi = 1;
                if( vdhp->stream_index == -1 )
                {
                    vdhp->stream_index = stream_index;
                    video_info = (video_frame_info_t *)lw_malloc_zero( video_info_count * sizeof(video_frame_info_t) );
                    if( !video_info )
                        goto fail_parsing;
                }
            }
            if( stream_index == vdhp->stream_index )
            {
                int key;
                int width;
                int height;
                char pix_fmt[64];
                if( sscanf( buf, "Key=%d,Width=%d,Height=%d,PixelFormat=%s",
                    &key, &width, &height, pix_fmt ) != 4 )
                    goto fail_parsing;
                if( vdhp->codec_id == AV_CODEC_ID_NONE )
                    vdhp->codec_id = (enum AVCodecID)codec_id;
                if( vdhp->initial_width == 0 || vdhp->initial_height == 0 )
                {
                    vdhp->initial_width  = width;
                    vdhp->initial_height = height;
                    vdhp->max_width      = width;
                    vdhp->max_height     = height;
                }
                else
                {
                    if( vdhp->max_width  < width )
                        vdhp->max_width  = width;
                    if( vdhp->max_height < width )
                        vdhp->max_height = height;
                }
                if( vdhp->initial_pix_fmt == AV_PIX_FMT_NONE )
                    vdhp->initial_pix_fmt = av_get_pix_fmt( (const char *)pix_fmt );
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
                    video_frame_info_t *temp = (video_frame_info_t *)realloc( video_info, video_info_count * sizeof(video_frame_info_t) );
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
            if( stream_index == adhp->stream_index )
            {
                uint64_t layout;
                int      channels;
                int      sample_rate;
                char     sample_fmt[64];
                int      bits_per_sample;
                int      frame_length;
                if( sscanf( buf, "Channels=%d:0x%"SCNx64",SampleRate=%d,SampleFormat=%[^,],BitsPerSample=%d,Length=%d",
                            &channels, &layout, &sample_rate, sample_fmt, &bits_per_sample, &frame_length ) != 6 )
                    goto fail_parsing;
                if( adhp->codec_id == AV_CODEC_ID_NONE )
                    adhp->codec_id = (enum AVCodecID)codec_id;
                if( (channels | layout | sample_rate | bits_per_sample) && audio_duration <= INT32_MAX )
                {
                    if( audio_sample_rate == 0 )
                        audio_sample_rate = sample_rate;
                    if( audio_time_base.num == 0 || audio_time_base.den == 0 )
                    {
                        audio_time_base.num = time_base.num;
                        audio_time_base.den = time_base.den;
                    }
                    if( layout == 0 )
                        layout = av_get_default_channel_layout( channels );
                    if( av_get_channel_layout_nb_channels( layout )
                      > av_get_channel_layout_nb_channels( aohp->output_channel_layout ) )
                        aohp->output_channel_layout = layout;
                    aohp->output_sample_format   = select_better_sample_format( aohp->output_sample_format,
                                                                                av_get_sample_fmt( (const char *)sample_fmt ) );
                    aohp->output_sample_rate     = MAX( aohp->output_sample_rate, audio_sample_rate );
                    aohp->output_bits_per_sample = MAX( aohp->output_bits_per_sample, bits_per_sample );
                    ++audio_sample_count;
                    audio_info[audio_sample_count].pts           = pts;
                    audio_info[audio_sample_count].dts           = dts;
                    audio_info[audio_sample_count].file_offset   = pos;
                    audio_info[audio_sample_count].sample_number = audio_sample_count;
                    audio_info[audio_sample_count].sample_rate   = sample_rate;
                }
                else
                    for( uint32_t i = 1; i <= adhp->delay_count; i++ )
                    {
                        uint32_t audio_frame_number = audio_sample_count - adhp->delay_count + i;
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
                    audio_frame_info_t *temp = (audio_frame_info_t *)realloc( audio_info, audio_info_count * sizeof(audio_frame_info_t) );
                    if( !temp )
                        goto fail_parsing;
                    audio_info = temp;
                }
                if( frame_length == -1 )
                    ++ adhp->delay_count;
                else if( audio_sample_count > adhp->delay_count )
                {
                    uint32_t audio_frame_number = audio_sample_count - adhp->delay_count;
                    audio_info[audio_frame_number].length = frame_length;
                    if( audio_frame_number > 1 && audio_info[audio_frame_number].length != audio_info[audio_frame_number - 1].length )
                        constant_frame_length = 0;
                    audio_duration += frame_length;
                }
            }
        }
    }
    if( video_present && opt->force_video && opt->force_video_index != -1
     && (video_sample_count == 0 || vdhp->initial_pix_fmt == AV_PIX_FMT_NONE || vdhp->initial_width == 0 || vdhp->initial_height == 0) )
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
            if( stream_index != vdhp->stream_index && stream_index != adhp->stream_index )
            {
                for( int i = 0; i < index_entries_count; i++ )
                    if( !fgets( buf, sizeof(buf), index ) )
                        goto fail_parsing;
            }
            else if( codec_type == AVMEDIA_TYPE_VIDEO && stream_index == vdhp->stream_index )
            {
                vdhp->index_entries_count = index_entries_count;
                vdhp->index_entries = (AVIndexEntry *)av_malloc( vdhp->index_entries_count * sizeof(AVIndexEntry) );
                if( !vdhp->index_entries )
                    goto fail_parsing;
                for( int i = 0; i < vdhp->index_entries_count; i++ )
                {
                    AVIndexEntry ie;
                    int size;
                    int flags;
                    if( sscanf( buf, "POS=%"SCNd64",TS=%"SCNd64",Flags=%x,Size=%d,Distance=%d",
                                &ie.pos, &ie.timestamp, &flags, &size, &ie.min_distance ) != 5 )
                        break;
                    ie.size  = size;
                    ie.flags = flags;
                    vdhp->index_entries[i] = ie;
                    if( !fgets( buf, sizeof(buf), index ) )
                        goto fail_parsing;
                }
            }
            else if( codec_type == AVMEDIA_TYPE_AUDIO && stream_index == adhp->stream_index )
            {
                adhp->index_entries_count = index_entries_count;
                adhp->index_entries = (AVIndexEntry *)av_malloc( adhp->index_entries_count * sizeof(AVIndexEntry) );
                if( !adhp->index_entries )
                    goto fail_parsing;
                for( int i = 0; i < adhp->index_entries_count; i++ )
                {
                    AVIndexEntry ie;
                    int size;
                    int flags;
                    if( sscanf( buf, "POS=%"SCNd64",TS=%"SCNd64",Flags=%x,Size=%d,Distance=%d",
                                &ie.pos, &ie.timestamp, &flags, &size, &ie.min_distance ) != 5 )
                        break;
                    ie.size  = size;
                    ie.flags = flags;
                    adhp->index_entries[i] = ie;
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
        if( vdhp->stream_index >= 0 )
        {
            vdhp->keyframe_list = (uint8_t *)lw_malloc_zero( (video_sample_count + 1) * sizeof(uint8_t) );
            if( !vdhp->keyframe_list )
                goto fail_parsing;
            for( uint32_t i = 0; i <= video_sample_count; i++ )
                vdhp->keyframe_list[i] = video_info[i].keyframe;
            vdhp->frame_list  = video_info;
            vdhp->frame_count = video_sample_count;
            if( decide_video_seek_method( lwhp, vdhp, video_sample_count ) )
                goto fail_parsing;
        }
        if( adhp->stream_index >= 0 )
        {
            if( adhp->dv_in_avi == 1 && adhp->index_entries_count == 0 )
            {
                /* DV in AVI Type-1 */
                audio_sample_count = MIN( video_sample_count, audio_sample_count );
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
                if( adhp->dv_in_avi == 1 && ((!opt->force_video && active_video_index == -1) || (opt->force_video && opt->force_video_index == -1)) )
                {
                    /* Disable DV video stream. */
                    disable_video_stream( vdhp );
                    video_info = NULL;
                }
                adhp->dv_in_avi = 0;
            }
            adhp->frame_list   = audio_info;
            adhp->frame_count  = audio_sample_count;
            adhp->frame_length = constant_frame_length ? audio_info[1].length : 0;
            decide_audio_seek_method( lwhp, adhp, audio_sample_count );
            if( opt->av_sync && vdhp->stream_index >= 0 )
                lwhp->av_gap = calculate_av_gap( vdhp, adhp, video_time_base, audio_time_base, audio_sample_rate );
        }
        if( vdhp->stream_index != active_video_index || adhp->stream_index != active_audio_index )
        {
            /* Update the active stream indexes when specifying different stream indexes. */
            fseek( index, active_index_pos, SEEK_SET );
            fprintf( index, "<ActiveVideoStreamIndex>%+011d</ActiveVideoStreamIndex>\n", vdhp->stream_index );
            fprintf( index, "<ActiveAudioStreamIndex>%+011d</ActiveAudioStreamIndex>\n", adhp->stream_index );
        }
        return 0;
    }
fail_parsing:
    vdhp->frame_list = NULL;
    adhp->frame_list = NULL;
    if( video_info )
        free( video_info );
    if( audio_info )
        free( audio_info );
    return -1;
}

int lwlibav_construct_index
(
    lwlibav_file_handler_t *lwhp,
    video_decode_handler_t *vdhp,
    audio_decode_handler_t *adhp,
    audio_output_handler_t *aohp,
    error_handler_t        *ehp,
    lwlibav_option_t       *opt,
    progress_indicator_t   *indicator,
    progress_handler_t     *php
)
{
    /* Allocate frame buffer. */
    vdhp->frame_buffer = avcodec_alloc_frame();
    if( !vdhp->frame_buffer )
        return -1;
    adhp->frame_buffer = avcodec_alloc_frame();
    if( !adhp->frame_buffer )
    {
        avcodec_free_frame( &vdhp->frame_buffer );
        return -1;
    }
    /* Try to open the index file. */
    int file_path_length = strlen( opt->file_path );
    char *index_file_path = (char *)lw_malloc_zero(file_path_length + 5);
    if( !index_file_path )
    {
        avcodec_free_frame( &vdhp->frame_buffer );
        avcodec_free_frame( &adhp->frame_buffer );
        return -1;
    }
    memcpy( index_file_path, opt->file_path, file_path_length );
    const char *ext = file_path_length >= 5 ? &opt->file_path[file_path_length - 4] : NULL;
    if( ext && !strncmp( ext, ".lwi", strlen( ".lwi" ) ) )
        index_file_path[file_path_length] = '\0';
    else
    {
        memcpy( index_file_path + file_path_length, ".lwi", strlen( ".lwi" ) );
        index_file_path[file_path_length + 4] = '\0';
    }
    FILE *index = fopen( index_file_path, (opt->force_video || opt->force_audio) ? "r+b" : "rb" );
    free( index_file_path );
    if( index )
    {
        int version = 0;
        int ret = fscanf( index, "<LibavReaderIndexFile=%d>\n", &version );
        if( ret == 1
         && version == INDEX_FILE_VERSION
         && parse_index( lwhp, vdhp, adhp, aohp, opt, index ) == 0 )
        {
            /* Opening and parsing the index file succeeded. */
            fclose( index );
            av_register_all();
            avcodec_register_all();
            lwhp->threads = opt->threads;
            return 0;
        }
        fclose( index );
    }
    /* Open file. */
    if( !lwhp->file_path )
    {
        lwhp->file_path = (char *)lw_malloc_zero( file_path_length + 1 );
        if( !lwhp->file_path )
            goto fail;
        memcpy( lwhp->file_path, opt->file_path, file_path_length );
    }
    av_register_all();
    avcodec_register_all();
    AVFormatContext *format_ctx = NULL;
    if( lavf_open_file( &format_ctx, opt->file_path, ehp ) )
    {
        if( format_ctx )
            lavf_close_file( &format_ctx );
        goto fail;
    }
    lwhp->threads      = opt->threads;
    vdhp->stream_index = -1;
    adhp->stream_index = -1;
    /* Create the index file. */
    create_index( lwhp, vdhp, adhp, aohp, format_ctx, opt, indicator, php );
    /* Close file.
     * By opening file for video and audio separately, indecent work about frame reading can be avoidable. */
    lavf_close_file( &format_ctx );
    vdhp->ctx = NULL;
    adhp->ctx = NULL;
    return 0;
fail:
    if( vdhp->frame_buffer )
        avcodec_free_frame( &vdhp->frame_buffer );
    if( adhp->frame_buffer )
        avcodec_free_frame( &adhp->frame_buffer );
    if( lwhp->file_path )
        lw_freep( lwhp->file_path );
    return -1;
}
