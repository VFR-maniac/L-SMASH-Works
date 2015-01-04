/*****************************************************************************
 * lwlibav_source.h
 *****************************************************************************
 * Copyright (C) 2013-2015 L-SMASH Works project
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

#define NO_PROGRESS_HANDLER

#include "../common/progress.h"
#include "../common/lwlibav_dec.h"
#include "../common/lwlibav_video.h"
#include "../common/lwlibav_audio.h"
#include "../common/lwindex.h"

class LWLibavVideoSource : public IClip
{
private:
    VideoInfo                      vi;
    lwlibav_file_handler_t         lwh;
    lwlibav_video_decode_handler_t vdh;
    lwlibav_video_output_handler_t voh;
    void prepare_video_decoding
    (
        int                 direct_rendering,
        int                 stacked_format,
        enum AVPixelFormat  pixel_format,
        IScriptEnvironment *env
    );
public:
    LWLibavVideoSource
    (
        lwlibav_option_t   *opt,
        int                 seek_mode,
        uint32_t            forward_seek_threshold,
        int                 direct_rendering,
        int                 stacked_format,
        enum AVPixelFormat  pixel_format,
        IScriptEnvironment *env
    );
    ~LWLibavVideoSource();
    PVideoFrame __stdcall GetFrame( int n, IScriptEnvironment *env );
    bool __stdcall GetParity( int n );
    void __stdcall GetAudio( void *buf, __int64 start, __int64 count, IScriptEnvironment *env ) {}
    void __stdcall SetCacheHints( int cachehints, int frame_range ) {}
    const VideoInfo& __stdcall GetVideoInfo() { return vi; }
};

class LWLibavAudioSource : public IClip
{
private:
    VideoInfo                      vi;
    lwlibav_file_handler_t         lwh;
    lwlibav_audio_decode_handler_t adh;
    lwlibav_audio_output_handler_t aoh;
    void prepare_audio_decoding
    (
        uint64_t            channel_layout,
        int                 sample_rate,
        IScriptEnvironment *env
    );
    int delay_audio( int64_t *start, int64_t wanted_length );
public:
    LWLibavAudioSource
    (
        lwlibav_option_t   *opt,
        uint64_t            channel_layout,
        int                 sample_rate,
        IScriptEnvironment *env
    );
    ~LWLibavAudioSource();
    PVideoFrame __stdcall GetFrame( int n, IScriptEnvironment *env ) { return NULL; }
    bool __stdcall GetParity( int n ) { return false; }
    void __stdcall GetAudio( void *buf, __int64 start, __int64 wanted_length, IScriptEnvironment *env );
    void __stdcall SetCacheHints( int cachehints, int frame_range ) {}
    const VideoInfo& __stdcall GetVideoInfo() { return vi; }
};
