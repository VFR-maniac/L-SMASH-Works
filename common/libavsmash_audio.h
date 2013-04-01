/*****************************************************************************
 * libavsmash_audio.h
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

typedef lw_audio_output_handler_t libavsmash_audio_output_handler_t;

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
} libavsmash_audio_decode_handler_t;

uint64_t libavsmash_count_overall_pcm_samples
(
    libavsmash_audio_decode_handler_t *adhp,
    int                                output_sample_rate,
    uint64_t                          *skip_decoded_samples
);

uint64_t libavsmash_get_pcm_audio_samples
(
    libavsmash_audio_decode_handler_t *adhp,
    libavsmash_audio_output_handler_t *aohp,
    void                              *buf,
    int64_t                            start,
    int64_t                            wanted_length
);

void libavsmash_cleanup_audio_decode_handler
(
    libavsmash_audio_decode_handler_t *adhp
);
