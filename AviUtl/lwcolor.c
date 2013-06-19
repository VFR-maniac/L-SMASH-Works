/*****************************************************************************
 * lwcolor.c
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

#include <windows.h>

#include "color.h"

#include "lwcolor.h"
#include "config.h"

COLOR_PLUGIN_TABLE color_plugin_table =
{
    0,                                      /* flags */
    "LW ColorSpace",                        /* Name of plugin */
    "L-SMASH Works Color Space Converter"   /* Information of plugin */
    " r" LSMASHWORKS_REV "\0",
    NULL,                                   /* Pointer to function called when opening DLL (If NULL, won't be called.) */
    NULL,                                   /* Pointer to function called when closing DLL (If NULL, won't be called.) */
    func_pixel2yc,                          /* Convert DIB format image into PIXEL_YC format image (If NULL, won't be called.) */
    func_yc2pixel,                          /* Convert PIXEL_YC format image into DIB format image (If NULL, won't be called.) */
};


EXTERN_C COLOR_PLUGIN_TABLE __declspec(dllexport) * __stdcall GetColorPluginTable( void )
{
    return &color_plugin_table;
}

EXTERN_C COLOR_PLUGIN_TABLE __declspec(dllexport) * __stdcall GetColorPluginTableYUY2( void )
{
	return &color_plugin_table;
}

BOOL func_pixel2yc( COLOR_PROC_INFO *cpip )
{
    if( cpip->format != OUTPUT_TAG_LW48 )
        return FALSE;
    /* LW48->LW48 */
    BYTE *ycp    = (BYTE *)cpip->ycp;
    BYTE *pixelp = (BYTE *)cpip->pixelp;
    int linesize = LW48_SIZE * cpip->w;
    for( int y = 0; y < cpip->h; y++ )
    {
        memcpy( ycp, pixelp, linesize );
        ycp    += cpip->line_size;
        pixelp += linesize;
    }
    return TRUE;
}

#define CLIP_BYTE( value ) ((value) > 255 ? 255 : (value) < 0 ? 0 : (value))

BOOL func_yc2pixel( COLOR_PROC_INFO *cpip )
{
    if( cpip->format == OUTPUT_TAG_LW48 )
    {
        /* LW48 -> LW48 */
        BYTE *ycp    = (BYTE *)cpip->ycp;
        BYTE *pixelp = (BYTE *)cpip->pixelp;
        int linesize = LW48_SIZE * cpip->w;
        for( int y = 0; y < cpip->h; y++ )
        {
            memcpy( pixelp, ycp, linesize );
            ycp    += cpip->line_size;
            pixelp += linesize;
        }
    }
    else if( cpip->format == OUTPUT_TAG_YUY2 )
    {
        /* LW48 -> YUY2 */
        BYTE *pixelp = (BYTE *)cpip->pixelp;
        BYTE *_ycp   = (BYTE *)cpip->ycp;
        for( int y = 0; y < cpip->h; y++ )
        {
            PIXEL_LW48 *ycp = (PIXEL_LW48 *)_ycp;
            for( int x = 0; x < cpip->w; x += 2 )
            {
                pixelp[0] = ycp->y  >> 8;
                pixelp[1] = ycp->cb >> 8;
                pixelp[3] = ycp->cr >> 8;
                ++ycp;
                pixelp[2] = ycp->y  >> 8;
                ++ycp;
                pixelp += 4;
            }
            _ycp += cpip->line_size;
        }
    }
    else if( cpip->format == OUTPUT_TAG_RGB )
    {
        /* LW48 -> RGB24 */
        BYTE *ycp_line   = (BYTE *)cpip->ycp + (cpip->h - 1) * cpip->line_size;
        BYTE *pixel_line = (BYTE *)cpip->pixelp;
        int rgb_linesize = (cpip->w * 3 + 3) & ~3;
        for( int y = 0; y < cpip->h; y++ )
        {
            PIXEL_LW48 *ycp = (PIXEL_LW48 *)ycp_line;
            BYTE *pixelp = pixel_line;
            for( int x = 0; x < cpip->w; x++ )
            {
                int _y = (ycp->y - 4096) * 9539;
                int _cb = ycp->cb - 32768;
                int _cr = ycp->cr - 32768;
                ++ycp;
                int r = (_y               + 13074 * _cr + (1<<20)) >> 21;
                int g = (_y -  3203 * _cb -  6808 * _cr + (1<<20)) >> 21;
                int b = (_y + 16531 * _cb               + (1<<20)) >> 21;
                pixelp[0] = CLIP_BYTE( b );
                pixelp[1] = CLIP_BYTE( g );
                pixelp[2] = CLIP_BYTE( r );
                pixelp += 3;
            }
            ycp_line   -= cpip->line_size;
            pixel_line += rgb_linesize;
        }
    }
    else
        return FALSE;
    return TRUE;
}
