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

typedef void func_make_black_background( PVideoFrame &frame );

typedef int func_make_frame
(
    struct SwsContext  *sws_ctx,
    AVFrame            *picture,
    PVideoFrame        &frame,
    IScriptEnvironment *env
);

typedef struct
{
    struct SwsContext          *sws_ctx;
    int                         scaler_flags;
    enum PixelFormat            output_pixel_format;
    PVideoFrame                *first_valid_frame;
    uint32_t                    first_valid_frame_number;
    func_make_black_background *make_black_background;
    func_make_frame            *make_frame;
} video_output_handler_t;

int determine_colorspace_conversion
(
    video_output_handler_t *vohp,
    enum AVPixelFormat     *input_pixel_format,
    int                    *output_pixel_type
);

int make_frame
(
    video_output_handler_t *vohp,
    AVFrame                *picture,
    PVideoFrame            &frame,
    IScriptEnvironment     *env
);
