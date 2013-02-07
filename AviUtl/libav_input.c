/*****************************************************************************
 * libav_input.c
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
 * However, when distributing its binary file, it will be under LGPL or GPL.
 * Don't distribute it if its license is GPL. */

/* Libav
 * The binary file will be LGPLed or GPLed. */
#include <libavformat/avformat.h>       /* Demuxer */
#include <libavcodec/avcodec.h>         /* Decoder */
#include <libswscale/swscale.h>         /* Colorspace converter */
#include <libavresample/avresample.h>   /* Audio resampler */
#include <libavutil/opt.h>

#include "../common/resample.h"
#include "../common/progress.h"
#include "../common/lwlibav_dec.h"
#include "../common/lwlibav_video.h"
#include "../common/lwlibav_audio.h"
#include "../common/lwindex.h"

#include "lsmashinput.h"
#include "colorspace.h"
#include "audio_output.h"
#include "resource.h"
#include "progress_dlg.h"

typedef struct
{
    struct SwsContext       *sws_ctx;
    int                      scaler_flags;
    enum PixelFormat         output_pixel_format;
    uint32_t                 first_valid_frame_number;
    uint8_t                 *first_valid_frame_data;
    int                      output_linesize;
    uint32_t                 output_frame_size;
    uint8_t                 *back_ground;
    func_convert_colorspace *convert_colorspace;
} video_output_handler_t;

typedef struct libav_handler_tag
{
    UINT                   uType;
    lwlibav_file_handler_t lwh;
    /* Video stuff */
    video_decode_handler_t vdh;
    video_output_handler_t voh;
    /* Audio stuff */
    audio_decode_handler_t adh;
    audio_output_handler_t aoh;
} libav_handler_t;

struct progress_handler_tag
{
    progress_dlg_t dlg;
    const char    *module_name;
    int            template_id;
};

static void message_box_desktop( void *message_priv, const char *message, ... )
{
    char temp[256];
    va_list args;
    va_start( args, message );
    wvsprintf( temp, message, args );
    va_end( args );
    UINT uType = *(UINT *)message_priv;
    MessageBox( HWND_DESKTOP, temp, "lsmashinput", uType );
}

static void open_indicator( progress_handler_t *php )
{
    init_progress_dlg( &php->dlg, php->module_name, php->template_id );
}

static int update_indicator( progress_handler_t *php, const char *message, int percent )
{
    return update_progress_dlg( &php->dlg, message, percent );
}

static void close_indicator( progress_handler_t *php )
{
    close_progress_dlg( &php->dlg );
}

static void *open_file( char *file_path, reader_option_t *opt )
{
    libav_handler_t *hp = lw_malloc_zero( sizeof(libav_handler_t) );
    if( !hp )
        return NULL;
    /* Set up error handler. */
    error_handler_t eh = { 0 };
    eh.message_priv  = &hp->uType;
    eh.error_message = message_box_desktop;
    hp->uType = MB_ICONERROR | MB_OK;
    /* Set options. */
    lwlibav_option_t lwlibav_opt;
    lwlibav_opt.file_path         = file_path;
    lwlibav_opt.threads           = opt->threads;
    lwlibav_opt.av_sync           = opt->av_sync;
    lwlibav_opt.no_create_index   = opt->no_create_index;
    lwlibav_opt.force_video       = opt->force_video;
    lwlibav_opt.force_video_index = opt->force_video_index;
    lwlibav_opt.force_audio       = opt->force_audio;
    lwlibav_opt.force_audio_index = opt->force_audio_index;
    /* Set up progress indicator. */
    progress_indicator_t indicator;
    indicator.open   = open_indicator;
    indicator.update = update_indicator;
    indicator.close  = close_indicator;
    progress_handler_t ph = { { 0 } };
    ph.module_name = "lsmashinput.aui";
    ph.template_id = IDD_PROGRESS_ABORTABLE;
    /* Construct index. */
    if( lwlibav_construct_index( &hp->lwh, &hp->vdh, &hp->adh, &hp->aoh, &eh, &lwlibav_opt, &indicator, &ph ) < 0 )
    {
        free( hp );
        return NULL;
    }
    return hp;
}

static int get_video_track( lsmash_handler_t *h )
{
    libav_handler_t *hp = (libav_handler_t *)h->video_private;
    if( lwlibav_get_desired_video_track( hp->lwh.file_path, &hp->vdh, hp->lwh.threads ) < 0 )
        return -1;
    error_handler_t *ehp = &hp->vdh.eh;
    ehp->message_priv  = &hp->uType;
    ehp->error_message = message_box_desktop;
    return 0;
}

static int get_audio_track( lsmash_handler_t *h )
{
    libav_handler_t *hp = (libav_handler_t *)h->audio_private;
    if( lwlibav_get_desired_audio_track( hp->lwh.file_path, &hp->adh, hp->lwh.threads ) < 0 )
        return -1;
    error_handler_t *ehp = &hp->adh.eh;
    ehp->message_priv  = &hp->uType;
    ehp->error_message = message_box_desktop;
    return 0;
}

static int convert_colorspace( libav_handler_t *hp, AVFrame *picture, uint8_t *buf )
{
    /* Convert color space. We don't change the presentation resolution. */
    int64_t width;
    int64_t height;
    int64_t format;
    av_opt_get_int( hp->voh.sws_ctx, "srcw",       0, &width );
    av_opt_get_int( hp->voh.sws_ctx, "srch",       0, &height );
    av_opt_get_int( hp->voh.sws_ctx, "src_format", 0, &format );
    avoid_yuv_scale_conversion( &picture->format );
    if( !hp->voh.sws_ctx || picture->width != width || picture->height != height || picture->format != format )
    {
        hp->voh.sws_ctx = sws_getCachedContext( hp->voh.sws_ctx,
                                                picture->width, picture->height, picture->format,
                                                picture->width, picture->height, hp->voh.output_pixel_format,
                                                hp->voh.scaler_flags, NULL, NULL, NULL );
        if( !hp->voh.sws_ctx )
            return 0;
        memcpy( buf, hp->voh.back_ground, hp->voh.output_frame_size );
    }
    if( hp->voh.convert_colorspace( hp->vdh.ctx, hp->voh.sws_ctx, picture, buf, hp->voh.output_linesize ) < 0 )
        return 0;
    return hp->voh.output_frame_size;
}

static int prepare_video_decoding( lsmash_handler_t *h, video_option_t *opt )
{
    libav_handler_t *hp = (libav_handler_t *)h->video_private;
    if( !hp->vdh.ctx )
        return 0;
    hp->vdh.seek_mode              = opt->seek_mode;
    hp->vdh.forward_seek_threshold = opt->forward_seek_threshold;
    hp->vdh.input_buffer_size     += FF_INPUT_BUFFER_PADDING_SIZE;
    hp->vdh.input_buffer           = av_mallocz( hp->vdh.input_buffer_size );
    if( !hp->vdh.input_buffer )
    {
        DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to allocate memory to the input buffer for video." );
        return -1;
    }
    h->video_sample_count = hp->vdh.frame_count;
    if( hp->vdh.index_entries )
    {
        AVStream *video_stream = hp->vdh.format->streams[ hp->vdh.stream_index ];
        for( int i = 0; i < hp->vdh.index_entries_count; i++ )
        {
            AVIndexEntry *ie = &hp->vdh.index_entries[i];
            if( av_add_index_entry( video_stream, ie->pos, ie->timestamp, ie->size, ie->min_distance, ie->flags ) < 0 )
                return -1;
        }
        av_freep( &hp->vdh.index_entries );
    }
    /* Set up timestamp info. */
    hp->uType = MB_OK;
    lwlibav_setup_timestamp_info( &hp->vdh, &h->framerate_num, &h->framerate_den );
    hp->uType = MB_ICONERROR | MB_OK;
    /* swscale */
    hp->vdh.ctx->width   = hp->vdh.initial_width;
    hp->vdh.ctx->height  = hp->vdh.initial_height;
    hp->vdh.ctx->pix_fmt = hp->vdh.initial_pix_fmt;
    output_colorspace_index index = determine_colorspace_conversion( &hp->vdh.ctx->pix_fmt, &hp->voh.output_pixel_format );
    static const struct
    {
        func_convert_colorspace *convert_colorspace;
        int                      pixel_size;
        output_colorspace_tag    compression;
    } colorspace_table[4] =
        {
            { to_yuy2,            YUY2_SIZE,  OUTPUT_TAG_YUY2 },
            { to_rgb24,           RGB24_SIZE, OUTPUT_TAG_RGB  },
            { to_rgba,            RGBA_SIZE,  OUTPUT_TAG_RGBA },
            { to_yuv16le_to_yc48, YC48_SIZE,  OUTPUT_TAG_YC48 }
        };
    hp->voh.scaler_flags = 1 << opt->scaler;
    if( hp->voh.scaler_flags != SWS_FAST_BILINEAR )
        hp->voh.scaler_flags |= SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP | SWS_ACCURATE_RND;
    hp->voh.sws_ctx = sws_getCachedContext( NULL,
                                            hp->vdh.ctx->width, hp->vdh.ctx->height, hp->vdh.ctx->pix_fmt,
                                            hp->vdh.ctx->width, hp->vdh.ctx->height, hp->voh.output_pixel_format,
                                            hp->voh.scaler_flags, NULL, NULL, NULL );
    if( !hp->voh.sws_ctx )
    {
        DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to get swscale context." );
        return -1;
    }
    hp->voh.convert_colorspace = colorspace_table[index].convert_colorspace;
    /* BITMAPINFOHEADER */
    h->video_format.biSize        = sizeof( BITMAPINFOHEADER );
    h->video_format.biWidth       = hp->vdh.max_width;
    h->video_format.biHeight      = hp->vdh.max_height;
    h->video_format.biBitCount    = colorspace_table[index].pixel_size << 3;
    h->video_format.biCompression = colorspace_table[index].compression;
    /* Set up a black frame of back ground. */
    hp->voh.output_linesize   = MAKE_AVIUTL_PITCH( hp->vdh.max_width * h->video_format.biBitCount );
    hp->voh.output_frame_size = hp->voh.output_linesize * hp->vdh.max_height;
    hp->voh.back_ground       = hp->voh.output_frame_size ? lw_malloc_zero( hp->voh.output_frame_size ) : NULL;
    if( !hp->voh.back_ground )
        return -1;
    if( h->video_format.biCompression == OUTPUT_TAG_YUY2 )
    {
        uint8_t *pic = hp->voh.back_ground;
        for( int i = 0; i < hp->vdh.max_height; i++ )
        {
            for( int j = 0; j < hp->voh.output_linesize; j += 2 )
            {
                pic[j    ] = 0;
                pic[j + 1] = 128;
            }
            pic += hp->voh.output_linesize;
        }
    }
    /* Find the first valid video frame. */
    hp->vdh.seek_flags = (hp->vdh.seek_base & SEEK_FILE_OFFSET_BASED) ? AVSEEK_FLAG_BYTE : hp->vdh.seek_base == 0 ? AVSEEK_FLAG_FRAME : 0;
    if( h->video_sample_count != 1 )
    {
        hp->vdh.seek_flags |= AVSEEK_FLAG_BACKWARD;
        uint32_t rap_number;
        lwlibav_find_random_accessible_point( &hp->vdh, 1, 0, &rap_number );
        int64_t rap_pos = lwlibav_get_random_accessible_point_position( &hp->vdh, rap_number );
        if( av_seek_frame( hp->vdh.format, hp->vdh.stream_index, rap_pos, hp->vdh.seek_flags ) < 0 )
            av_seek_frame( hp->vdh.format, hp->vdh.stream_index, rap_pos, hp->vdh.seek_flags | AVSEEK_FLAG_ANY );
    }
    for( uint32_t i = 1; i <= h->video_sample_count + get_decoder_delay( hp->vdh.ctx ); i++ )
    {
        AVPacket pkt = { 0 };
        get_av_frame( hp->vdh.format, hp->vdh.stream_index, &hp->vdh.input_buffer, &hp->vdh.input_buffer_size, &pkt );
        AVFrame *picture = hp->vdh.frame_buffer;
        avcodec_get_frame_defaults( picture );
        int got_picture;
        if( avcodec_decode_video2( hp->vdh.ctx, picture, &got_picture, &pkt ) >= 0 && got_picture )
        {
            hp->voh.first_valid_frame_number = i - MIN( get_decoder_delay( hp->vdh.ctx ), hp->vdh.delay_count );
            if( hp->voh.first_valid_frame_number > 1 || h->video_sample_count == 1 )
            {
                if( !hp->voh.first_valid_frame_data )
                {
                    hp->voh.first_valid_frame_data = lw_memdup( hp->voh.back_ground, hp->voh.output_frame_size );
                    if( !hp->voh.first_valid_frame_data )
                        return -1;
                }
                if( hp->voh.output_frame_size != convert_colorspace( hp, picture, hp->voh.first_valid_frame_data ) )
                    continue;
            }
            break;
        }
        else if( pkt.data )
            ++ hp->vdh.delay_count;
    }
    hp->vdh.last_frame_number = h->video_sample_count + 1;  /* Force seeking at the first reading. */
    return 0;
}

static int prepare_audio_decoding( lsmash_handler_t *h )
{
    libav_handler_t *hp = (libav_handler_t *)h->audio_private;
    audio_decode_handler_t *adhp = &hp->adh;
    if( !adhp->ctx )
        return 0;
    adhp->input_buffer_size += FF_INPUT_BUFFER_PADDING_SIZE;
    adhp->input_buffer       = av_mallocz( adhp->input_buffer_size );
    if( !adhp->input_buffer )
    {
        DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to allocate memory to the input buffer for audio." );
        return -1;
    }
    audio_output_handler_t *aohp = &hp->aoh;
    h->audio_pcm_sample_count = lwlibav_count_overall_pcm_samples( adhp, aohp->output_sample_rate );
    if( h->audio_pcm_sample_count == 0 )
    {
        DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "No valid audio frame." );
        return -1;
    }
    if( hp->lwh.av_gap && aohp->output_sample_rate != adhp->ctx->sample_rate )
        hp->lwh.av_gap = ((int64_t)hp->lwh.av_gap * aohp->output_sample_rate - 1) / adhp->ctx->sample_rate + 1;
    h->audio_pcm_sample_count += hp->lwh.av_gap;
    adhp->next_pcm_sample_number = h->audio_pcm_sample_count + 1;     /* Force seeking at the first reading. */
    if( adhp->index_entries )
    {
        AVStream *audio_stream = adhp->format->streams[ adhp->stream_index ];
        for( int i = 0; i < adhp->index_entries_count; i++ )
        {
            AVIndexEntry *ie = &adhp->index_entries[i];
            if( av_add_index_entry( audio_stream, ie->pos, ie->timestamp, ie->size, ie->min_distance, ie->flags ) < 0 )
                return -1;
        }
        av_freep( &adhp->index_entries );
    }
    avcodec_get_frame_defaults( adhp->frame_buffer );
    /* Set up resampler. */
    aohp->avr_ctx = avresample_alloc_context();
    if( !aohp->avr_ctx )
    {
        DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to avresample_alloc_context." );
        return -1;
    }
    if( adhp->ctx->channel_layout == 0 )
        adhp->ctx->channel_layout = av_get_default_channel_layout( adhp->ctx->channels );
    aohp->output_sample_format = decide_audio_output_sample_format( aohp->output_sample_format, aohp->output_bits_per_sample );
    av_opt_set_int( aohp->avr_ctx, "in_channel_layout",   adhp->ctx->channel_layout,   0 );
    av_opt_set_int( aohp->avr_ctx, "in_sample_fmt",       adhp->ctx->sample_fmt,       0 );
    av_opt_set_int( aohp->avr_ctx, "in_sample_rate",      adhp->ctx->sample_rate,      0 );
    av_opt_set_int( aohp->avr_ctx, "out_channel_layout",  aohp->output_channel_layout, 0 );
    av_opt_set_int( aohp->avr_ctx, "out_sample_fmt",      aohp->output_sample_format,  0 );
    av_opt_set_int( aohp->avr_ctx, "out_sample_rate",     aohp->output_sample_rate,    0 );
    av_opt_set_int( aohp->avr_ctx, "internal_sample_fmt", AV_SAMPLE_FMT_FLTP,          0 );
    if( avresample_open( aohp->avr_ctx ) < 0 )
    {
        DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to open resampler." );
        return -1;
    }
    /* Decide output Bits Per Sample. */
    int output_channels = av_get_channel_layout_nb_channels( aohp->output_channel_layout );
    if( aohp->output_sample_format == AV_SAMPLE_FMT_S32
     && (aohp->output_bits_per_sample == 0 || aohp->output_bits_per_sample == 24) )
    {
        /* 24bit signed integer output */
        if( adhp->frame_length )
        {
            aohp->resampled_buffer_size = get_linesize( output_channels, adhp->frame_length, aohp->output_sample_format );
            aohp->resampled_buffer      = av_malloc( aohp->resampled_buffer_size );
            if( !aohp->resampled_buffer )
            {
                DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to allocate memory for resampling." );
                return -1;
            }
        }
        aohp->s24_output             = 1;
        aohp->output_bits_per_sample = 24;
    }
    else
        aohp->output_bits_per_sample = av_get_bytes_per_sample( aohp->output_sample_format ) * 8;
    /* Support of WAVEFORMATEXTENSIBLE is much restrictive on AviUtl, so we always use WAVEFORMATEX instead. */
    WAVEFORMATEX *Format = &h->audio_format.Format;
    Format->nChannels       = output_channels;
    Format->nSamplesPerSec  = aohp->output_sample_rate;
    Format->wBitsPerSample  = aohp->output_bits_per_sample;
    Format->nBlockAlign     = (Format->nChannels * Format->wBitsPerSample) / 8;
    Format->nAvgBytesPerSec = Format->nSamplesPerSec * Format->nBlockAlign;
    Format->wFormatTag      = WAVE_FORMAT_PCM;
    Format->cbSize          = 0;
    /* Set up the number of planes and the block alignment of decoded and output data. */
    int input_channels = av_get_channel_layout_nb_channels( adhp->ctx->channel_layout );
    if( av_sample_fmt_is_planar( adhp->ctx->sample_fmt ) )
    {
        aohp->input_planes      = input_channels;
        aohp->input_block_align = av_get_bytes_per_sample( adhp->ctx->sample_fmt );
    }
    else
    {
        aohp->input_planes      = 1;
        aohp->input_block_align = av_get_bytes_per_sample( adhp->ctx->sample_fmt ) * input_channels;
    }
    aohp->output_block_align = Format->nBlockAlign;
    return 0;
}

static int read_video( lsmash_handler_t *h, int frame_number, void *buf )
{
    libav_handler_t        *hp   = (libav_handler_t *)h->video_private;
    video_decode_handler_t *vdhp = &hp->vdh;
    if( vdhp->eh.error )
        return 0;
    video_output_handler_t *vohp = &hp->voh;
    ++frame_number;            /* frame_number is 1-origin. */
    if( frame_number == 1 )
        memcpy( buf, vohp->back_ground, vohp->output_frame_size );
    if( frame_number < vohp->first_valid_frame_number || h->video_sample_count == 1 )
    {
        /* Copy the first valid video frame data. */
        memcpy( buf, vohp->first_valid_frame_data, vohp->output_frame_size );
        vdhp->last_frame_number = h->video_sample_count + 1;    /* Force seeking at the next access for valid video frame. */
        return vohp->output_frame_size;
    }
    if( lwlibav_get_video_frame( vdhp, frame_number, h->video_sample_count ) )
        return 0;
    return convert_colorspace( hp, vdhp->frame_buffer, buf );
}

static int read_audio( lsmash_handler_t *h, int start, int wanted_length, void *buf )
{
    libav_handler_t *hp = (libav_handler_t *)h->audio_private;
    return lwlibav_get_pcm_audio_samples( &hp->adh, &hp->aoh, buf, start, wanted_length );
}

static int is_keyframe( lsmash_handler_t *h, int sample_number )
{
    libav_handler_t *hp = (libav_handler_t *)h->video_private;
    return hp->vdh.frame_list[sample_number + 1].keyframe;
}

static int delay_audio( lsmash_handler_t *h, int *start, int wanted_length, int audio_delay )
{
    /* Even if start become negative, its absolute value shall be equal to wanted_length or smaller. */
    libav_handler_t *hp = (libav_handler_t *)h->audio_private;
    int end = *start + wanted_length;
    audio_delay += hp->lwh.av_gap;
    if( *start < audio_delay && end <= audio_delay )
    {
        hp->adh.next_pcm_sample_number = h->audio_pcm_sample_count + 1;   /* Force seeking at the next access for valid audio frame. */
        return 0;
    }
    *start -= audio_delay;
    return 1;
}

static void video_cleanup( lsmash_handler_t *h )
{
    libav_handler_t *hp = (libav_handler_t *)h->video_private;
    if( !hp )
        return;
    if( hp->voh.back_ground )
        free( hp->voh.back_ground );
    if( hp->voh.first_valid_frame_data )
        free( hp->voh.first_valid_frame_data );
    if( hp->voh.sws_ctx )
        sws_freeContext( hp->voh.sws_ctx );
    lwlibav_cleanup_video_decode_handler( &hp->vdh );
}

static void audio_cleanup( lsmash_handler_t *h )
{
    libav_handler_t *hp = (libav_handler_t *)h->audio_private;
    if( !hp )
        return;
    lwlibav_cleanup_audio_decode_handler( &hp->adh );
    lwlibav_cleanup_audio_output_handler( &hp->aoh );
}

static void close_file( void *private_stuff )
{
    libav_handler_t *hp = (libav_handler_t *)private_stuff;
    if( !hp )
        return;
    if( hp->vdh.frame_buffer )
        avcodec_free_frame( &hp->vdh.frame_buffer );
    if( hp->adh.frame_buffer )
        avcodec_free_frame( &hp->adh.frame_buffer );
    if( hp->lwh.file_path )
        free( hp->lwh.file_path );
    free( hp );
}

lsmash_reader_t libav_reader =
{
    LIBAV_READER,
    open_file,
    get_video_track,
    get_audio_track,
    NULL,
    prepare_video_decoding,
    prepare_audio_decoding,
    read_video,
    read_audio,
    is_keyframe,
    delay_audio,
    video_cleanup,
    audio_cleanup,
    close_file
};
