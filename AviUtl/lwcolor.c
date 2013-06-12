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
    PIXEL_LW48 *pixelp = (PIXEL_LW48 *)cpip->pixelp;
    for( int y = 0; y < cpip->h; y++ )
    {
        PIXEL_YC *ycp = (PIXEL_YC *)((BYTE *)cpip->ycp + y * cpip->line_size);
        for( int x = 0; x < cpip->w; x++ )
            *(ycp++) = *((PIXEL_YC *)pixelp++);
    }
    return TRUE;
}

#define CLIP_BYTE( value ) ((value) > 255 ? 255 : (value) < 0 ? 0 : (value))

BOOL func_yc2pixel( COLOR_PROC_INFO *cpip )
{
    if( cpip->format == OUTPUT_TAG_LW48 )
    {
        /* LW48->LW48 */
        PIXEL_LW48 *pixelp = (PIXEL_LW48 *)cpip->pixelp;
        for( int y = 0; y < cpip->h; y++ )
        {
            PIXEL_YC *ycp = (PIXEL_YC *)((BYTE *)cpip->ycp + y * cpip->line_size);
            for( int x = 0; x < cpip->w; x++ )
                *(pixelp++) = *((PIXEL_LW48 *)ycp++);
        }
        return TRUE;
    }
    else if( cpip->format == 0 )
    {
        /* LW48->RGB */
        BYTE *pixelp = (BYTE *)cpip->pixelp;
        BYTE *_ycp   = (BYTE *)cpip->ycp + (cpip->h - 1) * cpip->line_size;
        for( int y = 0; y < cpip->h; y++ )
        {
            PIXEL_YC *ycp = (PIXEL_YC *)(_ycp - y * cpip->line_size);
            for( int x = 0; x < cpip->w; x++ )
            {
                double _y  = 1.164 * ((unsigned char)(ycp->y >> 8) - 16);
                double _cb = (unsigned char)(ycp->cb >> 8) - 128;
                double _cr = (unsigned char)(ycp->cr >> 8) - 128;
                ++ycp;
                double r = _y               + 1.596 * _cr;
                double g = _y - 0.391 * _cb - 0.813 * _cr;
                double b = _y + 2.018 * _cb;
                pixelp[0] = CLIP_BYTE( b );
                pixelp[1] = CLIP_BYTE( g );
                pixelp[2] = CLIP_BYTE( r );
                pixelp += 3;
            }
        }
        return TRUE;
    }
    return FALSE;
}
