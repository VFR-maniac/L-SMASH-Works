/*****************************************************************************
 * lsmashdumper.c
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

/* This file is available under an ISC license. */

#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <windows.h>

/* L-SMASH */
#define LSMASH_DEMUXER_ENABLED
#include <lsmash.h>

#include "filter.h"

/* File filter */
#define DUMP_FILE_EXT "Dump File (*.txt)\0*.txt\0"

FILTER_DLL filter =
{
    FILTER_FLAG_EXPORT|FILTER_FLAG_NO_CONFIG|FILTER_FLAG_ALWAYS_ACTIVE|FILTER_FLAG_PRIORITY_LOWEST|FILTER_FLAG_EX_INFORMATION,
    0,0,                        /* Size of configuration window */
    "L-SMASH Dumper",           /* Name of filter plugin */
    0,                          /* Number of trackbars */
    NULL,                       /* Pointer to group of names of trackbar */
    NULL,                       /* Pointer to group of initial values of trackbar */
    NULL,                       /* Minimum of trackbar */
    NULL,                       /* Maximum of trackbar */
    0,                          /* Number of checkboxes */
    NULL,                       /* Pointer to group of names of checkbox */
    NULL,                       /* Pointer to group of initial values of checkbox */
    NULL,                       /* Pointer to filter process function (If NULL, won't be called.) */
    NULL,                       /* Pointer to function called when beginning (If NULL, won't be called.) */
    NULL,                       /* Pointer to function called when ending (If NULL, won't be called.) */
    NULL,                       /* Pointer to function called when its configuration is updated (If NULL, won't be called.) */
    func_WndProc,               /* Pointer to function called when window message comes on configuration window (If NULL, won't be called.) */
    NULL,                       /* Pointer to group of set points of trackbar */
    NULL,                       /* Pointer to group of set points of checkbox */
    NULL,                       /* Pointer to extended data region (Valid only if FILTER_FLAG_EX_DATA is enabled.) */
    0,                          /* Size of extended data (Valid only if FILTER_FLAG_EX_DATA is enabled.) */
    "L-SMASH Dumper",           /* Information of filter plugin */
};

EXTERN_C FILTER_DLL __declspec(dllexport) * __stdcall GetFilterTable( void )
{
    return &filter;
}

EXTERN_C FILTER_DLL __declspec(dllexport) * __stdcall GetFilterTableYUY2( void )
{
    return &filter;
}

BOOL func_WndProc( HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, void *editp, FILTER *fp )
{
    if( !fp->exfunc->is_editing( editp ) )
        return FALSE;
    if( message != WM_FILTER_EXPORT )
        return FALSE;
    /* Open the input file. */
    FRAME_STATUS fs;
    if( !fp->exfunc->get_frame_status( editp, 0, &fs ) )
    {
        MessageBox( HWND_DESKTOP, "Failed to get the status of the first frame.", "lsmashdumper", MB_ICONERROR | MB_OK );
        return -1;
    }
    int source_file_id;
    int source_video_number;
    if( !fp->exfunc->get_source_video_number( editp, fs.video, &source_file_id, &source_video_number ) )
    {
        MessageBox( HWND_DESKTOP, "Failed to get the number of the source video.", "lsmashdumper", MB_ICONERROR | MB_OK );
        return -1;
    }
    FILE_INFO fi;
    if( !fp->exfunc->get_source_file_info( editp, &fi, source_file_id ) )
    {
        MessageBox( HWND_DESKTOP, "Failed to get the information of the source file.", "lsmashdumper", MB_ICONERROR | MB_OK );
        return -1;
    }
    lsmash_root_t *root = lsmash_open_movie( fi.name, LSMASH_FILE_MODE_READ | LSMASH_FILE_MODE_DUMP );
    if( !root )
    {
        MessageBox( HWND_DESKTOP, "Failed to open the input file.", "lsmashdumper", MB_ICONERROR | MB_OK );
        return FALSE;
    }
    /* Open the output file to dump the input file. */
    char file_name[MAX_PATH];
    if( !fp->exfunc->dlg_get_save_name( (LPSTR)file_name, DUMP_FILE_EXT, NULL ) )
    {
        lsmash_destroy_root( root );
        return FALSE;
    }
    if( lsmash_print_movie( root, file_name ) )
    {
        MessageBox( HWND_DESKTOP, "Failed to dump the box structure.", "lsmashdumper", MB_ICONERROR | MB_OK );
        lsmash_destroy_root( root );
        return FALSE;
    }
    lsmash_destroy_root( root );
    return FALSE;
}
