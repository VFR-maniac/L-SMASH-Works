/*****************************************************************************
 * libavsmash_audio.h
 *****************************************************************************
 * Copyright (C) 2013 L-SMASH Works project
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

typedef struct
{
    lsmash_root_t        *root;
    uint32_t              track_ID;
    codec_configuration_t config;
    AVFrame              *frame_buffer;
    AVPacket              packet;
    uint64_t              next_pcm_sample_number;
    uint32_t              last_frame_number;
    uint32_t              frame_count;
    int                   implicit_preroll;
} audio_decode_handler_t;

typedef struct
{
    AVAudioResampleContext *avr_ctx;
    uint8_t                *resampled_buffer;
    int                     resampled_buffer_size;
    int                     input_planes;
    int                     input_block_align;
    uint64_t                skip_decoded_samples;   /* Upsampling is considered. */
    uint64_t                output_channel_layout;
    enum AVSampleFormat     output_sample_format;
    int                     output_block_align;
    int                     output_sample_rate;
    int                     output_bits_per_sample;
    int                     s24_output;
} audio_output_handler_t;

uint64_t count_overall_pcm_samples
(
    audio_decode_handler_t *adhp,
    int                     output_sample_rate,
    uint64_t               *skip_decoded_samples
);

uint64_t get_pcm_audio_samples
(
    audio_decode_handler_t *adhp,
    audio_output_handler_t *aohp,
    void                   *buf,
    int64_t                 start,
    int64_t                 wanted_length
);
