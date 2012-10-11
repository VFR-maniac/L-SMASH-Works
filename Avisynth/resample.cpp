/*****************************************************************************
 * resample.cpp
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

#ifndef __STDC_CONSTANT_MACROS
#define __STDC_CONSTANT_MACROS
#endif

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavresample/avresample.h>
#include <libavutil/samplefmt.h>
#include <libavutil/opt.h>
}

#include "resample.h"

int flush_resampler_buffers( AVAudioResampleContext *avr )
{
    int64_t in_channel_layout;
    int64_t in_sample_fmt;
    int64_t in_sample_rate;
    int64_t out_channel_layout;
    int64_t out_sample_fmt;
    int64_t out_sample_rate;
    av_opt_get_int( avr, "in_channel_layout",   0, &in_channel_layout );
    av_opt_get_int( avr, "in_sample_fmt",       0, &in_sample_fmt );
    av_opt_get_int( avr, "in_sample_rate",      0, &in_sample_rate );
    av_opt_get_int( avr, "out_channel_layout",  0, &out_channel_layout );
    av_opt_get_int( avr, "out_sample_fmt",      0, &out_sample_fmt );
    av_opt_get_int( avr, "out_sample_rate",     0, &out_sample_rate );
    avresample_close( avr );
    av_opt_set_int( avr, "in_channel_layout",   in_channel_layout,  0 );
    av_opt_set_int( avr, "in_sample_fmt",       in_sample_fmt,      0 );
    av_opt_set_int( avr, "in_sample_rate",      in_sample_rate,     0 );
    av_opt_set_int( avr, "out_channel_layout",  out_channel_layout, 0 );
    av_opt_set_int( avr, "out_sample_fmt",      out_sample_fmt,     0 );
    av_opt_set_int( avr, "out_sample_rate",     out_sample_rate,    0 );
    av_opt_set_int( avr, "internal_sample_fmt", AV_SAMPLE_FMT_FLTP, 0 );
    return avresample_open( avr ) < 0 ? -1 : 0;
}

int update_resampler_configuration( AVAudioResampleContext *avr,
                                    uint64_t out_channel_layout, int out_sample_rate, enum AVSampleFormat out_sample_fmt,
                                    uint64_t  in_channel_layout, int  in_sample_rate, enum AVSampleFormat  in_sample_fmt,
                                    int *input_planes, int *input_block_align )
{
    /* Reopen the resampler. */
    avresample_close( avr );
    av_opt_set_int( avr, "in_channel_layout",   in_channel_layout,  0 );
    av_opt_set_int( avr, "in_sample_fmt",       in_sample_fmt,      0 );
    av_opt_set_int( avr, "in_sample_rate",      in_sample_rate,     0 );
    av_opt_set_int( avr, "out_channel_layout",  out_channel_layout, 0 );
    av_opt_set_int( avr, "out_sample_fmt",      out_sample_fmt,     0 );
    av_opt_set_int( avr, "out_sample_rate",     out_sample_rate,    0 );
    av_opt_set_int( avr, "internal_sample_fmt", AV_SAMPLE_FMT_FLTP, 0 );
    if( avresample_open( avr ) < 0 )
        return -1;
    /* Set up the number of planes and the block alignment of input audio frame. */
    int input_channels = av_get_channel_layout_nb_channels( in_channel_layout );
    if( av_sample_fmt_is_planar( in_sample_fmt ) )
    {
        *input_planes      = input_channels;
        *input_block_align = av_get_bytes_per_sample( in_sample_fmt );
    }
    else
    {
        *input_planes      = 1;
        *input_block_align = av_get_bytes_per_sample( in_sample_fmt ) * input_channels;
    }
    return 0;
}

int resample_audio( AVAudioResampleContext *avr, audio_samples_t *out, audio_samples_t *in )
{
    /* Don't call this function over different block aligns. */
    uint8_t *out_orig = *out->data;
    int out_channels = get_channel_layout_nb_channels( out->channel_layout );
    int block_align = av_get_bytes_per_sample( out->sample_format ) * out_channels;
    int request_sample_count = out->sample_count;
    if( avresample_available( avr ) > 0 )
    {
        int resampled_count = avresample_read( avr, out->data, request_sample_count );
        if( resampled_count < 0 )
            return 0;
        request_sample_count -= resampled_count;
        *out->data += resampled_count * block_align;
    }
    uint8_t **in_data = in->sample_count > 0 ? in->data : NULL;
    int in_channels  = get_channel_layout_nb_channels( in->channel_layout );
    int in_linesize  = get_linesize( in_channels, in->sample_count, in->sample_format );
    int out_linesize = get_linesize( out_channels, request_sample_count, out->sample_format );
    int resampled_count = avresample_convert( avr, out->data, out_linesize, request_sample_count,
                                                     in_data,  in_linesize,     in->sample_count );
    if( resampled_count < 0 )
        return 0;
    *out->data += resampled_count * block_align;
    return *out->data - out_orig;
}
