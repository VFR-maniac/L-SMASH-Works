/*****************************************************************************
 * lwindex.h
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

#define INDEX_FILE_VERSION 5

typedef struct
{
    const char *file_path;
    int         threads;
    int         av_sync;
    int         no_create_index;
    int         force_video;
    int         force_video_index;
    int         force_audio;
    int         force_audio_index;
} lwlibav_option_t;

typedef struct
{
    char   *file_path;
    char   *format_name;
    int     format_flags;
    int     threads;
    int64_t av_gap;
} lwlibav_file_handler_t;

int lwlibav_construct_index
(
    lwlibav_file_handler_t *lwhp,
    video_decode_handler_t *vdhp,
    audio_decode_handler_t *adhp,
    audio_output_handler_t *aohp,
    error_handler_t        *ehp,
    lwlibav_option_t       *opt,
    progress_indicator_t   *indicator,
    progress_handler_t     *php
);
