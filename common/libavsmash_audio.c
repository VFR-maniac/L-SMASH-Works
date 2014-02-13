/*****************************************************************************
 * libavsmash_audio.c / libavsmash_audio.cpp
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
#include <libavresample/avresample.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#ifdef __cplusplus
}
#endif  /* __cplusplus */

#include "utils.h"
#include "audio_output.h"
#include "resample.h"
#include "libavsmash.h"
#include "libavsmash_audio.h"

#define RESAMPLE_PCM_COUNT( SAMPLES ) (((SAMPLES) * output_sample_rate - 1) / current_sample_rate + 1)

static uint64_t count_sequence_output_pcm_samples
(
    uint64_t upsampled_sequence_pcm_count,
    uint64_t skip_decoded_samples,
    int      current_sample_rate,
    int      output_sample_rate
)
{
    uint64_t resampled_sample_count;
    uint64_t skip_output_samples;
    if( output_sample_rate == current_sample_rate )
    {
        resampled_sample_count = upsampled_sequence_pcm_count;
        skip_output_samples    = skip_decoded_samples;
    }
    else
    {
        resampled_sample_count = upsampled_sequence_pcm_count ? RESAMPLE_PCM_COUNT( upsampled_sequence_pcm_count ) : 0;
        skip_output_samples    = skip_decoded_samples         ? RESAMPLE_PCM_COUNT( skip_decoded_samples )         : 0;
    }
    return resampled_sample_count > skip_output_samples ? resampled_sample_count - skip_output_samples : 0;
}

uint64_t libavsmash_count_overall_pcm_samples
(
    libavsmash_audio_decode_handler_t *adhp,
    int                                output_sample_rate,
    uint64_t                          *skip_decoded_samples     /* converted to upsampled in this function */
)
{
    codec_configuration_t *config = &adhp->config;
    extended_summary_t    *es     = NULL;
    /* Here, the decoder upsampling is defined only when libavcodec doesn't return upsampled length as AVCodecContext.frame_size. */
    int      current_sample_rate       = 0;     /* after the decoder upsampling */
    uint32_t current_index             = 0;
    uint32_t current_frame_length      = 0;     /* before the decoder upsampling */
    uint64_t sequence_pcm_count        = 0;     /* before the decoder upsampling */
    uint64_t prior_sequences_pcm_count = 0;     /* before the decoder upsampling */
    uint64_t overall_pcm_count         = 0;     /* after the decoder and the resampler upsampling */
    uint64_t orig_skip_decoded_samples = *skip_decoded_samples; /* before the decoder upsampling */
    /* Count the number of output PCM audio samples in each sequence. */
    *skip_decoded_samples = 0;
    for( uint32_t i = 1; i <= adhp->frame_count; i++ )
    {
        /* Get configuration index. */
        lsmash_sample_t sample;
        if( lsmash_get_sample_info_from_media_timeline( adhp->root, adhp->track_ID, i, &sample ) )
            continue;
        if( current_index != sample.index )
        {
            es = &config->entries[ sample.index - 1 ].extended;
            current_index = sample.index;
        }
        else if( !es )
            continue;
        /* Get audio frame length. */
        uint32_t frame_length;
        if( es->frame_length )
            frame_length = es->frame_length;
        else if( lsmash_get_sample_delta_from_media_timeline( adhp->root, adhp->track_ID, i, &frame_length ) )
            continue;
        /* */
        if( (current_sample_rate != es->sample_rate && es->sample_rate > 0)
         || current_frame_length != frame_length )
        {
            /* Encountered a new sequence. */
            if( current_sample_rate > 0 )
            {
                /* Add the number of decoded PCM audio samples which shall be skipped. */
                if( orig_skip_decoded_samples > prior_sequences_pcm_count )
                {
                    if( orig_skip_decoded_samples >= prior_sequences_pcm_count + sequence_pcm_count )
                        /* All decoded PCM audio samples in the previous sequence shall be skipped. */
                        *skip_decoded_samples += sequence_pcm_count * es->upsampling;
                    else
                        /* 0 < orig_skip_decoded_samples - prior_sequences_pcm_count < sequence_pcm_count
                         * Partial decoded PCM audio samples in the previous sequence are not skipped. */
                        *skip_decoded_samples += (orig_skip_decoded_samples - prior_sequences_pcm_count) * es->upsampling;
                }
                prior_sequences_pcm_count += sequence_pcm_count;
                /* Add the number of output PCM audio samples in the previous sequence. */
                overall_pcm_count += count_sequence_output_pcm_samples( sequence_pcm_count * es->upsampling,
                                                                        *skip_decoded_samples,
                                                                        current_sample_rate,
                                                                        output_sample_rate );
                sequence_pcm_count = 0;
            }
            current_sample_rate  = es->sample_rate > 0 ? es->sample_rate : config->ctx->sample_rate;
            current_frame_length = frame_length;
        }
        sequence_pcm_count += frame_length;
    }
    if( !es || (sequence_pcm_count == 0 && overall_pcm_count == 0) )
        return 0;
    /* Count the number of output PCM audio samples in the last sequence. */
    if( orig_skip_decoded_samples > prior_sequences_pcm_count )
        *skip_decoded_samples += (orig_skip_decoded_samples - prior_sequences_pcm_count) * es->upsampling;
    current_sample_rate = es->sample_rate > 0 ? es->sample_rate : config->ctx->sample_rate;
    overall_pcm_count += count_sequence_output_pcm_samples( sequence_pcm_count * es->upsampling,
                                                            *skip_decoded_samples,
                                                            current_sample_rate,
                                                            output_sample_rate );
    /* Return the number of output PCM audio samples. */
    return overall_pcm_count;
}

static inline int get_frame_length
(
    libavsmash_audio_decode_handler_t *adhp,
    uint32_t                           frame_number,
    uint32_t                          *frame_length,
    libavsmash_summary_t             **sp
)
{
    lsmash_sample_t sample;
    if( lsmash_get_sample_info_from_media_timeline( adhp->root, adhp->track_ID, frame_number, &sample ) )
        return -1;
    *sp = &adhp->config.entries[ sample.index - 1 ];
    libavsmash_summary_t *s = *sp;
    if( s->extended.frame_length == 0 )
    {
        /* variable frame length
         * Guess the frame length from sample duration. */
        if( lsmash_get_sample_delta_from_media_timeline( adhp->root, adhp->track_ID, frame_number, frame_length ) )
            return -1;
        *frame_length *= s->extended.upsampling;
    }
    else
        /* constant frame length */
        *frame_length = s->extended.frame_length;
    return 0;
}

static uint32_t get_preroll_samples
(
    libavsmash_audio_decode_handler_t *adhp,
    uint64_t                           skip_decoded_samples,
    uint32_t                          *frame_number
)
{
    /* Some audio CODEC requires pre-roll for correct composition. */
    lsmash_sample_property_t prop;
    if( lsmash_get_sample_property_from_media_timeline( adhp->root, adhp->track_ID, *frame_number, &prop ) )
        return 0;
    if( prop.pre_roll.distance == 0 )
    {
        if( skip_decoded_samples == 0 || !adhp->implicit_preroll )
            return 0;
        /* Estimate pre-roll distance. */
        for( uint32_t i = 1; i <= adhp->frame_count || skip_decoded_samples; i++ )
        {
            libavsmash_summary_t *dummy = NULL;
            uint32_t frame_length;
            if( get_frame_length( adhp, i, &frame_length, &dummy ) )
                break;
            if( skip_decoded_samples < frame_length )
                skip_decoded_samples = 0;
            else
                skip_decoded_samples -= frame_length;
            ++ prop.pre_roll.distance;
        }
    }
    uint32_t preroll_samples = 0;
    for( uint32_t i = 0; i < prop.pre_roll.distance; i++ )
    {
        if( *frame_number > 1 )
            --(*frame_number);
        else
            break;
        libavsmash_summary_t *dummy = NULL;
        uint32_t frame_length;
        if( get_frame_length( adhp, *frame_number, &frame_length, &dummy ) )
            break;
        preroll_samples += frame_length;
    }
    return preroll_samples;
}

static int find_start_audio_frame
(
    libavsmash_audio_decode_handler_t *adhp,
    int                                output_sample_rate,
    uint64_t                           skip_decoded_samples,
    uint64_t                           start_frame_pos,
    uint64_t                          *start_offset
)
{
    uint32_t frame_number                    = 1;
    uint64_t current_frame_pos               = 0;
    uint64_t next_frame_pos                  = 0;
    int      current_sample_rate             = 0;
    uint32_t current_frame_length            = 0;
    uint64_t pcm_sample_count                = 0;   /* the number of accumulated PCM samples before resampling per sequence */
    uint64_t resampled_sample_count          = 0;   /* the number of accumulated PCM samples after resampling per sequence */
    uint64_t prior_sequences_resampled_count = 0;   /* the number of accumulated PCM samples of all prior sequences */
    do
    {
        current_frame_pos = next_frame_pos;
        libavsmash_summary_t *s = NULL;
        uint32_t frame_length;
        if( get_frame_length( adhp, frame_number, &frame_length, &s ) )
        {
            ++frame_number;
            continue;
        }
        if( (current_sample_rate != s->extended.sample_rate && s->extended.sample_rate > 0)
         || current_frame_length != frame_length )
        {
            /* Encountered a new sequence. */
            prior_sequences_resampled_count += resampled_sample_count;
            pcm_sample_count = 0;
            current_sample_rate  = s->extended.sample_rate > 0 ? s->extended.sample_rate : adhp->config.ctx->sample_rate;
            current_frame_length = frame_length;
        }
        pcm_sample_count += frame_length;
        resampled_sample_count = output_sample_rate == current_sample_rate || pcm_sample_count == 0
                               ? pcm_sample_count
                               : RESAMPLE_PCM_COUNT( pcm_sample_count );
        next_frame_pos = prior_sequences_resampled_count + resampled_sample_count;
        if( start_frame_pos < next_frame_pos )
            break;
        ++frame_number;
    } while( frame_number <= adhp->frame_count );
    *start_offset = start_frame_pos - current_frame_pos;
    if( *start_offset && current_sample_rate != output_sample_rate )
        /* start_offset is applied at the decoder sampling rate. */
        *start_offset = (*start_offset * current_sample_rate - 1) / output_sample_rate + 1;
    *start_offset += get_preroll_samples( adhp, skip_decoded_samples, &frame_number );
    return frame_number;
}

uint64_t libavsmash_get_pcm_audio_samples
(
    libavsmash_audio_decode_handler_t *adhp,
    libavsmash_audio_output_handler_t *aohp,
    void                              *buf,
    int64_t                            start,
    int64_t                            wanted_length
)
{
    codec_configuration_t *config = &adhp->config;
    if( config->error )
        return 0;
    uint32_t               frame_number;
    uint64_t               output_length = 0;
    enum audio_output_flag output_flags;
    aohp->request_length = wanted_length;
    if( start > 0 && start == adhp->next_pcm_sample_number )
    {
        frame_number   = adhp->last_frame_number;
        output_flags   = AUDIO_OUTPUT_NO_FLAGS;
        output_length += output_pcm_samples_from_buffer( aohp, adhp->frame_buffer, (uint8_t **)&buf, &output_flags );
        if( output_flags & AUDIO_OUTPUT_ENOUGH )
            goto audio_out;
        if( adhp->packet.size <= 0 )
            ++frame_number;
        aohp->output_sample_offset = 0;
    }
    else
    {
        /* Seek audio stream. */
        if( flush_resampler_buffers( aohp->avr_ctx ) < 0 )
        {
            config->error = 1;
            if( config->lh.show_log )
                config->lh.show_log( &config->lh, LW_LOG_FATAL,
                                     "Failed to flush resampler buffers.\n"
                                     "It is recommended you reopen the file." );
            return 0;
        }
        libavsmash_flush_buffers( config );
        if( config->error )
            return 0;
        adhp->next_pcm_sample_number = 0;
        adhp->last_frame_number      = 0;
        uint64_t start_frame_pos;
        if( start >= 0 )
            start_frame_pos = start;
        else
        {
            uint64_t silence_length = -start;
            put_silence_audio_samples( (int)(silence_length * aohp->output_block_align), aohp->output_bits_per_sample == 8, (uint8_t **)&buf );
            output_length        += silence_length;
            aohp->request_length -= silence_length;
            start_frame_pos = 0;
        }
        start_frame_pos += aohp->skip_decoded_samples;
        frame_number = find_start_audio_frame( adhp, aohp->output_sample_rate, aohp->skip_decoded_samples, start_frame_pos, &aohp->output_sample_offset );
    }
    do
    {
        AVPacket *pkt = &adhp->packet;
        if( frame_number > adhp->frame_count )
        {
            if( config->delay_count )
            {
                /* Null packet */
                av_init_packet( pkt );
                pkt->data = NULL;
                pkt->size = 0;
                -- config->delay_count;
            }
            else
                goto audio_out;
        }
        else if( pkt->size <= 0 )
            /* Getting an audio packet must be after flushing all remaining samples in resampler's FIFO buffer. */
            while( get_sample( adhp->root, adhp->track_ID, frame_number, config, pkt ) == 2 )
                if( config->update_pending )
                    /* Update the decoder configuration. */
                    update_configuration( adhp->root, adhp->track_ID, config );
        /* Decode and output from an audio packet. */
        output_flags   = AUDIO_OUTPUT_NO_FLAGS;
        output_length += output_pcm_samples_from_packet( aohp, config->ctx, pkt, adhp->frame_buffer, (uint8_t **)&buf, &output_flags );
        if( output_flags & AUDIO_DECODER_DELAY )
            ++ config->delay_count;
        if( output_flags & AUDIO_RECONFIG_FAILURE )
        {
            config->error = 1;
            if( config->lh.show_log )
                config->lh.show_log( &config->lh, LW_LOG_FATAL,
                                     "Failed to reconfigure resampler.\n"
                                     "It is recommended you reopen the file." );
            goto audio_out;
        }
        if( output_flags & AUDIO_OUTPUT_ENOUGH )
            goto audio_out;
        ++frame_number;
    } while( 1 );
audio_out:
    adhp->next_pcm_sample_number = start + output_length;
    adhp->last_frame_number      = frame_number;
    return output_length;
}

void libavsmash_cleanup_audio_decode_handler
(
    libavsmash_audio_decode_handler_t *adhp
)
{
    if( adhp->frame_buffer )
        av_frame_free( &adhp->frame_buffer );
    cleanup_configuration( &adhp->config );
}
