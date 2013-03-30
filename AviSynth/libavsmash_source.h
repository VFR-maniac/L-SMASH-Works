/*****************************************************************************
 * libavsmash_source.h
 *****************************************************************************
 * Copyright (C) 2012-2013 L-SMASH Works project
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

#include "../common/libavsmash.h"
#include "../common/libavsmash_video.h"
#include "../common/libavsmash_audio.h"

class LSMASHVideoSource : public IClip
{
private:
    VideoInfo                         vi;
    libavsmash_video_decode_handler_t vdh;
    libavsmash_video_output_handler_t voh;
    AVFormatContext                  *format_ctx;
    uint32_t open_file
    (
        const char         *source,
        IScriptEnvironment *env
    );
    void get_video_track
    (
        const char         *source,
        uint32_t            track_number,
        int                 threads,
        IScriptEnvironment *env
    );
    void prepare_video_decoding
    (
        int                 direct_rendering,
        IScriptEnvironment *env
    );
public:
    LSMASHVideoSource
    (
        const char         *source,
        uint32_t            track_number,
        int                 threads,
        int                 seek_mode,
        uint32_t            forward_seek_threshold,
        int                 direct_rendering,
        IScriptEnvironment *env
    );
    ~LSMASHVideoSource();
    PVideoFrame __stdcall GetFrame( int n, IScriptEnvironment *env );
    bool __stdcall GetParity( int n ) { return false; }
    void __stdcall GetAudio( void *buf, __int64 start, __int64 count, IScriptEnvironment *env ) {}
    void __stdcall SetCacheHints( int cachehints, int frame_range ) {}
    const VideoInfo& __stdcall GetVideoInfo() { return vi; }
};

class LSMASHAudioSource : public IClip
{
private:
    VideoInfo                         vi;
    libavsmash_audio_decode_handler_t adh;
    libavsmash_audio_output_handler_t aoh;
    AVFormatContext                  *format_ctx;
    uint32_t open_file
    (
        const char         *source,
        IScriptEnvironment *env
    );
    void get_audio_track
    (
        const char         *source,
        uint32_t            track_number,
        bool                skip_priming,
        IScriptEnvironment *env
    );
    void prepare_audio_decoding
    (
        uint64_t            channel_layout,
        int                 sample_rate,
        IScriptEnvironment *env
    );
public:
    LSMASHAudioSource
    (
        const char         *source,
        uint32_t            track_number,
        bool                skip_priming,
        uint64_t            channel_layout,
        int                 sample_rate,
        IScriptEnvironment *env
    );
    ~LSMASHAudioSource();
    PVideoFrame __stdcall GetFrame( int n, IScriptEnvironment *env ) { return NULL; }
    bool __stdcall GetParity( int n ) { return false; }
    void __stdcall GetAudio( void *buf, __int64 start, __int64 wanted_length, IScriptEnvironment *env );
    void __stdcall SetCacheHints( int cachehints, int frame_range ) {}
    const VideoInfo& __stdcall GetVideoInfo() { return vi; }
};
