/*****************************************************************************
 * lwlibav_source.cpp
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

#include "lsmashsource.h"

extern "C"
{
/* Libav
 * The binary file will be LGPLed or GPLed. */
#include <libavformat/avformat.h>       /* Codec specific info importer */
#include <libavcodec/avcodec.h>         /* Decoder */
#include <libswscale/swscale.h>         /* Colorspace converter */
#include <libavresample/avresample.h>   /* Audio resampler */
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
}

#include "video_output.h"
#include "audio_output.h"
#include "lwlibav_source.h"

#pragma warning( disable:4996 )

LWLibavVideoSource::LWLibavVideoSource
(
    lwlibav_option_t   *opt,
    int                 seek_mode,
    uint32_t            forward_seek_threshold,
    int                 direct_rendering,
    int                 stacked_format,
    enum AVPixelFormat  pixel_format,
    IScriptEnvironment *env
)
{
    memset( &vi,  0, sizeof(VideoInfo) );
    memset( &lwh, 0, sizeof(lwlibav_file_handler_t) );
    memset( &vdh, 0, sizeof(lwlibav_video_decode_handler_t) );
    memset( &voh, 0, sizeof(lwlibav_video_output_handler_t) );
    vdh.seek_mode              = seek_mode;
    vdh.forward_seek_threshold = forward_seek_threshold;
    as_video_output_handler_t *as_vohp = (as_video_output_handler_t *)lw_malloc_zero( sizeof(as_video_output_handler_t) );
    if( !as_vohp )
        env->ThrowError( "LWLibavVideoSource: failed to allocate the AviSynth video output handler." );
    as_vohp->vi  = &vi;
    as_vohp->env = env;
    voh.private_handler      = as_vohp;
    voh.free_private_handler = as_free_video_output_handler;
    /* Set up error handler. */
    lw_log_handler_t lh;
    lh.level    = LW_LOG_FATAL; /* Ignore other than fatal error. */
    lh.priv     = env;
    lh.show_log = throw_error;
    /* Set up progress indicator. */
    progress_indicator_t indicator;
    indicator.open   = NULL;
    indicator.update = NULL;
    indicator.close  = NULL;
    /* Construct index. */
    lwlibav_audio_decode_handler_t adh = { 0 };
    lwlibav_audio_output_handler_t aoh = { 0 };
    int ret = lwlibav_construct_index( &lwh, &vdh, &voh, &adh, &aoh, &lh, opt, &indicator, NULL );
    lwlibav_cleanup_audio_decode_handler( &adh );
    lwlibav_cleanup_audio_output_handler( &aoh );
    if( ret < 0 )
        env->ThrowError( "LWLibavVideoSource: failed to construct index." );
    /* Get the desired video track. */
    if( lwlibav_get_desired_video_track( lwh.file_path, &vdh, lwh.threads ) < 0 )
        env->ThrowError( "LWLibavVideoSource: failed to get the video track." );
    vdh.lh = lh;
    vi.num_frames = voh.frame_count;
    /* Set average framerate. */
    int64_t fps_num = 25;
    int64_t fps_den = 1;
    lwlibav_setup_timestamp_info( &lwh, &vdh, &voh, &fps_num, &fps_den );
    vi.fps_numerator   = (unsigned int)fps_num;
    vi.fps_denominator = (unsigned int)fps_den;
    /* */
    prepare_video_decoding( direct_rendering, stacked_format, pixel_format, env );
}

LWLibavVideoSource::~LWLibavVideoSource()
{
    lwlibav_cleanup_video_decode_handler( &vdh );
    lwlibav_cleanup_video_output_handler( &voh );
    if( lwh.file_path )
        free( lwh.file_path );
}

void LWLibavVideoSource::prepare_video_decoding
(
    int                 direct_rendering,
    int                 stacked_format,
    enum AVPixelFormat  pixel_format,
    IScriptEnvironment *env
)
{
    vdh.lh.priv = env;
    /* Import AVIndexEntrys. */
    if( lwlibav_import_av_index_entry( (lwlibav_decode_handler_t *)&vdh ) < 0 )
        env->ThrowError( "LWLibavVideoSource: failed to import AVIndexEntrys for video." );
    /* Set up output format. */
    vdh.ctx->width      = vdh.initial_width;
    vdh.ctx->height     = vdh.initial_height;
    vdh.ctx->pix_fmt    = vdh.initial_pix_fmt;
    vdh.ctx->colorspace = vdh.initial_colorspace;
    vdh.exh.get_buffer = as_setup_video_rendering( &voh, vdh.ctx, "LWLibavVideoSource",
                                                   direct_rendering, stacked_format, pixel_format,
                                                   vdh.max_width, vdh.max_height );
    /* Find the first valid video sample. */
    if( lwlibav_find_first_valid_video_frame( &vdh ) < 0 )
        env->ThrowError( "LWLibavVideoSource: failed to find the first valid video frame." );
    /* Force seeking at the first reading. */
    vdh.last_frame_number = vi.num_frames + 1;
}

PVideoFrame __stdcall LWLibavVideoSource::GetFrame( int n, IScriptEnvironment *env )
{
    uint32_t frame_number = n + 1;     /* frame_number is 1-origin. */
    vdh.lh.priv = env;
    if( vdh.error )
        return env->NewVideoFrame( vi );
    if( lwlibav_get_video_frame( &vdh, &voh, frame_number ) < 0 )
        return env->NewVideoFrame( vi );
    PVideoFrame as_frame;
    if( make_frame( &voh, vdh.ctx, vdh.frame_buffer, as_frame, env ) < 0 )
        env->ThrowError( "LWLibavVideoSource: failed to make a frame." );
    return as_frame;
}

bool __stdcall LWLibavVideoSource::GetParity( int n )
{
    uint32_t frame_number = n + 1;     /* frame_number is 1-origin. */
    if( !voh.repeat_control )
        return vdh.frame_list[frame_number].field_info == LW_FIELD_INFO_TOP ? true : false;
    uint32_t t = voh.frame_order_list[frame_number].top;
    uint32_t b = voh.frame_order_list[frame_number].bottom;
    return t < b ? true : false;
}

LWLibavAudioSource::LWLibavAudioSource
(
    lwlibav_option_t   *opt,
    uint64_t            channel_layout,
    int                 sample_rate,
    IScriptEnvironment *env
)
{
    memset( &vi,  0, sizeof(VideoInfo) );
    memset( &lwh, 0, sizeof(lwlibav_file_handler_t) );
    memset( &adh, 0, sizeof(lwlibav_audio_decode_handler_t) );
    memset( &aoh, 0, sizeof(lwlibav_audio_output_handler_t) );
    /* Set up error handler. */
    lw_log_handler_t lh;
    lh.level    = LW_LOG_FATAL; /* Ignore other than fatal error. */
    lh.priv     = env;
    lh.show_log = throw_error;
    /* Set up progress indicator. */
    progress_indicator_t indicator;
    indicator.open   = NULL;
    indicator.update = NULL;
    indicator.close  = NULL;
    /* Construct index. */
    lwlibav_video_decode_handler_t vdh = { 0 };
    lwlibav_video_output_handler_t voh = { 0 };
    if( lwlibav_construct_index( &lwh, &vdh, &voh, &adh, &aoh, &lh, opt, &indicator, NULL ) < 0 )
        env->ThrowError( "LWLibavAudioSource: failed to get construct index." );
    lwlibav_cleanup_video_decode_handler( &vdh );
    lwlibav_cleanup_video_output_handler( &voh );
    /* Get the desired video track. */
    if( lwlibav_get_desired_audio_track( lwh.file_path, &adh, lwh.threads ) < 0 )
        env->ThrowError( "LWLibavAudioSource: failed to get the audio track." );
    adh.lh = lh;
    prepare_audio_decoding( channel_layout, sample_rate, env );
}

LWLibavAudioSource::~LWLibavAudioSource()
{
    lwlibav_cleanup_audio_decode_handler( &adh );
    lwlibav_cleanup_audio_output_handler( &aoh );
    if( lwh.file_path )
        free( lwh.file_path );
}

void LWLibavAudioSource::prepare_audio_decoding
(
    uint64_t            channel_layout,
    int                 sample_rate,
    IScriptEnvironment *env
)
{
    adh.lh.priv = env;
    /* Import AVIndexEntrys. */
    if( lwlibav_import_av_index_entry( (lwlibav_decode_handler_t *)&adh ) < 0 )
        env->ThrowError( "LWLibavAudioSource: failed to import AVIndexEntrys for audio." );
    /* */
    avcodec_get_frame_defaults( adh.frame_buffer );
    as_setup_audio_rendering( &aoh, adh.ctx, &vi, env, "LWLibavAudioSource", channel_layout, sample_rate );
    /* Count the number of PCM audio samples. */
    vi.num_audio_samples = lwlibav_count_overall_pcm_samples( &adh, aoh.output_sample_rate );
    if( vi.num_audio_samples == 0 )
        env->ThrowError( "LWLibavAudioSource: no valid audio frame." );
    if( lwh.av_gap && aoh.output_sample_rate != adh.ctx->sample_rate )
        lwh.av_gap = ((int64_t)lwh.av_gap * aoh.output_sample_rate - 1) / adh.ctx->sample_rate + 1;
    vi.num_audio_samples += lwh.av_gap;
    /* Force seeking at the first reading. */
    adh.next_pcm_sample_number = vi.num_audio_samples + 1;
}

int LWLibavAudioSource::delay_audio( int64_t *start, int64_t wanted_length )
{
    /* Even if start become negative, its absolute value shall be equal to wanted_length or smaller. */
    int64_t end         = *start + wanted_length;
    int64_t audio_delay = lwh.av_gap;
    if( *start < audio_delay && end <= audio_delay )
    {
        adh.next_pcm_sample_number = vi.num_audio_samples + 1;  /* Force seeking at the next access for valid audio frame. */
        return 0;
    }
    *start -= audio_delay;
    return 1;
}

void __stdcall LWLibavAudioSource::GetAudio( void *buf, __int64 start, __int64 wanted_length, IScriptEnvironment *env )
{
    adh.lh.priv = env;
    if( delay_audio( &start, wanted_length ) )
        return (void)lwlibav_get_pcm_audio_samples( &adh, &aoh, buf, start, wanted_length );
    uint8_t silence = vi.sample_type == SAMPLE_INT8 ? 128 : 0;
    memset( buf, silence, (size_t)(wanted_length * aoh.output_block_align) );
}

AVSValue __cdecl CreateLWLibavVideoSource( AVSValue args, void *user_data, IScriptEnvironment *env )
{
#ifdef NDEBUG
    av_log_set_level( AV_LOG_QUIET );
#endif
    const char *source                 = args[0].AsString();
    int         stream_index           = args[1].AsInt( -1 );
    int         threads                = args[2].AsInt( 0 );
    int         no_create_index        = args[3].AsBool( true ) ? 0 : 1;
    int         seek_mode              = args[4].AsInt( 0 );
    uint32_t    forward_seek_threshold = args[5].AsInt( 10 );
    int         direct_rendering       = args[6].AsBool( false ) ? 1 : 0;
    int         apply_repeat_flag      = args[7].AsBool( false ) ? 1 : 0;
    int         field_dominance        = args[8].AsInt( 0 );
    int         stacked_format         = args[9].AsBool( false ) ? 1 : 0;
    enum AVPixelFormat pixel_format    = get_av_output_pixel_format( args[10].AsString( NULL ) );
    /* Set LW-Libav options. */
    lwlibav_option_t opt;
    opt.file_path         = source;
    opt.threads           = threads >= 0 ? threads : 0;
    opt.av_sync           = 0;
    opt.no_create_index   = no_create_index;
    opt.force_video       = (stream_index >= 0);
    opt.force_video_index = stream_index >= 0 ? stream_index : -1;
    opt.force_audio       = 0;
    opt.force_audio_index = -1;
    opt.apply_repeat_flag = apply_repeat_flag;
    opt.field_dominance   = CLIP_VALUE( field_dominance, 0, 2 );    /* 0: Obey source flags, 1: TFF, 2: BFF */
    seek_mode              = CLIP_VALUE( seek_mode, 0, 2 );
    forward_seek_threshold = CLIP_VALUE( forward_seek_threshold, 1, 999 );
    direct_rendering      &= (pixel_format == AV_PIX_FMT_NONE);
    return new LWLibavVideoSource( &opt, seek_mode, forward_seek_threshold, direct_rendering, stacked_format, pixel_format, env );
}

AVSValue __cdecl CreateLWLibavAudioSource( AVSValue args, void *user_data, IScriptEnvironment *env )
{
#ifdef NDEBUG
    av_log_set_level( AV_LOG_QUIET );
#endif
    const char *source          = args[0].AsString();
    int         stream_index    = args[1].AsInt( -1 );
    int         no_create_index = args[2].AsBool( true  ) ? 0 : 1;
    int         av_sync         = args[3].AsBool( false ) ? 1 : 0;
    const char *layout_string   = args[4].AsString( NULL );
    uint32_t    sample_rate     = args[5].AsInt( 0 );
    /* Set LW-Libav options. */
    lwlibav_option_t opt;
    opt.file_path         = source;
    opt.threads           = 0;
    opt.av_sync           = av_sync;
    opt.no_create_index   = no_create_index;
    opt.force_video       = 0;
    opt.force_video_index = -1;
    opt.force_audio       = (stream_index >= 0);
    opt.force_audio_index = stream_index >= 0 ? stream_index : -1;
    opt.apply_repeat_flag = 0;
    opt.field_dominance   = 0;
    uint64_t channel_layout = layout_string ? av_get_channel_layout( layout_string ) : 0;
    return new LWLibavAudioSource( &opt, channel_layout, sample_rate, env );
}
