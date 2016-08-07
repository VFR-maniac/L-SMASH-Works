/*****************************************************************************
 * decode.c / decode.cpp
 *****************************************************************************
 * Copyright (C) 2012-2016 L-SMASH Works project
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

/* This file is available under an ISC license. */

#ifdef __cplusplus
extern "C"
{
#endif  /* __cplusplus */
#include <libavcodec/avcodec.h>
#ifdef __cplusplus
}
#endif  /* __cplusplus */

#include "decode.h"
#include "qsv.h"

const AVCodec *find_decoder
(
    enum AVCodecID  codec_id,
    const char    **preferred_decoder_names
)
{
    AVCodec *codec = avcodec_find_decoder( codec_id );
    if( codec && preferred_decoder_names )
        for( const char **decoder_name = preferred_decoder_names; *decoder_name != NULL; decoder_name++ )
        {
            AVCodec *preferred_decoder = avcodec_find_decoder_by_name( *decoder_name );
            if( preferred_decoder
             && preferred_decoder->id == codec->id )
            {
                codec = preferred_decoder;
                break;
            }
        }
    return codec;
}

int open_decoder
(
    AVCodecContext         **ctx,
    const AVCodecParameters *codecpar,
    const AVCodec           *codec,
    const int                thread_count,
    const int                refcounted_frames
)
{
    AVCodecContext *c = avcodec_alloc_context3( codec );
    if( !c )
        return -1;
    int ret;
    if( (ret = avcodec_parameters_to_context( c, codecpar )) < 0 )
        goto fail;
    c->thread_count = thread_count;
    c->codec_id     = AV_CODEC_ID_NONE; /* AVCodecContext.codec_id is supposed to be set properly in avcodec_open2().
                                         * This avoids avcodec_open2() failure by the difference of enum AVCodecID.
                                         * For instance, when stream is encoded as AC-3,
                                         * AVCodecContext.codec_id might have been set to AV_CODEC_ID_EAC3
                                         * while AVCodec.id is set to AV_CODEC_ID_AC3. */
    if( (ret = avcodec_open2( c, codec, NULL )) < 0 )
        goto fail;
    if( is_qsv_decoder( c->codec ) )
        if( (ret = do_qsv_decoder_workaround( c )) < 0 )
            goto fail;
    c->refcounted_frames = refcounted_frames;
    *ctx = c;
    return ret;
fail:
    avcodec_free_context( &c );
    return ret;
}

int find_and_open_decoder
(
    AVCodecContext         **ctx,
    const AVCodecParameters *codecpar,
    const char             **preferred_decoder_names,
    const int                thread_count,
    const int                refcounted_frames
)
{
    const AVCodec *codec = find_decoder( codecpar->codec_id, preferred_decoder_names );
    if( !codec )
        return -1;
    return open_decoder( ctx, codecpar, codec, thread_count, refcounted_frames );
}

int decode_video_packet
(
    AVCodecContext *ctx,
    AVFrame        *av_frame,
    int            *got_frame,
    AVPacket       *pkt
)
{
    return avcodec_decode_video2( ctx, av_frame, got_frame, pkt );
}

int decode_audio_packet
(
    AVCodecContext *ctx,
    AVFrame        *av_frame,
    int            *got_frame,
    AVPacket       *pkt
)
{
    return avcodec_decode_audio4( ctx, av_frame, got_frame, pkt );
}
