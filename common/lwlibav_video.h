/*****************************************************************************
 * lwlibav_video.h
 *****************************************************************************
 * Copyright (C) 2012-2015 L-SMASH Works project
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

typedef lw_video_scaler_handler_t lwlibav_video_scaler_handler_t;
typedef lw_video_output_handler_t lwlibav_video_output_handler_t;

#define LW_VFRAME_FLAG_KEY                 0x1
#define LW_VFRAME_FLAG_LEADING             0x2
#define LW_VFRAME_FLAG_CORRUPT             0x4
#define LW_VFRAME_FLAG_INVISIBLE           0x8
#define LW_VFRAME_FLAG_COUNTERPART_MISSING 0x10

typedef enum
{
    LW_FIELD_INFO_UNKNOWN = 0,  /* unknown */
    LW_FIELD_INFO_TOP,          /* top field first or top field coded */
    LW_FIELD_INFO_BOTTOM,       /* bottom field first or bottom field coded */
} lw_field_info_t;

typedef struct
{
    int64_t         pts;
    int64_t         dts;
    int64_t         file_offset;
    uint32_t        sample_number;      /* unique value in decoding order */
    int             extradata_index;
    int             flags;
    int             pict_type;          /* may be stored as enum AVPictureType */
    int             poc;
    int             repeat_pict;
    lw_field_info_t field_info;
} video_frame_info_t;

typedef struct
{
    uint32_t decoding_to_presentation;
} order_converter_t;

typedef struct
{
    /* common */
    AVFormatContext    *format;
    int                 stream_index;
    int                 error;
    lw_log_handler_t    lh;
    lwlibav_extradata_handler_t exh;
    AVCodecContext     *ctx;
    AVIndexEntry       *index_entries;
    int                 index_entries_count;
    int                 lw_seek_flags;
    int                 av_seek_flags;
    int                 dv_in_avi;          /* unused */
    enum AVCodecID      codec_id;
    const char        **preferred_decoder_names;
    AVRational          time_base;
    uint32_t            frame_count;
    AVFrame            *frame_buffer;
    video_frame_info_t *frame_list;         /* stored in presentation order */
    /* */
    uint32_t            forward_seek_threshold;
    int                 seek_mode;
    int                 max_width;
    int                 max_height;
    int                 initial_width;
    int                 initial_height;
    enum AVPixelFormat  initial_pix_fmt;
    enum AVColorSpace   initial_colorspace;
    AVPacket            packet;
    order_converter_t  *order_converter;    /* stored in decoding order */
    uint8_t            *keyframe_list;      /* stored in decoding order */
    uint32_t            last_half_frame;
    uint32_t            last_half_offset;
    uint32_t            last_frame_number;
    uint32_t            last_rap_number;
    uint32_t            first_valid_frame_number;
    AVFrame            *first_valid_frame;
    AVFrame            *last_frame_buffer;
    AVFrame            *movable_frame_buffer;
    int64_t             stream_duration;
    int64_t             min_ts;
    uint32_t            last_ts_frame_number;
    AVRational          actual_time_base;
    int                 strict_cfr;
} lwlibav_video_decode_handler_t;

int lwlibav_get_desired_video_track
(
    const char                     *file_path,
    lwlibav_video_decode_handler_t *vdhp,
    int                             threads
);

void lwlibav_setup_timestamp_info
(
    lwlibav_file_handler_t         *lwhp,
    lwlibav_video_decode_handler_t *vdhp,
    lwlibav_video_output_handler_t *vohp,
    int64_t                        *framerate_num,
    int64_t                        *framerate_den
);

void lwlibav_find_random_accessible_point
(
    lwlibav_video_decode_handler_t *vdhp,
    uint32_t                        presentation_sample_number,
    uint32_t                        decoding_sample_number,
    uint32_t                       *rap_number
);

int64_t lwlibav_get_random_accessible_point_position
(
    lwlibav_video_decode_handler_t *vdhp,
    uint32_t                        rap_number
);

int lwlibav_get_video_frame
(
    lwlibav_video_decode_handler_t *vdhp,
    lwlibav_video_output_handler_t *vohp,
    uint32_t                        frame_number
);

int lwlibav_is_keyframe
(
    lwlibav_video_decode_handler_t *vdhp,
    lwlibav_video_output_handler_t *vohp,
    uint32_t                        frame_number
);

void lwlibav_cleanup_video_decode_handler
(
    lwlibav_video_decode_handler_t *vdhp
);

static inline void lwlibav_cleanup_video_output_handler
(
    lwlibav_video_output_handler_t *vohp
)
{
    lw_cleanup_video_output_handler( vohp );
}

int lwlibav_find_first_valid_video_frame
(
    lwlibav_video_decode_handler_t *vdhp
);
