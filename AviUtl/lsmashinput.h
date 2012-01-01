/*****************************************************************************
 * lsmashinput.h
 *****************************************************************************
 * Copyright (C) 2011-2012 L-SMASH Works project
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

#define YC48_SIZE  6
#define RGB24_SIZE 3
#define YUY2_SIZE  2

#define MESSAGE_BOX_DESKTOP( uType, ... ) \
do \
{ \
    char temp[256]; \
    wsprintf( temp, __VA_ARGS__ ); \
    MessageBox( HWND_DESKTOP, temp, "lsmashinput", uType ); \
} while( 0 )

/* Macros for debug */
#if defined( DEBUG_VIDEO ) || defined( DEBUG_AUDIO )
#define DEBUG_MESSAGE_BOX_DESKTOP( uType, ... ) MESSAGE_BOX_DESKTOP( uType, __VA_ARGS__ )
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

typedef enum
{
    READER_NONE       = 0,
    LIBAVSMASH_READER = 1,
    FFMS_READER       = 2,
} reader_type;

typedef enum
{
    OUTPUT_YC48  = 0,
    OUTPUT_RGB24 = 1,
    OUTPUT_YUY2  = 2,
} output_colorspace;

typedef struct lsmash_handler_tag lsmash_handler_t;

typedef struct
{
    reader_type type;
    void *(*open_file)             ( lsmash_handler_t *h, char *file_name, int threads );
    int   (*get_first_video_track) ( lsmash_handler_t *h );
    int   (*get_first_audio_track) ( lsmash_handler_t *h );
    void  (*destroy_disposable)    ( void *private_stuff );
    int   (*prepare_video_decoding)( lsmash_handler_t *h );
    int   (*prepare_audio_decoding)( lsmash_handler_t *h );
    int   (*read_video)            ( lsmash_handler_t *h, int sample_number, void *buf );
    int   (*read_audio)            ( lsmash_handler_t *h, int start, int wanted_length, void *buf );
    int   (*is_keyframe)           ( lsmash_handler_t *h, int sample_number );
    void  (*video_cleanup)         ( lsmash_handler_t *h );
    void  (*audio_cleanup)         ( lsmash_handler_t *h );
    void  (*close_file)            ( void *private_stuff );
} lsmash_reader_t;

struct lsmash_handler_tag
{
    void              *global_private;
    void (*close_file)( void *private_stuff );
    /* Video stuff */
    reader_type        video_reader;
    void              *video_private;
    BITMAPINFOHEADER   video_format;
    int                framerate_num;
    int                framerate_den;
    uint32_t           video_sample_count;
    int  (*read_video)      ( lsmash_handler_t *h, int sample_number, void *buf );
    int  (*is_keyframe)     ( lsmash_handler_t *h, int sample_number );
    void (*video_cleanup)   ( lsmash_handler_t *h );
    void (*close_video_file)( void *private_stuff );
    /* Audio stuff */
    reader_type        audio_reader;
    void              *audio_private;
    WAVEFORMATEX       audio_format;
    uint32_t           audio_pcm_sample_count;
    int  (*read_audio)      ( lsmash_handler_t *h, int start, int wanted_length, void *buf );
    void (*audio_cleanup)   ( lsmash_handler_t *h );
    void (*close_audio_file)( void *private_stuff );
};

void *malloc_zero( size_t size );
int check_sse2();
output_colorspace determine_colorspace_conversion( int *input_pixel_format, int *output_pixel_format );

typedef void func_get_output( uint8_t *out_data, int out_linesize, uint8_t **in_data, int in_linesize, int height, int full_range );
