/*****************************************************************************
 * lsmashsource.cpp
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

#ifndef __STDC_CONSTANT_MACROS
#define __STDC_CONSTANT_MACROS
#endif

#include <windows.h>
#include "avisynth.h"

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

#include "../common/resample.h"
#include "../common/libavsmash.h"
#include "../common/libavsmash_audio.h"

#pragma warning( disable:4996 )

#pragma comment( lib, "libgcc.a" )
#pragma comment( lib, "libz.a" )
#pragma comment( lib, "libbz2.a" )
#pragma comment( lib, "liblsmash.a" )
#pragma comment( lib, "libavutil.a" )
#pragma comment( lib, "libavcodec.a" )
#pragma comment( lib, "libavformat.a" )
#pragma comment( lib, "libswscale.a" )
#pragma comment( lib, "libavresample.a" )
#pragma comment( lib, "libwsock32.a" )

#ifndef INT32_MAX
#define INT32_MAX 0x7fffffffL
#endif

#define CLIP_VALUE( value, min, max ) ((value) > (max) ? (max) : (value) < (min) ? (min) : (value))

#define SEEK_MODE_NORMAL     0
#define SEEK_MODE_UNSAFE     1
#define SEEK_MODE_AGGRESSIVE 2

static void throw_error( void *message_priv, const char *message, ... )
{
    IScriptEnvironment *env = (IScriptEnvironment *)message_priv;
    char temp[256];
    va_list args;
    va_start( args, message );
    vsprintf( temp, message, args );
    va_end( args );
    env->ThrowError( (const char *)temp );
}

typedef struct
{
    uint32_t composition_to_decoding;
} order_converter_t;

typedef struct
{
    lsmash_root_t        *root;
    uint32_t              track_ID;
    uint32_t              forward_seek_threshold;
    int                   seek_mode;
    codec_configuration_t config;
    AVFormatContext      *format_ctx;
    AVFrame              *frame_buffer;
    order_converter_t    *order_converter;
    uint32_t              last_sample_number;
    uint32_t              last_rap_number;
} video_decode_handler_t;

typedef void func_make_black_background( PVideoFrame &frame );
typedef int func_make_frame( struct SwsContext *sws_ctx, AVFrame *picture, PVideoFrame &frame, IScriptEnvironment *env );

typedef struct
{
    struct SwsContext          *sws_ctx;
    int                         scaler_flags;
    enum PixelFormat            output_pixel_format;
    PVideoFrame                *first_valid_frame;
    uint32_t                    first_valid_frame_number;
    func_make_black_background *make_black_background;
    func_make_frame            *make_frame;
} video_output_handler_t;

class LSMASHVideoSource : public IClip
{
private:
    VideoInfo              vi;
    video_decode_handler_t vh;
    video_output_handler_t voh;
    uint32_t open_file( const char *source, IScriptEnvironment *env );
    void get_video_track( const char *source, uint32_t track_number, int threads, IScriptEnvironment *env );
    void prepare_video_decoding( IScriptEnvironment *env );
public:
    LSMASHVideoSource( const char *source, uint32_t track_number, int threads, int seek_mode, uint32_t forward_seek_threshold, IScriptEnvironment *env );
    ~LSMASHVideoSource();
    PVideoFrame __stdcall GetFrame( int n, IScriptEnvironment *env );
    bool __stdcall GetParity( int n ) { return false; }
    void __stdcall GetAudio( void *buf, __int64 start, __int64 count, IScriptEnvironment *env ) {}
    void __stdcall SetCacheHints( int cachehints, int frame_range ) {}
    const VideoInfo& __stdcall GetVideoInfo() { return vi; }
};

LSMASHVideoSource::LSMASHVideoSource( const char *source, uint32_t track_number, int threads, int seek_mode, uint32_t forward_seek_threshold, IScriptEnvironment *env )
{
    memset( &vi, 0, sizeof(VideoInfo) );
    memset( &vh, 0, sizeof(video_decode_handler_t) );
    memset( &voh, 0, sizeof(video_output_handler_t) );
    vh.seek_mode              = seek_mode;
    vh.forward_seek_threshold = forward_seek_threshold;
    voh.first_valid_frame     = NULL;
    get_video_track( source, track_number, threads, env );
    lsmash_discard_boxes( vh.root );
    prepare_video_decoding( env );
}

LSMASHVideoSource::~LSMASHVideoSource()
{
    if( voh.first_valid_frame )
        delete voh.first_valid_frame;
    if( vh.order_converter )
        delete [] vh.order_converter;
    if( vh.frame_buffer )
        avcodec_free_frame( &vh.frame_buffer );
    if( voh.sws_ctx )
        sws_freeContext( voh.sws_ctx );
    cleanup_configuration( &vh.config );
    if( vh.format_ctx )
        avformat_close_input( &vh.format_ctx );
    lsmash_destroy_root( vh.root );
}

uint32_t LSMASHVideoSource::open_file( const char *source, IScriptEnvironment *env )
{
    /* L-SMASH */
    vh.root = lsmash_open_movie( source, LSMASH_FILE_MODE_READ );
    if( !vh.root )
        env->ThrowError( "LSMASHVideoSource: failed to lsmash_open_movie." );
    lsmash_movie_parameters_t movie_param;
    lsmash_initialize_movie_parameters( &movie_param );
    lsmash_get_movie_parameters( vh.root, &movie_param );
    if( movie_param.number_of_tracks == 0 )
        env->ThrowError( "LSMASHVideoSource: the number of tracks equals 0." );
    /* libavformat */
    av_register_all();
    avcodec_register_all();
    if( avformat_open_input( &vh.format_ctx, source, NULL, NULL ) )
        env->ThrowError( "LSMASHVideoSource: failed to avformat_open_input." );
    if( avformat_find_stream_info( vh.format_ctx, NULL ) < 0 )
        env->ThrowError( "LSMASHVideoSource: failed to avformat_find_stream_info." );
    /* */
    vh.config.error_message = throw_error;
    return movie_param.number_of_tracks;
}

static inline uint64_t get_gcd( uint64_t a, uint64_t b )
{
    if( !b )
        return a;
    while( 1 )
    {
        uint64_t c = a % b;
        if( !c )
            return b;
        a = b;
        b = c;
    }
}

static inline uint64_t reduce_fraction( uint64_t *a, uint64_t *b )
{
    uint64_t reduce = get_gcd( *a, *b );
    *a /= reduce;
    *b /= reduce;
    return reduce;
}

static void setup_timestamp_info( video_decode_handler_t *hp, VideoInfo *vi, uint64_t media_timescale, IScriptEnvironment *env )
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
            vh.track_ID = lsmash_get_track_ID( vh.root, i );
            if( vh.track_ID == 0 )
                env->ThrowError( "LSMASHVideoSource: failed to find video track." );
            lsmash_initialize_media_parameters( &media_param );
            if( lsmash_get_media_parameters( vh.root, vh.track_ID, &media_param ) )
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
        vh.track_ID = lsmash_get_track_ID( vh.root, track_number );
        if( vh.track_ID == 0 )
            env->ThrowError( "LSMASHVideoSource: failed to find video track." );
        lsmash_initialize_media_parameters( &media_param );
        if( lsmash_get_media_parameters( vh.root, vh.track_ID, &media_param ) )
            env->ThrowError( "LSMASHVideoSource: failed to get media parameters." );
        if( media_param.handler_type != ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK )
            env->ThrowError( "LSMASHVideoSource: the track you specified is not a video track." );
    }
    if( lsmash_construct_timeline( vh.root, vh.track_ID ) )
        env->ThrowError( "LSMASHVideoSource: failed to get construct timeline." );
    if( get_summaries( vh.root, vh.track_ID, &vh.config ) )
        env->ThrowError( "LSMASHVideoSource: failed to get summaries." );
    vi.num_frames = lsmash_get_sample_count_in_media_timeline( vh.root, vh.track_ID );
    setup_timestamp_info( &vh, &vi, media_param.timescale, env );
    /* libavformat */
    for( i = 0; i < vh.format_ctx->nb_streams && vh.format_ctx->streams[i]->codec->codec_type != AVMEDIA_TYPE_VIDEO; i++ );
    if( i == vh.format_ctx->nb_streams )
        env->ThrowError( "LSMASHVideoSource: failed to find stream by libavformat." );
    /* libavcodec */
    AVStream *stream = vh.format_ctx->streams[i];
    AVCodecContext *ctx = stream->codec;
    vh.config.ctx = ctx;
    AVCodec *codec = avcodec_find_decoder( ctx->codec_id );
    if( !codec )
        env->ThrowError( "LSMASHVideoSource: failed to find %s decoder.", codec->name );
    ctx->thread_count = threads;
    if( avcodec_open2( ctx, codec, NULL ) < 0 )
        env->ThrowError( "LSMASHVideoSource: failed to avcodec_open2." );
}

static int make_frame_yuv420p( struct SwsContext *sws_ctx, AVFrame *picture, PVideoFrame &frame, IScriptEnvironment *env )
{
    uint8_t *dst_data    [4];
    int      dst_linesize[4];
    if( av_image_alloc( dst_data, dst_linesize, picture->width, picture->height, AV_PIX_FMT_YUV420P, 16 ) < 0 )
        return -1;
    sws_scale( sws_ctx, (const uint8_t* const*)picture->data, picture->linesize, 0, picture->height, dst_data, dst_linesize );
    env->BitBlt( frame->GetWritePtr( PLANAR_Y ), frame->GetPitch( PLANAR_Y ), dst_data[0], dst_linesize[0], picture->width,     picture->height ); 
    env->BitBlt( frame->GetWritePtr( PLANAR_U ), frame->GetPitch( PLANAR_U ), dst_data[1], dst_linesize[1], picture->width / 2, picture->height / 2 ); 
    env->BitBlt( frame->GetWritePtr( PLANAR_V ), frame->GetPitch( PLANAR_V ), dst_data[2], dst_linesize[2], picture->width / 2, picture->height / 2 ); 
    av_free( dst_data[0] );
    return 0;
}

static int make_frame_yuv422( struct SwsContext *sws_ctx, AVFrame *picture, PVideoFrame &frame, IScriptEnvironment *env )
{
    uint8_t *dst_data    [4];
    int      dst_linesize[4];
    if( av_image_alloc( dst_data, dst_linesize, picture->width, picture->height, AV_PIX_FMT_YUYV422, 16 ) < 0 )
        return -1;
    sws_scale( sws_ctx, (const uint8_t* const*)picture->data, picture->linesize, 0, picture->height, dst_data, dst_linesize );
    env->BitBlt( frame->GetWritePtr(), frame->GetPitch(), dst_data[0], dst_linesize[0], picture->width * 2, picture->height );
    av_free( dst_data[0] );
    return 0;
}

static int make_frame_rgba32( struct SwsContext *sws_ctx, AVFrame *picture, PVideoFrame &frame, IScriptEnvironment *env )
{
    uint8_t *dst_data    [4];
    int      dst_linesize[4];
    if( av_image_alloc( dst_data, dst_linesize, picture->width, picture->height, AV_PIX_FMT_BGRA, 16 ) < 0 )
        return -1;
    sws_scale( sws_ctx, (const uint8_t* const*)picture->data, picture->linesize, 0, picture->height, dst_data, dst_linesize );
    env->BitBlt( frame->GetWritePtr() + frame->GetPitch() * (frame->GetHeight() - 1), -frame->GetPitch(), dst_data[0], dst_linesize[0], picture->width * 4, picture->height ); 
    av_free( dst_data[0] );
    return 0;
}

static void avoid_yuv_scale_conversion( enum AVPixelFormat *input_pixel_format )
{
    static const struct
    {
        enum AVPixelFormat full;
        enum AVPixelFormat limited;
    } range_hack_table[]
        = {
            { AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUV420P },
            { AV_PIX_FMT_YUVJ422P, AV_PIX_FMT_YUV422P },
            { AV_PIX_FMT_NONE,     AV_PIX_FMT_NONE    }
          };
    for( int i = 0; range_hack_table[i].full != AV_PIX_FMT_NONE; i++ )
        if( *input_pixel_format == range_hack_table[i].full )
            *input_pixel_format = range_hack_table[i].limited;
}

int determine_colorspace_conversion( enum AVPixelFormat *input_pixel_format, enum AVPixelFormat *output_pixel_format, int *output_pixel_type )
{
    avoid_yuv_scale_conversion( input_pixel_format );
    switch( *input_pixel_format )
    {
        case AV_PIX_FMT_YUV420P :
        case AV_PIX_FMT_NV12 :
        case AV_PIX_FMT_NV21 :
            *output_pixel_format = AV_PIX_FMT_YUV420P;  /* planar YUV 4:2:0, 12bpp, (1 Cr & Cb sample per 2x2 Y samples) */
            *output_pixel_type   = VideoInfo::CS_I420;
            return 1;
        case AV_PIX_FMT_YUYV422 :
        case AV_PIX_FMT_YUV422P :
        case AV_PIX_FMT_UYVY422 :
            *output_pixel_format = AV_PIX_FMT_YUYV422;  /* packed YUV 4:2:2, 16bpp */
            *output_pixel_type   = VideoInfo::CS_YUY2;
            return 2;
        case AV_PIX_FMT_ARGB :
        case AV_PIX_FMT_RGBA :
        case AV_PIX_FMT_ABGR :
        case AV_PIX_FMT_BGRA :
        case AV_PIX_FMT_RGB24 :
        case AV_PIX_FMT_BGR24 :
        case AV_PIX_FMT_GBRP :
            *output_pixel_format = AV_PIX_FMT_BGRA;     /* packed BGRA 8:8:8:8, 32bpp, BGRABGRA... */
            *output_pixel_type   = VideoInfo::CS_BGR32;
            return 3;
        default :
            *output_pixel_format = AV_PIX_FMT_NONE;
            *output_pixel_type   = VideoInfo::CS_UNKNOWN;
            return 0;
    }
}

static void make_black_background_yuv420p( PVideoFrame &frame )
{
    memset( frame->GetWritePtr( PLANAR_Y ), 0x00, frame->GetPitch( PLANAR_Y ) * frame->GetHeight( PLANAR_Y ) );
    memset( frame->GetWritePtr( PLANAR_U ), 0x80, frame->GetPitch( PLANAR_U ) * frame->GetHeight( PLANAR_U ) );
    memset( frame->GetWritePtr( PLANAR_V ), 0x80, frame->GetPitch( PLANAR_V ) * frame->GetHeight( PLANAR_V ) );
}

static void make_black_background_yuv422( PVideoFrame &frame )
{
    uint32_t *p = (uint32_t *)frame->GetWritePtr();
    int num_loops = frame->GetPitch() * frame->GetHeight() / 4;
    for( int i = 0; i < num_loops; i++ )
        *p++ = 0x00800080;
}

static void make_black_background_rgba32( PVideoFrame &frame )
{
    memset( frame->GetWritePtr(), 0x00, frame->GetPitch() * frame->GetHeight() );
}

static int make_frame( video_output_handler_t *ohp, AVFrame *picture, PVideoFrame &frame, IScriptEnvironment *env )
{
    /* Convert color space. We don't change the presentation resolution. */
    int64_t width;
    int64_t height;
    int64_t format;
    av_opt_get_int( ohp->sws_ctx, "srcw",       0, &width );
    av_opt_get_int( ohp->sws_ctx, "srch",       0, &height );
    av_opt_get_int( ohp->sws_ctx, "src_format", 0, &format );
    avoid_yuv_scale_conversion( (enum AVPixelFormat *)&picture->format );
    if( !ohp->sws_ctx || picture->width != width || picture->height != height || picture->format != format )
    {
        /* Update scaler. */
        ohp->sws_ctx = sws_getCachedContext( ohp->sws_ctx,
                                             picture->width, picture->height, (enum AVPixelFormat)picture->format,
                                             picture->width, picture->height, ohp->output_pixel_format,
                                             ohp->scaler_flags, NULL, NULL, NULL );
        if( !ohp->sws_ctx )
            return -1;
    }
    ohp->make_black_background( frame );
    return ohp->make_frame( ohp->sws_ctx, picture, frame, env );
}

void LSMASHVideoSource::prepare_video_decoding( IScriptEnvironment *env )
{
    vh.frame_buffer = avcodec_alloc_frame();
    if( !vh.frame_buffer )
        env->ThrowError( "LSMASHVideoSource: failed to allocate video frame buffer." );
    /* Initialize the video decoder configuration. */
    codec_configuration_t *config = &vh.config;
    config->message_priv = env;
    if( initialize_decoder_configuration( vh.root, vh.track_ID, config ) )
        env->ThrowError( "LSMASHVideoSource: failed to initialize the decoder configuration." );
    /* Set up output format. */
    enum AVPixelFormat input_pixel_format = config->ctx->pix_fmt;
    static const struct
    {
        func_make_black_background *make_black_background;
        func_make_frame            *make_frame;
    } frame_maker_func_table[] =
        {
            { NULL, NULL },
            { make_black_background_yuv420p, make_frame_yuv420p },
            { make_black_background_yuv422,  make_frame_yuv422  },
            { make_black_background_rgba32,  make_frame_rgba32  }
        };
    int frame_maker_index = determine_colorspace_conversion( &config->ctx->pix_fmt, &voh.output_pixel_format, &vi.pixel_type );
    if( frame_maker_index == 0 )
        env->ThrowError( "LSMASHVideoSource: %s is not supported", av_get_pix_fmt_name( input_pixel_format ) );
    vi.width  = config->prefer.width;
    vi.height = config->prefer.height;
    voh.make_black_background = frame_maker_func_table[frame_maker_index].make_black_background;
    voh.make_frame            = frame_maker_func_table[frame_maker_index].make_frame;
    voh.scaler_flags = SWS_FAST_BILINEAR;
    voh.sws_ctx = sws_getCachedContext( NULL,
                                        config->ctx->width, config->ctx->height, config->ctx->pix_fmt,
                                        config->ctx->width, config->ctx->height, voh.output_pixel_format,
                                        voh.scaler_flags, NULL, NULL, NULL );
    if( !voh.sws_ctx )
        env->ThrowError( "LSMASHVideoSource: failed to get swscale context." );
    /* Find the first valid video sample. */
    for( uint32_t i = 1; i <= vi.num_frames + get_decoder_delay( config->ctx ); i++ )
    {
        AVPacket pkt = { 0 };
        get_sample( vh.root, vh.track_ID, i, &vh.config, &pkt );
        AVFrame *picture = vh.frame_buffer;
        avcodec_get_frame_defaults( picture );
        int got_picture;
        if( avcodec_decode_video2( config->ctx, picture, &got_picture, &pkt ) >= 0 && got_picture )
        {
            voh.first_valid_frame_number = i - min( get_decoder_delay( config->ctx ), config->delay_count );
            if( voh.first_valid_frame_number > 1 || vi.num_frames == 1 )
            {
                PVideoFrame temp = env->NewVideoFrame( vi );
                if( !temp )
                    env->ThrowError( "LSMASHVideoSource: failed to allocate memory for the first valid video frame data." );
                if( make_frame( &voh, picture, temp, env ) )
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
    vh.last_sample_number = vi.num_frames + 1;  /* Force seeking at the first reading. */
}

static int decode_video_sample( video_decode_handler_t *hp, AVFrame *picture, int *got_picture, uint32_t sample_number )
{
    AVPacket pkt = { 0 };
    int ret = get_sample( hp->root, hp->track_ID, sample_number, &hp->config, &pkt );
    if( ret )
        return ret;
    if( pkt.flags != ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE )
    {
        pkt.flags = AV_PKT_FLAG_KEY;
        hp->last_rap_number = sample_number;
    }
    else
        pkt.flags = 0;
    avcodec_get_frame_defaults( picture );
    uint64_t cts = pkt.pts;
    ret = avcodec_decode_video2( hp->config.ctx, picture, got_picture, &pkt );
    picture->pts = cts;
    return ret < 0 ? -1 : 0;
}

static inline uint32_t get_decoding_sample_number( order_converter_t *order_converter, uint32_t composition_sample_number )
{
    return order_converter
         ? order_converter[composition_sample_number].composition_to_decoding
         : composition_sample_number;
}

static int find_random_accessible_point( video_decode_handler_t *hp, uint32_t composition_sample_number, uint32_t decoding_sample_number, uint32_t *rap_number )
{
    if( decoding_sample_number == 0 )
        decoding_sample_number = get_decoding_sample_number( hp->order_converter, composition_sample_number );
    lsmash_random_access_flag ra_flags;
    uint32_t distance;  /* distance from the closest random accessible point to the previous. */
    uint32_t number_of_leadings;
    if( lsmash_get_closest_random_accessible_point_detail_from_media_timeline( hp->root, hp->track_ID, decoding_sample_number,
                                                                               rap_number, &ra_flags, &number_of_leadings, &distance ) )
        *rap_number = 1;
    int roll_recovery = !!(ra_flags & ISOM_SAMPLE_RANDOM_ACCESS_FLAG_GDR);
    int is_leading    = number_of_leadings && (decoding_sample_number - *rap_number <= number_of_leadings);
    if( (roll_recovery || is_leading) && *rap_number > distance )
        *rap_number -= distance;
    /* Check whether random accessible point has the same decoder configuration or not. */
    decoding_sample_number = get_decoding_sample_number( hp->order_converter, composition_sample_number );
    do
    {
        lsmash_sample_t sample;
        lsmash_sample_t rap_sample;
        if( lsmash_get_sample_info_from_media_timeline( hp->root, hp->track_ID, decoding_sample_number, &sample )
         || lsmash_get_sample_info_from_media_timeline( hp->root, hp->track_ID, *rap_number, &rap_sample ) )
        {
            /* Fatal error. */
            *rap_number = hp->last_rap_number;
            return 0;
        }
        if( sample.index == rap_sample.index )
            break;
        uint32_t sample_index = sample.index;
        for( uint32_t i = decoding_sample_number - 1; i; i-- )
        {
            if( lsmash_get_sample_info_from_media_timeline( hp->root, hp->track_ID, i, &sample ) )
            {
                /* Fatal error. */
                *rap_number = hp->last_rap_number;
                return 0;
            }
            if( sample.index != sample_index )
            {
                if( distance )
                {
                    *rap_number += distance;
                    distance = 0;
                    continue;
                }
                else
                    *rap_number = i + 1;
            }
        }
        break;
    } while( 1 );
    return roll_recovery;
}

static uint32_t seek_video( video_decode_handler_t *hp, AVFrame *picture, uint32_t composition_sample_number, uint32_t rap_number, int error_ignorance )
{
    /* Prepare to decode from random accessible sample. */
    codec_configuration_t *config = &hp->config;
    if( config->update_pending )
        /* Update the decoder configuration. */
        update_configuration( hp->root, hp->track_ID, config );
    else
        flush_buffers( config );
    if( config->error )
        return 0;
    int dummy;
    uint64_t rap_cts = 0;
    uint32_t i;
    uint32_t decoder_delay = get_decoder_delay( config->ctx );
    for( i = rap_number; i < composition_sample_number + decoder_delay; i++ )
    {
        if( config->index == config->queue.index )
            config->delay_count = min( decoder_delay, i - rap_number );
        int ret = decode_video_sample( hp, picture, &dummy, i );
        /* Some decoders return -1 when feeding a leading sample.
         * We don't consider as an error if the return value -1 is caused by a leading sample since it's not fatal at all. */
        if( i == hp->last_rap_number )
            rap_cts = picture->pts;
        if( ret == -1 && (uint64_t)picture->pts >= rap_cts && !error_ignorance )
            return 0;
        else if( ret >= 1 )
            /* No decoding occurs. */
            break;
    }
    if( config->index == config->queue.index )
        config->delay_count = min( decoder_delay, i - rap_number );
    return i;
}

static int get_picture( video_decode_handler_t *hp, AVFrame *picture, uint32_t current, uint32_t goal, uint32_t sample_count )
{
    codec_configuration_t *config = &hp->config;
    int got_picture = (current > goal);
    while( current <= goal )
    {
        int ret = decode_video_sample( hp, picture, &got_picture, current );
        if( ret == -1 )
            return -1;
        else if( ret == 1 )
            /* Sample doesn't exist. */
            break;
        ++current;
        if( config->update_pending )
            /* A new decoder configuration is needed. Anyway, stop getting picture. */
            break;
        if( !got_picture )
            ++ config->delay_count;
    }
    /* Flush the last frames. */
    if( current > sample_count && get_decoder_delay( config->ctx ) )
        while( current <= goal )
        {
            AVPacket pkt = { 0 };
            av_init_packet( &pkt );
            pkt.data = NULL;
            pkt.size = 0;
            avcodec_get_frame_defaults( picture );
            if( avcodec_decode_video2( config->ctx, picture, &got_picture, &pkt ) < 0 )
                return -1;
            ++current;
        }
    return got_picture ? 0 : -1;
}

PVideoFrame __stdcall LSMASHVideoSource::GetFrame( int n, IScriptEnvironment *env )
{
#define MAX_ERROR_COUNT 3       /* arbitrary */
    uint32_t sample_number = n + 1;     /* For L-SMASH, sample_number is 1-origin. */
    if( sample_number < voh.first_valid_frame_number || vi.num_frames == 1 )
    {
        /* Copy the first valid video frame. */
        vh.last_sample_number = vi.num_frames + 1;  /* Force seeking at the next access for valid video sample. */
        return *voh.first_valid_frame;
    }
    codec_configuration_t *config = &vh.config;
    config->message_priv = env;
    PVideoFrame frame = env->NewVideoFrame( vi );
    if( config->error )
        return frame;
    AVFrame *picture = vh.frame_buffer;
    uint32_t start_number;  /* number of sample, for normal decoding, where decoding starts excluding decoding delay */
    uint32_t rap_number;    /* number of sample, for seeking, where decoding starts excluding decoding delay */
    int seek_mode = vh.seek_mode;
    int roll_recovery = 0;
    if( sample_number > vh.last_sample_number
     && sample_number <= vh.last_sample_number + vh.forward_seek_threshold )
    {
        start_number = vh.last_sample_number + 1 + config->delay_count;
        rap_number = vh.last_rap_number;
    }
    else
    {
        roll_recovery = find_random_accessible_point( &vh, sample_number, 0, &rap_number );
        if( rap_number == vh.last_rap_number && sample_number > vh.last_sample_number )
        {
            roll_recovery = 0;
            start_number = vh.last_sample_number + 1 + config->delay_count;
        }
        else
        {
            /* Require starting to decode from random accessible sample. */
            vh.last_rap_number = rap_number;
            start_number = seek_video( &vh, picture, sample_number, rap_number, roll_recovery || seek_mode != SEEK_MODE_NORMAL );
        }
    }
    /* Get desired picture. */
    int error_count = 0;
    while( start_number == 0    /* Failed to seek. */
     || config->update_pending  /* Need to update the decoder configuration to decode pictures. */
     || get_picture( &vh, picture, start_number, sample_number + config->delay_count, vi.num_frames ) )
    {
        if( config->update_pending )
        {
            roll_recovery = find_random_accessible_point( &vh, sample_number, 0, &rap_number );
            vh.last_rap_number = rap_number;
        }
        else
        {
            /* Failed to get desired picture. */
            if( config->error || seek_mode == SEEK_MODE_AGGRESSIVE )
                env->ThrowError( "LSMASHVideoSource: fatal error of decoding." );
            if( ++error_count > MAX_ERROR_COUNT || rap_number <= 1 )
            {
                if( seek_mode == SEEK_MODE_UNSAFE )
                    env->ThrowError( "LSMASHVideoSource: fatal error of decoding." );
                /* Retry to decode from the same random accessible sample with error ignorance. */
                seek_mode = SEEK_MODE_AGGRESSIVE;
            }
            else
            {
                /* Retry to decode from more past random accessible sample. */
                roll_recovery = find_random_accessible_point( &vh, sample_number, rap_number - 1, &rap_number );
                if( vh.last_rap_number == rap_number )
                    env->ThrowError( "LSMASHVideoSource: fatal error of decoding." );
                vh.last_rap_number = rap_number;
            }
        }
        start_number = seek_video( &vh, picture, sample_number, rap_number, roll_recovery || seek_mode != SEEK_MODE_NORMAL );
    }
    vh.last_sample_number = sample_number;
    if( make_frame( &voh, picture, frame, env ) )
        env->ThrowError( "LSMASHVideoSource: failed to make a frame." );
    return frame;
#undef MAX_ERROR_COUNT
}

class LSMASHAudioSource : public IClip
{
private:
    VideoInfo              vi;
    audio_decode_handler_t adh;
    audio_output_handler_t aoh;
    AVFormatContext       *format_ctx;
    uint32_t open_file( const char *source, IScriptEnvironment *env );
    void get_audio_track( const char *source, uint32_t track_number, bool skip_priming, IScriptEnvironment *env );
    void prepare_audio_decoding( IScriptEnvironment *env );
public:
    LSMASHAudioSource( const char *source, uint32_t track_number, bool skip_priming, IScriptEnvironment *env );
    ~LSMASHAudioSource();
    PVideoFrame __stdcall GetFrame( int n, IScriptEnvironment *env ) { return NULL; }
    bool __stdcall GetParity( int n ) { return false; }
    void __stdcall GetAudio( void *buf, __int64 start, __int64 wanted_length, IScriptEnvironment *env );
    void __stdcall SetCacheHints( int cachehints, int frame_range ) {}
    const VideoInfo& __stdcall GetVideoInfo() { return vi; }
};

LSMASHAudioSource::LSMASHAudioSource( const char *source, uint32_t track_number, bool skip_priming, IScriptEnvironment *env )
{
    memset( &vi,  0, sizeof(VideoInfo) );
    memset( &adh, 0, sizeof(audio_decode_handler_t) );
    memset( &aoh, 0, sizeof(audio_output_handler_t) );
    format_ctx = NULL;
    get_audio_track( source, track_number, skip_priming, env );
    lsmash_discard_boxes( adh.root );
    prepare_audio_decoding( env );
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

static inline enum AVSampleFormat decide_audio_output_sample_format( enum AVSampleFormat input_sample_format )
{
    /* Avisynth doesn't support IEEE double precision floating point format. */
    switch( input_sample_format )
    {
        case AV_SAMPLE_FMT_U8 :
        case AV_SAMPLE_FMT_U8P :
            return AV_SAMPLE_FMT_U8;
        case AV_SAMPLE_FMT_S16 :
        case AV_SAMPLE_FMT_S16P :
            return AV_SAMPLE_FMT_S16;
        case AV_SAMPLE_FMT_S32 :
        case AV_SAMPLE_FMT_S32P :
            return AV_SAMPLE_FMT_S32;
        default :
            return AV_SAMPLE_FMT_FLT;
    }
}

void LSMASHAudioSource::prepare_audio_decoding( IScriptEnvironment *env )
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
    vi.num_audio_samples = count_overall_pcm_samples( &adh, aoh.output_sample_rate, &aoh.skip_decoded_samples );
    if( vi.num_audio_samples == 0 )
        env->ThrowError( "LSMASHAudioSource: no valid audio frame." );
    adh.next_pcm_sample_number = vi.num_audio_samples + 1;  /* Force seeking at the first reading. */
    /* Set up resampler. */
    aoh.avr_ctx = avresample_alloc_context();
    if( !aoh.avr_ctx )
        env->ThrowError( "LSMASHAudioSource: failed to avresample_alloc_context." );
    if( config->ctx->channel_layout == 0 )
        config->ctx->channel_layout = av_get_default_channel_layout( config->ctx->channels );
    aoh.output_sample_format = decide_audio_output_sample_format( aoh.output_sample_format );
    av_opt_set_int( aoh.avr_ctx, "in_channel_layout",   config->ctx->channel_layout, 0 );
    av_opt_set_int( aoh.avr_ctx, "in_sample_fmt",       config->ctx->sample_fmt,     0 );
    av_opt_set_int( aoh.avr_ctx, "in_sample_rate",      config->ctx->sample_rate,    0 );
    av_opt_set_int( aoh.avr_ctx, "out_channel_layout",  aoh.output_channel_layout,    0 );
    av_opt_set_int( aoh.avr_ctx, "out_sample_fmt",      aoh.output_sample_format,     0 );
    av_opt_set_int( aoh.avr_ctx, "out_sample_rate",     aoh.output_sample_rate,       0 );
    av_opt_set_int( aoh.avr_ctx, "internal_sample_fmt", AV_SAMPLE_FMT_FLTP,          0 );
    if( avresample_open( aoh.avr_ctx ) < 0 )
        env->ThrowError( "LSMASHAudioSource: failed to open resampler." );
    /* Decide output Bits Per Sample. */
    int output_channels = av_get_channel_layout_nb_channels( aoh.output_channel_layout );
    if( aoh.output_sample_format == AV_SAMPLE_FMT_S32
     && (aoh.output_bits_per_sample == 0 || aoh.output_bits_per_sample == 24) )
    {
        /* 24bit signed integer output */
        if( config->ctx->frame_size )
        {
            aoh.resampled_buffer_size = get_linesize( output_channels, config->ctx->frame_size, aoh.output_sample_format );
            aoh.resampled_buffer      = (uint8_t *)av_malloc( aoh.resampled_buffer_size );
            if( !aoh.resampled_buffer )
                env->ThrowError( "LSMASHAudioSource: failed to allocate memory for resampling." );
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
    return (void)get_pcm_audio_samples( &adh, &aoh, buf, start, wanted_length );
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
    const char *source       = args[0].AsString();
    uint32_t    track_number = args[1].AsInt( 0 );
    bool        skip_priming = args[2].AsBool( true );
    return new LSMASHAudioSource( source, track_number, skip_priming, env );
}

extern "C" __declspec(dllexport) const char * __stdcall AvisynthPluginInit2( IScriptEnvironment *env )
{
    env->AddFunction( "LSMASHVideoSource", "[source]s[track]i[threads]i[seek_mode]i[seek_threshold]i", CreateLSMASHVideoSource, 0 );
    env->AddFunction( "LSMASHAudioSource", "[source]s[track]i[skip_priming]b", CreateLSMASHAudioSource, 0 );
    return "LSMASHSource";
}
