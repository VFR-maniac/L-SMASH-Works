/*****************************************************************************
 * video_output.h
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

#define REPEAT_CONTROL_CACHE_NUM 2

typedef int func_get_buffer_t( struct AVCodecContext *, AVFrame *, int );

typedef struct
{
    int                enabled;
    int                flags;
    int                input_width;
    int                input_height;
    enum AVPixelFormat input_pixel_format;
    enum AVPixelFormat output_pixel_format;
    enum AVColorSpace  input_colorspace;
    int                input_yuv_range;
    struct SwsContext *sws_ctx;
} lw_video_scaler_handler_t;

typedef struct
{
    uint32_t top;
    uint32_t bottom;
} lw_video_frame_order_t;

typedef struct
{
    lw_video_scaler_handler_t scaler;
    int                       output_width;
    int                       output_height;
    int                       output_linesize;
    uint32_t                  output_frame_size;
    /* VFR->CFR conversion */
    int                       vfr2cfr;
    uint32_t                  cfr_num;
    uint32_t                  cfr_den;
    /* Repeat control */
    int                       repeat_control;
    int64_t                   repeat_correction_ts;
    uint32_t                  frame_count;
    uint32_t                  frame_order_count;
    lw_video_frame_order_t   *frame_order_list;
    AVFrame                  *frame_cache_buffers[REPEAT_CONTROL_CACHE_NUM];
    uint32_t                  frame_cache_numbers[REPEAT_CONTROL_CACHE_NUM];
    /* Application private extension */
    void                     *private_handler;
    void (*free_private_handler)( void *private_handler );
} lw_video_output_handler_t;

int avoid_yuv_scale_conversion( enum AVPixelFormat *pixel_format );

int initialize_scaler_handler
(
    lw_video_scaler_handler_t *vshp,
    AVCodecContext            *ctx,
    int                        enabled,
    int                        flags,
    enum AVPixelFormat         output_pixel_format
);

struct SwsContext *update_scaler_configuration
(
    struct SwsContext *sws_ctx,
    int                flags,
    int                width,
    int                height,
    enum AVPixelFormat input_pixel_format,
    enum AVPixelFormat output_pixel_format,
    enum AVColorSpace  colorspace,
    int                yuv_range
);

void lw_cleanup_video_output_handler
(
    lw_video_output_handler_t *vohp
);
