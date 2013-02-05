/*****************************************************************************
 * libav_audio.h
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

typedef struct
{
    int      length;
    int      keyframe;
    int      sample_rate;
    uint32_t sample_number;
    int64_t  pts;
    int64_t  dts;
    int64_t  file_offset;
} audio_frame_info_t;

typedef struct
{
    AVFormatContext    *format;
    int                 stream_index;
    /* */
    error_handler_t     eh;
    AVCodecContext     *ctx;
    AVIndexEntry       *index_entries;
    int                 index_entries_count;
    int                 dv_in_avi;      /* 1 = 'DV in AVI Type-1', 0 = otherwise */
    int                 seek_base;
    enum AVCodecID      codec_id;
    uint32_t            frame_count;
    uint32_t            delay_count;
    uint32_t            input_buffer_size;
    uint8_t            *input_buffer;
    AVFrame            *frame_buffer;
    AVPacket            packet;
    audio_frame_info_t *frame_list;
    uint32_t            frame_length;
    uint32_t            last_frame_number;
    uint64_t            next_pcm_sample_number;
} audio_decode_handler_t;

typedef struct
{
    AVAudioResampleContext *avr_ctx;
    uint8_t                *resampled_buffer;
    int                     resampled_buffer_size;
    int                     input_planes;
    int                     input_block_align;
    uint64_t                output_channel_layout;
    enum AVSampleFormat     output_sample_format;
    int                     output_block_align;
    int                     output_sample_rate;
    int                     output_bits_per_sample;
    int                     s24_output;
} audio_output_handler_t;

int get_desired_audio_track
(
    const char             *file_path,
    audio_decode_handler_t *adhp,
    int                     threads
);

uint64_t count_overall_pcm_samples
(
    audio_decode_handler_t *adhp,
    int                     output_sample_rate
);

uint64_t get_pcm_audio_samples
(
    audio_decode_handler_t *adhp,
    audio_output_handler_t *aohp,
    void                   *buf,
    int64_t                 start,
    int64_t                 wanted_length
);
