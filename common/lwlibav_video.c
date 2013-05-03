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
#include <libavutil/imgutils.h>
#ifdef __cplusplus
}
#endif  /* __cplusplus */

#include "utils.h"
#include "video_output.h"
#include "lwlibav_dec.h"
#include "lwlibav_video.h"

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
             || lavf_open_file( &vdhp->format, file_path, &vdhp->lh );
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
    ctx->refcounted_frames = 1;
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
    lwlibav_video_output_handler_t *vohp,
    int                            *framerate_num,
    int                            *framerate_den
)
{
    AVStream *video_stream = vdhp->format->streams[ vdhp->stream_index ];
    if( vdhp->frame_count == 1 || !(vdhp->lw_seek_flags & (SEEK_DTS_BASED | SEEK_PTS_BASED)) )
    {
        *framerate_num = video_stream->avg_frame_rate.num;
        *framerate_den = video_stream->avg_frame_rate.den;
        return;
    }
    video_frame_info_t *info = vdhp->frame_list;
    int64_t  first_ts;
    int64_t  largest_ts;
    int64_t  second_largest_ts;
    uint64_t stream_timebase;
    if( vdhp->lw_seek_flags & (SEEK_PTS_BASED | SEEK_PTS_GENERATED) )
    {
        first_ts          = info[1].pts;
        largest_ts        = info[2].pts;
        second_largest_ts = info[1].pts;
        stream_timebase   = info[2].pts - info[1].pts;
        for( uint32_t i = 3; i <= vdhp->frame_count; i++ )
        {
            if( info[i].pts == info[i - 1].pts )
            {
                if( vdhp->lh.show_log )
                    vdhp->lh.show_log( &vdhp->lh, LW_LOG_WARNING,
                                       "Detected PTS %"PRId64" duplication at frame %"PRIu32, info[i].pts, i );
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
                if( vdhp->lh.show_log )
                    vdhp->lh.show_log( &vdhp->lh, LW_LOG_WARNING,
                                       "Detected DTS %"PRId64" duplication at frame %"PRIu32, info[i].dts, i );
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
    double stream_framerate = (vohp->frame_count - (vohp->repeat_correction_ts ? 1 : 0))
                            * ((double)stream_timescale / stream_duration);
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

static uint32_t correct_current_frame_number
(
    lwlibav_video_decode_handler_t *vdhp,
    AVPacket                       *pkt,
    uint32_t                        i,      /* frame_number */
    uint32_t                        goal
)
{
#define MATCH_DTS( j ) (info[j].dts == pkt->dts)
#define MATCH_POS( j ) ((vdhp->lw_seek_flags & SEEK_POS_CORRECTION) && info[j].file_offset == pkt->pos)
    order_converter_t  *oc   = vdhp->order_converter;
    video_frame_info_t *info = vdhp->frame_list;
    uint32_t p = oc ? oc[i].decoding_to_presentation : i;
    if( pkt->dts == AV_NOPTS_VALUE || MATCH_DTS( p ) || MATCH_POS( p ) )
        return i;
    if( pkt->dts > info[p].dts )
    {
        /* too forward */
        uint32_t limit = MIN( goal, vdhp->frame_count );
        if( oc )
            while( !MATCH_DTS( oc[++i].decoding_to_presentation )
                && !MATCH_POS( oc[  i].decoding_to_presentation )
                && i <= limit );
        else
            while( !MATCH_DTS( ++i )
                && !MATCH_POS(   i )
                && i <= limit );
        if( i > limit )
            return 0;
    }
    else
    {
        /* too backward */
        if( oc )
            while( !MATCH_DTS( oc[--i].decoding_to_presentation )
                && !MATCH_POS( oc[  i].decoding_to_presentation )
                && i );
        else
            while( !MATCH_DTS( --i )
                && !MATCH_POS(   i )
                && i );
        if( i == 0 )
            return 0;
    }
    return i;
#undef MATCH_DTS
#undef MATCH_POS
}

static int decode_video_picture
(
    lwlibav_video_decode_handler_t *vdhp,
    AVFrame                        *picture,
    int                            *got_picture,
    uint32_t                       *current,
    uint32_t                        goal,
    uint32_t                        rap_number
)
{
    uint32_t frame_number = *current;
    AVPacket *pkt = &vdhp->packet;
    int ret = lwlibav_get_av_frame( vdhp->format, vdhp->stream_index, frame_number, pkt );
    if( ret > 0 )
        return ret;
    /* Correct the current frame number in order to match DTS since libavformat might have sought wrong position. */
    uint32_t correction_distance = 0;
    if( frame_number == rap_number && (vdhp->lw_seek_flags & SEEK_DTS_BASED) )
    {
        frame_number = correct_current_frame_number( vdhp, pkt, frame_number, goal );
        if( frame_number == 0 )
            return -2;
        if( *current > frame_number )
            /* It seems we got a more backward frame rather than what we requested. */
            correction_distance = *current - frame_number;
        *current = frame_number;
    }
    if( pkt->flags & AV_PKT_FLAG_KEY )
        vdhp->last_rap_number = frame_number;
    /* Avoid decoding frames until the seek correction caused by too backward is done. */
    while( correction_distance )
    {
        ret = lwlibav_get_av_frame( vdhp->format, vdhp->stream_index, ++frame_number, pkt );
        if( ret > 0 )
            return ret;
        if( pkt->flags & AV_PKT_FLAG_KEY )
            vdhp->last_rap_number = frame_number;
        *current = frame_number;
        --correction_distance;
    }
    int64_t pts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
    AVFrame *mov_frame = vdhp->movable_frame_buffer;
    av_frame_unref( mov_frame );
    ret = avcodec_decode_video2( vdhp->ctx, mov_frame, got_picture, pkt );
    /* We can't get the requested frame by feeding a picture if that picture is PAFF field coded.
     * This branch avoids putting empty data on the frame buffer. */
    if( *got_picture )
    {
        av_frame_unref( picture );
        av_frame_move_ref( picture, mov_frame );
    }
    picture->pts = pts;
    if( ret < 0 )
    {
        if( vdhp->lh.show_log )
            vdhp->lh.show_log( &vdhp->lh, LW_LOG_ERROR, "Failed to decode a video frame." );
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
    return (vdhp->lw_seek_flags & SEEK_POS_BASED) ? vdhp->frame_list[presentation_rap_number].file_offset
         : (vdhp->lw_seek_flags & SEEK_PTS_BASED) ? vdhp->frame_list[presentation_rap_number].pts
         : (vdhp->lw_seek_flags & SEEK_DTS_BASED) ? vdhp->frame_list[presentation_rap_number].dts
         :                                          vdhp->frame_list[presentation_rap_number].sample_number;
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
    lwlibav_extradata_handler_t *exhp = &vdhp->exh;
    int extradata_index = vdhp->frame_list[rap_number].extradata_index;
    if( extradata_index != exhp->current_index )
        /* Update the decoder configuration. */
        lwlibav_update_configuration( (lwlibav_decode_handler_t *)vdhp, rap_number, extradata_index, rap_pos );
    else
        lwlibav_flush_buffers( (lwlibav_decode_handler_t *)vdhp );
    if( vdhp->error )
        return 0;
    if( av_seek_frame( vdhp->format, vdhp->stream_index, rap_pos, vdhp->av_seek_flags ) < 0 )
        av_seek_frame( vdhp->format, vdhp->stream_index, rap_pos, vdhp->av_seek_flags | AVSEEK_FLAG_ANY );
    int      got_picture = 0;
    int64_t  rap_pts = AV_NOPTS_VALUE;
    uint32_t current;
    uint32_t decoder_delay = get_decoder_delay( vdhp->ctx );
    uint32_t thread_delay  = decoder_delay - vdhp->ctx->has_b_frames;
    uint32_t goal = presentation_sample_number + decoder_delay;
    exhp->delay_count     = 0;
    vdhp->last_half_frame = 0;
    for( current = rap_number; current <= goal; current++ )
    {
        int ret = decode_video_picture( vdhp, picture, &got_picture, &current, goal, rap_number );
        if( ret == -2 )
            return 0;
        else if( ret >= 1 )
        {
            /* No decoding occurs. */
            got_picture = 0;
            break;
        }
        /* Handle decoder delay derived from PAFF field coded pictures. */
        if( current <= vdhp->frame_count && current >= rap_number + decoder_delay
         && !got_picture && vdhp->frame_list[current].repeat_pict == 0 )
        {
            /* No output picture since the second field coded picture of the next frame is not decoded yet. */
            if( decoder_delay - thread_delay < 2 * vdhp->ctx->has_b_frames + 1UL )
            {
                uint32_t new_decoder_delay = thread_delay + 2 * vdhp->ctx->has_b_frames + 1UL;
                goal += new_decoder_delay - decoder_delay;
                decoder_delay = new_decoder_delay;
            }
        }
        if( got_picture )
        {
            exhp->delay_count = MIN( decoder_delay, current - rap_number );
            uint32_t frame_number = current - exhp->delay_count;
            vdhp->last_half_frame = (frame_number <= vdhp->frame_count && vdhp->frame_list[frame_number].repeat_pict == 0);
        }
        /* Some decoders return -1 when feeding a leading sample.
         * We don't consider as an error if the return value -1 is caused by a leading sample since it's not fatal at all. */
        if( current == vdhp->last_rap_number && picture->pts != AV_NOPTS_VALUE )
            rap_pts = picture->pts;
        if( ret == -1 && (picture->pts == AV_NOPTS_VALUE || picture->pts >= rap_pts) && !error_ignorance )
        {
            if( vdhp->lh.show_log )
                vdhp->lh.show_log( &vdhp->lh, LW_LOG_ERROR, "Failed to decode a video frame." );
            return 0;
        }
    }
    exhp->delay_count = MIN( decoder_delay, current - rap_number );
    if( current > rap_number && vdhp->last_half_frame )
    {
        if( got_picture )
            /* first field of PAFF field coded picture */
            vdhp->last_half_offset = 0;
        else
        {
            /* second field of PAFF field coded picture */
            vdhp->last_half_frame  = UINT32_MAX;
            vdhp->last_half_offset = 1;     /* A picture of the second field is already decoded. */
        }
    }
    else
    {
        vdhp->last_half_frame  = 0;
        vdhp->last_half_offset = 0;
    }
    return current;
}

static inline int copy_last_frame
(
    lwlibav_video_decode_handler_t *vdhp,
    AVFrame                        *picture
)
{
    /* Copy the last decoded and output frame. */
    assert( vdhp->last_frame_buffer );
    if( picture == vdhp->last_frame_buffer )
        return 0;
    av_frame_unref( picture );
    return av_frame_ref( picture, vdhp->last_frame_buffer );
}

static int get_picture
(
    lwlibav_video_decode_handler_t *vdhp,
    AVFrame                        *picture,
    uint32_t                        current,
    uint32_t                        goal,
    uint32_t                        rap_number
)
{
    int got_picture = (current > goal);
    if( got_picture )
    {
        /* The last decoded and output frame is the requested frame. */
        uint32_t frame_number = goal - vdhp->exh.delay_count;
        if( frame_number == vdhp->last_frame_number )
            return copy_last_frame( vdhp, picture );
        else
            return 0;
    }
    while( current <= goal )
    {
        int ret = decode_video_picture( vdhp, picture, &got_picture, &current, goal, rap_number );
        if( ret < 0 )
            return -1;
        else if( ret == 1 )
            /* No more frames. */
            break;
        uint32_t frame_number = current - vdhp->exh.delay_count;
        if( got_picture )
        {
            /* frame coded picture or first field of PAFF field coded picture. */
            vdhp->last_half_frame  = (frame_number <= vdhp->frame_count && vdhp->frame_list[frame_number].repeat_pict == 0);
            vdhp->last_half_offset = 0;
        }
        else
        {
            if( vdhp->last_half_frame )
            {
                /* second field of PAFF field coded picture */
                vdhp->last_half_frame  = UINT32_MAX;
                vdhp->last_half_offset = 1;     /* A picture of the second field is already decoded. */
                if( current == goal )
                {
                    /* The last decoded and output frame is the requested frame. */
                    if( frame_number == vdhp->last_frame_number + 1 )
                        return copy_last_frame( vdhp, picture );
                    else
                        return 0;
                }
            }
            else
            {
                /* frame coded picture but delayed by picture reordering */
                vdhp->last_half_offset = 0;
                vdhp->exh.delay_count += 1;
            }
        }
        ++current;
    }
    /* Flush the last frames. */
    if( current <= goal && current > vdhp->frame_count && vdhp->exh.delay_count )
        while( current <= goal )
        {
            uint32_t frame_number = current - vdhp->exh.delay_count;
            if( vdhp->last_half_frame && vdhp->last_half_offset == 0 )
            {
                /* A picture of the second field is already decoded. */
                vdhp->last_half_frame  = UINT32_MAX;
                vdhp->last_half_offset = 1;
                if( current == goal )
                {
                    /* The last decoded and output frame is the requested frame. */
                    if( frame_number == vdhp->last_frame_number + 1 )
                        return copy_last_frame( vdhp, picture );
                    else
                        return 0;
                }
                ++current;
                continue;
            }
            AVPacket pkt = { 0 };
            av_init_packet( &pkt );
            pkt.data = NULL;
            pkt.size = 0;
            av_frame_unref( picture );
            if( avcodec_decode_video2( vdhp->ctx, picture, &got_picture, &pkt ) < 0 )
            {
                if( vdhp->lh.show_log )
                    vdhp->lh.show_log( &vdhp->lh, LW_LOG_ERROR, "Failed to decode and flush a video frame." );
                return -1;
            }
            if( !got_picture )
                break;
            vdhp->last_half_frame  = (frame_number <= vdhp->frame_count && vdhp->frame_list[frame_number].repeat_pict == 0);
            vdhp->last_half_offset = 0;
            ++current;
        }
    return got_picture ? 0 : -1;
}

static int get_requested_picture
(
    lwlibav_video_decode_handler_t *vdhp,
    AVFrame                        *picture,
    uint32_t                        frame_number
)
{
#define MAX_ERROR_COUNT 3   /* arbitrary */
    if( frame_number > vdhp->frame_count )
        frame_number = vdhp->frame_count;
    if( frame_number == vdhp->last_frame_number
     || frame_number == vdhp->last_frame_number + vdhp->last_half_offset )
    {
        /* The last frame is the requested frame. */
        if( copy_last_frame( vdhp, picture ) < 0 )
            goto video_fail;
        return 0;
    }
    if( frame_number < vdhp->first_valid_frame_number || vdhp->frame_count == 1 )
    {
        /* Copy the first valid video frame data. */
        av_frame_unref( picture );
        if( av_frame_ref( picture, vdhp->first_valid_frame ) < 0 )
            goto video_fail;
        /* Force seeking at the next access for valid video frame. */
        vdhp->last_frame_number = vdhp->frame_count + 1;
        vdhp->last_frame_buffer = picture;
        return 0;
    }
    uint32_t start_number;  /* number of sample, for normal decoding, where decoding starts excluding decoding delay */
    uint32_t rap_number;    /* number of sample, for seeking, where decoding starts excluding decoding delay */
    uint32_t last_frame_number = vdhp->last_frame_number + vdhp->last_half_offset;
    int      seek_mode         = vdhp->seek_mode;
    int64_t  rap_pos           = INT64_MIN;
    if( frame_number > last_frame_number
     && frame_number <= last_frame_number + vdhp->forward_seek_threshold )
    {
        start_number = last_frame_number + 1 + vdhp->exh.delay_count;
        rap_number   = vdhp->last_rap_number;
    }
    else
    {
        lwlibav_find_random_accessible_point( vdhp, frame_number, 0, &rap_number );
        if( rap_number == vdhp->last_rap_number && frame_number > last_frame_number )
            start_number = last_frame_number + 1 + vdhp->exh.delay_count;
        else
        {
            /* Require starting to decode from random accessible sample. */
            rap_pos = lwlibav_get_random_accessible_point_position( vdhp, rap_number );
            vdhp->last_rap_number = rap_number;
            start_number = seek_video( vdhp, picture, frame_number, rap_number, rap_pos, seek_mode != SEEK_MODE_NORMAL );
        }
    }
    /* Get requested picture. */
    int error_count = 0;
    while( start_number == 0
        || get_picture( vdhp, picture, start_number, frame_number + vdhp->exh.delay_count, rap_number ) < 0 )
    {
        /* Failed to get desired picture. */
        if( vdhp->error || seek_mode == SEEK_MODE_AGGRESSIVE )
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
    vdhp->last_frame_buffer = picture;
    if( vdhp->last_half_frame == UINT32_MAX )
    {
        /* The second field was requested in this time.
         * Shift the last frame number to the first field number. */
        vdhp->last_frame_number -= 1;
        vdhp->last_half_frame    = 1;
    }
    return 0;
video_fail:
    /* fatal error of decoding */
    if( vdhp->lh.show_log )
        vdhp->lh.show_log( &vdhp->lh, LW_LOG_ERROR, "Couldn't get the requested video frame." );
    return -1;
#undef MAX_ERROR_COUNT
}

static inline int check_frame_buffer_identical
(
    AVFrame *a,
    AVFrame *b
)
{
    return !memcmp( a->data,     b->data,     sizeof(a->data) )
        && !memcmp( a->linesize, b->linesize, sizeof(a->linesize) );
}

static inline int copy_frame
(
    lw_log_handler_t *lhp,
    AVFrame          *dst,
    AVFrame          *src
)
{
    av_frame_unref( dst );
    if( av_frame_ref( dst, src ) < 0 )
    {
        if( lhp->show_log )
            lhp->show_log( lhp, LW_LOG_ERROR, "Failed to reference a video frame.\n" );
        return -1;
    }
    /* Treat this frame as interlaced. */
    dst->interlaced_frame = 1;
    return 0;
}

static inline int copy_field
(
    lw_log_handler_t *lhp,
    AVFrame          *dst,
    AVFrame          *src,
    int               line_offset
)
{
    /* Check if the destination is writable. */
    if( av_frame_is_writable( dst ) == 0 )
    {
        /* The destination is NOT writable, so allocate new buffers and copy the data. */
        av_frame_unref( dst );
        if( av_frame_ref( dst, src ) < 0 )
        {
            if( lhp->show_log )
                lhp->show_log( lhp, LW_LOG_ERROR, "Failed to reference a video frame.\n" );
            return -1;
        }
        if( av_frame_make_writable( dst ) < 0 )
        {
            if( lhp->show_log )
                lhp->show_log( lhp, LW_LOG_ERROR, "Failed to make a video frame writable.\n" );
            return -1;
        }
        /* For direct rendering, the destination can not know
         * whether the value at the address held by the opaque pointer is valid or not.
         * Anyway, the opaque pointer for direct rendering shall be set to NULL. */
        dst->opaque = NULL;
    }
    else
    {
        /* The destination is writable. Copy field data from the source. */
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get( (enum AVPixelFormat)dst->format );
        int number_of_planes = av_pix_fmt_count_planes( (enum AVPixelFormat)dst->format );
        int height           = MIN( dst->height, src->height );
        for( int i = 0; i < number_of_planes; i++ )
        {
            int r_shift = 1 + ((i == 1 || i == 2) ? desc->log2_chroma_h : 0);
            int field_height = (height >> r_shift) + (line_offset == 0 && (height & 1) ? 1 : 0);
            av_image_copy_plane( dst->data[i] + dst->linesize[i] * line_offset, 2 * dst->linesize[i],
                                 src->data[i] + src->linesize[i] * line_offset, 2 * src->linesize[i],
                                 MIN( dst->linesize[i], src->linesize[i] ),
                                 field_height );
        }
    }
    /* Treat this frame as interlaced. */
    dst->interlaced_frame = 1;
    return 0;
}

int lwlibav_get_video_frame
(
    lwlibav_video_decode_handler_t *vdhp,
    lwlibav_video_output_handler_t *vohp,
    uint32_t                        frame_number
)
{
    if( !vohp->repeat_control )
        return get_requested_picture( vdhp, vdhp->frame_buffer, frame_number );
    /* Get picture to applied the repeat control. */
    uint32_t t = vohp->frame_order_list[frame_number].top;
    uint32_t b = vohp->frame_order_list[frame_number].bottom;
    uint32_t first_field_number  = MIN( t, b );
    uint32_t second_field_number = MAX( t, b );
    /* Check repeat targets and cache datas. */
    enum
    {
        REPEAT_CONTROL_COPIED_FROM_CACHE   = 0x00,
        REPEAT_CONTROL_DECODE_TOP_FIELD    = 0x01,
        REPEAT_CONTROL_DECODE_BOTTOM_FIELD = 0x02,
        REPEAT_CONTROL_DECODE_BOTH_FIELDS  = 0x03,  /* REPEAT_CONTROL_DECODE_TOP_FIELD | REPEAT_CONTROL_DECODE_BOTTOM_FIELD */
        REPEAT_CONTROL_DECODE_ONE_FRAME    = 0x04
    };
    int repeat_control;
    if( first_field_number == second_field_number )
    {
        repeat_control = REPEAT_CONTROL_DECODE_ONE_FRAME;
        if( first_field_number == vohp->frame_cache_numbers[0] )
            return copy_frame( &vdhp->lh, vdhp->frame_buffer, vohp->frame_cache_buffers[0] );
        if( first_field_number == vohp->frame_cache_numbers[1] )
            return copy_frame( &vdhp->lh, vdhp->frame_buffer, vohp->frame_cache_buffers[1] );
        if( first_field_number != vohp->frame_order_list[frame_number - 1].top
         && first_field_number != vohp->frame_order_list[frame_number - 1].bottom
         && first_field_number != vohp->frame_order_list[frame_number + 1].top
         && first_field_number != vohp->frame_order_list[frame_number + 1].bottom )
        {
            if( get_requested_picture( vdhp, vdhp->frame_buffer, first_field_number ) < 0 )
                return -1;
            /* Treat this frame as interlaced. */
            vdhp->frame_buffer->interlaced_frame = 1;
            return 0;
        }
    }
    else
    {
        repeat_control = REPEAT_CONTROL_DECODE_BOTH_FIELDS;
        for( int i = 0; i < REPEAT_CONTROL_CACHE_NUM; i++ )
        {
            if( t == vohp->frame_cache_numbers[i] )
            {
                if( copy_field( &vdhp->lh, vdhp->frame_buffer, vohp->frame_cache_buffers[i], 0 ) < 0 )
                    return -1;
                repeat_control &= ~REPEAT_CONTROL_DECODE_TOP_FIELD;
            }
            if( b == vohp->frame_cache_numbers[i] )
            {
                if( copy_field( &vdhp->lh, vdhp->frame_buffer, vohp->frame_cache_buffers[i], 1 ) < 0 )
                    return -1;
                repeat_control &= ~REPEAT_CONTROL_DECODE_BOTTOM_FIELD;
            }
        }
        if( repeat_control == REPEAT_CONTROL_COPIED_FROM_CACHE )
            return 0;
    }
    /* Decode target frames and copy to output buffer. */
    if( repeat_control == REPEAT_CONTROL_DECODE_BOTH_FIELDS )
    {
        /* Decode 2 frames, and copy each a top and bottom fields. */
        if( get_requested_picture( vdhp, vohp->frame_cache_buffers[0], first_field_number ) < 0 )
            return -1;
        vohp->frame_cache_numbers[0] = first_field_number;
        if( get_requested_picture( vdhp, vohp->frame_cache_buffers[1], second_field_number ) < 0 )
            return -1;
        vohp->frame_cache_numbers[1] = second_field_number;
        if( check_frame_buffer_identical( vohp->frame_cache_buffers[0], vohp->frame_cache_buffers[1] ) )
            return copy_frame( &vdhp->lh, vdhp->frame_buffer, vohp->frame_cache_buffers[0] );
        if( copy_field( &vdhp->lh, vdhp->frame_buffer, vohp->frame_cache_buffers[0], t > b ? 1 : 0 ) < 0
         || copy_field( &vdhp->lh, vdhp->frame_buffer, vohp->frame_cache_buffers[1], t < b ? 1 : 0 ) < 0 )
            return -1;
        return 0;
    }
    else
    {
        /* Decode 1 frame, and copy 1 frame or 1 field. */
        int decode_number = repeat_control == REPEAT_CONTROL_DECODE_ONE_FRAME ? first_field_number
                          : repeat_control == REPEAT_CONTROL_DECODE_TOP_FIELD ? t : b;
        int idx = vohp->frame_cache_numbers[0] > vohp->frame_cache_numbers[1] ? 1 : 0;
        if( get_requested_picture( vdhp, vohp->frame_cache_buffers[idx], decode_number ) < 0 )
            return -1;
        vohp->frame_cache_numbers[idx] = decode_number;
        if( repeat_control == REPEAT_CONTROL_DECODE_ONE_FRAME )
            return copy_frame( &vdhp->lh, vdhp->frame_buffer, vohp->frame_cache_buffers[idx] );
        else
            return copy_field( &vdhp->lh, vdhp->frame_buffer, vohp->frame_cache_buffers[idx],
                               repeat_control == REPEAT_CONTROL_DECODE_TOP_FIELD ? 0 : 1 );
    }
}

int lwlibav_is_keyframe
(
    lwlibav_video_decode_handler_t *vdhp,
    lwlibav_video_output_handler_t *vohp,
    uint32_t                        frame_number
)
{
    assert( frame_number );
    if( vohp->repeat_control )
    {
        lw_video_frame_order_t *order = &vohp->frame_order_list[frame_number];
        return (vdhp->frame_list[ order->top    ].keyframe
             && order->top    != (order - 1)->top && order->top    != (order - 1)->bottom)
            || (vdhp->frame_list[ order->bottom ].keyframe
             && order->bottom != (order - 1)->top && order->bottom != (order - 1)->bottom);
    }
    return vdhp->frame_list[frame_number].keyframe;
}

void lwlibav_cleanup_video_decode_handler
(
    lwlibav_video_decode_handler_t *vdhp
)
{
    lwlibav_extradata_handler_t *exhp = &vdhp->exh;
    if( exhp->entries )
    {
        for( int i = 0; i < exhp->entry_count; i++ )
            if( exhp->entries[i].extradata )
                av_free( exhp->entries[i].extradata );
        free( exhp->entries );
    }
    av_free_packet( &vdhp->packet );
    if( vdhp->frame_list )
        lw_freep( &vdhp->frame_list );
    if( vdhp->order_converter )
        lw_freep( &vdhp->order_converter );
    if( vdhp->keyframe_list )
        lw_freep( &vdhp->keyframe_list );
    if( vdhp->index_entries )
        av_freep( &vdhp->index_entries );
    if( vdhp->frame_buffer )
    {
        /* Libavcodec frees the buffers internally in avcodec_close() when reference-count is disabled.
         * In that case, av_frame_free() will make double free and this branch shall avoid this. */
        if( vdhp->ctx && vdhp->ctx->refcounted_frames )
            av_frame_free( &vdhp->frame_buffer );
        else
            avcodec_free_frame( &vdhp->frame_buffer );
    }
    if( vdhp->first_valid_frame )
        av_frame_free( &vdhp->first_valid_frame );
    if( vdhp->movable_frame_buffer )
        av_frame_free( &vdhp->movable_frame_buffer );
    if( vdhp->ctx )
    {
        avcodec_close( vdhp->ctx );
        vdhp->ctx = NULL;
    }
    if( vdhp->format )
        lavf_close_file( &vdhp->format );
}

int lwlibav_find_first_valid_video_frame
(
    lwlibav_video_decode_handler_t *vdhp
)
{
    vdhp->movable_frame_buffer = av_frame_alloc();
    if( !vdhp->movable_frame_buffer )
        return -1;
    vdhp->av_seek_flags = (vdhp->lw_seek_flags & SEEK_POS_BASED) ? AVSEEK_FLAG_BYTE
                        : vdhp->lw_seek_flags == 0               ? AVSEEK_FLAG_FRAME
                        : 0;
    if( vdhp->frame_count != 1 )
    {
        vdhp->av_seek_flags |= AVSEEK_FLAG_BACKWARD;
        uint32_t rap_number;
        lwlibav_find_random_accessible_point( vdhp, 1, 0, &rap_number );
        int64_t rap_pos = lwlibav_get_random_accessible_point_position( vdhp, rap_number );
        if( av_seek_frame( vdhp->format, vdhp->stream_index, rap_pos, vdhp->av_seek_flags ) < 0 )
            av_seek_frame( vdhp->format, vdhp->stream_index, rap_pos, vdhp->av_seek_flags | AVSEEK_FLAG_ANY );
    }
    uint32_t decoder_delay = get_decoder_delay( vdhp->ctx );
    uint32_t thread_delay  = decoder_delay - vdhp->ctx->has_b_frames;
    AVPacket *pkt = &vdhp->packet;
    for( uint32_t i = 1; i <= vdhp->frame_count + vdhp->exh.delay_count; i++ )
    {
        lwlibav_get_av_frame( vdhp->format, vdhp->stream_index, i, pkt );
        av_frame_unref( vdhp->frame_buffer );
        int got_picture;
        int ret = avcodec_decode_video2( vdhp->ctx, vdhp->frame_buffer, &got_picture, pkt );
        /* Handle decoder delay derived from PAFF field coded pictures. */
        if( i <= vdhp->frame_count && i > decoder_delay
         && !got_picture && vdhp->frame_list[i].repeat_pict == 0 )
        {
            /* No output picture since the second field coded picture of the next frame is not decoded yet. */
            if( decoder_delay - thread_delay < 2 * vdhp->ctx->has_b_frames + 1UL )
                decoder_delay = thread_delay + 2 * vdhp->ctx->has_b_frames + 1UL;
        }
        if( ret >= 0 )
        {
            if( got_picture )
            {
                /* Found the first valid video frame. */
                vdhp->first_valid_frame_number = i - MIN( decoder_delay, vdhp->exh.delay_count );
                if( vdhp->first_valid_frame_number > 1 || vdhp->frame_count == 1 )
                {
                    vdhp->first_valid_frame = av_frame_clone( vdhp->frame_buffer );
                    if( !vdhp->first_valid_frame )
                        return -1;
                    av_frame_unref( vdhp->frame_buffer );
                }
                break;
            }
            else if( pkt->data )
                /* Output is delayed. */
                ++ vdhp->exh.delay_count;
            else
                /* No more output.
                 * Failed to find the first valid video frame. */
                return -1;
        }
    }
    return 0;
}

void set_video_basic_settings
(
    lwlibav_decode_handler_t *dhp,
    uint32_t                  frame_number
)
{
    lwlibav_video_decode_handler_t *vdhp = (lwlibav_video_decode_handler_t *)dhp;
    AVCodecContext *ctx = vdhp->format->streams[ vdhp->stream_index ]->codec;
    lwlibav_extradata_t *entry = &vdhp->exh.entries[ vdhp->frame_list[frame_number].extradata_index ];
    ctx->width                 = entry->width;
    ctx->height                = entry->height;
    ctx->pix_fmt               = entry->pixel_format;
    ctx->bits_per_coded_sample = entry->bits_per_sample;
}

int try_decode_video_frame
(
    lwlibav_decode_handler_t *dhp,
    uint32_t                  frame_number,
    int64_t                   rap_pos,
    char                     *error_string
)
{
    AVFrame *picture = av_frame_alloc();
    if( !picture )
    {
        strcpy( error_string, "Failed to alloc AVFrame to set up a decoder configuration.\n" );
        return -1;
    }
    lwlibav_video_decode_handler_t *vdhp = (lwlibav_video_decode_handler_t *)dhp;
    AVFormatContext *format_ctx   = vdhp->format;
    int              stream_index = vdhp->stream_index;
    AVCodecContext  *ctx          = format_ctx->streams[stream_index]->codec;
    ctx->refcounted_frames = 1;
    if( av_seek_frame( format_ctx, stream_index, rap_pos, vdhp->av_seek_flags ) < 0 )
        av_seek_frame( format_ctx, stream_index, rap_pos, vdhp->av_seek_flags | AVSEEK_FLAG_ANY );
    do
    {
        if( frame_number > vdhp->frame_count )
            break;
        /* Get a frame. */
        AVPacket pkt = { 0 };
        int extradata_index = vdhp->frame_list[frame_number].extradata_index;
        if( extradata_index != vdhp->exh.current_index )
            break;
        int ret = lwlibav_get_av_frame( format_ctx, stream_index, frame_number, &pkt );
        if( ret > 0 )
            break;
        else if( ret < 0 )
        {
            if( ctx->pix_fmt == AV_PIX_FMT_NONE )
                strcpy( error_string, "Failed to set up pixel format.\n" );
            else
                strcpy( error_string, "Failed to set up resolution.\n" );
            av_frame_free( &picture );
            return -1;
        }
        /* Try decode a frame. */
        av_frame_unref( picture );
        int dummy;
        avcodec_decode_video2( ctx, picture, &dummy, &pkt );
        ++frame_number;
    } while( ctx->width == 0 || ctx->height == 0 || ctx->pix_fmt == AV_PIX_FMT_NONE );
    av_frame_free( &picture );
    return 0;
}
