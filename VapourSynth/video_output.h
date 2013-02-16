/*****************************************************************************
 * video_output.h
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

#include "../common/video_output.h"

typedef int component_reorder_t;

typedef void func_make_black_background
(
    VSFrameRef  *vs_frame,
    const VSAPI *vsapi
);

typedef void func_make_frame
(
    AVPicture                 *av_picture,
    int                        width,
    int                        height,
    const component_reorder_t *component_reorder,
    VSFrameRef                *vs_frame,
    VSFrameContext            *frame_ctx,
    const VSAPI               *vsapi
);

typedef struct
{
    int                         variable_info;
    const component_reorder_t  *component_reorder;
    VSPresetFormat              vs_output_pixel_format;
    VSFrameRef                 *background_frame;
    func_make_black_background *make_black_background;
    func_make_frame            *make_frame;
} vs_video_output_handler_t;

VSPresetFormat get_vs_output_pixel_format( const char *format_name );

int determine_colorspace_conversion
(
    lw_video_output_handler_t *vohp,
    enum AVPixelFormat        *input_pixel_format
);

VSFrameRef *new_output_video_frame
(
    lw_video_output_handler_t *vohp,
    AVFrame                   *av_frame,
    VSFrameContext            *frame_ctx,
    VSCore                    *core,
    const VSAPI               *vsapi
);

VSFrameRef *make_frame
(
    lw_video_output_handler_t *vohp,
    AVFrame                   *av_frame,
    VSFrameContext            *frame_ctx,
    VSCore                    *core,
    const VSAPI               *vsapi
);
