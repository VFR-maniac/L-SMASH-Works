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

#define NO_PROGRESS_HANDLER

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

#include "../common/resample.h"
#include "../common/progress.h"

#include "video_output.h"
#include "audio_output.h"
#include "lwlibav_source.h"

#pragma warning( disable:4996 )

LWLibavVideoSource::LWLibavVideoSource
(
    lwlibav_option_t   *opt,
    int                 seek_mode,
    uint32_t            forward_seek_threshold,
    IScriptEnvironment *env
)
{
    memset( &vi,  0, sizeof(VideoInfo) );
    memset( &lwh, 0, sizeof(lwlibav_file_handler_t) );
    memset( &vdh, 0, sizeof(video_decode_handler_t) );
    memset( &voh, 0, sizeof(video_output_handler_t) );
    vdh.seek_mode              = seek_mode;
    vdh.forward_seek_threshold = forward_seek_threshold;
    voh.first_valid_frame      = NULL;
    as_video_output_handler_t *as_vohp = (as_video_output_handler_t *)lw_malloc_zero( sizeof(as_video_output_handler_t) );
    if( !as_vohp )
        env->ThrowError( "LWLibavVideoSource: failed to allocate the AviSynth video output handler." );
    voh.private_handler      = as_vohp;
    voh.free_private_handler = free;
    /* Set up error handler. */
    error_handler_t eh = { 0 };
    eh.message_priv  = env;
    eh.error_message = throw_error;
    /* Set up progress indicator. */
    progress_indicator_t indicator;
    indicator.open   = NULL;
    indicator.update = NULL;
    indicator.close  = NULL;
    /* Construct index. */
    audio_decode_handler_t adh = { 0 };
    audio_output_handler_t aoh = { 0 };
    int ret = lwlibav_construct_index( &lwh, &vdh, &adh, &aoh, &eh, opt, &indicator, NULL );
    lwlibav_cleanup_audio_decode_handler( &adh );
    lwlibav_cleanup_audio_output_handler( &aoh );
    if( ret < 0 )
        env->ThrowError( "LWLibavVideoSource: failed to construct index." );
    /* Get the desired video track. */
    if( lwlibav_get_desired_video_track( lwh.file_path, &vdh, lwh.threads ) < 0 )
        env->ThrowError( "LWLibavVideoSource: failed to get the video track." );
    vdh.eh = eh;
    vi.num_frames = vdh.frame_count;
    int fps_num;
    int fps_den;
    lwlibav_setup_timestamp_info( &vdh, &fps_num, &fps_den );
    vi.fps_numerator   = (unsigned int)fps_num;
    vi.fps_denominator = (unsigned int)fps_den;
    prepare_video_decoding( env );
}

LWLibavVideoSource::~LWLibavVideoSource()
{
    if( voh.first_valid_frame )
        delete voh.first_valid_frame;
    if( voh.scaler.sws_ctx )
        sws_freeContext( voh.scaler.sws_ctx );
    if( voh.free_private_handler && voh.private_handler )
        voh.free_private_handler( voh.private_handler );
    lwlibav_cleanup_video_decode_handler( &vdh );
    if( lwh.file_path )
        free( lwh.file_path );
}

void LWLibavVideoSource::prepare_video_decoding( IScriptEnvironment *env )
{
    vdh.eh.message_priv    = env;
    vdh.input_buffer_size += FF_INPUT_BUFFER_PADDING_SIZE;
    vdh.input_buffer       = (uint8_t *)av_mallocz( vdh.input_buffer_size );
    if( !vdh.input_buffer )
        env->ThrowError( "LWLibavVideoSource: failed to allocate memory to the input buffer for video." );
    if( vdh.index_entries )
    {
        AVStream *video_stream = vdh.format->streams[ vdh.stream_index ];
        for( int i = 0; i < vdh.index_entries_count; i++ )
        {
            AVIndexEntry *ie = &vdh.index_entries[i];
            if( av_add_index_entry( video_stream, ie->pos, ie->timestamp, ie->size, ie->min_distance, ie->flags ) < 0 )
                env->ThrowError( "LWLibavVideoSource: failed to import AVIndexEntrys for video." );
        }
        av_freep( &vdh.index_entries );
    }
    /* Set up output format. */
    vdh.ctx->width   = vdh.initial_width;
    vdh.ctx->height  = vdh.initial_height;
    vdh.ctx->pix_fmt = vdh.initial_pix_fmt;
    vi.width  = vdh.max_width;
    vi.height = vdh.max_height;
    enum AVPixelFormat input_pixel_format = vdh.ctx->pix_fmt;
    if( determine_colorspace_conversion( &voh, &vdh.ctx->pix_fmt, &vi.pixel_type ) < 0 )
        env->ThrowError( "LWLibavVideoSource: %s is not supported", av_get_pix_fmt_name( input_pixel_format ) );
    video_scaler_handler_t *vshp = &voh.scaler;
    vshp->enabled = 1;
    vshp->flags   = SWS_FAST_BILINEAR;
    vshp->sws_ctx = sws_getCachedContext( NULL,
                                          vdh.ctx->width, vdh.ctx->height, vdh.ctx->pix_fmt,
                                          vdh.ctx->width, vdh.ctx->height, vshp->output_pixel_format,
                                          vshp->flags, NULL, NULL, NULL );
    if( !vshp->sws_ctx )
        env->ThrowError( "LWLibavVideoSource: failed to get swscale context." );
    /* Find the first valid video sample. */
    vdh.seek_flags = (vdh.seek_base & SEEK_FILE_OFFSET_BASED) ? AVSEEK_FLAG_BYTE : vdh.seek_base == 0 ? AVSEEK_FLAG_FRAME : 0;
    if( vi.num_frames != 1 )
    {
        vdh.seek_flags |= AVSEEK_FLAG_BACKWARD;
        uint32_t rap_number;
        lwlibav_find_random_accessible_point( &vdh, 1, 0, &rap_number );
        int64_t rap_pos = lwlibav_get_random_accessible_point_position( &vdh, rap_number );
        if( av_seek_frame( vdh.format, vdh.stream_index, rap_pos, vdh.seek_flags ) < 0 )
            av_seek_frame( vdh.format, vdh.stream_index, rap_pos, vdh.seek_flags | AVSEEK_FLAG_ANY );
    }
    for( uint32_t i = 1; i <= vi.num_frames + get_decoder_delay( vdh.ctx ); i++ )
    {
        AVPacket pkt = { 0 };
        get_av_frame( vdh.format, vdh.stream_index, &vdh.input_buffer, &vdh.input_buffer_size, &pkt );
        AVFrame *picture = vdh.frame_buffer;
        avcodec_get_frame_defaults( picture );
        int got_picture;
        if( avcodec_decode_video2( vdh.ctx, picture, &got_picture, &pkt ) >= 0 && got_picture )
        {
            voh.first_valid_frame_number = i - MIN( get_decoder_delay( vdh.ctx ), vdh.delay_count );
            if( voh.first_valid_frame_number > 1 || vi.num_frames == 1 )
            {
                PVideoFrame temp = env->NewVideoFrame( vi );
                if( !temp )
                    env->ThrowError( "LWLibavVideoSource: failed to allocate memory for the first valid video frame data." );
                if( make_frame( &voh, picture, temp, env ) )
                    continue;
                voh.first_valid_frame = new PVideoFrame( temp );
                if( !voh.first_valid_frame )
                    env->ThrowError( "LWLibavVideoSource: failed to allocate the first valid frame." );
            }
            break;
        }
        else if( pkt.data )
            ++ vdh.delay_count;
    }
    vdh.last_frame_number = vi.num_frames + 1;  /* Force seeking at the first reading. */
}

PVideoFrame __stdcall LWLibavVideoSource::GetFrame( int n, IScriptEnvironment *env )
{
    uint32_t frame_number = n + 1;     /* frame_number is 1-origin. */
    if( frame_number < voh.first_valid_frame_number || vi.num_frames == 1 )
    {
        /* Copy the first valid video frame. */
        vdh.last_frame_number = vi.num_frames + 1;  /* Force seeking at the next access for valid video sample. */
        return *(PVideoFrame *)voh.first_valid_frame;
    }
    vdh.eh.message_priv = env;
    PVideoFrame frame = env->NewVideoFrame( vi );
    if( vdh.eh.error )
        return frame;
    if( lwlibav_get_video_frame( &vdh, frame_number, vi.num_frames ) )
        return frame;
    if( make_frame( &voh, vdh.frame_buffer, frame, env ) )
        env->ThrowError( "LWLibavVideoSource: failed to make a frame." );
    return frame;
}

LWLibavAudioSource::LWLibavAudioSource
(
    lwlibav_option_t   *opt,
    IScriptEnvironment *env
)
{
    memset( &vi,  0, sizeof(VideoInfo) );
    memset( &lwh, 0, sizeof(lwlibav_file_handler_t) );
    memset( &adh, 0, sizeof(audio_decode_handler_t) );
    memset( &aoh, 0, sizeof(audio_output_handler_t) );
    /* Set up error handler. */
    error_handler_t eh = { 0 };
    eh.message_priv  = env;
    eh.error_message = throw_error;
    /* Set up progress indicator. */
    progress_indicator_t indicator;
    indicator.open   = NULL;
    indicator.update = NULL;
    indicator.close  = NULL;
    /* Construct index. */
    video_decode_handler_t vdh = { 0 };
    if( lwlibav_construct_index( &lwh, &vdh, &adh, &aoh, &eh, opt, &indicator, NULL ) < 0 )
        env->ThrowError( "LWLibavAudioSource: failed to get construct index." );
    lwlibav_cleanup_video_decode_handler( &vdh );
    /* Get the desired video track. */
    if( lwlibav_get_desired_audio_track( lwh.file_path, &adh, lwh.threads ) < 0 )
        env->ThrowError( "LWLibavAudioSource: failed to get the audio track." );
    adh.eh = eh;
    prepare_audio_decoding( env );
}

LWLibavAudioSource::~LWLibavAudioSource()
{
    lwlibav_cleanup_audio_decode_handler( &adh );
    lwlibav_cleanup_audio_output_handler( &aoh );
    if( lwh.file_path )
        free( lwh.file_path );
}

void LWLibavAudioSource::prepare_audio_decoding( IScriptEnvironment *env )
{
    adh.eh.message_priv    = env;
    adh.input_buffer_size += FF_INPUT_BUFFER_PADDING_SIZE;
    adh.input_buffer       = (uint8_t *)av_mallocz( adh.input_buffer_size );
    if( !adh.input_buffer )
        env->ThrowError( "LWLibavAudioSource: failed to allocate memory to the input buffer for audio." );
    /* */
    vi.num_audio_samples = lwlibav_count_overall_pcm_samples( &adh, aoh.output_sample_rate );
    if( vi.num_audio_samples == 0 )
        env->ThrowError( "LWLibavAudioSource: no valid audio frame." );
    adh.next_pcm_sample_number = vi.num_audio_samples + 1;  /* Force seeking at the first reading. */
    if( adh.index_entries )
    {
        AVStream *audio_stream = adh.format->streams[ adh.stream_index ];
        for( int i = 0; i < adh.index_entries_count; i++ )
        {
            AVIndexEntry *ie = &adh.index_entries[i];
            if( av_add_index_entry( audio_stream, ie->pos, ie->timestamp, ie->size, ie->min_distance, ie->flags ) < 0 )
                env->ThrowError( "LWLibavVideoSource: failed to import AVIndexEntrys for audio." );
        }
        av_freep( &adh.index_entries );
    }
    avcodec_get_frame_defaults( adh.frame_buffer );
    /* Set up resampler. */
    aoh.avr_ctx = avresample_alloc_context();
    if( !aoh.avr_ctx )
        env->ThrowError( "LWLibavAudioSource: failed to avresample_alloc_context." );
    if( adh.ctx->channel_layout == 0 )
        adh.ctx->channel_layout = av_get_default_channel_layout( adh.ctx->channels );
    aoh.output_sample_format = decide_audio_output_sample_format( aoh.output_sample_format );
    av_opt_set_int( aoh.avr_ctx, "in_channel_layout",   adh.ctx->channel_layout,   0 );
    av_opt_set_int( aoh.avr_ctx, "in_sample_fmt",       adh.ctx->sample_fmt,       0 );
    av_opt_set_int( aoh.avr_ctx, "in_sample_rate",      adh.ctx->sample_rate,      0 );
    av_opt_set_int( aoh.avr_ctx, "out_channel_layout",  aoh.output_channel_layout, 0 );
    av_opt_set_int( aoh.avr_ctx, "out_sample_fmt",      aoh.output_sample_format,  0 );
    av_opt_set_int( aoh.avr_ctx, "out_sample_rate",     aoh.output_sample_rate,    0 );
    av_opt_set_int( aoh.avr_ctx, "internal_sample_fmt", AV_SAMPLE_FMT_FLTP,        0 );
    if( avresample_open( aoh.avr_ctx ) < 0 )
        env->ThrowError( "LWLibavAudioSource: failed to open resampler." );
    /* Decide output Bits Per Sample. */
    int output_channels = av_get_channel_layout_nb_channels( aoh.output_channel_layout );
    if( aoh.output_sample_format == AV_SAMPLE_FMT_S32
     && (aoh.output_bits_per_sample == 0 || aoh.output_bits_per_sample == 24) )
    {
        /* 24bit signed integer output */
        if( adh.ctx->frame_size )
        {
            aoh.resampled_buffer_size = get_linesize( output_channels, adh.ctx->frame_size, aoh.output_sample_format );
            aoh.resampled_buffer      = (uint8_t *)av_malloc( aoh.resampled_buffer_size );
            if( !aoh.resampled_buffer )
                env->ThrowError( "LWLibavAudioSource: failed to allocate memory for resampling." );
        }
        aoh.s24_output             = 1;
        aoh.output_bits_per_sample = 24;
    }
    else
        aoh.output_bits_per_sample = av_get_bytes_per_sample( aoh.output_sample_format ) * 8;
    /* */
    vi.nchannels                = output_channels;
    vi.audio_samples_per_second = aoh.output_sample_rate;
    switch ( aoh.output_sample_format )
    {
        case AV_SAMPLE_FMT_U8 :
        case AV_SAMPLE_FMT_U8P :
            vi.sample_type = SAMPLE_INT8;
            break;
        case AV_SAMPLE_FMT_S16 :
        case AV_SAMPLE_FMT_S16P :
            vi.sample_type = SAMPLE_INT16;
            break;
        case AV_SAMPLE_FMT_S32 :
        case AV_SAMPLE_FMT_S32P :
            vi.sample_type = aoh.s24_output ? SAMPLE_INT24 : SAMPLE_INT32;
            break;
        case AV_SAMPLE_FMT_FLT :
        case AV_SAMPLE_FMT_FLTP :
            vi.sample_type = SAMPLE_FLOAT;
            break;
        default :
            env->ThrowError( "LWLibavAudioSource: %s is not supported.", av_get_sample_fmt_name( adh.ctx->sample_fmt ) );
    }
    /* Set up the number of planes and the block alignment of decoded and output data. */
    int input_channels = av_get_channel_layout_nb_channels( adh.ctx->channel_layout );
    if( av_sample_fmt_is_planar( adh.ctx->sample_fmt ) )
    {
        aoh.input_planes      = input_channels;
        aoh.input_block_align = av_get_bytes_per_sample( adh.ctx->sample_fmt );
    }
    else
    {
        aoh.input_planes      = 1;
        aoh.input_block_align = av_get_bytes_per_sample( adh.ctx->sample_fmt ) * input_channels;
    }
    aoh.output_block_align = (output_channels * aoh.output_bits_per_sample) / 8;
}

void __stdcall LWLibavAudioSource::GetAudio( void *buf, __int64 start, __int64 wanted_length, IScriptEnvironment *env )
{
    return (void)lwlibav_get_pcm_audio_samples( &adh, &aoh, buf, start, wanted_length );
}

AVSValue __cdecl CreateLWLibavVideoSource( AVSValue args, void *user_data, IScriptEnvironment *env )
{
    const char *source                 = args[0].AsString();
    int         stream_index           = args[1].AsInt( -1 );
    int         threads                = args[2].AsInt( 0 );
    int         no_create_index        = args[3].AsBool( false ) ? 0 : 1;
    int         seek_mode              = args[4].AsInt( 0 );
    uint32_t    forward_seek_threshold = args[5].AsInt( 10 );
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
    seek_mode              = CLIP_VALUE( seek_mode, 0, 2 );
    forward_seek_threshold = CLIP_VALUE( forward_seek_threshold, 1, 999 );
    return new LWLibavVideoSource( &opt, seek_mode, forward_seek_threshold, env );
}

AVSValue __cdecl CreateLWLibavAudioSource( AVSValue args, void *user_data, IScriptEnvironment *env )
{
    const char *source          = args[0].AsString();
    int         stream_index    = args[1].AsInt( -1 );
    int         no_create_index = args[2].AsBool( false ) ? 0 : 1;
    /* Set LW-Libav options. */
    lwlibav_option_t opt;
    opt.file_path         = source;
    opt.threads           = 0;
    opt.av_sync           = 0;
    opt.no_create_index   = no_create_index;
    opt.force_video       = 0;
    opt.force_video_index = -1;
    opt.force_audio       = (stream_index >= 0);
    opt.force_audio_index = stream_index >= 0 ? stream_index : -1;
    return new LWLibavAudioSource( &opt, env );
}
