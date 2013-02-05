/*****************************************************************************
 * libav_video.c
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

/* This file is available under an ISC license.
 * However, when distributing its binary file, it will be under LGPL or GPL.
 * Don't distribute it if its license is GPL. */

#include <libavformat/avformat.h>   /* Demuxer */
#include <libavcodec/avcodec.h>     /* Decoder */

#include "libav_dec.h"
#include "libav_video.h"

#define SEEK_MODE_NORMAL     0
#define SEEK_MODE_UNSAFE     1
#define SEEK_MODE_AGGRESSIVE 2

int get_desired_video_track
(
    const char             *file_path,
    video_decode_handler_t *vdhp,
    int                     threads
)
{
    int error = vdhp->stream_index < 0
             || vdhp->frame_count == 0
             || lavf_open_file( &vdhp->format, file_path, &vdhp->eh );
    AVCodecContext *ctx = !error ? vdhp->format->streams[ vdhp->stream_index ]->codec : NULL;
    if( error || open_decoder( ctx, vdhp->codec_id, threads ) )
    {
        if( vdhp->index_entries )
        {
            av_free( vdhp->index_entries );
            vdhp->index_entries = NULL;
        }
        if( vdhp->frame_list )
        {
            free( vdhp->frame_list );
            vdhp->frame_list = NULL;
        }
        if( vdhp->order_converter )
        {
            free( vdhp->order_converter );
            vdhp->order_converter = NULL;
        }
        if( vdhp->keyframe_list )
        {
            free( vdhp->keyframe_list );
            vdhp->keyframe_list = NULL;
        }
        if( vdhp->format )
        {
            lavf_close_file( &vdhp->format );
            vdhp->format = NULL;
        }
        return -1;
    }
    vdhp->ctx = ctx;
    return 0;
}

static int decode_video_sample
(
    video_decode_handler_t *vdhp,
    AVFrame                *picture,
    int                    *got_picture,
    uint32_t                sample_number
)
{
    AVPacket pkt = { 0 };
    if( get_av_frame( vdhp->format, vdhp->stream_index, &vdhp->input_buffer, &vdhp->input_buffer_size, &pkt ) )
        return 1;
    if( pkt.flags & AV_PKT_FLAG_KEY )
        vdhp->last_rap_number = sample_number;
    avcodec_get_frame_defaults( picture );
    int64_t pts = pkt.pts != AV_NOPTS_VALUE ? pkt.pts : pkt.dts;
    int ret = avcodec_decode_video2( vdhp->ctx, picture, got_picture, &pkt );
    picture->pts = pts;
    if( ret < 0 )
    {
#ifdef DEBUG_VIDEO
        if( vdhp->eh.error_message )
            vdhp->eh.error_message( vdhp->eh.message_priv, "Failed to decode a video frame." );
#endif
        return -1;
    }
    return 0;
}

void find_random_accessible_point
(
    video_decode_handler_t *vdhp,
    uint32_t                presentation_sample_number,
    uint32_t                decoding_sample_number,
    uint32_t               *rap_number
)
{
    uint8_t is_leading = vdhp->frame_list[presentation_sample_number].is_leading;
    if( decoding_sample_number == 0 )
        decoding_sample_number = vdhp->frame_list[presentation_sample_number].sample_number;
    *rap_number = decoding_sample_number;
    while( *rap_number )
    {
        if( vdhp->keyframe_list[ *rap_number ] )
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

int64_t get_random_accessible_point_position
(
    video_decode_handler_t *vdhp,
    uint32_t                rap_number
)
{
    uint32_t presentation_rap_number = vdhp->order_converter
                                     ? vdhp->order_converter[rap_number].decoding_to_presentation
                                     : rap_number;
    return (vdhp->seek_base & SEEK_FILE_OFFSET_BASED) ? vdhp->frame_list[presentation_rap_number].file_offset
         : (vdhp->seek_base & SEEK_PTS_BASED)         ? vdhp->frame_list[presentation_rap_number].pts
         : (vdhp->seek_base & SEEK_DTS_BASED)         ? vdhp->frame_list[presentation_rap_number].dts
         :                                              vdhp->frame_list[presentation_rap_number].sample_number;
}

static uint32_t seek_video
(
    video_decode_handler_t *vdhp,
    AVFrame                *picture,
    uint32_t                presentation_sample_number,
    uint32_t                rap_number,
    int64_t                 rap_pos,
    int                     error_ignorance
)
{
    /* Prepare to decode from random accessible sample. */
    flush_buffers( vdhp->ctx, &vdhp->eh );
    if( vdhp->eh.error )
        return 0;
    if( av_seek_frame( vdhp->format, vdhp->stream_index, rap_pos, vdhp->seek_flags ) < 0 )
        av_seek_frame( vdhp->format, vdhp->stream_index, rap_pos, vdhp->seek_flags | AVSEEK_FLAG_ANY );
    vdhp->delay_count = 0;
    int dummy;
    int64_t  rap_pts = AV_NOPTS_VALUE;
    uint32_t i;
    for( i = rap_number; i < presentation_sample_number + get_decoder_delay( vdhp->ctx ); i++ )
    {
        int ret = decode_video_sample( vdhp, picture, &dummy, i );
        /* Some decoders return -1 when feeding a leading sample.
         * We don't consider as an error if the return value -1 is caused by a leading sample since it's not fatal at all. */
        if( i == vdhp->last_rap_number && picture->pts != AV_NOPTS_VALUE )
            rap_pts = picture->pts;
        if( ret == -1 && (picture->pts == AV_NOPTS_VALUE || picture->pts >= rap_pts) && !error_ignorance )
        {
#ifdef DEBUG_VIDEO
            if( vdhp->eh.error_message )
                vdhp->eh.error_message( vdhp->eh.message_priv, "Failed to decode a video frame." );
#endif
            return 0;
        }
        else if( ret == 1 )
            break;      /* Sample doesn't exist. */
    }
    vdhp->delay_count = MIN( get_decoder_delay( vdhp->ctx ), i - rap_number );
    return i;
}

static int get_picture
(
    video_decode_handler_t *vdhp,
    AVFrame                *picture,
    uint32_t                current,
    uint32_t                goal,
    uint32_t                video_sample_count
)
{
    int got_picture = 0;
    while( current <= goal )
    {
        int ret = decode_video_sample( vdhp, picture, &got_picture, current );
        if( ret == -1 )
            return -1;
        else if( ret == 1 )
            break;      /* Sample doesn't exist. */
        ++current;
        if( !got_picture )
            ++ vdhp->delay_count;
    }
    /* Flush the last frames. */
    if( current > video_sample_count && get_decoder_delay( vdhp->ctx ) )
        while( current <= goal )
        {
            AVPacket pkt = { 0 };
            av_init_packet( &pkt );
            pkt.data = NULL;
            pkt.size = 0;
            avcodec_get_frame_defaults( picture );
            if( avcodec_decode_video2( vdhp->ctx, picture, &got_picture, &pkt ) < 0 )
            {
#ifdef DEBUG_VIDEO
                if( vdhp->eh.error_message )
                    vdhp->eh.error_message( vdhp->eh.message_priv, "Failed to decode and flush a video frame." );
#endif
                return -1;
            }
            ++current;
        }
    return got_picture ? 0 : -1;
}

int get_video_frame
(
    video_decode_handler_t *vdhp,
    uint32_t                frame_number,
    uint32_t                frame_count
)
{
#define MAX_ERROR_COUNT 3   /* arbitrary */
    AVFrame *picture = vdhp->frame_buffer;
    uint32_t start_number;  /* number of sample, for normal decoding, where decoding starts excluding decoding delay */
    uint32_t rap_number;    /* number of sample, for seeking, where decoding starts excluding decoding delay */
    int      seek_mode = vdhp->seek_mode;
    int64_t  rap_pos   = INT64_MIN;
    if( frame_number > vdhp->last_frame_number
     && frame_number <= vdhp->last_frame_number + vdhp->forward_seek_threshold )
    {
        start_number = vdhp->last_frame_number + 1 + vdhp->delay_count;
        rap_number   = vdhp->last_rap_number;
    }
    else
    {
        find_random_accessible_point( vdhp, frame_number, 0, &rap_number );
        if( rap_number == vdhp->last_rap_number && frame_number > vdhp->last_frame_number )
            start_number = vdhp->last_frame_number + 1 + vdhp->delay_count;
        else
        {
            /* Require starting to decode from random accessible sample. */
            rap_pos = get_random_accessible_point_position( vdhp, rap_number );
            vdhp->last_rap_number = rap_number;
            start_number = seek_video( vdhp, picture, frame_number, rap_number, rap_pos, seek_mode != SEEK_MODE_NORMAL );
        }
    }
    /* Get desired picture. */
    int error_count = 0;
    while( start_number == 0 || get_picture( vdhp, picture, start_number, frame_number + vdhp->delay_count, frame_count ) )
    {
        /* Failed to get desired picture. */
        if( vdhp->eh.error || seek_mode == SEEK_MODE_AGGRESSIVE )
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
            find_random_accessible_point( vdhp, frame_number, rap_number - 1, &rap_number );
            rap_pos = get_random_accessible_point_position( vdhp, rap_number );
            vdhp->last_rap_number = rap_number;
        }
        start_number = seek_video( vdhp, picture, frame_number, rap_number, rap_pos, seek_mode != SEEK_MODE_NORMAL );
    }
    vdhp->last_frame_number = frame_number;
    return 0;
video_fail:
    /* fatal error of decoding */
#ifdef DEBUG_VIDEO
    if( vdhp->eh.error_message )
        vdhp->eh.error_message( vdhp->eh.message_priv, "Couldn't read video frame." );
#endif
    return -1;
#undef MAX_ERROR_COUNT
}
