/*****************************************************************************
 * lwlibav_audio.c / lwlibav_audio.cpp
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
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#ifdef __cplusplus
}
#endif  /* __cplusplus */

#include "lwlibav_dec.h"
#include "lwlibav_audio.h"

#include "utils.h"
#include "resample.h"

int lwlibav_get_desired_audio_track
(
    const char             *file_path,
    audio_decode_handler_t *adhp,
    int                     threads
)
{
    int error = adhp->stream_index < 0
             || adhp->frame_count == 0
             || lavf_open_file( &adhp->format, file_path, &adhp->eh );
    AVCodecContext *ctx = !error ? adhp->format->streams[ adhp->stream_index ]->codec : NULL;
    if( error || open_decoder( ctx, adhp->codec_id, threads ) )
    {
        if( adhp->index_entries )
            av_freep( &adhp->index_entries );
        if( adhp->frame_list )
            lw_freep( &adhp->frame_list );
        if( adhp->format )
        {
            lavf_close_file( &adhp->format );
            adhp->format = NULL;
        }
        return -1;
    }
    adhp->ctx = ctx;
    return 0;
}

uint64_t lwlibav_count_overall_pcm_samples
(
    audio_decode_handler_t *adhp,
    int                     output_sample_rate
)
{
    audio_frame_info_t *frame_list    = adhp->frame_list;
    int      current_sample_rate      = frame_list[1].sample_rate > 0 ? frame_list[1].sample_rate : adhp->ctx->sample_rate;
    uint32_t current_frame_length     = frame_list[1].length;
    uint32_t audio_frame_count        = 0;
    uint64_t pcm_sample_count         = 0;
    uint64_t overall_pcm_sample_count = 0;
    for( uint32_t i = 1; i <= adhp->frame_count; i++ )
    {
        if( (current_sample_rate != frame_list[i].sample_rate && frame_list[i].sample_rate > 0)
         || current_frame_length != frame_list[i].length )
        {
            uint64_t resampled_sample_count = output_sample_rate == current_sample_rate || pcm_sample_count == 0
                                            ? pcm_sample_count
                                            : (pcm_sample_count * output_sample_rate - 1) / current_sample_rate + 1;
            overall_pcm_sample_count += resampled_sample_count;
            audio_frame_count = 0;
            pcm_sample_count  = 0;
            current_sample_rate  = frame_list[i].sample_rate > 0 ? frame_list[i].sample_rate : adhp->ctx->sample_rate;
            current_frame_length = frame_list[i].length;
        }
        pcm_sample_count += frame_list[i].length;
        ++audio_frame_count;
    }
    current_sample_rate = frame_list[ adhp->frame_count ].sample_rate > 0
                        ? frame_list[ adhp->frame_count ].sample_rate
                        : adhp->ctx->sample_rate;
    if( pcm_sample_count )
        overall_pcm_sample_count += (pcm_sample_count * output_sample_rate - 1) / current_sample_rate + 1;
    return overall_pcm_sample_count;
}

static int find_start_audio_frame
(
    audio_decode_handler_t *adhp,
    int                     output_sample_rate,
    uint64_t                start_frame_pos,
    uint64_t               *start_offset
)
{
    audio_frame_info_t *frame_list = adhp->frame_list;
    uint32_t frame_number                    = 1;
    uint64_t current_frame_pos               = 0;
    uint64_t next_frame_pos                  = 0;
    int      current_sample_rate             = frame_list[frame_number].sample_rate > 0 ? frame_list[frame_number].sample_rate : adhp->ctx->sample_rate;
    uint32_t current_frame_length            = frame_list[frame_number].length;
    uint64_t resampled_sample_count          = 0;   /* the number of accumulated PCM samples after resampling per sequence */
    uint64_t pcm_sample_count                = 0;   /* the number of accumulated PCM samples before resampling per sequence */
    uint64_t prior_sequences_resampled_count = 0;   /* the number of accumulated PCM samples of all prior sequences */
    do
    {
        current_frame_pos = next_frame_pos;
        if( (current_sample_rate != frame_list[frame_number].sample_rate && frame_list[frame_number].sample_rate > 0)
         || current_frame_length != frame_list[frame_number].length )
        {
            /* Encountered a new sequence. */
            prior_sequences_resampled_count += resampled_sample_count;
            pcm_sample_count = 0;
            current_sample_rate  = frame_list[frame_number].sample_rate > 0 ? frame_list[frame_number].sample_rate : adhp->ctx->sample_rate;
            current_frame_length = frame_list[frame_number].length;
        }
        pcm_sample_count += current_frame_length;
        resampled_sample_count = output_sample_rate == current_sample_rate || pcm_sample_count == 0
                               ? pcm_sample_count
                               : (pcm_sample_count * output_sample_rate - 1) / current_sample_rate + 1;
        next_frame_pos = prior_sequences_resampled_count + resampled_sample_count;
        if( start_frame_pos < next_frame_pos )
            break;
        ++frame_number;
    } while( frame_number <= adhp->frame_count );
    *start_offset = start_frame_pos - current_frame_pos;
    if( *start_offset && current_sample_rate != output_sample_rate )
        *start_offset = (*start_offset * current_sample_rate - 1) / output_sample_rate + 1;
    return frame_number;
}

static void seek_audio
(
    audio_decode_handler_t *adhp,
    uint32_t                frame_number,
    AVPacket               *pkt
)
{
    /* Get an unique value of the closest past audio keyframe. */
    uint32_t rap_number = frame_number;
    while( rap_number && !adhp->frame_list[rap_number].keyframe )
        --rap_number;
    if( rap_number == 0 )
        rap_number = 1;
    int64_t rap_pos = (adhp->seek_base & SEEK_FILE_OFFSET_BASED) ? adhp->frame_list[rap_number].file_offset
                    : (adhp->seek_base & SEEK_PTS_BASED)         ? adhp->frame_list[rap_number].pts
                    : (adhp->seek_base & SEEK_DTS_BASED)         ? adhp->frame_list[rap_number].dts
                    :                                              adhp->frame_list[rap_number].sample_number;
    /* Seek to audio keyframe.
     * Note: av_seek_frame() for DV in AVI Type-1 requires stream_index = 0. */
    int flags = (adhp->seek_base & SEEK_FILE_OFFSET_BASED) ? AVSEEK_FLAG_BYTE : adhp->seek_base == 0 ? AVSEEK_FLAG_FRAME : 0;
    int stream_index = adhp->dv_in_avi == 1 ? 0 : adhp->stream_index;
    if( av_seek_frame( adhp->format, stream_index, rap_pos, flags | AVSEEK_FLAG_BACKWARD ) < 0 )
        av_seek_frame( adhp->format, stream_index, rap_pos, flags | AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY );
    /* Seek to the target audio frame and get it. */
    for( uint32_t i = rap_number; i <= frame_number; )
    {
        if( get_av_frame( adhp->format, adhp->stream_index, &adhp->input_buffer, &adhp->input_buffer_size, pkt ) )
            break;
        if( i == rap_number
         && (((adhp->seek_base & SEEK_FILE_OFFSET_BASED) && (pkt->pos == -1 || adhp->frame_list[i].file_offset > pkt->pos))
         ||  ((adhp->seek_base & SEEK_PTS_BASED)         && (pkt->pts == AV_NOPTS_VALUE || adhp->frame_list[i].pts > pkt->pts))
         ||  ((adhp->seek_base & SEEK_DTS_BASED)         && (pkt->dts == AV_NOPTS_VALUE || adhp->frame_list[i].dts > pkt->dts))) )
            continue;   /* Seeking was too backward. */
        ++i;
    }
}

static int consume_decoded_audio_samples
(
    audio_output_handler_t *aohp,
    AVFrame                *frame,
    int                     input_sample_count,
    int                     wanted_sample_count,
    uint8_t               **out_data,
    int                     sample_offset
)
{
    /* Input */
    uint8_t *in_data[AVRESAMPLE_MAX_CHANNELS];
    int decoded_data_offset = sample_offset * aohp->input_block_align;
    for( int i = 0; i < aohp->input_planes; i++ )
        in_data[i] = frame->extended_data[i] + decoded_data_offset;
    audio_samples_t in;
    in.channel_layout = frame->channel_layout;
    in.sample_count   = input_sample_count;
    in.sample_format  = (enum AVSampleFormat)frame->format;
    in.data           = in_data;
    /* Output */
    uint8_t *resampled_buffer = NULL;
    if( aohp->s24_output )
    {
        int out_channels = get_channel_layout_nb_channels( aohp->output_channel_layout );
        int out_linesize = get_linesize( out_channels, wanted_sample_count, aohp->output_sample_format );
        if( !aohp->resampled_buffer || out_linesize > aohp->resampled_buffer_size )
        {
            uint8_t *temp = (uint8_t *)av_realloc( aohp->resampled_buffer, out_linesize );
            if( !temp )
                return 0;
            aohp->resampled_buffer_size = out_linesize;
            aohp->resampled_buffer      = temp;
        }
        resampled_buffer = aohp->resampled_buffer;
    }
    audio_samples_t out;
    out.channel_layout = aohp->output_channel_layout;
    out.sample_count   = wanted_sample_count;
    out.sample_format  = aohp->output_sample_format;
    out.data           = resampled_buffer ? &resampled_buffer : out_data;
    /* Resample */
    int resampled_size = resample_audio( aohp->avr_ctx, &out, &in );
    if( resampled_buffer && resampled_size > 0 )
        resampled_size = resample_s32_to_s24( out_data, aohp->resampled_buffer, resampled_size );
    return resampled_size > 0 ? resampled_size / aohp->output_block_align : 0;
}

uint64_t lwlibav_get_pcm_audio_samples
(
    audio_decode_handler_t *adhp,
    audio_output_handler_t *aohp,
    void                   *buf,
    int64_t                 start,
    int64_t                 wanted_length
)
{
    if( adhp->eh.error )
        return 0;
    uint32_t frame_number;
    uint64_t seek_offset;
    uint64_t output_length = 0;
    int      already_gotten;
    AVPacket *pkt = &adhp->packet;
    if( start > 0 && start == adhp->next_pcm_sample_number )
    {
        frame_number = adhp->last_frame_number;
        if( adhp->frame_buffer->extended_data
         && adhp->frame_buffer->extended_data[0] )
        {
            /* Flush remaing audio samples. */
            int resampled_length = consume_decoded_audio_samples( aohp, adhp->frame_buffer, 0, (int)wanted_length, (uint8_t **)&buf, 0 );
            output_length += resampled_length;
            wanted_length -= resampled_length;
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
        if( flush_resampler_buffers( aohp->avr_ctx ) < 0 )
        {
            adhp->eh.error = 1;
            if( adhp->eh.error_message )
                adhp->eh.error_message( adhp->eh.message_priv,
                                        "Failed to flush resampler buffers.\n"
                                        "It is recommended you reopen the file." );
            return 0;
        }
        flush_buffers( adhp->ctx, &adhp->eh );
        if( adhp->eh.error )
            return 0;
        adhp->delay_count            = 0;
        adhp->next_pcm_sample_number = 0;
        adhp->last_frame_number      = 0;
        uint64_t start_frame_pos;
        if( start >= 0 )
            start_frame_pos = start;
        else
        {
            uint64_t silence_length = -start;
            put_silence_audio_samples( (int)(silence_length * aohp->output_block_align), aohp->output_bits_per_sample == 8, (uint8_t **)&buf );
            output_length += silence_length;
            wanted_length -= silence_length;
            start_frame_pos = 0;
        }
        frame_number = find_start_audio_frame( adhp, aohp->output_sample_rate, start_frame_pos, &seek_offset );
        seek_audio( adhp, frame_number, pkt );
        already_gotten = 1;
    }
    do
    {
        if( already_gotten )
            already_gotten = 0;
        else if( frame_number > adhp->frame_count )
        {
            if( adhp->delay_count )
            {
                /* Null packet */
                av_init_packet( pkt );
                pkt->data = NULL;
                pkt->size = 0;
                -- adhp->delay_count;
            }
            else
                goto audio_out;
        }
        else if( pkt->size <= 0 )
            /* Getting a sample must be after flushing all remaining samples in resampler's FIFO buffer. */
            get_av_frame( adhp->format, adhp->stream_index, &adhp->input_buffer, &adhp->input_buffer_size, pkt );
        int output_audio = 0;
        do
        {
            avcodec_get_frame_defaults( adhp->frame_buffer );
            int decode_complete;
            int wasted_data_length = avcodec_decode_audio4( adhp->ctx, adhp->frame_buffer, &decode_complete, pkt );
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
            if( decode_complete
             && adhp->frame_buffer->extended_data
             && adhp->frame_buffer->extended_data[0] )
            {
                /* Check channel layout, sample rate and sample format of decoded audio samples. */
                int64_t channel_layout;
                int64_t sample_rate;
                int64_t sample_format;
                av_opt_get_int( aohp->avr_ctx, "in_channel_layout", 0, &channel_layout );
                av_opt_get_int( aohp->avr_ctx, "in_sample_rate",    0, &sample_rate );
                av_opt_get_int( aohp->avr_ctx, "in_sample_fmt",     0, &sample_format );
                if( adhp->frame_buffer->channel_layout == 0 )
                    adhp->frame_buffer->channel_layout = av_get_default_channel_layout( adhp->ctx->channels );
                if( adhp->frame_buffer->channel_layout != channel_layout
                 || adhp->frame_buffer->sample_rate    != sample_rate
                 || adhp->frame_buffer->format         != sample_format )
                {
                    /* Detected a change of channel layout, sample rate or sample format.
                     * Reconfigure audio resampler. */
                    if( update_resampler_configuration( aohp->avr_ctx,
                                                        aohp->output_channel_layout,
                                                        aohp->output_sample_rate,
                                                        aohp->output_sample_format,
                                                        adhp->frame_buffer->channel_layout,
                                                        adhp->frame_buffer->sample_rate,
                                                        (enum AVSampleFormat)adhp->frame_buffer->format,
                                                        &aohp->input_planes,
                                                        &aohp->input_block_align ) < 0 )
                    {
                        adhp->eh.error = 1;
                        if( adhp->eh.error_message )
                            adhp->eh.error_message( adhp->eh.message_priv,
                                                    "Failed to reconfigure resampler.\n"
                                                    "It is recommended you reopen the file." );
                        goto audio_out;
                    }
                }
                /* Process decoded audio samples. */
                int decoded_length = adhp->frame_buffer->nb_samples;
                if( decoded_length > seek_offset )
                {
                    /* Send decoded audio data to resampler and get desired resampled audio as you want as much as possible. */
                    int useful_length = (int)(decoded_length - seek_offset);
                    int resampled_length = consume_decoded_audio_samples( aohp, adhp->frame_buffer, useful_length, (int)wanted_length, (uint8_t **)&buf, (int)seek_offset );
                    output_length += resampled_length;
                    wanted_length -= resampled_length;
                    seek_offset = 0;
                    if( wanted_length <= 0 )
                        goto audio_out;
                }
                else
                    seek_offset -= decoded_length;
                output_audio = 1;
            }
        } while( pkt->size > 0 );
        if( !output_audio && pkt->data )    /* Count audio frame delay only if feeding non-NULL packet. */
            ++ adhp->delay_count;
        ++frame_number;
    } while( 1 );
audio_out:
    adhp->next_pcm_sample_number = start + output_length;
    adhp->last_frame_number = frame_number;
    return output_length;
}

void lwlibav_cleanup_audio_decode_handler( audio_decode_handler_t *adhp )
{
    if( adhp->input_buffer )
        av_freep( &adhp->input_buffer );
    if( adhp->index_entries )
        av_freep( &adhp->index_entries );
    if( adhp->frame_buffer )
        avcodec_free_frame( &adhp->frame_buffer );
    if( adhp->ctx )
    {
        avcodec_close( adhp->ctx );
        adhp->ctx = NULL;
    }
    if( adhp->format )
        lavf_close_file( &adhp->format );
}

void lwlibav_cleanup_audio_output_handler( audio_output_handler_t *aohp )
{
    if( aohp->resampled_buffer )
        av_freep( &aohp->resampled_buffer );
    if( aohp->avr_ctx )
        avresample_free( &aohp->avr_ctx );
}
