/*****************************************************************************
 * audio_output.h
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

static inline enum AVSampleFormat decide_audio_output_sample_format( enum AVSampleFormat input_sample_format,
                                                                     int input_bits_per_sample )
{
    /* AviUtl doesn't support IEEE floating point format. */
    switch ( input_sample_format )
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
            if( input_bits_per_sample == 0 )
                return AV_SAMPLE_FMT_S32;
            else if( input_bits_per_sample <= 8 )
                return AV_SAMPLE_FMT_U8;
            else if( input_bits_per_sample <= 16 )
                return AV_SAMPLE_FMT_S16;
            else
                return AV_SAMPLE_FMT_S32;
    }
}
