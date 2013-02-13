/*****************************************************************************
 * lwlibav_video.c / lwlibav_video.cpp
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
#include <libavformat/avformat.h>   /* Demuxer */
#include <libavcodec/avcodec.h>     /* Decoder */
#ifdef __cplusplus
}
#endif  /* __cplusplus */

#include "lwlibav_dec.h"
#include "lwlibav_video.h"

#include "utils.h"

#define SEEK_MODE_NORMAL     0
#define SEEK_MODE_UNSAFE     1
#define SEEK_MODE_AGGRESSIVE 2

int lwlibav_get_desired_video_track
(
    const char                     *file_path,
    lwlibav_video_decode_handler_t *vdhp,
    int                             threads
)
{
    int error = vdhp->stream_index < 0
             || vdhp->frame_count == 0
             || lavf_open_file( &vdhp->format, file_path, &vdhp->eh );
    AVCodecContext *ctx = !error ? vdhp->format->streams[ vdhp->stream_index ]->codec : NULL;
    if( error || open_decoder( ctx, vdhp->codec_id, threads ) )
    {
        if( vdhp->index_entries )
            av_freep( &vdhp->index_entries );
        if( vdhp->frame_list )
            lw_freep( &vdhp->frame_list );
        if( vdhp->order_converter )
            lw_freep( &vdhp->order_converter );
        if( vdhp->keyframe_list )
            lw_freep( &vdhp->keyframe_list );
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

static inline double lwlibav_round( double x )
{
    return x > 0 ? floor( x + 0.5 ) : ceil( x - 0.5 );
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

static int try_ntsc_framerate
(
    double framerate,
    int   *framerate_num,
    int   *framerate_den
)
{
#define DOUBLE_EPSILON 5e-5
    if( framerate == 0 )
        return 0;
    double exponent;
    double fps_sig = sigexp10( framerate, &exponent );
    uint64_t fps_den;
    uint64_t fps_num;
    int i = 1;
    while( 1 )
    {
        fps_den = i * 1001;
        fps_num = (uint64_t)(lwlibav_round( fps_den * fps_sig ) * exponent);
        if( fps_num > INT_MAX )
            return 0;
        if( fabs( ((double)fps_num / fps_den) / exponent - fps_sig ) < DOUBLE_EPSILON )
            break;
        ++i;
    }
    *framerate_num = (int)fps_num;
    *framerate_den = (int)fps_den;
    return 1;
#undef DOUBLE_EPSILON
}

void lwlibav_setup_timestamp_info
(
    lwlibav_video_decode_handler_t *vdhp,
    int                            *framerate_num,
    int                            *framerate_den
)
{
    AVStream *video_stream = vdhp->format->streams[ vdhp->stream_index ];
    if( vdhp->frame_count == 1 || !(vdhp->seek_base & (SEEK_DTS_BASED | SEEK_PTS_BASED)) )
    {
        *framerate_num = video_stream->r_frame_rate.num;
        *framerate_den = video_stream->r_frame_rate.den;
        return;
    }
    video_frame_info_t *info = vdhp->frame_list;
    int64_t  first_ts;
    int64_t  largest_ts;
    int64_t  second_largest_ts;
    uint64_t stream_timebase;
    if( vdhp->seek_base & SEEK_PTS_BASED )
    {
        first_ts          = info[1].pts;
        largest_ts        = info[2].pts;
        second_largest_ts = info[1].pts;
        stream_timebase   = info[2].pts - info[1].pts;
        for( uint32_t i = 3; i <= vdhp->frame_count; i++ )
        {
            if( info[i].pts == info[i - 1].pts )
            {
                if( vdhp->eh.error_message )
                    vdhp->eh.error_message( vdhp->eh.message_priv,
                                            "Detected PTS %"PRId64" duplication at frame %"PRIu32,
                                            info[i].pts, i );
                goto fail;
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
        for( uint32_t i = 3; i <= vdhp->frame_count; i++ )
        {
            if( info[i].dts == info[i - 1].dts )
            {
                if( vdhp->eh.error_message )
                    vdhp->eh.error_message( vdhp->eh.message_priv,
                                            "Detected DTS %"PRId64" duplication at frame %"PRIu32,
                                            info[i].dts, i );
                goto fail;
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
    double stream_framerate = vdhp->frame_count * ((double)stream_timescale / stream_duration);
    if( try_ntsc_framerate( stream_framerate, framerate_num, framerate_den ) )
        return;
    if( stream_timebase > INT_MAX || (uint64_t)(stream_framerate * stream_timebase + 0.5) > INT_MAX )
        goto fail;
    *framerate_num = (int)(stream_framerate * stream_timebase + 0.5);
    *framerate_den = (int)stream_timebase;
    return;
fail:
    *framerate_num = video_stream->avg_frame_rate.num;
    *framerate_den = video_stream->avg_frame_rate.den;
    return;
}

static int decode_video_sample
(
    lwlibav_video_decode_handler_t *vdhp,
    AVFrame                        *picture,
    int                            *got_picture,
    uint32_t                        sample_number
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

void lwlibav_find_random_accessible_point
(
    lwlibav_video_decode_handler_t *vdhp,
    uint32_t                        presentation_sample_number,
    uint32_t                        decoding_sample_number,
    uint32_t                       *rap_number
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

int64_t lwlibav_get_random_accessible_point_position
(
    lwlibav_video_decode_handler_t *vdhp,
    uint32_t                        rap_number
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
    lwlibav_video_decode_handler_t *vdhp,
    AVFrame                        *picture,
    uint32_t                        presentation_sample_number,
    uint32_t                        rap_number,
    int64_t                         rap_pos,
    int                             error_ignorance
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
    lwlibav_video_decode_handler_t *vdhp,
    AVFrame                        *picture,
    uint32_t                        current,
    uint32_t                        goal,
    uint32_t                        video_sample_count
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

int lwlibav_get_video_frame
(
    lwlibav_video_decode_handler_t *vdhp,
    uint32_t                        frame_number,
    uint32_t                        frame_count
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
        lwlibav_find_random_accessible_point( vdhp, frame_number, 0, &rap_number );
        if( rap_number == vdhp->last_rap_number && frame_number > vdhp->last_frame_number )
            start_number = vdhp->last_frame_number + 1 + vdhp->delay_count;
        else
        {
            /* Require starting to decode from random accessible sample. */
            rap_pos = lwlibav_get_random_accessible_point_position( vdhp, rap_number );
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
            lwlibav_find_random_accessible_point( vdhp, frame_number, rap_number - 1, &rap_number );
            rap_pos = lwlibav_get_random_accessible_point_position( vdhp, rap_number );
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

void lwlibav_cleanup_video_decode_handler( lwlibav_video_decode_handler_t *vdhp )
{
    if( vdhp->input_buffer )
        av_freep( &vdhp->input_buffer );
    if( vdhp->frame_list )
        lw_freep( &vdhp->frame_list );
    if( vdhp->order_converter )
        lw_freep( &vdhp->order_converter );
    if( vdhp->keyframe_list )
        lw_freep( &vdhp->keyframe_list );
    if( vdhp->index_entries )
        av_freep( &vdhp->index_entries );
    if( vdhp->frame_buffer )
        avcodec_free_frame( &vdhp->frame_buffer );
    if( vdhp->ctx )
    {
        avcodec_close( vdhp->ctx );
        vdhp->ctx = NULL;
    }
    if( vdhp->format )
        lavf_close_file( &vdhp->format );
}
