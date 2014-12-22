/*****************************************************************************
 * libavsmash_video.h
 *****************************************************************************
 * Copyright (C) 2012-2014 L-SMASH Works project
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

#define SEEK_MODE_NORMAL     0
#define SEEK_MODE_UNSAFE     1
#define SEEK_MODE_AGGRESSIVE 2

typedef lw_video_scaler_handler_t libavsmash_video_scaler_handler_t;
typedef lw_video_output_handler_t libavsmash_video_output_handler_t;

typedef struct
{
    uint32_t composition_to_decoding;
} order_converter_t;

typedef struct
{
    lsmash_root_t        *root;
    uint32_t              track_ID;
    uint32_t              forward_seek_threshold;
    int                   seek_mode;
    codec_configuration_t config;
    AVFrame              *frame_buffer;
    order_converter_t    *order_converter;
    uint8_t              *keyframe_list;
    uint32_t              sample_count;
    uint32_t              last_sample_number;
    uint32_t              last_rap_number;
    uint32_t              first_valid_frame_number;
    AVFrame              *first_valid_frame;
    uint32_t              media_timescale;
    uint64_t              media_duration;
    double                min_cts;
} libavsmash_video_decode_handler_t;

int libavsmash_setup_timestamp_info
(
    libavsmash_video_decode_handler_t *vdhp,
    int64_t                           *framerate_num,
    int64_t                           *framerate_den
);

static inline uint32_t get_decoding_sample_number
(
    order_converter_t *order_converter,
    uint32_t           composition_sample_number
)
{
    return order_converter
         ? order_converter[composition_sample_number].composition_to_decoding
         : composition_sample_number;
}

int libavsmash_get_video_frame
(
    libavsmash_video_decode_handler_t *vdhp,
    libavsmash_video_output_handler_t *vohp,
    uint32_t                           sample_number
);

int libavsmash_find_first_valid_video_frame
(
    libavsmash_video_decode_handler_t *vdhp,
    uint32_t                           sample_count
);

int libavsmash_create_keyframe_list
(
    libavsmash_video_decode_handler_t *vdhp
);

int libavsmash_is_keyframe
(
    libavsmash_video_decode_handler_t *vdhp,
    libavsmash_video_output_handler_t *vohp,
    uint32_t                           sample_number
);

void libavsmash_cleanup_video_decode_handler
(
    libavsmash_video_decode_handler_t *vdhp
);

static inline void libavsmash_cleanup_video_output_handler
(
    libavsmash_video_output_handler_t *vohp
)
{
    lw_cleanup_video_output_handler( vohp );
};
