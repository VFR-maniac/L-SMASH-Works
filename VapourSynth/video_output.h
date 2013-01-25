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

typedef void func_make_black_background( VSFrameRef *frame, const VSAPI *vsapi );
typedef int func_make_frame( struct SwsContext *sws_ctx, AVFrame *picture, VSFrameRef *frame, VSFrameContext *frame_ctx, const VSAPI *vsapi );

typedef struct
{
    int                         variable_info;
    struct SwsContext          *sws_ctx;
    int                         scaler_flags;
    enum AVPixelFormat          av_output_pixel_format;
    VSPresetFormat              vs_output_pixel_format;
    VSFrameRef                 *background_frame;
    VSFrameRef                 *first_valid_frame;
    uint32_t                    first_valid_frame_number;
    func_make_black_background *make_black_background;
    func_make_frame            *make_frame;
} video_output_handler_t;

func_make_frame make_frame_yuv420p;

VSPresetFormat get_vs_output_pixel_format( const char *format_name );
int determine_colorspace_conversion( video_output_handler_t *vohp, enum AVPixelFormat *input_pixel_format );
int make_frame( video_output_handler_t *ohp, AVFrame *picture, VSFrameRef *frame, VSFrameContext *frame_ctx, const VSAPI *vsapi );
