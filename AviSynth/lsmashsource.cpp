/*****************************************************************************
 * lsmashsource.cpp
 *****************************************************************************
 * Copyright (C) 2012-2015 L-SMASH Works project
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

#include "lsmashsource.h"

void throw_error
(
    lw_log_handler_t *lhp,
    lw_log_level      level,
    const char       *message
)
{
    IScriptEnvironment *env = (IScriptEnvironment *)lhp->priv;
    env->ThrowError( message );
}

extern AVSValue __cdecl CreateLSMASHVideoSource( AVSValue args, void *user_data, IScriptEnvironment *env );
extern AVSValue __cdecl CreateLSMASHAudioSource( AVSValue args, void *user_data, IScriptEnvironment *env );
extern AVSValue __cdecl CreateLWLibavVideoSource( AVSValue args, void *user_data, IScriptEnvironment *env );
extern AVSValue __cdecl CreateLWLibavAudioSource( AVSValue args, void *user_data, IScriptEnvironment *env );

extern "C" __declspec(dllexport) const char * __stdcall AvisynthPluginInit2( IScriptEnvironment *env )
{
    /* LSMASHVideoSource */
    env->AddFunction
    (
        "LSMASHVideoSource",
        "[source]s[track]i[threads]i[seek_mode]i[seek_threshold]i[dr]b[fpsnum]i[fpsden]i[stacked]b[format]s[decoder]s",
        CreateLSMASHVideoSource,
        0
    );
    /* LSMASHAudioSource */
    env->AddFunction
    (
        "LSMASHAudioSource",
        "[source]s[track]i[skip_priming]b[layout]s[rate]i[decoder]s",
        CreateLSMASHAudioSource,
        0
    );
    /* LWLibavVideoSource */
    env->AddFunction
    (
        "LWLibavVideoSource",
        "[source]s[stream_index]i[threads]i[cache]b[seek_mode]i[seek_threshold]i[dr]b[fpsnum]i[fpsden]i[repeat]b[dominance]i[stacked]b[format]s[decoder]s",
        CreateLWLibavVideoSource,
        0
    );
    /* LWLibavAudioSource */
    env->AddFunction
    (
        "LWLibavAudioSource",
        "[source]s[stream_index]i[cache]b[av_sync]b[layout]s[rate]i[decoder]s",
        CreateLWLibavAudioSource,
        0
    );
    return "LSMASHSource";
}
