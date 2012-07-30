/*****************************************************************************
 * lsmashsource.cpp
 *****************************************************************************
 * Copyright (C) 2012 L-SMASH Works project
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
 * However, when distributing its binary file, it will be under LGPL or GPL.
 * Don't distribute it if its license is GPL. */

#include <windows.h>
#include "avisynth.h"

#ifndef __STDC_CONSTANT_MACROS
#define __STDC_CONSTANT_MACROS
#endif

extern "C"
{
/* L-SMASH */
#define LSMASH_DEMUXER_ENABLED
#include <lsmash.h>                 /* Demuxer */

/* Libav
 * The binary file will be LGPLed or GPLed. */
#include <libavformat/avformat.h>   /* Codec specific info importer */
#include <libavcodec/avcodec.h>     /* Decoder */
#include <libswscale/swscale.h>     /* Colorspace converter */
#include <libavutil/pixdesc.h>
}

#pragma comment( lib, "libgcc.a" )
#pragma comment( lib, "libz.a" )
#pragma comment( lib, "libbz2.a" )
#pragma comment( lib, "liblsmash.a" )
#pragma comment( lib, "libavutil.a" )
#pragma comment( lib, "libavcodec.a" )
#pragma comment( lib, "libavformat.a" )
#pragma comment( lib, "libswscale.a" )
#pragma comment( lib, "libwsock32.a" )

#ifndef INT32_MAX
#define INT32_MAX 0x7fffffffL
#endif

#define CLIP_VALUE( value, min, max ) ((value) > (max) ? (max) : (value) < (min) ? (min) : (value))

#define SEEK_MODE_NORMAL     0
#define SEEK_MODE_UNSAFE     1
#define SEEK_MODE_AGGRESSIVE 2

typedef enum
{
    DECODE_REQUIRE_INITIAL = 0,
    DECODE_INITIALIZING    = 1,
    DECODE_INITIALIZED     = 2
} decode_status_t;

typedef struct
{
    uint32_t composition_to_decoding;
} order_converter_t;

typedef struct
{
    lsmash_root_t     *root;
    uint32_t           track_ID;
    uint32_t           forward_seek_threshold;
    int                seek_mode;
    AVCodecContext    *codec_ctx;
    AVFormatContext   *format_ctx;
    struct SwsContext *sws_ctx;
    order_converter_t *order_converter;
    uint8_t           *input_buffer;
    uint32_t           last_sample_number;
    uint32_t           last_rap_number;
    uint32_t           delay_count;
    int                error;
    decode_status_t    decode_status;
} video_decode_handler_t;

typedef int func_make_frame( AVCodecContext *codec_ctx, struct SwsContext *sws_ctx, AVFrame *picture, PVideoFrame &frame, IScriptEnvironment *env );

class LSMASHVideoSource : public IClip
{
private:
    VideoInfo              vi;
    video_decode_handler_t vh;
    PVideoFrame           *first_valid_frame;
    uint32_t               first_valid_frame_number;
    func_make_frame       *make_frame;
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
    vh.seek_mode              = seek_mode;
    vh.forward_seek_threshold = forward_seek_threshold;
    first_valid_frame         = NULL;
    get_video_track( source, track_number, threads, env );
    lsmash_discard_boxes( vh.root );
    prepare_video_decoding( env );
}

LSMASHVideoSource::~LSMASHVideoSource()
{
    if( first_valid_frame )
        delete first_valid_frame;
    if( vh.order_converter )
        delete [] vh.order_converter;
    if( vh.input_buffer )
        av_free( vh.input_buffer );
    if( vh.sws_ctx )
        sws_freeContext( vh.sws_ctx );
    if( vh.codec_ctx )
        avcodec_close( vh.codec_ctx );
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
        uint64_t media_duration = lsmash_get_media_duration( hp->root, hp->track_ID );
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
    vi.num_frames = lsmash_get_sample_count_in_media_timeline( vh.root, vh.track_ID );
    setup_timestamp_info( &vh, &vi, media_param.timescale, env );
    /* libavformat */
    for( i = 0; i < vh.format_ctx->nb_streams && vh.format_ctx->streams[i]->codec->codec_type != AVMEDIA_TYPE_VIDEO; i++ );
    if( i == vh.format_ctx->nb_streams )
        env->ThrowError( "LSMASHVideoSource: failed to find stream by libavformat." );
    /* libavcodec */
    AVStream *stream = vh.format_ctx->streams[i];
    vh.codec_ctx = stream->codec;
    AVCodec *codec = avcodec_find_decoder( vh.codec_ctx->codec_id );
    if( !codec )
        env->ThrowError( "LSMASHVideoSource: failed to find %s decoder.", codec->name );
    vh.codec_ctx->thread_count = threads;
    if( avcodec_open2( vh.codec_ctx, codec, NULL ) < 0 )
        env->ThrowError( "LSMASHVideoSource: failed to avcodec_open2." );
}

static inline uint32_t get_decoder_delay( AVCodecContext *ctx )
{
    return ctx->has_b_frames + ((ctx->active_thread_type & FF_THREAD_FRAME) ? ctx->thread_count - 1 : 0);
}

static int get_sample( lsmash_root_t *root, uint32_t track_ID, uint32_t sample_number, uint8_t *buffer, AVPacket *pkt )
{
    av_init_packet( pkt );
    lsmash_sample_t *sample = lsmash_get_sample_from_media_timeline( root, track_ID, sample_number );
    if( !sample )
    {
        pkt->data = NULL;
        pkt->size = 0;
        return 1;
    }
    pkt->flags = sample->prop.random_access_type == ISOM_SAMPLE_RANDOM_ACCESS_TYPE_NONE ? 0 : AV_PKT_FLAG_KEY;
    pkt->size  = sample->length;
    pkt->data  = buffer;
    memcpy( pkt->data, sample->data, sample->length );
    lsmash_delete_sample( sample );
    return 0;
}

static int get_conversion_multiplier( enum PixelFormat dst_pix_fmt, enum PixelFormat src_pix_fmt )
{
    int src_size = 0;
    for( int i = 0; i < 4; i++ )
    {
        const AVComponentDescriptor *comp = &av_pix_fmt_descriptors[src_pix_fmt].comp[i];
        if( comp->plane | comp->step_minus1 | comp->offset_plus1 | comp->shift | comp->depth_minus1 )
            src_size += ((comp->depth_minus1 + 8) >> 3) << 3;
    }
    if( src_size == 0 )
        return 1;
    int dst_size = 0;
    for( int i = 0; i < 4; i++ )
    {
        const AVComponentDescriptor *comp = &av_pix_fmt_descriptors[dst_pix_fmt].comp[i];
        if( comp->plane | comp->step_minus1 | comp->offset_plus1 | comp->shift | comp->depth_minus1 )
            dst_size += ((comp->depth_minus1 + 8) >> 3) << 3;
    }
    return (dst_size - 1) / src_size + 1;
}

static int make_frame_yuv420p( AVCodecContext *codec_ctx, struct SwsContext *sws_ctx, AVFrame *picture, PVideoFrame &frame, IScriptEnvironment *env )
{
    int abs_dst_linesize = picture->linesize[0] > 0 ? picture->linesize[0] : -picture->linesize[0];
    if( abs_dst_linesize & 15 )
        abs_dst_linesize = (abs_dst_linesize & 0xfffffff0) + 16;  /* Make mod16. */
    uint8_t *dst_data[4];
    dst_data[0] = (uint8_t *)av_mallocz( abs_dst_linesize * codec_ctx->height * 3 );
    if( !dst_data[0] )
        return -1;
    for( int i = 1; i < 3; i++ )
        dst_data[i] = dst_data[i - 1] + abs_dst_linesize * codec_ctx->height;
    dst_data[3] = NULL;
    const int dst_linesize[4] = { abs_dst_linesize, abs_dst_linesize, abs_dst_linesize, 0 };
    sws_scale( sws_ctx, (const uint8_t* const*)picture->data, picture->linesize, 0, codec_ctx->height, dst_data, dst_linesize );
    env->BitBlt( frame->GetWritePtr( PLANAR_Y ), frame->GetPitch( PLANAR_Y ), dst_data[0], dst_linesize[0], frame->GetRowSize( PLANAR_Y ), frame->GetHeight( PLANAR_Y ) ); 
    env->BitBlt( frame->GetWritePtr( PLANAR_U ), frame->GetPitch( PLANAR_U ), dst_data[1], dst_linesize[1], frame->GetRowSize( PLANAR_U ), frame->GetHeight( PLANAR_U ) ); 
    env->BitBlt( frame->GetWritePtr( PLANAR_V ), frame->GetPitch( PLANAR_V ), dst_data[2], dst_linesize[2], frame->GetRowSize( PLANAR_V ), frame->GetHeight( PLANAR_V ) ); 
    av_free( dst_data[0] );
    return 0;
}

static int make_frame_yuv422( AVCodecContext *codec_ctx, struct SwsContext *sws_ctx, AVFrame *picture, PVideoFrame &frame, IScriptEnvironment *env )
{
    int abs_dst_linesize = picture->linesize[0] + picture->linesize[1] + picture->linesize[2] + picture->linesize[3];
    if( abs_dst_linesize < 0 )
        abs_dst_linesize = -abs_dst_linesize;
    const int dst_linesize[4] = { abs_dst_linesize, 0, 0, 0 };
    uint8_t  *dst_data    [4] = { NULL, NULL, NULL, NULL };
    dst_data[0] = (uint8_t *)av_mallocz( dst_linesize[0] * codec_ctx->height );
    if( !dst_data[0] )
        return -1;
    sws_scale( sws_ctx, (const uint8_t* const*)picture->data, picture->linesize, 0, codec_ctx->height, dst_data, dst_linesize );
    env->BitBlt( frame->GetWritePtr(), frame->GetPitch(), dst_data[0], dst_linesize[0], frame->GetRowSize(), frame->GetHeight() );
    av_free( dst_data[0] );
    return 0;
}

static int make_frame_rgba32( AVCodecContext *codec_ctx, struct SwsContext *sws_ctx, AVFrame *picture, PVideoFrame &frame, IScriptEnvironment *env )
{
    int abs_dst_linesize = picture->linesize[0] + picture->linesize[1] + picture->linesize[2] + picture->linesize[3];
    if( abs_dst_linesize < 0 )
        abs_dst_linesize = -abs_dst_linesize;
    abs_dst_linesize *= get_conversion_multiplier( PIX_FMT_BGRA, codec_ctx->pix_fmt );
    const int dst_linesize[4] = { abs_dst_linesize, 0, 0, 0 };
    uint8_t  *dst_data    [4] = { NULL, NULL, NULL, NULL };
    dst_data[0] = (uint8_t *)av_mallocz( dst_linesize[0] * codec_ctx->height );
    if( !dst_data[0] )
        return -1;
    sws_scale( sws_ctx, (const uint8_t* const*)picture->data, picture->linesize, 0, codec_ctx->height, dst_data, dst_linesize );
    env->BitBlt( frame->GetWritePtr() + frame->GetPitch() * (frame->GetHeight() - 1), -frame->GetPitch(), dst_data[0], dst_linesize[0], frame->GetRowSize(), frame->GetHeight() ); 
    av_free( dst_data[0] );
    return 0;
}

static void avoid_yuv_scale_conversion( enum PixelFormat *input_pixel_format )
{
    static const struct
    {
        enum PixelFormat full;
        enum PixelFormat limited;
    } range_hack_table[]
        = {
            { PIX_FMT_YUVJ420P, PIX_FMT_YUV420P },
            { PIX_FMT_YUVJ422P, PIX_FMT_YUV422P },
            { PIX_FMT_NONE,     PIX_FMT_NONE    }
          };
    for( int i = 0; range_hack_table[i].full != PIX_FMT_NONE; i++ )
        if( *input_pixel_format == range_hack_table[i].full )
            *input_pixel_format = range_hack_table[i].limited;
}

func_make_frame *determine_colorspace_conversion( enum PixelFormat *input_pixel_format, enum PixelFormat *output_pixel_format, int *output_pixel_type )
{
    avoid_yuv_scale_conversion( input_pixel_format );
    switch( *input_pixel_format )
    {
        case PIX_FMT_YUV420P :
        case PIX_FMT_NV12 :
        case PIX_FMT_NV21 :
            *output_pixel_format = PIX_FMT_YUV420P;     /* planar YUV 4:2:0, 12bpp, (1 Cr & Cb sample per 2x2 Y samples) */
            *output_pixel_type   = VideoInfo::CS_I420;
            return make_frame_yuv420p;
        case PIX_FMT_YUYV422 :
        case PIX_FMT_YUV422P :
        case PIX_FMT_UYVY422 :
            *output_pixel_format = PIX_FMT_YUYV422;     /* packed YUV 4:2:2, 16bpp */
            *output_pixel_type   = VideoInfo::CS_YUY2;
            return make_frame_yuv422;
        case PIX_FMT_ARGB :
        case PIX_FMT_RGBA :
        case PIX_FMT_ABGR :
        case PIX_FMT_BGRA :
        case PIX_FMT_RGB24 :
        case PIX_FMT_BGR24 :
        case PIX_FMT_GBRP :
            *output_pixel_format = PIX_FMT_BGRA;        /* packed BGRA 8:8:8:8, 32bpp, BGRABGRA... */
            *output_pixel_type   = VideoInfo::CS_BGR32;
            return make_frame_rgba32;
        default :
            *output_pixel_format = PIX_FMT_NONE;
            *output_pixel_type   = VideoInfo::CS_UNKNOWN;
            return NULL;
    }
}

void LSMASHVideoSource::prepare_video_decoding( IScriptEnvironment *env )
{
    /* Note: the input buffer for avcodec_decode_video2 must be FF_INPUT_BUFFER_PADDING_SIZE larger than the actual read bytes. */
    uint32_t input_buffer_size = lsmash_get_max_sample_size_in_media_timeline( vh.root, vh.track_ID );
    if( input_buffer_size == 0 )
        env->ThrowError( "LSMASHVideoSource: no valid video sample found." );
    vh.input_buffer = (uint8_t *)av_mallocz( input_buffer_size + FF_INPUT_BUFFER_PADDING_SIZE );
    if( !vh.input_buffer )
        env->ThrowError( "LSMASHVideoSource: failed to allocate memory to the input buffer for video." );
    /* swscale */
    enum PixelFormat input_pixel_format = vh.codec_ctx->pix_fmt;
    enum PixelFormat output_pixel_format;
    make_frame = determine_colorspace_conversion( &vh.codec_ctx->pix_fmt, &output_pixel_format, &vi.pixel_type );
    if( !make_frame )
        env->ThrowError( "LSMASHVideoSource: %s is not supported", av_get_pix_fmt_name( input_pixel_format ) );
    vi.width  = vh.codec_ctx->width;
    vi.height = vh.codec_ctx->height;
    vh.sws_ctx = sws_getCachedContext( NULL,
                                       vh.codec_ctx->width, vh.codec_ctx->height, vh.codec_ctx->pix_fmt,
                                       vh.codec_ctx->width, vh.codec_ctx->height, output_pixel_format,
                                       SWS_FAST_BILINEAR, NULL, NULL, NULL );
    if( !vh.sws_ctx )
        env->ThrowError( "LSMASHVideoSource: failed to get swscale context." );
    /* Find the first valid video sample. */
    for( uint32_t i = 1; i <= vi.num_frames + get_decoder_delay( vh.codec_ctx ); i++ )
    {
        AVPacket pkt;
        get_sample( vh.root, vh.track_ID, i, vh.input_buffer, &pkt );
        AVFrame picture;
        avcodec_get_frame_defaults( &picture );
        int got_picture;
        if( avcodec_decode_video2( vh.codec_ctx, &picture, &got_picture, &pkt ) >= 0 && got_picture )
        {
            first_valid_frame_number = i - min( get_decoder_delay( vh.codec_ctx ), vh.delay_count );
            if( first_valid_frame_number > 1 || vi.num_frames == 1 )
            {
                PVideoFrame temp = env->NewVideoFrame( vi );
                if( !temp )
                    env->ThrowError( "LSMASHVideoSource: failed to allocate memory for the first valid video frame data." );
                if( make_frame( vh.codec_ctx, vh.sws_ctx, &picture, temp, env ) )
                    continue;
                first_valid_frame = new PVideoFrame( temp );
            }
            break;
        }
        else if( pkt.data )
            ++ vh.delay_count;
    }
    vh.last_sample_number = vi.num_frames + 1;  /* Force seeking at the first reading. */
}

static int decode_video_sample( video_decode_handler_t *hp, AVFrame *picture, int *got_picture, uint32_t sample_number )
{
    AVPacket pkt;
    if( get_sample( hp->root, hp->track_ID, sample_number, hp->input_buffer, &pkt ) )
        return 1;
    if( pkt.flags == AV_PKT_FLAG_KEY )
        hp->last_rap_number = sample_number;
    avcodec_get_frame_defaults( picture );
    if( avcodec_decode_video2( hp->codec_ctx, picture, got_picture, &pkt ) < 0 )
        return -1;
    return 0;
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
    lsmash_random_access_type rap_type;
    uint32_t distance;  /* distance from the closest random accessible point to the previous. */
    uint32_t number_of_leadings;
    if( lsmash_get_closest_random_accessible_point_detail_from_media_timeline( hp->root, hp->track_ID, decoding_sample_number,
                                                                               rap_number, &rap_type, &number_of_leadings, &distance ) )
        *rap_number = 1;
    int roll_recovery = (rap_type == ISOM_SAMPLE_RANDOM_ACCESS_TYPE_POST_ROLL || rap_type == ISOM_SAMPLE_RANDOM_ACCESS_TYPE_PRE_ROLL);
    int is_leading    = number_of_leadings && (decoding_sample_number - *rap_number <= number_of_leadings);
    if( (roll_recovery || is_leading) && *rap_number > distance )
        *rap_number -= distance;
    return roll_recovery;
}

static void flush_buffers( AVCodecContext *ctx, int *error )
{
    /* Close and reopen the decoder even if the decoder implements avcodec_flush_buffers().
     * It seems this brings about more stable composition when seeking. */
    AVCodec *codec = ctx->codec;
    avcodec_close( ctx );
    if( avcodec_open2( ctx, codec, NULL ) < 0 )
        *error = 1;
}

static uint32_t seek_video( video_decode_handler_t *hp, AVFrame *picture, uint32_t composition_sample_number, uint32_t rap_number, int error_ignorance )
{
    /* Prepare to decode from random accessible sample. */
    flush_buffers( hp->codec_ctx, &hp->error );
    if( hp->error )
        return 0;
    hp->delay_count = 0;
    hp->decode_status = DECODE_REQUIRE_INITIAL;
    int dummy;
    uint32_t i;
    for( i = rap_number; i < composition_sample_number + get_decoder_delay( hp->codec_ctx ); i++ )
    {
        int ret = decode_video_sample( hp, picture, &dummy, i );
        if( ret == -1 && !error_ignorance )
            return 0;
        else if( ret == 1 )
            break;      /* Sample doesn't exist. */
    }
    hp->delay_count = min( get_decoder_delay( hp->codec_ctx ), i - rap_number );
    return i;
}

static int get_picture( video_decode_handler_t *hp, AVFrame *picture, uint32_t current, uint32_t goal, uint32_t sample_count )
{
    if( hp->decode_status == DECODE_INITIALIZING )
    {
        if( hp->delay_count > get_decoder_delay( hp->codec_ctx ) )
            -- hp->delay_count;
        else
            hp->decode_status = DECODE_INITIALIZED;
    }
    int got_picture = 0;
    while( current <= goal )
    {
        int ret = decode_video_sample( hp, picture, &got_picture, current );
        if( ret == -1 )
            return -1;
        else if( ret == 1 )
            break;      /* Sample doesn't exist. */
        ++current;
        if( !got_picture )
            ++ hp->delay_count;
        if( hp->delay_count > get_decoder_delay( hp->codec_ctx ) && hp->decode_status == DECODE_INITIALIZED )
            break;
    }
    /* Flush the last frames. */
    if( current > sample_count && get_decoder_delay( hp->codec_ctx ) )
        while( current <= goal )
        {
            AVPacket pkt;
            av_init_packet( &pkt );
            pkt.data = NULL;
            pkt.size = 0;
            avcodec_get_frame_defaults( picture );
            if( avcodec_decode_video2( hp->codec_ctx, picture, &got_picture, &pkt ) < 0 )
                return -1;
            ++current;
        }
    if( hp->decode_status == DECODE_REQUIRE_INITIAL )
        hp->decode_status = DECODE_INITIALIZING;
    return got_picture ? 0 : -1;
}

PVideoFrame __stdcall LSMASHVideoSource::GetFrame( int n, IScriptEnvironment *env )
{
#define MAX_ERROR_COUNT 3       /* arbitrary */
    uint32_t sample_number = n + 1;     /* For L-SMASH, sample_number is 1-origin. */
    if( sample_number < first_valid_frame_number || vi.num_frames == 1 )
    {
        /* Copy the first valid video frame. */
        vh.last_sample_number = vi.num_frames + 1;  /* Force seeking at the next access for valid video sample. */
        return *first_valid_frame;
    }
    PVideoFrame frame = env->NewVideoFrame( vi );
    if( vh.error )
        return frame;
    AVFrame picture;            /* Decoded video data will be stored here. */
    uint32_t start_number;      /* number of sample, for normal decoding, where decoding starts excluding decoding delay */
    uint32_t rap_number;        /* number of sample, for seeking, where decoding starts excluding decoding delay */
    int seek_mode = vh.seek_mode;
    int roll_recovery = 0;
    if( sample_number > vh.last_sample_number
     && sample_number <= vh.last_sample_number + vh.forward_seek_threshold )
    {
        start_number = vh.last_sample_number + 1 + vh.delay_count;
        rap_number = vh.last_rap_number;
    }
    else
    {
        roll_recovery = find_random_accessible_point( &vh, sample_number, 0, &rap_number );
        if( rap_number == vh.last_rap_number && sample_number > vh.last_sample_number )
        {
            roll_recovery = 0;
            start_number = vh.last_sample_number + 1 + vh.delay_count;
        }
        else
        {
            /* Require starting to decode from random accessible sample. */
            vh.last_rap_number = rap_number;
            start_number = seek_video( &vh, &picture, sample_number, rap_number, roll_recovery || seek_mode != SEEK_MODE_NORMAL );
        }
    }
    /* Get desired picture. */
    int error_count = 0;
    while( start_number == 0 || get_picture( &vh, &picture, start_number, sample_number + vh.delay_count, vi.num_frames ) )
    {
        /* Failed to get desired picture. */
        if( vh.error || seek_mode == SEEK_MODE_AGGRESSIVE )
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
            vh.last_rap_number = rap_number;
        }
        start_number = seek_video( &vh, &picture, sample_number, rap_number, roll_recovery || seek_mode != SEEK_MODE_NORMAL );
    }
    vh.last_sample_number = sample_number;
    if( make_frame( vh.codec_ctx, vh.sws_ctx, &picture, frame, env ) )
        env->ThrowError( "LSMASHVideoSource: failed to make a frame." );
    return frame;
#undef MAX_ERROR_COUNT
}

typedef struct
{
    lsmash_root_t     *root;
    uint32_t           track_ID;
    AVCodecContext    *codec_ctx;
    AVFormatContext   *format_ctx;
    uint8_t           *input_buffer;
    AVFrame            frame_buffer;
    AVPacket           packet;
    uint32_t           frame_count;
    uint32_t           delay_count;
    uint32_t           frame_length;
    uint32_t           last_frame_number;
    uint64_t           last_remainder_size;
    uint64_t           last_remainder_offset;
    uint64_t           next_pcm_sample_number;
    uint64_t           skip_samples;
    int                upsampling;
    int                planes;
    int                input_block_align;
    int                output_block_align;
    int                error;
} audio_decode_handler_t;

class LSMASHAudioSource : public IClip
{
private:
    VideoInfo              vi;
    audio_decode_handler_t ah;
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
    memset( &vi, 0, sizeof(VideoInfo) );
    memset( &ah, 0, sizeof(audio_decode_handler_t) );
    get_audio_track( source, track_number, skip_priming, env );
    lsmash_discard_boxes( ah.root );
    prepare_audio_decoding( env );
}

LSMASHAudioSource::~LSMASHAudioSource()
{
    if( ah.input_buffer )
        av_free( ah.input_buffer );
    if( ah.codec_ctx )
        avcodec_close( ah.codec_ctx );
    if( ah.format_ctx )
        avformat_close_input( &ah.format_ctx );
    lsmash_destroy_root( ah.root );
}

uint32_t LSMASHAudioSource::open_file( const char *source, IScriptEnvironment *env )
{
    /* L-SMASH */
    ah.root = lsmash_open_movie( source, LSMASH_FILE_MODE_READ );
    if( !ah.root )
        env->ThrowError( "LSMASHAudioSource: failed to lsmash_open_movie." );
    lsmash_movie_parameters_t movie_param;
    lsmash_initialize_movie_parameters( &movie_param );
    lsmash_get_movie_parameters( ah.root, &movie_param );
    if( movie_param.number_of_tracks == 0 )
        env->ThrowError( "LSMASHAudioSource: the number of tracks equals 0." );
    /* libavformat */
    av_register_all();
    avcodec_register_all();
    if( avformat_open_input( &ah.format_ctx, source, NULL, NULL ) )
        env->ThrowError( "LSMASHAudioSource: failed to avformat_open_input." );
    if( avformat_find_stream_info( ah.format_ctx, NULL ) < 0 )
        env->ThrowError( "LSMASHAudioSource: failed to avformat_find_stream_info." );
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
            ah.track_ID = lsmash_get_track_ID( ah.root, i );
            if( ah.track_ID == 0 )
                env->ThrowError( "LSMASHAudioSource: failed to find audio track." );
            lsmash_initialize_media_parameters( &media_param );
            if( lsmash_get_media_parameters( ah.root, ah.track_ID, &media_param ) )
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
        ah.track_ID = lsmash_get_track_ID( ah.root, track_number );
        if( ah.track_ID == 0 )
            env->ThrowError( "LSMASHAudioSource: failed to find audio track." );
        lsmash_initialize_media_parameters( &media_param );
        if( lsmash_get_media_parameters( ah.root, ah.track_ID, &media_param ) )
            env->ThrowError( "LSMASHAudioSource: failed to get media parameters." );
        if( media_param.handler_type != ISOM_MEDIA_HANDLER_TYPE_AUDIO_TRACK )
            env->ThrowError( "LSMASHAudioSource: the track you specified is not an audio track." );
    }
    if( lsmash_construct_timeline( ah.root, ah.track_ID ) )
        env->ThrowError( "LSMASHAudioSource: failed to get construct timeline." );
    if( skip_priming )
    {
        uint32_t ctd_shift;
        if( lsmash_get_composition_to_decode_shift_from_media_timeline( ah.root, ah.track_ID, &ctd_shift ) )
            env->ThrowError( "LSMASHAudioSource: failed to get the timeline shift." );
        ah.skip_samples = ctd_shift + get_start_time( ah.root, ah.track_ID );
    }
    ah.frame_count = lsmash_get_sample_count_in_media_timeline( ah.root, ah.track_ID );
    vi.num_audio_samples = lsmash_get_media_duration( ah.root, ah.track_ID );
    /* libavformat */
    for( i = 0; i < ah.format_ctx->nb_streams && ah.format_ctx->streams[i]->codec->codec_type != AVMEDIA_TYPE_AUDIO; i++ );
    if( i == ah.format_ctx->nb_streams )
        env->ThrowError( "LSMASHAudioSource: failed to find stream by libavformat." );
    /* libavcodec */
    AVStream *stream = ah.format_ctx->streams[i];
    ah.codec_ctx = stream->codec;
    AVCodec *codec = avcodec_find_decoder( ah.codec_ctx->codec_id );
    if( !codec )
        env->ThrowError( "LSMASHAudioSource: failed to find %s decoder.", codec->name );
    ah.codec_ctx->thread_count = 0;
    if( avcodec_open2( ah.codec_ctx, codec, NULL ) < 0 )
        env->ThrowError( "LSMASHAudioSource: failed to avcodec_open2." );
}

void LSMASHAudioSource::prepare_audio_decoding( IScriptEnvironment *env )
{
    /* Note: the input buffer for avcodec_decode_audio4 must be FF_INPUT_BUFFER_PADDING_SIZE larger than the actual read bytes. */
    uint32_t input_buffer_size = lsmash_get_max_sample_size_in_media_timeline( ah.root, ah.track_ID );
    if( input_buffer_size == 0 )
        env->ThrowError( "LSMASHAudioSource: No valid audio sample found." );
    ah.input_buffer = (uint8_t *)av_mallocz( input_buffer_size + FF_INPUT_BUFFER_PADDING_SIZE );
    if( !ah.input_buffer )
        env->ThrowError( "LSMASHAudioSource: failed to allocate memory to the input buffer for audio." );
    avcodec_get_frame_defaults( &ah.frame_buffer );
    ah.frame_length = ah.codec_ctx->frame_size;
    if( vi.num_audio_samples * 2 <= ah.frame_count * ah.frame_length )
    {
        /* for HE-AAC upsampling */
        ah.upsampling         = 2;
        ah.skip_samples      *= ah.upsampling;
        vi.num_audio_samples *= ah.upsampling;
    }
    else
        ah.upsampling = 1;
    vi.num_audio_samples       -= ah.skip_samples;
    vi.nchannels                = ah.codec_ctx->channels;
    vi.audio_samples_per_second = ah.codec_ctx->sample_rate;
    ah.next_pcm_sample_number   = vi.num_audio_samples + 1;   /* Force seeking at the first reading. */
    ah.planes                   = av_sample_fmt_is_planar( ah.codec_ctx->sample_fmt ) ? vi.nchannels : 1;
    ah.output_block_align       = vi.nchannels * av_get_bytes_per_sample( ah.codec_ctx->sample_fmt );
    ah.input_block_align        = ah.output_block_align / ah.planes;
    switch ( ah.codec_ctx->sample_fmt )
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
            vi.sample_type = SAMPLE_INT32;
            break;
        case AV_SAMPLE_FMT_FLT :
        case AV_SAMPLE_FMT_FLTP :
            vi.sample_type = SAMPLE_FLOAT;
            break;
        default :
            env->ThrowError( "LSMASHAudioSource: %s is not supported.", av_get_sample_fmt_name( ah.codec_ctx->sample_fmt ) );
    }
}

static inline int get_frame_length( audio_decode_handler_t *hp, uint32_t frame_number, uint32_t *frame_length )
{
    if( hp->frame_length == 0 )
    {
        /* variable frame length
         * Guess the frame length from sample duration. */
        if( lsmash_get_sample_delta_from_media_timeline( hp->root, hp->track_ID, frame_number, frame_length ) )
            return -1;
    }
    else
        /* constant frame length */
        *frame_length = hp->frame_length;
    return 0;
}

static uint32_t get_preroll_samples( audio_decode_handler_t *hp, uint32_t *frame_number )
{
    /* Some audio CODEC requires pre-roll for correct composition. */
    lsmash_sample_property_t prop;
    if( lsmash_get_sample_property_from_media_timeline( hp->root, hp->track_ID, *frame_number, &prop ) )
        return 0;
    if( prop.pre_roll.distance == 0 )
        return 0;
    uint32_t preroll_samples = 0;
    for( uint32_t i = 0; i < prop.pre_roll.distance; i++ )
    {
        if( *frame_number > 1 )
            --(*frame_number);
        else
            break;
        uint32_t frame_length;
        if( get_frame_length( hp, *frame_number, &frame_length ) )
            break;
        preroll_samples += frame_length;
    }
    return preroll_samples;
}

static inline void waste_decoded_audio_samples( audio_decode_handler_t *hp, uint64_t wasted_data_size, uint8_t **p_buf, uint64_t data_offset )
{
    for( uint64_t i = 0; i < wasted_data_size; i += hp->input_block_align )
        for( int j = 0; j < hp->planes; j++ )
            for( uint64_t k = 0; k < hp->input_block_align; k++ )
            {
                **p_buf = hp->frame_buffer.data[j][i + k + data_offset];
                ++(*p_buf);
            }
}

static inline void waste_remainder_audio_samples( audio_decode_handler_t *hp, uint64_t wasted_data_size, uint8_t **p_buf )
{
    waste_decoded_audio_samples( hp, wasted_data_size, p_buf, hp->last_remainder_offset );
    hp->last_remainder_size -= wasted_data_size;
    if( hp->last_remainder_size == 0 )
        hp->last_remainder_offset = 0;
}

static inline void put_silence_audio_samples( uint64_t silence_data_size, uint8_t **p_buf )
{
    for( uint64_t i = 0; i < silence_data_size; i++ )
    {
        **p_buf = 0;
        ++(*p_buf);
    }
}

void __stdcall LSMASHAudioSource::GetAudio( void *buf, __int64 start, __int64 wanted_length, IScriptEnvironment *env )
{
    uint32_t frame_number;
    uint64_t data_offset;
    uint64_t copy_size;
    uint64_t output_length = 0;
    uint64_t block_align = ah.input_block_align;
    if( start > 0 && start == ah.next_pcm_sample_number )
    {
        frame_number = ah.last_frame_number;
        if( ah.last_remainder_size && ah.frame_buffer.data[0] )
        {
            copy_size = min( ah.last_remainder_size, wanted_length * block_align );
            waste_remainder_audio_samples( &ah, copy_size, (uint8_t **)&buf );
            uint64_t copied_length = copy_size / block_align;
            output_length += copied_length;
            wanted_length -= copied_length;
            if( wanted_length <= 0 )
                goto audio_out;
        }
        if( ah.packet.size <= 0 )
            ++frame_number;
        data_offset = 0;
    }
    else
    {
        /* Seek audio stream. */
        flush_buffers( ah.codec_ctx, &ah.error );
        if( ah.error )
            return;
        ah.delay_count            = 0;
        ah.last_remainder_size          = 0;
        ah.last_remainder_offset        = 0;
        ah.next_pcm_sample_number = 0;
        ah.last_frame_number      = 0;
        uint64_t start_frame_pos;
        if( start >= 0 )
            start_frame_pos = start;
        else
        {
            uint64_t silence_length = -start;
            put_silence_audio_samples( silence_length * ah.output_block_align, (uint8_t **)&buf );
            output_length += silence_length;
            wanted_length -= silence_length;
            start_frame_pos = 0;
        }
        start_frame_pos += ah.skip_samples;
        frame_number = 1;
        uint64_t next_frame_pos = 0;
        uint32_t frame_length   = 0;
        do
        {
            if( get_frame_length( &ah, frame_number, &frame_length ) )
                break;
            next_frame_pos += (uint64_t)frame_length * ah.upsampling;
            if( start_frame_pos < next_frame_pos )
                break;
            ++frame_number;
        } while( frame_number <= ah.frame_count );
        uint32_t preroll_samples = get_preroll_samples( &ah, &frame_number );
        data_offset = (start_frame_pos + (preroll_samples + frame_length) * ah.upsampling - next_frame_pos) * block_align;
    }
    do
    {
        AVPacket *pkt = &ah.packet;
        if( frame_number > ah.frame_count )
        {
            if( ah.delay_count )
            {
                /* Null packet */
                av_init_packet( pkt );
                pkt->data = NULL;
                pkt->size = 0;
                -- ah.delay_count;
            }
            else
            {
                copy_size = 0;
                goto audio_out;
            }
        }
        else if( pkt->size <= 0 )
            get_sample( ah.root, ah.track_ID, frame_number, ah.input_buffer, pkt );
        int output_audio = 0;
        do
        {
            ah.last_remainder_size   = 0;
            ah.last_remainder_offset = 0;
            copy_size = 0;
            int decode_complete;
            int wasted_data_length = avcodec_decode_audio4( ah.codec_ctx, &ah.frame_buffer, &decode_complete, pkt );
            if( wasted_data_length < 0 )
            {
                pkt->size = 0;  /* Force to get the next sample. */
                break;
            }
            if( pkt->data )
            {
                pkt->size -= wasted_data_length;
                pkt->data += wasted_data_length;
            }
            else if( !decode_complete )
                goto audio_out;
            if( decode_complete && ah.frame_buffer.data[0] )
            {
                uint64_t decoded_data_size = ah.frame_buffer.nb_samples * block_align;
                if( decoded_data_size > data_offset )
                {
                    copy_size = min( decoded_data_size - data_offset, wanted_length * block_align );
                    waste_decoded_audio_samples( &ah, copy_size, (uint8_t **)&buf, data_offset );
                    uint64_t copied_length = copy_size / block_align;
                    output_length += copied_length;
                    wanted_length -= copied_length;
                    data_offset = 0;
                    if( wanted_length <= 0 )
                    {
                        ah.last_remainder_size = decoded_data_size - copy_size;
                        goto audio_out;
                    }
                }
                else
                    data_offset -= decoded_data_size;
                output_audio = 1;
            }
        } while( pkt->size > 0 );
        if( !output_audio && pkt->data )    /* Count audio frame delay only if feeding non-NULL packet. */
            ++ ah.delay_count;
        ++frame_number;
    } while( 1 );
audio_out:
    ah.next_pcm_sample_number = start + output_length;
    ah.last_frame_number = frame_number;
    if( ah.last_remainder_size )
        ah.last_remainder_offset += copy_size;
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
