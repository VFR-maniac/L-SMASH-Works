/*****************************************************************************
 * resample.c
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

#include <libavutil/samplefmt.h>
#include <libavresample/avresample.h>

#include "resample.h"

void resample_audio( AVAudioResampleContext *avr, audio_samples_t *out, audio_samples_t *in )
{
    int out_channels = av_get_channel_layout_nb_channels( out->channel_layout );
    if( out_channels <= 0 )
        out_channels = 1;
    int out_linesize = get_linesize( out_channels, out->sample_count, out->sample_format );
    int in_channels  = av_get_channel_layout_nb_channels( in->channel_layout );
    if( in_channels <= 0 )
        in_channels = 1;
    int in_linesize  = get_linesize( in_channels, in->sample_count, in->sample_format );
    int sample_count = avresample_convert( avr, (void **)out->data, out_linesize, out->sample_count,
                                                (void **) in->data,  in_linesize,  in->sample_count );
    if( sample_count > 0 )
        out->data[0] += sample_count * av_get_bytes_per_sample( out->sample_format ) * out_channels;
}
