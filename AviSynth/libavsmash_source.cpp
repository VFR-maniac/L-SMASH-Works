/*****************************************************************************
 * libavsmash_source.cpp
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

#include "lsmashsource.h"

extern "C"
{
/* L-SMASH */
#define LSMASH_DEMUXER_ENABLED
#include <lsmash.h>                 /* Demuxer */

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

#include "../common/audio_output.h"
#include "../common/libavsmash.h"
#include "../common/libavsmash_video.h"
#include "../common/libavsmash_audio.h"

#include "video_output.h"
#include "audio_output.h"
#include "libavsmash_source.h"

LSMASHVideoSource::LSMASHVideoSource( const char *source, uint32_t track_number, int threads, int seek_mode, uint32_t forward_seek_threshold, IScriptEnvironment *env )
{
    memset( &vi,  0, sizeof(VideoInfo) );
    memset( &vdh, 0, sizeof(libavsmash_video_decode_handler_t) );
    memset( &voh, 0, sizeof(libavsmash_video_output_handler_t) );
    format_ctx                 = NULL;
    vdh.seek_mode              = seek_mode;
    vdh.forward_seek_threshold = forward_seek_threshold;
    voh.first_valid_frame      = NULL;
    as_video_output_handler_t *as_vohp = (as_video_output_handler_t *)lw_malloc_zero( sizeof(as_video_output_handler_t) );
    if( !as_vohp )
        env->ThrowError( "LSMASHVideoSource: failed to allocate the AviSynth video output handler." );
    as_vohp->vi  = &vi;
    as_vohp->env = env;
    voh.private_handler      = as_vohp;
    voh.free_private_handler = free;
    get_video_track( source, track_number, threads, env );
    lsmash_discard_boxes( vdh.root );
    prepare_video_decoding( env );
}

LSMASHVideoSource::~LSMASHVideoSource()
{
    if( voh.first_valid_frame )
        delete voh.first_valid_frame;
    if( vdh.order_converter )
        delete [] vdh.order_converter;
    if( vdh.frame_buffer )
        avcodec_free_frame( &vdh.frame_buffer );
    if( voh.scaler.sws_ctx )
        sws_freeContext( voh.scaler.sws_ctx );
    if( voh.free_private_handler && voh.private_handler )
        voh.free_private_handler( voh.private_handler );
    cleanup_configuration( &vdh.config );
    if( format_ctx )
        avformat_close_input( &format_ctx );
    lsmash_destroy_root( vdh.root );
}

uint32_t LSMASHVideoSource::open_file( const char *source, IScriptEnvironment *env )
{
    /* L-SMASH */
    vdh.root = lsmash_open_movie( source, LSMASH_FILE_MODE_READ );
    if( !vdh.root )
        env->ThrowError( "LSMASHVideoSource: failed to lsmash_open_movie." );
    lsmash_movie_parameters_t movie_param;
    lsmash_initialize_movie_parameters( &movie_param );
    lsmash_get_movie_parameters( vdh.root, &movie_param );
    if( movie_param.number_of_tracks == 0 )
        env->ThrowError( "LSMASHVideoSource: the number of tracks equals 0." );
    /* libavformat */
    av_register_all();
    avcodec_register_all();
    if( avformat_open_input( &format_ctx, source, NULL, NULL ) )
        env->ThrowError( "LSMASHVideoSource: failed to avformat_open_input." );
    if( avformat_find_stream_info( format_ctx, NULL ) < 0 )
        env->ThrowError( "LSMASHVideoSource: failed to avformat_find_stream_info." );
    /* */
    vdh.config.error_message = throw_error;
    return movie_param.number_of_tracks;
}

static void setup_timestamp_info( libavsmash_video_decode_handler_t *hp, VideoInfo *vi, uint64_t media_timescale, IScriptEnvironment *env )
{
    if( vi->num_frames == 1 )
    {
        /* Calculate average framerate. */
        uint64_t media_duration = lsmash_get_media_duration_from_media_timeline( hp->root, hp->track_ID );
        if( media_duration == 0 )
            media_duration = INT32_MAX;
        reduce_fraction( &media_timescale, &media_duration );
        vi->fps_numerator   = (unsigned int)media_timescale;
        vi->fps_denominator = (unsigned int)media_duration;
        return;
    }
    lsmash_media_ts_list_t ts_list;
    if( lsmash_get_media_timestamps( hp->root, hp->track_ID, &ts_list ) )
        env->ThrowError( "LSMASHVideoSource: failed to get timestamps." );
    if( ts_list.sample_count != vi->num_frames )
        env->ThrowError( "LSMASHVideoSource: failed to count number of video samples." );
    uint32_t composition_sample_delay;
    if( lsmash_get_max_sample_delay( &ts_list, &composition_sample_delay ) )
    {
        lsmash_delete_media_timestamps( &ts_list );
        env->ThrowError( "LSMASHVideoSource: failed to get composition delay." );
    }
    if( composition_sample_delay )
    {
        /* Consider composition order for keyframe detection.
         * Note: sample number for L-SMASH is 1-origin. */
        hp->order_converter = new order_converter_t[ts_list.sample_count + 1];
        if( !hp->order_converter )
        {
            lsmash_delete_media_timestamps( &ts_list );
            env->ThrowError( "LSMASHVideoSource: failed to allocate memory." );
        }
        for( uint32_t i = 0; i < ts_list.sample_count; i++ )
            ts_list.timestamp[i].dts = i + 1;
        lsmash_sort_timestamps_composition_order( &ts_list );
        for( uint32_t i = 0; i < ts_list.sample_count; i++ )
            hp->order_converter[i + 1].composition_to_decoding = (uint32_t)ts_list.timestamp[i].dts;
    }
    /* Calculate average framerate. */
    uint64_t largest_cts          = ts_list.timestamp[1].cts;
    uint64_t second_largest_cts   = ts_list.timestamp[0].cts;
    uint64_t composition_timebase = ts_list.timestamp[1].cts - ts_list.timestamp[0].cts;
    for( uint32_t i = 2; i < ts_list.sample_count; i++ )
    {
        if( ts_list.timestamp[i].cts == ts_list.timestamp[i - 1].cts )
        {
            lsmash_delete_media_timestamps( &ts_list );
            return;
        }
        composition_timebase = get_gcd( composition_timebase, ts_list.timestamp[i].cts - ts_list.timestamp[i - 1].cts );
        second_largest_cts = largest_cts;
        largest_cts = ts_list.timestamp[i].cts;
    }
    uint64_t reduce = reduce_fraction( &media_timescale, &composition_timebase );
    uint64_t composition_duration = ((largest_cts - ts_list.timestamp[0].cts) + (largest_cts - second_largest_cts)) / reduce;
    lsmash_delete_media_timestamps( &ts_list );
    vi->fps_numerator   = (unsigned int)((vi->num_frames * ((double)media_timescale / composition_duration)) * composition_timebase + 0.5);
    vi->fps_denominator = (unsigned int)composition_timebase;
}

void LSMASHVideoSource::get_video_track( const char *source, uint32_t track_number, int threads, IScriptEnvironment *env )
{
    uint32_t number_of_tracks = open_file( source, env );
    if( track_number && track_number > number_of_tracks )
        env->ThrowError( "LSMASHVideoSource: the number of tracks equals %I32u.", number_of_tracks );
    /* L-SMASH */
    uint32_t i;
    lsmash_media_parameters_t media_param;
    if( track_number == 0 )
    {
        /* Get the first video track. */
        for( i = 1; i <= number_of_tracks; i++ )
        {
            vdh.track_ID = lsmash_get_track_ID( vdh.root, i );
            if( vdh.track_ID == 0 )
                env->ThrowError( "LSMASHVideoSource: failed to find video track." );
            lsmash_initialize_media_parameters( &media_param );
            if( lsmash_get_media_parameters( vdh.root, vdh.track_ID, &media_param ) )
                env->ThrowError( "LSMASHVideoSource: failed to get media parameters." );
            if( media_param.handler_type == ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK )
                break;
        }
        if( i > number_of_tracks )
            env->ThrowError( "LSMASHVideoSource: failed to find video track." );
    }
    else
    {
        /* Get the desired video track. */
        vdh.track_ID = lsmash_get_track_ID( vdh.root, track_number );
        if( vdh.track_ID == 0 )
            env->ThrowError( "LSMASHVideoSource: failed to find video track." );
        lsmash_initialize_media_parameters( &media_param );
        if( lsmash_get_media_parameters( vdh.root, vdh.track_ID, &media_param ) )
            env->ThrowError( "LSMASHVideoSource: failed to get media parameters." );
        if( media_param.handler_type != ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK )
            env->ThrowError( "LSMASHVideoSource: the track you specified is not a video track." );
    }
    if( lsmash_construct_timeline( vdh.root, vdh.track_ID ) )
        env->ThrowError( "LSMASHVideoSource: failed to get construct timeline." );
    if( get_summaries( vdh.root, vdh.track_ID, &vdh.config ) )
        env->ThrowError( "LSMASHVideoSource: failed to get summaries." );
    vi.num_frames = lsmash_get_sample_count_in_media_timeline( vdh.root, vdh.track_ID );
    setup_timestamp_info( &vdh, &vi, media_param.timescale, env );
    /* libavformat */
    for( i = 0; i < format_ctx->nb_streams && format_ctx->streams[i]->codec->codec_type != AVMEDIA_TYPE_VIDEO; i++ );
    if( i == format_ctx->nb_streams )
        env->ThrowError( "LSMASHVideoSource: failed to find stream by libavformat." );
    /* libavcodec */
    AVStream *stream = format_ctx->streams[i];
    AVCodecContext *ctx = stream->codec;
    vdh.config.ctx = ctx;
    AVCodec *codec = avcodec_find_decoder( ctx->codec_id );
    if( !codec )
        env->ThrowError( "LSMASHVideoSource: failed to find %s decoder.", codec->name );
    ctx->thread_count = threads;
    if( avcodec_open2( ctx, codec, NULL ) < 0 )
        env->ThrowError( "LSMASHVideoSource: failed to avcodec_open2." );
}

void LSMASHVideoSource::prepare_video_decoding( IScriptEnvironment *env )
{
    vdh.frame_buffer = avcodec_alloc_frame();
    if( !vdh.frame_buffer )
        env->ThrowError( "LSMASHVideoSource: failed to allocate video frame buffer." );
    /* Initialize the video decoder configuration. */
    codec_configuration_t *config = &vdh.config;
    config->message_priv = env;
    if( initialize_decoder_configuration( vdh.root, vdh.track_ID, config ) )
        env->ThrowError( "LSMASHVideoSource: failed to initialize the decoder configuration." );
    /* Set up output format. */
    if( determine_colorspace_conversion( &voh, config->ctx->pix_fmt, &vi.pixel_type ) < 0 )
        env->ThrowError( "LSMASHVideoSource: %s is not supported", av_get_pix_fmt_name( config->ctx->pix_fmt ) );
    vi.width  = config->prefer.width;
    vi.height = config->prefer.height;
    voh.output_width  = vi.width;
    voh.output_height = vi.height;
    if( initialize_scaler_handler( &voh.scaler, config->ctx, 1, SWS_FAST_BILINEAR, voh.scaler.output_pixel_format ) < 0 )
        env->ThrowError( "LSMASHVideoSource: failed to initialize scaler handler." );
    /* Find the first valid video sample. */
    for( uint32_t i = 1; i <= vi.num_frames + get_decoder_delay( config->ctx ); i++ )
    {
        AVPacket pkt = { 0 };
        get_sample( vdh.root, vdh.track_ID, i, &vdh.config, &pkt );
        AVFrame *picture = vdh.frame_buffer;
        avcodec_get_frame_defaults( picture );
        int got_picture;
        if( avcodec_decode_video2( config->ctx, picture, &got_picture, &pkt ) >= 0 && got_picture )
        {
            voh.first_valid_frame_number = i - MIN( get_decoder_delay( config->ctx ), config->delay_count );
            if( voh.first_valid_frame_number > 1 || vi.num_frames == 1 )
            {
                PVideoFrame temp = env->NewVideoFrame( vi );
                if( !temp )
                    env->ThrowError( "LSMASHVideoSource: failed to allocate memory for the first valid video frame data." );
                if( make_frame( &voh, picture, temp, config->ctx->colorspace, env ) < 0 )
                    continue;
                voh.first_valid_frame = new PVideoFrame( temp );
                if( !voh.first_valid_frame )
                    env->ThrowError( "LSMASHVideoSource: failed to allocate the first valid frame." );
            }
            break;
        }
        else if( pkt.data )
            ++ config->delay_count;
    }
    vdh.last_sample_number = vi.num_frames + 1;  /* Force seeking at the first reading. */
}

PVideoFrame __stdcall LSMASHVideoSource::GetFrame( int n, IScriptEnvironment *env )
{
    uint32_t sample_number = n + 1;     /* For L-SMASH, sample_number is 1-origin. */
    if( sample_number < voh.first_valid_frame_number || vi.num_frames == 1 )
    {
        /* Copy the first valid video frame. */
        vdh.last_sample_number = vi.num_frames + 1;     /* Force seeking at the next access for valid video sample. */
        return *(PVideoFrame *)voh.first_valid_frame;
    }
    codec_configuration_t *config = &vdh.config;
    config->message_priv = env;
    if( config->error )
        return env->NewVideoFrame( vi );
    if( libavsmash_get_video_frame( &vdh, sample_number, vi.num_frames ) )
        return env->NewVideoFrame( vi );
    PVideoFrame as_frame;
    if( make_frame( &voh, vdh.frame_buffer, as_frame, config->ctx->colorspace, env ) < 0 )
        env->ThrowError( "LSMASHVideoSource: failed to make a frame." );
    return as_frame;
}

LSMASHAudioSource::LSMASHAudioSource
(
    const char         *source,
    uint32_t            track_number,
    bool                skip_priming,
    uint64_t            channel_layout,
    IScriptEnvironment *env
)
{
    memset( &vi,  0, sizeof(VideoInfo) );
    memset( &adh, 0, sizeof(libavsmash_audio_decode_handler_t) );
    memset( &aoh, 0, sizeof(libavsmash_audio_output_handler_t) );
    format_ctx = NULL;
    get_audio_track( source, track_number, skip_priming, env );
    lsmash_discard_boxes( adh.root );
    prepare_audio_decoding( channel_layout, env );
}

LSMASHAudioSource::~LSMASHAudioSource()
{
    if( aoh.resampled_buffer )
        av_free( aoh.resampled_buffer );
    if( aoh.avr_ctx )
        avresample_free( &aoh.avr_ctx );
    if( adh.frame_buffer )
        avcodec_free_frame( &adh.frame_buffer );
    cleanup_configuration( &adh.config );
    if( format_ctx )
        avformat_close_input( &format_ctx );
    lsmash_destroy_root( adh.root );
}

uint32_t LSMASHAudioSource::open_file( const char *source, IScriptEnvironment *env )
{
    /* L-SMASH */
    adh.root = lsmash_open_movie( source, LSMASH_FILE_MODE_READ );
    if( !adh.root )
        env->ThrowError( "LSMASHAudioSource: failed to lsmash_open_movie." );
    lsmash_movie_parameters_t movie_param;
    lsmash_initialize_movie_parameters( &movie_param );
    lsmash_get_movie_parameters( adh.root, &movie_param );
    if( movie_param.number_of_tracks == 0 )
        env->ThrowError( "LSMASHAudioSource: the number of tracks equals 0." );
    /* libavformat */
    av_register_all();
    avcodec_register_all();
    if( avformat_open_input( &format_ctx, source, NULL, NULL ) )
        env->ThrowError( "LSMASHAudioSource: failed to avformat_open_input." );
    if( avformat_find_stream_info( format_ctx, NULL ) < 0 )
        env->ThrowError( "LSMASHAudioSource: failed to avformat_find_stream_info." );
    /* */
    adh.config.error_message = throw_error;
    return movie_param.number_of_tracks;
}

static int64_t get_start_time( lsmash_root_t *root, uint32_t track_ID )
{
    /* Consider start time of this media if any non-empty edit is present. */
    uint32_t edit_count = lsmash_count_explicit_timeline_map( root, track_ID );
    for( uint32_t edit_number = 1; edit_number <= edit_count; edit_number++ )
    {
        lsmash_edit_t edit;
        if( lsmash_get_explicit_timeline_map( root, track_ID, edit_number, &edit ) )
            return 0;
        if( edit.duration == 0 )
            return 0;   /* no edits */
        if( edit.start_time >= 0 )
            return edit.start_time;
    }
    return 0;
}

static char *duplicate_as_string( void *src, size_t length )
{
    char *dst = new char[length + 1];
    if( !dst )
        return NULL;
    memcpy( dst, src, length );
    dst[length] = '\0';
    return dst;
}

void LSMASHAudioSource::get_audio_track( const char *source, uint32_t track_number, bool skip_priming, IScriptEnvironment *env )
{
    uint32_t number_of_tracks = open_file( source, env );
    if( track_number && track_number > number_of_tracks )
        env->ThrowError( "LSMASHAudioSource: the number of tracks equals %I32u.", number_of_tracks );
    /* L-SMASH */
    uint32_t i;
    lsmash_media_parameters_t media_param;
    if( track_number == 0 )
    {
        /* Get the first audio track. */
        for( i = 1; i <= number_of_tracks; i++ )
        {
            adh.track_ID = lsmash_get_track_ID( adh.root, i );
            if( adh.track_ID == 0 )
                env->ThrowError( "LSMASHAudioSource: failed to find audio track." );
            lsmash_initialize_media_parameters( &media_param );
            if( lsmash_get_media_parameters( adh.root, adh.track_ID, &media_param ) )
                env->ThrowError( "LSMASHAudioSource: failed to get media parameters." );
            if( media_param.handler_type == ISOM_MEDIA_HANDLER_TYPE_AUDIO_TRACK )
                break;
        }
        if( i > number_of_tracks )
            env->ThrowError( "LSMASHAudioSource: failed to find audio track." );
    }
    else
    {
        /* Get the desired audio track. */
        adh.track_ID = lsmash_get_track_ID( adh.root, track_number );
        if( adh.track_ID == 0 )
            env->ThrowError( "LSMASHAudioSource: failed to find audio track." );
        lsmash_initialize_media_parameters( &media_param );
        if( lsmash_get_media_parameters( adh.root, adh.track_ID, &media_param ) )
            env->ThrowError( "LSMASHAudioSource: failed to get media parameters." );
        if( media_param.handler_type != ISOM_MEDIA_HANDLER_TYPE_AUDIO_TRACK )
            env->ThrowError( "LSMASHAudioSource: the track you specified is not an audio track." );
    }
    if( lsmash_construct_timeline( adh.root, adh.track_ID ) )
        env->ThrowError( "LSMASHAudioSource: failed to get construct timeline." );
    if( get_summaries( adh.root, adh.track_ID, &adh.config ) )
        env->ThrowError( "LSMASHAudioSource: failed to get summaries." );
    adh.frame_count = lsmash_get_sample_count_in_media_timeline( adh.root, adh.track_ID );
    vi.num_audio_samples = lsmash_get_media_duration_from_media_timeline( adh.root, adh.track_ID );
    if( skip_priming )
    {
        uint32_t itunes_metadata_count = lsmash_count_itunes_metadata( adh.root );
        for( i = 1; i <= itunes_metadata_count; i++ )
        {
            lsmash_itunes_metadata_t metadata;
            if( lsmash_get_itunes_metadata( adh.root, i, &metadata ) )
                continue;
            if( metadata.item != ITUNES_METADATA_ITEM_CUSTOM
             || (metadata.type != ITUNES_METADATA_TYPE_STRING && metadata.type != ITUNES_METADATA_TYPE_BINARY)
             || !metadata.meaning || !metadata.name
             || memcmp( "com.apple.iTunes", metadata.meaning, strlen( metadata.meaning ) )
             || memcmp( "iTunSMPB", metadata.name, strlen( metadata.name ) ) )
                continue;
            char *value;
            if( metadata.type == ITUNES_METADATA_TYPE_STRING )
            {
                int length = strlen( metadata.value.string );
                if( length < 116 )
                    continue;
                value = duplicate_as_string( metadata.value.string, length );
            }
            else    /* metadata.type == ITUNES_METADATA_TYPE_BINARY */
            {
                if( metadata.value.binary.size < 116 )
                    continue;
                value = duplicate_as_string( metadata.value.binary.data, metadata.value.binary.size );
            }
            if( !value )
                continue;
            uint32_t dummy[9];
            uint32_t priming_samples;
            uint32_t padding;
            uint64_t duration;
            if( 12 != sscanf( value, " %I32x %I32x %I32x %I64x %I32x %I32x %I32x %I32x %I32x %I32x %I32x %I32x",
                              &dummy[0], &priming_samples, &padding, &duration, &dummy[1], &dummy[2],
                              &dummy[3], &dummy[4], &dummy[5], &dummy[6], &dummy[7], &dummy[8] ) )
            {
                delete [] value;
                continue;
            }
            delete [] value;
            adh.implicit_preroll     = 1;
            aoh.skip_decoded_samples = priming_samples;
            vi.num_audio_samples = duration + priming_samples;
            break;
        }
        if( aoh.skip_decoded_samples == 0 )
        {
            uint32_t ctd_shift;
            if( lsmash_get_composition_to_decode_shift_from_media_timeline( adh.root, adh.track_ID, &ctd_shift ) )
                env->ThrowError( "LSMASHAudioSource: failed to get the timeline shift." );
            aoh.skip_decoded_samples = ctd_shift + get_start_time( adh.root, adh.track_ID );
        }
    }
    /* libavformat */
    for( i = 0; i < format_ctx->nb_streams && format_ctx->streams[i]->codec->codec_type != AVMEDIA_TYPE_AUDIO; i++ );
    if( i == format_ctx->nb_streams )
        env->ThrowError( "LSMASHAudioSource: failed to find stream by libavformat." );
    /* libavcodec */
    AVStream *stream = format_ctx->streams[i];
    AVCodecContext *ctx = stream->codec;
    adh.config.ctx = ctx;
    AVCodec *codec = avcodec_find_decoder( ctx->codec_id );
    if( !codec )
        env->ThrowError( "LSMASHAudioSource: failed to find %s decoder.", codec->name );
    ctx->thread_count = 0;
    if( avcodec_open2( ctx, codec, NULL ) < 0 )
        env->ThrowError( "LSMASHAudioSource: failed to avcodec_open2." );
}

void LSMASHAudioSource::prepare_audio_decoding
(
    uint64_t            channel_layout,
    IScriptEnvironment *env
)
{
    adh.frame_buffer = avcodec_alloc_frame();
    if( !adh.frame_buffer )
        env->ThrowError( "LSMASHAudioSource: failed to allocate audio frame buffer." );
    /* Initialize the audio decoder configuration. */
    codec_configuration_t *config = &adh.config;
    config->message_priv = env;
    if( initialize_decoder_configuration( adh.root, adh.track_ID, config ) )
        env->ThrowError( "LSMASHAudioSource: failed to initialize the decoder configuration." );
    aoh.output_channel_layout  = config->prefer.channel_layout;
    aoh.output_sample_format   = config->prefer.sample_format;
    aoh.output_sample_rate     = config->prefer.sample_rate;
    aoh.output_bits_per_sample = config->prefer.bits_per_sample;
    /* */
    vi.num_audio_samples = libavsmash_count_overall_pcm_samples( &adh, aoh.output_sample_rate, &aoh.skip_decoded_samples );
    if( vi.num_audio_samples == 0 )
        env->ThrowError( "LSMASHAudioSource: no valid audio frame." );
    adh.next_pcm_sample_number = vi.num_audio_samples + 1;  /* Force seeking at the first reading. */
    /* Set up resampler. */
    aoh.avr_ctx = avresample_alloc_context();
    if( !aoh.avr_ctx )
        env->ThrowError( "LSMASHAudioSource: failed to avresample_alloc_context." );
    if( config->ctx->channel_layout == 0 )
        config->ctx->channel_layout = av_get_default_channel_layout( config->ctx->channels );
    if( channel_layout != 0 )
        aoh.output_channel_layout = channel_layout;
    aoh.output_sample_format = decide_audio_output_sample_format( aoh.output_sample_format );
    av_opt_set_int( aoh.avr_ctx, "in_channel_layout",   config->ctx->channel_layout, 0 );
    av_opt_set_int( aoh.avr_ctx, "in_sample_fmt",       config->ctx->sample_fmt,     0 );
    av_opt_set_int( aoh.avr_ctx, "in_sample_rate",      config->ctx->sample_rate,    0 );
    av_opt_set_int( aoh.avr_ctx, "out_channel_layout",  aoh.output_channel_layout,   0 );
    av_opt_set_int( aoh.avr_ctx, "out_sample_fmt",      aoh.output_sample_format,    0 );
    av_opt_set_int( aoh.avr_ctx, "out_sample_rate",     aoh.output_sample_rate,      0 );
    av_opt_set_int( aoh.avr_ctx, "internal_sample_fmt", AV_SAMPLE_FMT_FLTP,          0 );
    if( avresample_open( aoh.avr_ctx ) < 0 )
        env->ThrowError( "LSMASHAudioSource: failed to open resampler." );
    /* Decide output Bits Per Sample. */
    int output_channels = av_get_channel_layout_nb_channels( aoh.output_channel_layout );
    if( aoh.output_sample_format == AV_SAMPLE_FMT_S32
     && (aoh.output_bits_per_sample == 0 || aoh.output_bits_per_sample == 24) )
    {
        /* 24bit signed integer output */
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
            env->ThrowError( "LSMASHAudioSource: %s is not supported.", av_get_sample_fmt_name( config->ctx->sample_fmt ) );
    }
    /* Set up the number of planes and the block alignment of decoded and output data. */
    int input_channels = av_get_channel_layout_nb_channels( config->ctx->channel_layout );
    if( av_sample_fmt_is_planar( config->ctx->sample_fmt ) )
    {
        aoh.input_planes      = input_channels;
        aoh.input_block_align = av_get_bytes_per_sample( config->ctx->sample_fmt );
    }
    else
    {
        aoh.input_planes      = 1;
        aoh.input_block_align = av_get_bytes_per_sample( config->ctx->sample_fmt ) * input_channels;
    }
    aoh.output_block_align = (output_channels * aoh.output_bits_per_sample) / 8;
}

void __stdcall LSMASHAudioSource::GetAudio( void *buf, __int64 start, __int64 wanted_length, IScriptEnvironment *env )
{
    return (void)libavsmash_get_pcm_audio_samples( &adh, &aoh, buf, start, wanted_length );
}

AVSValue __cdecl CreateLSMASHVideoSource( AVSValue args, void *user_data, IScriptEnvironment *env )
{
    const char *source                 = args[0].AsString();
    uint32_t    track_number           = args[1].AsInt( 0 );
    int         threads                = args[2].AsInt( 0 );
    int         seek_mode              = args[3].AsInt( 0 );
    uint32_t    forward_seek_threshold = args[4].AsInt( 10 );
    threads                = threads >= 0 ? threads : 0;
    seek_mode              = CLIP_VALUE( seek_mode, 0, 2 );
    forward_seek_threshold = CLIP_VALUE( forward_seek_threshold, 1, 999 );
    return new LSMASHVideoSource( source, track_number, threads, seek_mode, forward_seek_threshold, env );
}

AVSValue __cdecl CreateLSMASHAudioSource( AVSValue args, void *user_data, IScriptEnvironment *env )
{
    const char *source        = args[0].AsString();
    uint32_t    track_number  = args[1].AsInt( 0 );
    bool        skip_priming  = args[2].AsBool( true );
    const char *layout_string = args[3].AsString( NULL );
    uint64_t channel_layout = layout_string ? av_get_channel_layout( layout_string ) : 0;
    return new LSMASHAudioSource( source, track_number, skip_priming, channel_layout, env );
}
