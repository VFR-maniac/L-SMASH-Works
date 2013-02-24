/*****************************************************************************
 * video_output.h
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
 * However, when distributing its binary file, it will be under LGPL or GPL. */

#include "../common/video_output.h"

typedef void func_make_black_background( PVideoFrame &frame );

typedef int func_make_frame
(
    struct SwsContext  *sws_ctx,
    AVFrame            *av_frame,
    PVideoFrame        &as_frame,
    IScriptEnvironment *env
);

typedef struct
{
    func_make_black_background *make_black_background;
    func_make_frame            *make_frame;
    IScriptEnvironment         *env;
    VideoInfo                  *vi;
} as_video_output_handler_t;

typedef struct
{
    PVideoFrame as_frame_buffer;
} as_video_buffer_handler_t;

int determine_colorspace_conversion
(
    lw_video_output_handler_t *vohp,
    enum AVPixelFormat         input_pixel_format,
    int                       *output_pixel_type
);

int make_frame
(
    lw_video_output_handler_t *vohp,
    AVFrame                   *av_frame,
    PVideoFrame               &as_frame,
    enum AVColorSpace          colorspace,
    IScriptEnvironment        *env
);

int as_check_dr_support_format( enum AVPixelFormat decoded_pixel_format );

int as_video_get_buffer
(
    AVCodecContext *ctx,
    AVFrame        *av_frame
);

void as_video_release_buffer
(
    AVCodecContext *ctx,
    AVFrame        *av_frame
);
