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
#include "libavsmash.h"
#include "libavsmash_video.h"

static int decode_video_sample
(
    video_decode_handler_t *vdhp,
    AVFrame                *picture,
    int                    *got_picture,
    uint32_t                sample_number
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
    avcodec_get_frame_defaults( picture );
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
    video_decode_handler_t *vdhp,
    uint32_t                composition_sample_number,
    uint32_t                decoding_sample_number,
    uint32_t               *rap_number
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
    video_decode_handler_t *vdhp,
    AVFrame                *picture,
    uint32_t                composition_sample_number,
    uint32_t                rap_number,
    int                     error_ignorance
)
{
    /* Prepare to decode from random accessible sample. */
    codec_configuration_t *config = &vdhp->config;
    if( config->update_pending )
        /* Update the decoder configuration. */
        update_configuration( vdhp->root, vdhp->track_ID, config );
    else
        flush_buffers( config );
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
    video_decode_handler_t *vdhp,
    AVFrame                *picture,
    uint32_t                current,
    uint32_t                goal,
    uint32_t                sample_count
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
            avcodec_get_frame_defaults( picture );
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
    video_decode_handler_t *vdhp,
    uint32_t                sample_number,
    uint32_t                sample_count
)
{
#define MAX_ERROR_COUNT 3       /* arbitrary */
    codec_configuration_t *config  = &vdhp->config;
    AVFrame               *picture = vdhp->frame_buffer;
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
