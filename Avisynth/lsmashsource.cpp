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
    void get_first_video_track( const char *source, int threads, IScriptEnvironment *env );
    void prepare_video_decoding( IScriptEnvironment *env );
public:
    LSMASHVideoSource( const char *source, int threads, int seek_mode, uint32_t forward_seek_threshold, IScriptEnvironment *env );
    ~LSMASHVideoSource();
    PVideoFrame __stdcall GetFrame( int n, IScriptEnvironment *env );
    bool __stdcall GetParity( int n ) { return false; }
    void __stdcall GetAudio( void *buf, __int64 start, __int64 count, IScriptEnvironment *env ) {}
    void __stdcall SetCacheHints( int cachehints, int frame_range ) {}
    const VideoInfo& __stdcall GetVideoInfo() { return vi; }
};

LSMASHVideoSource::LSMASHVideoSource( const char *source, int threads, int seek_mode, uint32_t forward_seek_threshold, IScriptEnvironment *env )
{
    memset( &vi, 0, sizeof(VideoInfo) );
    memset( &vh, 0, sizeof(video_decode_handler_t) );
    vh.seek_mode              = seek_mode;
    vh.forward_seek_threshold = forward_seek_threshold;
    first_valid_frame         = NULL;
    get_first_video_track( source, threads, env );
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

static uint32_t open_file( video_decode_handler_t *hp, const char *source, IScriptEnvironment *env )
{
    /* L-SMASH */
    hp->root = lsmash_open_movie( source, LSMASH_FILE_MODE_READ );
    if( !hp->root )
        env->ThrowError( "LSMASHVideoSource: failed to lsmash_open_movie." );
    lsmash_movie_parameters_t movie_param;
    lsmash_initialize_movie_parameters( &movie_param );
    lsmash_get_movie_parameters( hp->root, &movie_param );
    if( movie_param.number_of_tracks == 0 )
        env->ThrowError( "LSMASHVideoSource: the number of tracks equals 0." );
    /* libavformat */
    av_register_all();
    avcodec_register_all();
    if( avformat_open_input( &hp->format_ctx, source, NULL, NULL ) )
        env->ThrowError( "LSMASHVideoSource: failed to avformat_open_input." );
    if( avformat_find_stream_info( hp->format_ctx, NULL ) < 0 )
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

void LSMASHVideoSource::get_first_video_track( const char *source, int threads, IScriptEnvironment *env )
{
    uint32_t number_of_tracks = open_file( &vh, source, env );
    /* L-SMASH */
    uint32_t i;
    lsmash_media_parameters_t media_param;
    for( i = 1; i <= number_of_tracks; i++ )
    {
        vh.track_ID = lsmash_get_track_ID( vh.root, i );
        if( vh.track_ID == 0 )
            env->ThrowError( "LSMASHVideoSource: failed to get find video track." );
        lsmash_initialize_media_parameters( &media_param );
        if( lsmash_get_media_parameters( vh.root, vh.track_ID, &media_param ) )
            env->ThrowError( "LSMASHVideoSource: failed to get media parameters." );
        if( media_param.handler_type == ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK )
            break;
    }
    if( i > number_of_tracks )
        env->ThrowError( "LSMASHVideoSource: failed to find video track." );
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

static int make_frame_420p( AVCodecContext *codec_ctx, struct SwsContext *sws_ctx, AVFrame *picture, PVideoFrame &frame, IScriptEnvironment *env )
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
    env->BitBlt( frame->GetWritePtr( PLANAR_Y ), frame->GetPitch( PLANAR_Y ), dst_data[0], dst_linesize[0], frame->GetRowSize( PLANAR_Y ), frame->GetHeight( PLANAR_Y )); 
    env->BitBlt( frame->GetWritePtr( PLANAR_U ), frame->GetPitch( PLANAR_U ), dst_data[1], dst_linesize[1], frame->GetRowSize( PLANAR_U ), frame->GetHeight( PLANAR_U )); 
    env->BitBlt( frame->GetWritePtr( PLANAR_V ), frame->GetPitch( PLANAR_V ), dst_data[2], dst_linesize[2], frame->GetRowSize( PLANAR_V ), frame->GetHeight( PLANAR_V )); 
    av_free( dst_data[0] );
    return 0;
}

static int make_frame_422( AVCodecContext *codec_ctx, struct SwsContext *sws_ctx, AVFrame *picture, PVideoFrame &frame, IScriptEnvironment *env )
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
            return make_frame_420p;
        case PIX_FMT_YUYV422 :
        case PIX_FMT_YUV422P :
        case PIX_FMT_UYVY422 :
            *output_pixel_format = PIX_FMT_YUYV422;     /* packed YUV 4:2:2, 16bpp */
            *output_pixel_type   = VideoInfo::CS_YUY2;
            return make_frame_422;
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
    enum PixelFormat output_pixel_format;
    make_frame = determine_colorspace_conversion( &vh.codec_ctx->pix_fmt, &output_pixel_format, &vi.pixel_type );
    if( !make_frame )
        env->ThrowError( "LSMASHVideoSource: only support 4:2:0 or 4:2:2 colorspace." );
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

static int find_random_accessible_point( video_decode_handler_t *hp, uint32_t composition_sample_number,
                                         uint32_t decoding_sample_number, uint32_t *rap_number )
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

AVSValue __cdecl CreatLSMASHVideoSource( AVSValue args, void *user_data, IScriptEnvironment *env )
{
    const char *source                 = args[0].AsString();
    int         threads                = args[1].AsInt( 0 );
    int         seek_mode              = args[2].AsInt( 0 );
    uint32_t    forward_seek_threshold = args[3].AsInt( 10 );
    threads                = threads >= 0 ? threads : 0;
    seek_mode              = CLIP_VALUE( seek_mode, 0, 2 );
    forward_seek_threshold = CLIP_VALUE( forward_seek_threshold, 1, 999 );
    return new LSMASHVideoSource( source, threads, seek_mode, forward_seek_threshold, env );  
}

extern "C" __declspec(dllexport) const char * __stdcall AvisynthPluginInit2( IScriptEnvironment *env )
{
    env->AddFunction( "LSMASHVideoSource", "[source]s[threads]i[seek_mode]i[seek_threshold]i", CreatLSMASHVideoSource, 0 );
    return "LSMASHSource";
}
