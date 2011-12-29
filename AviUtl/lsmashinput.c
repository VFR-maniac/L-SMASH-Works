/*****************************************************************************
 * lsmashinput.c
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

#define MPEG4_FILE_EXT      "*.mp4;*.m4v;*.m4a;*.mov;*.qt;*.3gp;*.3g2;*.f4v"
#define ANY_FILE_EXT        "*.*"

INPUT_PLUGIN_TABLE input_plugin_table =
{
    INPUT_PLUGIN_FLAG_VIDEO | INPUT_INFO_FLAG_AUDIO,                /* INPUT_PLUGIN_FLAG_VIDEO : support images
                                                                     * INPUT_PLUGIN_FLAG_AUDIO : support audio */
    "Libav-SMASH File Reader",                                      /* Name of plugin */
    "MPEG-4 File (" MPEG4_FILE_EXT ")\0" MPEG4_FILE_EXT "\0"        /* Filter for Input file */
#ifdef HAVE_FFMS
    "Any File (" ANY_FILE_EXT ")\0" ANY_FILE_EXT "\0"
#endif
    ,
    "Libav-SMASH File Reader",                                      /* Information of plugin */
    NULL,                                                           /* Pointer to function called when opening DLL (If NULL, won't be called.) */
    NULL,                                                           /* Pointer to function called when closing DLL (If NULL, won't be called.) */
    func_open,                                                      /* Pointer to function to open input file */
    func_close,                                                     /* Pointer to function to close input file */
    func_info_get,                                                  /* Pointer to function to get information of input file */
    func_read_video,                                                /* Pointer to function to read image data */
    func_read_audio,                                                /* Pointer to function to read audio data */
    func_is_keyframe,                                               /* Pointer to function to check if it is a keyframe or not (If NULL, all is keyframe.) */
    NULL,                                                           /* Pointer to function called when configuration dialog is required */
};

EXTERN_C INPUT_PLUGIN_TABLE __declspec(dllexport) * __stdcall GetInputPluginTable( void )
{
    return &input_plugin_table;
}

void *malloc_zero( size_t size )
{
    void *p = malloc( size );
    if( !p )
        return NULL;
    memset( p, 0, size );
    return p;
}

INPUT_HANDLE func_open( LPSTR file )
{
    lsmash_handler_t *hp = (lsmash_handler_t *)malloc_zero( sizeof(lsmash_handler_t) );
    if( !hp )
        return NULL;
    int threads = atoi( getenv( "NUMBER_OF_PROCESSORS" ) );
    if( threads > MAX_NUM_THREADS )
        threads = MAX_NUM_THREADS;
    hp->video_reader = READER_NONE;
    hp->audio_reader = READER_NONE;
    extern lsmash_reader_t libavsmash_reader;
#ifdef HAVE_FFMS
    extern lsmash_reader_t ffms_reader;
#endif
    static lsmash_reader_t *lsmash_reader_table[] =
    {
        &libavsmash_reader,
#ifdef HAVE_FFMS
        &ffms_reader,
#endif
        NULL
    };
    for( int i = 0; lsmash_reader_table[i]; i++ )
    {
        int video_none = 1;
        int audio_none = 1;
        lsmash_reader_t reader = *lsmash_reader_table[i];
        void *private_stuff = reader.open_file( hp, file, threads );
        if( private_stuff )
        {
            if( !hp->video_private )
            {
                hp->video_private = private_stuff;
                if( !reader.get_first_video_track( hp ) )
                {
                    hp->video_reader     = reader.type;
                    hp->read_video       = reader.read_video;
                    hp->is_keyframe      = reader.is_keyframe;
                    hp->video_cleanup    = reader.video_cleanup;
                    hp->close_video_file = reader.close_file;
                    video_none = 0;
                }
                else
                    hp->video_private = NULL;
            }
            if( !hp->audio_private )
            {
                hp->audio_private = private_stuff;
                if( !reader.get_first_audio_track( hp ) )
                {
                    hp->audio_reader     = reader.type;
                    hp->read_audio       = reader.read_audio;
                    hp->audio_cleanup    = reader.audio_cleanup;
                    hp->close_audio_file = reader.close_file;
                    audio_none = 0;
                }
                else
                    hp->audio_private = NULL;
            }
        }
        if( video_none && audio_none )
        {
            if( reader.close_file )
                reader.close_file( private_stuff );
        }
        else
        {
            if( reader.destroy_disposable )
                reader.destroy_disposable( private_stuff );
            if( !video_none && reader.prepare_video_decoding( hp ) )
            {
                hp->video_cleanup( hp );
                hp->video_cleanup = NULL;
                hp->video_private = NULL;
                hp->video_reader  = READER_NONE;
                video_none = 1;
            }
            if( !audio_none && reader.prepare_audio_decoding( hp ) )
            {
                hp->audio_cleanup( hp );
                hp->audio_cleanup = NULL;
                hp->audio_private = NULL;
                hp->audio_reader  = READER_NONE;
                audio_none = 1;
            }
            if( video_none && audio_none && reader.close_file )
                reader.close_file( private_stuff );
        }
        /* Found both video and audio reader. */
        if( hp->video_reader != READER_NONE && hp->audio_reader != READER_NONE )
            break;
    }
    if( hp->video_reader == hp->audio_reader )
    {
        hp->global_private = hp->video_private;
        hp->close_file     = hp->close_video_file;
        hp->close_video_file = NULL;
        hp->close_audio_file = NULL;
    }
    if( hp->video_reader == READER_NONE && hp->audio_reader == READER_NONE )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_OK, "No readable video and/or audio stream" );
        func_close( hp );
        return NULL;
    }
    return hp;
}

BOOL func_close( INPUT_HANDLE ih )
{
    lsmash_handler_t *hp = (lsmash_handler_t *)ih;
    if( !hp )
        return TRUE;
    if( hp->video_cleanup )
        hp->video_cleanup( hp );
    if( hp->audio_cleanup )
        hp->audio_cleanup( hp );
    if( hp->close_file )
        hp->close_file( hp->global_private );
    else
    {
        if( hp->close_video_file )
            hp->close_video_file( hp->video_private );
        if( hp->close_audio_file )
            hp->close_audio_file( hp->audio_private );
    }
    free( hp );
    return TRUE;
}

BOOL func_info_get( INPUT_HANDLE ih, INPUT_INFO *iip )
{
    lsmash_handler_t *hp = (lsmash_handler_t *)ih;
    memset( iip, 0, sizeof(INPUT_INFO) );
    if( hp->video_reader != READER_NONE )
    {
        iip->flag             |= INPUT_INFO_FLAG_VIDEO;
        iip->rate              = hp->framerate_num;
        iip->scale             = hp->framerate_den;
        iip->n                 = hp->video_sample_count;
        iip->format            = &hp->video_format;
        iip->format_size       = hp->video_format.biSize;
        iip->handler           = 0;
    }
    if( hp->audio_reader != READER_NONE )
    {
        iip->flag             |= INPUT_INFO_FLAG_AUDIO;
        iip->audio_n           = hp->audio_pcm_sample_count;
        iip->audio_format      = &hp->audio_format;
        iip->audio_format_size = sizeof( WAVEFORMATEX );
    }
    return TRUE;
}

int func_read_video( INPUT_HANDLE ih, int sample_number, void *buf )
{
    lsmash_handler_t *hp = (lsmash_handler_t *)ih;
    return hp->read_video ? hp->read_video( hp, sample_number, buf ) : 0;
}

int func_read_audio( INPUT_HANDLE ih, int start, int length, void *buf )
{
    lsmash_handler_t *hp = (lsmash_handler_t *)ih;
    return hp->read_audio ? hp->read_audio( hp, start, length, buf ) : 0;
}

BOOL func_is_keyframe( INPUT_HANDLE ih, int sample_number )
{
    lsmash_handler_t *hp = (lsmash_handler_t *)ih;
    if( sample_number >= hp->video_sample_count )
        return FALSE;   /* In reading as double framerate, keyframe detection doesn't work at all
                         * since sample_number exceeds the number of video samples. */
    return hp->is_keyframe ? hp->is_keyframe( hp, sample_number ) : FALSE;
}
