/*****************************************************************************
 * libavsmash_video.c / libavsmash_video.cpp
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

#include <inttypes.h>

#ifdef __cplusplus
extern "C"
{
#endif  /* __cplusplus */
#define LSMASH_DEMUXER_ENABLED
#include <lsmash.h>
#include <libavcodec/avcodec.h>
#ifdef __cplusplus
}
#endif  /* __cplusplus */

#include "utils.h"
#include "video_output.h"
#include "libavsmash.h"
#include "libavsmash_video.h"

int libavsmash_setup_timestamp_info
(
    libavsmash_video_decode_handler_t *vdhp,
    int64_t                           *framerate_num,
    int64_t                           *framerate_den,
    uint32_t                           sample_count
)
{
    uint64_t media_timescale = lsmash_get_media_timescale( vdhp->root, vdhp->track_ID );
    if( sample_count == 1 )
    {
        /* Calculate average framerate. */
        uint64_t media_duration = lsmash_get_media_duration_from_media_timeline( vdhp->root, vdhp->track_ID );
        if( media_duration == 0 )
            media_duration = INT32_MAX;
        reduce_fraction( &media_timescale, &media_duration );
        *framerate_num = (int64_t)media_timescale;
        *framerate_den = (int64_t)media_duration;
        return 0;
    }
    lw_log_handler_t *lhp = &vdhp->config.lh;
    lsmash_media_ts_list_t ts_list;
    if( lsmash_get_media_timestamps( vdhp->root, vdhp->track_ID, &ts_list ) )
    {
        if( lhp->show_log )
            lhp->show_log( lhp, LW_LOG_ERROR, "Failed to get timestamps." );
        return -1;
    }
    if( ts_list.sample_count != sample_count )
    {
        if( lhp->show_log )
            lhp->show_log( lhp, LW_LOG_ERROR, "Failed to count number of video samples." );
        return -1;
    }
    uint32_t composition_sample_delay;
    if( lsmash_get_max_sample_delay( &ts_list, &composition_sample_delay ) )
    {
        lsmash_delete_media_timestamps( &ts_list );
        if( lhp->show_log )
            lhp->show_log( lhp, LW_LOG_ERROR, "Failed to get composition delay." );
        return -1;
    }
    if( composition_sample_delay )
    {
        /* Consider composition order for keyframe detection.
         * Note: sample number for L-SMASH is 1-origin. */
        vdhp->order_converter = (order_converter_t *)lw_malloc_zero( (ts_list.sample_count + 1) * sizeof(order_converter_t) );
        if( !vdhp->order_converter )
        {
            lsmash_delete_media_timestamps( &ts_list );
            if( lhp->show_log )
                lhp->show_log( lhp, LW_LOG_ERROR, "Failed to allocate memory." );
            return -1;
        }
        for( uint32_t i = 0; i < ts_list.sample_count; i++ )
            ts_list.timestamp[i].dts = i + 1;
        lsmash_sort_timestamps_composition_order( &ts_list );
        for( uint32_t i = 0; i < ts_list.sample_count; i++ )
            vdhp->order_converter[i + 1].composition_to_decoding = (uint32_t)ts_list.timestamp[i].dts;
    }
    /* Calculate average framerate. */
    uint64_t largest_cts          = ts_list.timestamp[0].cts;
    uint64_t second_largest_cts   = 0;
    uint64_t first_duration       = ts_list.timestamp[1].cts - ts_list.timestamp[0].cts;
    uint64_t composition_timebase = first_duration;
    int      strict_cfr           = 1;
    for( uint32_t i = 1; i < ts_list.sample_count; i++ )
    {
        uint64_t duration = ts_list.timestamp[i].cts - ts_list.timestamp[i - 1].cts;
        if( duration == 0 )
        {
            lsmash_delete_media_timestamps( &ts_list );
            if( lhp->show_log )
                lhp->show_log( lhp, LW_LOG_WARNING, "Detected CTS duplication at frame %"PRIu32, i );
            return 0;
        }
        if( strict_cfr && duration != first_duration )
            strict_cfr = 0;
        composition_timebase = get_gcd( composition_timebase, duration );
        second_largest_cts   = largest_cts;
        largest_cts          = ts_list.timestamp[i].cts;
    }
    uint64_t reduce = reduce_fraction( &media_timescale, &composition_timebase );
    uint64_t composition_duration = ((largest_cts - ts_list.timestamp[0].cts) + (largest_cts - second_largest_cts)) / reduce;
    lsmash_delete_media_timestamps( &ts_list );
    double avg_frame_rate = (sample_count * ((double)media_timescale / composition_duration));
    if( strict_cfr || !lw_try_rational_framerate( avg_frame_rate, framerate_num, framerate_den, composition_timebase ) )
    {
        uint64_t num = (uint64_t)(avg_frame_rate * composition_timebase + 0.5);
        uint64_t den = composition_timebase;
        if( num && den )
            reduce_fraction( &num, &den );
        else
        {
            num = 1;
            den = 1;
        }
        *framerate_num = (int64_t)num;
        *framerate_den = (int64_t)den;
    }
    return 0;
}

static int decode_video_sample
(
    libavsmash_video_decode_handler_t *vdhp,
    AVFrame                           *picture,
    int                               *got_picture,
    uint32_t                           sample_number
)
{
    AVPacket pkt = { 0 };
    int ret = get_sample( vdhp->root, vdhp->track_ID, sample_number, &vdhp->config, &pkt );
    if( ret )
        return ret;
    if( pkt.flags != ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE )
    {
        pkt.flags = AV_PKT_FLAG_KEY;
        vdhp->last_rap_number = sample_number;
    }
    else
        pkt.flags = 0;
    av_frame_unref( picture );
    uint64_t cts = pkt.pts;
    ret = avcodec_decode_video2( vdhp->config.ctx, picture, got_picture, &pkt );
    picture->pts = cts;
    if( ret < 0 )
    {
#ifdef DEBUG_VIDEO
        if( config->error_message )
            config->error_message( config->message_priv, "Failed to decode a video frame." );
#endif
        return -1;
    }
    return 0;
}

static int find_random_accessible_point
(
    libavsmash_video_decode_handler_t *vdhp,
    uint32_t                           composition_sample_number,
    uint32_t                           decoding_sample_number,
    uint32_t                          *rap_number
)
{
    if( decoding_sample_number == 0 )
        decoding_sample_number = get_decoding_sample_number( vdhp->order_converter, composition_sample_number );
    lsmash_random_access_flag ra_flags;
    uint32_t distance;  /* distance from the closest random accessible point to the previous. */
    uint32_t number_of_leadings;
    if( lsmash_get_closest_random_accessible_point_detail_from_media_timeline( vdhp->root, vdhp->track_ID, decoding_sample_number,
                                                                               rap_number, &ra_flags, &number_of_leadings, &distance ) )
        *rap_number = 1;
    int roll_recovery = !!(ra_flags & ISOM_SAMPLE_RANDOM_ACCESS_FLAG_GDR);
    int is_leading    = number_of_leadings && (decoding_sample_number - *rap_number <= number_of_leadings);
    if( (roll_recovery || is_leading) && *rap_number > distance )
        *rap_number -= distance;
    /* Check whether random accessible point has the same decoder configuration or not. */
    decoding_sample_number = get_decoding_sample_number( vdhp->order_converter, composition_sample_number );
    do
    {
        lsmash_sample_t sample;
        lsmash_sample_t rap_sample;
        if( lsmash_get_sample_info_from_media_timeline( vdhp->root, vdhp->track_ID, decoding_sample_number, &sample )
         || lsmash_get_sample_info_from_media_timeline( vdhp->root, vdhp->track_ID, *rap_number, &rap_sample ) )
        {
            /* Fatal error. */
            *rap_number = vdhp->last_rap_number;
            return 0;
        }
        if( sample.index == rap_sample.index )
            break;
        uint32_t sample_index = sample.index;
        for( uint32_t i = decoding_sample_number - 1; i; i-- )
        {
            if( lsmash_get_sample_info_from_media_timeline( vdhp->root, vdhp->track_ID, i, &sample ) )
            {
                /* Fatal error. */
                *rap_number = vdhp->last_rap_number;
                return 0;
            }
            if( sample.index != sample_index )
            {
                if( distance )
                {
                    *rap_number += distance;
                    distance = 0;
                    continue;
                }
                else
                    *rap_number = i + 1;
            }
        }
        break;
    } while( 1 );
    return roll_recovery;
}

/* This function returns the number of the next sample. */
static uint32_t seek_video
(
    libavsmash_video_decode_handler_t *vdhp,
    AVFrame                           *picture,
    uint32_t                           composition_sample_number,
    uint32_t                           rap_number,
    int                                error_ignorance
)
{
    /* Prepare to decode from random accessible sample. */
    codec_configuration_t *config = &vdhp->config;
    if( config->update_pending )
        /* Update the decoder configuration. */
        update_configuration( vdhp->root, vdhp->track_ID, config );
    else
        libavsmash_flush_buffers( config );
    if( config->error )
        return 0;
    int dummy;
    uint64_t rap_cts = 0;
    uint32_t i;
    uint32_t decoder_delay = get_decoder_delay( config->ctx );
    for( i = rap_number; i < composition_sample_number + decoder_delay; i++ )
    {
        if( config->index == config->queue.index )
            config->delay_count = MIN( decoder_delay, i - rap_number );
        int ret = decode_video_sample( vdhp, picture, &dummy, i );
        /* Some decoders return -1 when feeding a leading sample.
         * We don't consider as an error if the return value -1 is caused by a leading sample since it's not fatal at all. */
        if( i == vdhp->last_rap_number )
            rap_cts = picture->pts;
        if( ret == -1 && (uint64_t)picture->pts >= rap_cts && !error_ignorance )
        {
#ifdef DEBUG_VIDEO
            if( config->error_message )
                config->error_message( config->message_priv, "Failed to decode a video frame." );
#endif
            return 0;
        }
        else if( ret >= 1 )
            /* No decoding occurs. */
            break;
    }
    if( config->index == config->queue.index )
        config->delay_count = MIN( decoder_delay, i - rap_number );
    return i;
}

static int get_picture
(
    libavsmash_video_decode_handler_t *vdhp,
    AVFrame                           *picture,
    uint32_t                           current,
    uint32_t                           goal,
    uint32_t                           sample_count
)
{
    codec_configuration_t *config = &vdhp->config;
    int got_picture = (current > goal);
    while( current <= goal )
    {
        int ret = decode_video_sample( vdhp, picture, &got_picture, current );
        if( ret == -1 )
            return -1;
        else if( ret == 1 )
            /* Sample doesn't exist. */
            break;
        ++current;
        if( config->update_pending )
            /* A new decoder configuration is needed. Anyway, stop getting picture. */
            break;
        if( !got_picture )
            ++ config->delay_count;
    }
    /* Flush the last frames. */
    if( current > sample_count && get_decoder_delay( config->ctx ) )
        while( current <= goal )
        {
            AVPacket pkt = { 0 };
            av_init_packet( &pkt );
            pkt.data = NULL;
            pkt.size = 0;
            av_frame_unref( picture );
            if( avcodec_decode_video2( config->ctx, picture, &got_picture, &pkt ) < 0 )
            {
#ifdef DEBUG_VIDEO
                if( config->error_message )
                    config->error_message( config->message_priv, "Failed to decode and flush a video frame." );
#endif
                return -1;
            }
            ++current;
        }
    return got_picture ? 0 : -1;
}

int libavsmash_get_video_frame
(
    libavsmash_video_decode_handler_t *vdhp,
    uint32_t                           sample_number,
    uint32_t                           sample_count
)
{
#define MAX_ERROR_COUNT 3       /* arbitrary */
    codec_configuration_t *config  = &vdhp->config;
    AVFrame               *picture = vdhp->frame_buffer;
    uint32_t               config_index;
    if( sample_number < vdhp->first_valid_frame_number || sample_count == 1 )
    {
        /* Get the index of the decoder configuration. */
        lsmash_sample_t sample;
        uint32_t decoding_sample_number = get_decoding_sample_number( vdhp->order_converter, vdhp->first_valid_frame_number );
        if( lsmash_get_sample_info_from_media_timeline( vdhp->root, vdhp->track_ID, decoding_sample_number, &sample ) < 0 )
            goto video_fail;
        config_index = sample.index;
        /* Copy the first valid video frame data. */
        av_frame_unref( picture );
        if( av_frame_ref( picture, vdhp->first_valid_frame ) < 0 )
            goto video_fail;
        /* Force seeking at the next access for valid video frame. */
        vdhp->last_sample_number = sample_count + 1;
        goto return_frame;
    }
    uint32_t start_number;  /* number of sample, for normal decoding, where decoding starts excluding decoding delay */
    uint32_t rap_number;    /* number of sample, for seeking, where decoding starts excluding decoding delay */
    int seek_mode = vdhp->seek_mode;
    int roll_recovery = 0;
    if( sample_number > vdhp->last_sample_number
     && sample_number <= vdhp->last_sample_number + vdhp->forward_seek_threshold )
    {
        start_number = vdhp->last_sample_number + 1 + config->delay_count;
        rap_number   = vdhp->last_rap_number;
    }
    else
    {
        roll_recovery = find_random_accessible_point( vdhp, sample_number, 0, &rap_number );
        if( rap_number == vdhp->last_rap_number && sample_number > vdhp->last_sample_number )
        {
            roll_recovery = 0;
            start_number  = vdhp->last_sample_number + 1 + config->delay_count;
        }
        else
        {
            /* Require starting to decode from random accessible sample. */
            vdhp->last_rap_number = rap_number;
            start_number = seek_video( vdhp, picture, sample_number, rap_number, roll_recovery || seek_mode != SEEK_MODE_NORMAL );
        }
    }
    /* Get the desired picture. */
    int error_count = 0;
    while( start_number == 0    /* Failed to seek. */
     || config->update_pending  /* Need to update the decoder configuration to decode pictures. */
     || get_picture( vdhp, picture, start_number, sample_number + config->delay_count, sample_count ) )
    {
        if( config->update_pending )
        {
            roll_recovery = find_random_accessible_point( vdhp, sample_number, 0, &rap_number );
            vdhp->last_rap_number = rap_number;
        }
        else
        {
            /* Failed to get the desired picture. */
            if( config->error || seek_mode == SEEK_MODE_AGGRESSIVE )
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
                roll_recovery = find_random_accessible_point( vdhp, sample_number, rap_number - 1, &rap_number );
                if( vdhp->last_rap_number == rap_number )
                    goto video_fail;
                vdhp->last_rap_number = rap_number;
            }
        }
        start_number = seek_video( vdhp, picture, sample_number, rap_number, roll_recovery || seek_mode != SEEK_MODE_NORMAL );
    }
    vdhp->last_sample_number = sample_number;
    config_index = config->index;
return_frame:;
    /* Don't exceed the maximum presentation size specified for each sequence. */
    extended_summary_t *extended = &config->entries[ config_index - 1 ].extended;
    if( config->ctx->width > extended->width )
        config->ctx->width = extended->width;
    if( config->ctx->height > extended->height )
        config->ctx->height = extended->height;
    return 0;
video_fail:
    /* fatal error of decoding */
#ifdef DEBUG_VIDEO
    if( config->error_message )
        config->error_message( config->message_priv, "Couldn't read video frame." );
#endif
    return -1;
#undef MAX_ERROR_COUNT
}

int libavsmash_find_first_valid_video_frame
(
    libavsmash_video_decode_handler_t *vdhp,
    uint32_t                           sample_count
)
{
    codec_configuration_t *config = &vdhp->config;
    config->ctx->refcounted_frames = 1;
    for( uint32_t i = 1; i <= sample_count + get_decoder_delay( config->ctx ); i++ )
    {
        AVPacket pkt = { 0 };
        get_sample( vdhp->root, vdhp->track_ID, i, config, &pkt );
        av_frame_unref( vdhp->frame_buffer );
        int got_picture;
        if( avcodec_decode_video2( config->ctx, vdhp->frame_buffer, &got_picture, &pkt ) >= 0 && got_picture )
        {
            vdhp->first_valid_frame_number = i - MIN( get_decoder_delay( config->ctx ), config->delay_count );
            if( vdhp->first_valid_frame_number > 1 || sample_count == 1 )
            {
                vdhp->first_valid_frame = av_frame_clone( vdhp->frame_buffer );
                if( !vdhp->first_valid_frame )
                    return -1;
                av_frame_unref( vdhp->frame_buffer );
            }
            break;
        }
        else if( pkt.data )
            ++ config->delay_count;
    }
    return 0;
}

void libavsmash_cleanup_video_decode_handler
(
    libavsmash_video_decode_handler_t *vdhp
)
{
    if( vdhp->order_converter )
        lw_freep( &vdhp->order_converter );
    if( vdhp->frame_buffer )
        av_frame_free( &vdhp->frame_buffer );
    if( vdhp->first_valid_frame )
        av_frame_free( &vdhp->first_valid_frame );
    cleanup_configuration( &vdhp->config );
}
