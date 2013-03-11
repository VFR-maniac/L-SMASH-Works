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

#include "utils.h"
#include "audio_output.h"
#include "resample.h"

#include "lwlibav_dec.h"
#include "lwlibav_audio.h"

int lwlibav_get_desired_audio_track
(
    const char                     *file_path,
    lwlibav_audio_decode_handler_t *adhp,
    int                             threads
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
    lwlibav_audio_decode_handler_t *adhp,
    int                             output_sample_rate
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
    lwlibav_audio_decode_handler_t *adhp,
    int                             output_sample_rate,
    uint64_t                        start_frame_pos,
    uint64_t                       *start_offset
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

static uint32_t get_audio_rap
(
    lwlibav_audio_decode_handler_t *adhp,
    uint32_t                        frame_number
)
{
    if( frame_number > adhp->frame_count )
        return 0;
    /* Get an unique value of the closest past audio keyframe. */
    uint32_t rap_number = frame_number;
    while( rap_number && !adhp->frame_list[rap_number].keyframe )
        --rap_number;
    if( rap_number == 0 )
        rap_number = 1;
    return rap_number;
}

/* This function seeks the requested frame and get it. */
static void seek_audio
(
    lwlibav_audio_decode_handler_t *adhp,
    uint32_t                        frame_number,
    AVPacket                       *pkt
)
{
    uint32_t rap_number = get_audio_rap( adhp, frame_number );
    if( rap_number == 0 )
        return;
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
        if( lwlibav_get_av_frame( adhp->format, adhp->stream_index, i, pkt ) )
            break;
        if( i == rap_number
         && (((adhp->seek_base & SEEK_FILE_OFFSET_BASED) && (pkt->pos == -1 || adhp->frame_list[i].file_offset > pkt->pos))
         ||  ((adhp->seek_base & SEEK_PTS_BASED)         && (pkt->pts == AV_NOPTS_VALUE || adhp->frame_list[i].pts > pkt->pts))
         ||  ((adhp->seek_base & SEEK_DTS_BASED)         && (pkt->dts == AV_NOPTS_VALUE || adhp->frame_list[i].dts > pkt->dts))) )
            continue;   /* Seeking was too backward. */
        ++i;
    }
}

uint64_t lwlibav_get_pcm_audio_samples
(
    lwlibav_audio_decode_handler_t *adhp,
    lwlibav_audio_output_handler_t *aohp,
    void                           *buf,
    int64_t                         start,
    int64_t                         wanted_length
)
{
    if( adhp->eh.error )
        return 0;
    uint32_t               frame_number;
    uint64_t               output_length = 0;
    enum audio_output_flag output_flags;
    AVPacket              *pkt       = &adhp->packet;
    AVPacket              *alter_pkt = &adhp->alter_packet;
    int                    already_gotten;
    aohp->request_length = wanted_length;
    if( start > 0 && start == adhp->next_pcm_sample_number )
    {
        frame_number   = adhp->last_frame_number;
        output_flags   = AUDIO_OUTPUT_NO_FLAGS;
        output_length += output_pcm_samples_from_buffer( aohp, adhp->frame_buffer, (uint8_t **)&buf, &output_flags );
        if( output_flags & AUDIO_OUTPUT_ENOUGH )
            goto audio_out;
        if( alter_pkt->size <= 0 )
            ++frame_number;
        aohp->output_sample_offset = 0;
        already_gotten             = 0;
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
        av_free_packet( pkt );
        if( adhp->eh.error )
            return 0;
        adhp->next_pcm_sample_number = 0;
        adhp->last_frame_number      = 0;
        /* Get frame_number. */
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
        frame_number = find_start_audio_frame( adhp, aohp->output_sample_rate, start_frame_pos, &aohp->output_sample_offset );
        /* Flush audio decoder buffers. */
        lwlibav_extradata_handler_t *exhp = &adhp->exh;
        int extradata_index = adhp->frame_list[frame_number].extradata_index;
        if( extradata_index != exhp->current_index )
        {
            /* Update the extradata. */
            uint32_t rap_number = get_audio_rap( adhp, frame_number );
            assert( rap_number != 0 );
            lwlibav_update_configuration( (lwlibav_decode_handler_t *)adhp, rap_number, extradata_index, 0 );
        }
        else
            lwlibav_flush_buffers( (lwlibav_decode_handler_t *)adhp );
        /* Seek and get a audio packet. */
        seek_audio( adhp, frame_number, pkt );
        already_gotten = 1;
    }
    do
    {
        if( already_gotten )
        {
            already_gotten = 0;
            *alter_pkt = *pkt;
        }
        else if( frame_number > adhp->frame_count )
        {
            av_free_packet( pkt );
            if( adhp->exh.delay_count )
            {
                /* Null packet */
                av_init_packet( pkt );
                pkt->data = NULL;
                pkt->size = 0;
                *alter_pkt = *pkt;
                -- adhp->exh.delay_count;
            }
            else
                goto audio_out;
        }
        else if( alter_pkt->size <= 0 )
        {
            /* Getting an audio packet must be after flushing all remaining samples in resampler's FIFO buffer. */
            lwlibav_get_av_frame( adhp->format, adhp->stream_index, frame_number, pkt );
            *alter_pkt = *pkt;
        }
        /* Decode and output from an audio packet. */
        output_flags   = AUDIO_OUTPUT_NO_FLAGS;
        output_length += output_pcm_samples_from_packet( aohp, adhp->ctx, alter_pkt, adhp->frame_buffer, (uint8_t **)&buf, &output_flags );
        if( output_flags & AUDIO_DECODER_DELAY )
            ++ adhp->exh.delay_count;
        if( output_flags & AUDIO_RECONFIG_FAILURE )
        {
            adhp->eh.error = 1;
            if( adhp->eh.error_message )
                adhp->eh.error_message( adhp->eh.message_priv,
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

void lwlibav_cleanup_audio_decode_handler( lwlibav_audio_decode_handler_t *adhp )
{
    lwlibav_extradata_handler_t *exhp = &adhp->exh;
    if( exhp->entries )
    {
        for( int i = 0; i < exhp->entry_count; i++ )
            if( exhp->entries[i].extradata )
                av_free( exhp->entries[i].extradata );
        free( exhp->entries );
    }
    av_free_packet( &adhp->packet );
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

void lwlibav_cleanup_audio_output_handler( lwlibav_audio_output_handler_t *aohp )
{
    if( aohp->resampled_buffer )
        av_freep( &aohp->resampled_buffer );
    if( aohp->avr_ctx )
        avresample_free( &aohp->avr_ctx );
}

void set_audio_basic_settings
(
    lwlibav_decode_handler_t *dhp,
    uint32_t                  frame_number
)
{
    lwlibav_audio_decode_handler_t *adhp = (lwlibav_audio_decode_handler_t *)dhp;
    AVCodecContext *ctx = adhp->format->streams[ adhp->stream_index ]->codec;
    lwlibav_extradata_t *entry = &adhp->exh.entries[ adhp->frame_list[frame_number].extradata_index ];
    ctx->sample_rate           = entry->sample_rate;
    ctx->channel_layout        = entry->channel_layout;
    ctx->sample_fmt            = entry->sample_format;
    ctx->bits_per_coded_sample = entry->bits_per_sample;
    ctx->block_align           = entry->block_align;
    ctx->channels              = av_get_channel_layout_nb_channels( ctx->channel_layout );
}

int try_decode_audio_frame
(
    lwlibav_decode_handler_t *dhp,
    uint32_t                  frame_number,
    char                     *error_string
)
{
    AVFrame *picture = avcodec_alloc_frame();
    if( !picture )
    {
        strcpy( error_string, "Failed to alloc AVFrame to set up a decoder configuration.\n" );
        return -1;
    }
    lwlibav_audio_decode_handler_t *adhp = (lwlibav_audio_decode_handler_t *)dhp;
    AVFormatContext *format_ctx   = adhp->format;
    int              stream_index = adhp->stream_index;
    AVCodecContext  *ctx          = format_ctx->streams[stream_index]->codec;
    uint32_t         start_frame  = frame_number;
    do
    {
        if( frame_number > adhp->frame_count )
            break;
        /* Get a frame. */
        AVPacket pkt = { 0 };
        int extradata_index = adhp->frame_list[frame_number].extradata_index;
        if( extradata_index != adhp->exh.current_index )
            break;
        if( frame_number == start_frame )
            seek_audio( adhp, frame_number, &pkt );
        else
        {
            int ret = lwlibav_get_av_frame( format_ctx, stream_index, frame_number, &pkt );
            if( ret > 0 )
                break;
            else if( ret < 0 )
            {
                if( ctx->sample_rate == 0 )
                    strcpy( error_string, "Failed to set up sample rate.\n" );
                else if( ctx->channel_layout == 0 && ctx->channels == 0 )
                    strcpy( error_string, "Failed to set up channels.\n" );
                else
                    strcpy( error_string, "Failed to set up sample format.\n" );
                avcodec_free_frame( &picture );
                return -1;
            }
        }
        /* Try decode a frame. */
        avcodec_get_frame_defaults( picture );
        do
        {
            int dummy;
            int consumed_bytes = avcodec_decode_audio4( ctx, picture, &dummy, &pkt );
            if( pkt.data )
            {
                pkt.size -= consumed_bytes;
                pkt.data += consumed_bytes;
            }
        } while( pkt.size > 0 );
        ++frame_number;
    } while( ctx->sample_rate == 0 || (ctx->channel_layout == 0 && ctx->channels == 0) || ctx->sample_fmt == AV_SAMPLE_FMT_NONE );
    avcodec_free_frame( &picture );
    return 0;
}
