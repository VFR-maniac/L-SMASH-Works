/*****************************************************************************
 * libavsmash_input.h
 *****************************************************************************
 * Copyright (C) 2011 L-SMASH Works project
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

#include "lsmashinput.h"

/* L-SMASH */
#define LSMASH_DEMUXER_ENABLED
#include <lsmash.h>                 /* Demuxer */

/* Libav
 * The binary file will be LGPLed or GPLed. */
#include <libavformat/avformat.h>   /* Codec specific info importer */
#include <libavcodec/avcodec.h>     /* Decoder */
#include <libswscale/swscale.h>     /* Colorspace converter */
#ifdef DEBUG_VIDEO
#include <libavutil/pixdesc.h>
#endif
#define MAX_NUM_THREADS 4

typedef enum
{
    DECODE_REQUIRE_INITIAL = 0,
    DECODE_INITIALIZING    = 1,
    DECODE_INITIALIZED     = 2
} decode_status_t;

typedef struct
{
    uint32_t composition_to_decoding;
} order_converter_t;

typedef struct libavsmash_handler_tag
{
    /* L-SMASH's stuff */
    lsmash_root_t     *root;
    uint32_t           video_track_ID;
    uint32_t           audio_track_ID;
    /* Libav's stuff */
    AVCodecContext    *video_ctx;
    AVCodecContext    *audio_ctx;
    AVFormatContext   *format_ctx;
    struct SwsContext *sws_ctx;
    /* Video stuff */
    uint8_t           *video_input_buffer;
    uint32_t           video_input_buffer_size;
    int                full_range;
    int                pixel_size;
    uint32_t           last_video_sample_number;
    uint32_t           delay_count;
    decode_status_t    decode_status;
    order_converter_t *order_converter;
    int (*convert_colorspace)( struct libavsmash_handler_tag *, AVFrame *, uint8_t * );
    /* Audio stuff */
    uint8_t           *audio_input_buffer;
    uint32_t           audio_input_buffer_size;
    uint8_t           *audio_output_buffer;
    uint32_t           audio_frame_count;
    uint32_t           next_audio_pcm_sample_number;
    uint32_t           last_audio_sample_number;
    uint32_t           last_remainder_size;
} libavsmash_handler_t;

/* Colorspace converters */
int to_yuv16le_to_yc48( libavsmash_handler_t *hp, AVFrame *picture, uint8_t *buf );
int to_rgb24( libavsmash_handler_t *hp, AVFrame *picture, uint8_t *buf );
int to_yuy2( libavsmash_handler_t *hp, AVFrame *picture, uint8_t *buf );
