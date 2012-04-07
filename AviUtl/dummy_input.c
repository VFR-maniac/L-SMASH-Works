/*****************************************************************************
 * dummy_input.c
 *****************************************************************************
 * Copyright (C) 2012 L-SMASH Works project
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

typedef struct
{
    int out_linesize;
    int out_height;
} dummy_handler_t;

static void *open_file( char *file_name, int threads )
{
    return malloc_zero( sizeof(dummy_handler_t) );
}

static int get_first_video_track( lsmash_handler_t *h, int seek_mode )
{
    return 0;
}

static int prepare_video_decoding( lsmash_handler_t *h )
{
#define DUMMY_WIDTH  720
#define DUMMY_HEIGHT 480
    if( h->audio_pcm_sample_count == 0 )
        return -1;  /* Only available if audio stream is present. */
    dummy_handler_t *hp = (dummy_handler_t *)h->video_private;
    h->framerate_num = 24;
    h->framerate_den = 1;
    WAVEFORMATEX *Format = &h->audio_format.Format;
    uint32_t audio_duration = (h->audio_pcm_sample_count - 1) / Format->nSamplesPerSec + 1;
    h->video_sample_count = ((uint64_t)h->framerate_num * audio_duration) / h->framerate_den;
    hp->out_linesize = DUMMY_WIDTH * YUY2_SIZE;
    hp->out_height   = DUMMY_HEIGHT;
    /* BITMAPINFOHEADER */
    h->video_format.biSize        = sizeof( BITMAPINFOHEADER );
    h->video_format.biWidth       = DUMMY_WIDTH;
    h->video_format.biHeight      = DUMMY_HEIGHT;
    h->video_format.biBitCount    = YUY2_SIZE * 8;
    h->video_format.biCompression = MAKEFOURCC( 'Y', 'U', 'Y', '2' );
    return 0;
#undef DUMMY_WIDTH
#undef DUMMY_HEIGHT
}

static int read_video( lsmash_handler_t *h, int sample_number, void *buf )
{
    /* Generate a black video frame. */
    dummy_handler_t *hp = (dummy_handler_t *)h->video_private;
    uint8_t *pic = (uint8_t *)buf;
    int offset = 0;
    for( int i = 0; i < hp->out_height; i++ )
    {
        for( int j = 0; j < hp->out_linesize; j += 2 )
        {
            pic[offset + j    ] = 0;
            pic[offset + j + 1] = 128;
        }
        offset += hp->out_linesize;
    }
    return hp->out_linesize * hp->out_height;
}

static void close_file( void *private_stuff )
{
    dummy_handler_t *hp = (dummy_handler_t *)private_stuff;
    if( !hp )
        return;
    free( hp );
}

lsmash_reader_t dummy_reader =
{
    DUMMY_READER,
    open_file,
    get_first_video_track,
    NULL,
    NULL,
    prepare_video_decoding,
    NULL,
    read_video,
    NULL,
    NULL,
    NULL,
    NULL,
    close_file
};
