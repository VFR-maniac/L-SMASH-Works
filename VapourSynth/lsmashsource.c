/*****************************************************************************
 * lsmashsource.c
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

/* This file is available under an ISC license.
 * However, when distributing its binary file, it will be under LGPL or GPL. */

#include <stdio.h>
#include <stdarg.h>

#include "lsmashsource.h"

#include "../common/utils.h"

void set_error( void *message_priv, const char *message, ... )
{
    vs_basic_handler_t *eh = (vs_basic_handler_t *)message_priv;
    if( !eh || !eh->vsapi )
        return;
    char temp[256];
    va_list args;
    va_start( args, message );
    vsprintf( temp, message, args );
    va_end( args );
    if( eh->out )
        eh->vsapi->setError( eh->out, (const char *)temp );
    else if( eh->frame_ctx )
        eh->vsapi->setFilterError( (const char *)temp, eh->frame_ctx );
}

extern void VS_CC vs_libavsmashsource_create( const VSMap *in, VSMap *out, void *user_data, VSCore *core, const VSAPI *vsapi );
extern void VS_CC vs_lwlibavsource_create( const VSMap *in, VSMap *out, void *user_data, VSCore *core, const VSAPI *vsapi );

VS_EXTERNAL_API(void) VapourSynthPluginInit( VSConfigPlugin config_func, VSRegisterFunction register_func, VSPlugin *plugin )
{
    config_func
    (
        "systems.innocent.lsmas",
        "lsmas",
        "LSMASHSource for VapourSynth",
        VAPOURSYNTH_API_VERSION,
        1,
        plugin
    );
    register_func
    (
        "LibavSMASHSource",
        "source:data;track:int:opt;threads:int:opt;seek_mode:int:opt;seek_threshold:int:opt;variable:int:opt;format:data:opt;",
        vs_libavsmashsource_create,
        NULL,
        plugin
    );
    register_func
    (
        "LWLibavSource",
        "source:data;stream_index:int:opt;threads:int:opt;cache_index:int:opt;seek_mode:int:opt;seek_threshold:int:opt;variable:int:opt;format:data:opt;dr:int:opt;",
        vs_lwlibavsource_create,
        NULL,
        plugin
    );
}
