/*****************************************************************************
 * lsmashinput.h
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

#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <windows.h>

#include "input.h"

#define MAX_NUM_THREADS 4

#define YC48_SIZE  6
#define RGB24_SIZE 3
#define YUY2_SIZE  2

/* Macros for debug */
#if defined( DEBUG_VIDEO ) || defined( DEBUG_AUDIO )
#define DEBUG_MESSAGE_BOX_DESKTOP( uType, ... ) \
do \
{ \
    char temp[256]; \
    wsprintf( temp, __VA_ARGS__ ); \
    MessageBox( HWND_DESKTOP, temp, "lsmashinput", uType ); \
} while( 0 )
#else
#define DEBUG_MESSAGE_BOX_DESKTOP( uType, ... )
#endif

#ifdef DEBUG_VIDEO
#define DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( uType, ... ) DEBUG_MESSAGE_BOX_DESKTOP( uType, __VA_ARGS__ )
#else
#define DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( uType, ... )
#endif

#ifdef DEBUG_AUDIO
#define DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( uType, ... ) DEBUG_MESSAGE_BOX_DESKTOP( uType, __VA_ARGS__ )
#else
#define DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( uType, ... )
#endif

typedef struct lsmash_handler_tag lsmash_handler_t;

typedef struct
{
    void *private_stuff;
    BOOL (*open_file)  ( lsmash_handler_t *, char * );
    int  (*read_video) ( lsmash_handler_t *, int, void * );
    int  (*read_audio) ( lsmash_handler_t *, int, int, void * );
    BOOL (*is_keyframe)( lsmash_handler_t *, int );
    void (*cleanup)    ( lsmash_handler_t * );
} lsmash_reader_t;

struct lsmash_handler_tag
{
    lsmash_reader_t    reader;
    /* Video info */
    BITMAPINFOHEADER   video_format;
    int                framerate_num;
    int                framerate_den;
    uint32_t           video_sample_count;
    /* Audio info */
    WAVEFORMATEX       audio_format;
    uint32_t           audio_pcm_sample_count;
};

void *malloc_zero( size_t size );
