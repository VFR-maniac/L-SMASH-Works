/*****************************************************************************
 * lwlibav_input.c
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

#include "lwinput.h"
#include "resource.h"
#include "progress_dlg.h"
#include "video_output.h"
#include "audio_output.h"

#include "../common/progress.h"
#include "../common/lwlibav_dec.h"
#include "../common/lwlibav_video.h"
#include "../common/lwlibav_audio.h"
#include "../common/lwindex.h"

typedef struct libav_handler_tag
{
    UINT                           uType;
    lwlibav_file_handler_t         lwh;
    /* Video stuff */
    lwlibav_video_decode_handler_t vdh;
    lwlibav_video_output_handler_t voh;
    /* Audio stuff */
    lwlibav_audio_decode_handler_t adh;
    lwlibav_audio_output_handler_t aoh;
} libav_handler_t;

struct progress_handler_tag
{
    progress_dlg_t dlg;
    const char    *module_name;
    int            template_id;
};

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
    lw_log_handler_t lh = { 0 };
    lh.level    = LW_LOG_FATAL;
    lh.priv     = &hp->uType;
    lh.show_log = NULL;
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
    lwlibav_opt.apply_repeat_flag = opt->video_opt.apply_repeat_flag;
    lwlibav_opt.field_dominance   = opt->video_opt.field_dominance;
    /* Set up progress indicator. */
    progress_indicator_t indicator;
    indicator.open   = open_indicator;
    indicator.update = update_indicator;
    indicator.close  = close_indicator;
    progress_handler_t ph = { { 0 } };
    ph.module_name = "lwinput.aui";
    ph.template_id = IDD_PROGRESS_ABORTABLE;
    /* Construct index. */
    if( lwlibav_construct_index( &hp->lwh, &hp->vdh, &hp->voh, &hp->adh, &hp->aoh, &lh, &lwlibav_opt, &indicator, &ph ) < 0 )
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
    lw_log_handler_t *lhp = &hp->vdh.lh;
    lhp->level    = LW_LOG_WARNING;
    lhp->priv     = &hp->uType;
    lhp->show_log = au_message_box_desktop;
    return 0;
}

static int get_audio_track( lsmash_handler_t *h )
{
    libav_handler_t *hp = (libav_handler_t *)h->audio_private;
    if( lwlibav_get_desired_audio_track( hp->lwh.file_path, &hp->adh, hp->lwh.threads ) < 0 )
        return -1;
    lw_log_handler_t *lhp = &hp->adh.lh;
    lhp->level    = LW_LOG_WARNING;
    lhp->priv     = &hp->uType;
    lhp->show_log = au_message_box_desktop;
    return 0;
}

static int prepare_video_decoding( lsmash_handler_t *h, video_option_t *opt )
{
    libav_handler_t *hp = (libav_handler_t *)h->video_private;
    lwlibav_video_decode_handler_t *vdhp = &hp->vdh;
    if( !vdhp->ctx )
        return 0;
    vdhp->seek_mode              = opt->seek_mode;
    vdhp->forward_seek_threshold = opt->forward_seek_threshold;
    lwlibav_video_output_handler_t *vohp = &hp->voh;
    h->video_sample_count = vohp->frame_count;
    /* Import AVIndexEntrys. */
    if( lwlibav_import_av_index_entry( (lwlibav_decode_handler_t *)vdhp ) < 0 )
        return -1;
    /* Set up timestamp info. */
    hp->uType = MB_OK;
    int64_t fps_num = 25;
    int64_t fps_den = 1;
    lwlibav_setup_timestamp_info( &hp->lwh, vdhp, vohp, &fps_num, &fps_den );
    h->framerate_num = (int)fps_num;
    h->framerate_den = (int)fps_den;
    hp->uType = MB_ICONERROR | MB_OK;
    /* Set up the initial input format. */
    vdhp->ctx->width      = vdhp->initial_width;
    vdhp->ctx->height     = vdhp->initial_height;
    vdhp->ctx->pix_fmt    = vdhp->initial_pix_fmt;
    vdhp->ctx->colorspace = vdhp->initial_colorspace;
    /* Set up video rendering. */
    vdhp->exh.get_buffer = au_setup_video_rendering( vohp, vdhp->ctx, opt, &h->video_format, vdhp->max_width, vdhp->max_height );
    if( !vdhp->exh.get_buffer )
        return -1;
#ifndef DEBUG_VIDEO
    vdhp->lh.level = LW_LOG_FATAL;
#endif
    /* Find the first valid video frame. */
    if( lwlibav_find_first_valid_video_frame( vdhp ) < 0 )
        return -1;
    /* Force seeking at the first reading. */
    vdhp->last_frame_number = h->video_sample_count + 1;
    return 0;
}

static int prepare_audio_decoding( lsmash_handler_t *h, audio_option_t *opt )
{
    libav_handler_t *hp = (libav_handler_t *)h->audio_private;
    lwlibav_audio_decode_handler_t *adhp = &hp->adh;
    if( !adhp->ctx )
        return 0;
    /* Import AVIndexEntrys. */
    if( lwlibav_import_av_index_entry( (lwlibav_decode_handler_t *)adhp ) < 0 )
        return -1;
    avcodec_get_frame_defaults( adhp->frame_buffer );
#ifndef DEBUG_AUDIO
    adhp->lh.level = LW_LOG_FATAL;
#endif
    lwlibav_audio_output_handler_t *aohp = &hp->aoh;
    if( au_setup_audio_rendering( aohp, adhp->ctx, opt, &h->audio_format.Format ) < 0 )
        return -1;
    /* Count the number of PCM audio samples. */
    h->audio_pcm_sample_count = lwlibav_count_overall_pcm_samples( adhp, aohp->output_sample_rate );
    if( h->audio_pcm_sample_count == 0 )
    {
        DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "No valid audio frame." );
        return -1;
    }
    if( hp->lwh.av_gap && aohp->output_sample_rate != adhp->ctx->sample_rate )
        hp->lwh.av_gap = ((int64_t)hp->lwh.av_gap * aohp->output_sample_rate - 1) / adhp->ctx->sample_rate + 1;
    h->audio_pcm_sample_count += hp->lwh.av_gap;
    /* Force seeking at the first reading. */
    adhp->next_pcm_sample_number = h->audio_pcm_sample_count + 1;
    return 0;
}

static int read_video( lsmash_handler_t *h, int frame_number, void *buf )
{
    libav_handler_t *hp = (libav_handler_t *)h->video_private;
    lwlibav_video_decode_handler_t *vdhp = &hp->vdh;
    if( vdhp->error )
        return 0;
    lwlibav_video_output_handler_t *vohp = &hp->voh;
    ++frame_number;            /* frame_number is 1-origin. */
    if( frame_number == 1 )
    {
        au_video_output_handler_t *au_vohp = (au_video_output_handler_t *)vohp->private_handler;
        memcpy( buf, au_vohp->back_ground, vohp->output_frame_size );
    }
    if( lwlibav_get_video_frame( vdhp, vohp, frame_number ) < 0 )
        return 0;
    return convert_colorspace( vohp, vdhp->ctx, vdhp->frame_buffer, buf );
}

static int read_audio( lsmash_handler_t *h, int start, int wanted_length, void *buf )
{
    libav_handler_t *hp = (libav_handler_t *)h->audio_private;
    return lwlibav_get_pcm_audio_samples( &hp->adh, &hp->aoh, buf, start, wanted_length );
}

static int is_keyframe( lsmash_handler_t *h, int frame_number )
{
    libav_handler_t *hp = (libav_handler_t *)h->video_private;
    return lwlibav_is_keyframe( &hp->vdh, &hp->voh, frame_number + 1 );
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
    lwlibav_cleanup_video_decode_handler( &hp->vdh );
    lwlibav_cleanup_video_output_handler( &hp->voh );
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
