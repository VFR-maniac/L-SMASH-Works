/*****************************************************************************
 * qsv.c / qsv.cpp
 *****************************************************************************
 * Copyright (C) 2015 L-SMASH Works project
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

#include "cpp_compat.h"

#include <string.h>

#ifdef __cplusplus
extern "C"
{
#endif  /* __cplusplus */
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
#ifdef __cplusplus
}
#endif  /* __cplusplus */

int is_qsv_decoder
(
    const AVCodec *codec
)
{
    if( codec && codec->pix_fmts )
        for( const enum AVPixelFormat *pix_fmt = codec->pix_fmts; *pix_fmt != AV_PIX_FMT_NONE; pix_fmt++ )
            if( *pix_fmt == AV_PIX_FMT_QSV )
                return 1;
    return 0;
}

static inline void restore_extradata
(
    AVCodecContext *ctx,
    uint8_t       **extradata,
    int             extradata_size
)
{
    ctx->extradata_size = extradata_size;
    ctx->extradata      = *extradata;
    *extradata = NULL;
}

/* Workarounds for Intel QuickSync Video Decoder through libavcodec */
int do_qsv_decoder_workaround
(
    AVCodecContext *ctx
)
{
    ctx->has_b_frames = 16; /* the maximum decoder latency for AVC and HEVC frame */
    if( ctx->codec_id != AV_CODEC_ID_H264 )
        return 0;
    /* Feed an initializer packet to the decoder since libavcodec does not append parameter sets to access units
     * containing no IDR NAL units. Here we append parameter sets before the fake IDR NAL unit. Without this,
     * MFXVideoDECODE_DecodeHeader will return MFX_ERR_MORE_DATA. */
    static const uint8_t fake_idr[] = { 0x00, 0x00, 0x00, 0x01, 0x65 }; /* valid for both start-code and size-field prefixes */
    int ret = -1;
    AVPacket initializer = { 0 };
    av_init_packet( &initializer );
    if( ctx->extradata[0] == 1 )
    {
        /* Allocate another extradata for backup. */
        uint8_t *extradata = (uint8_t *)av_mallocz( ctx->extradata_size + FF_INPUT_BUFFER_PADDING_SIZE );
        if( !extradata )
            return ret;
        int extradata_size = ctx->extradata_size;
        /* Convert into AnnexB Byte Stream Format. */
        AVBitStreamFilterContext *bsf = av_bitstream_filter_init( "h264_mp4toannexb" );
        if( !bsf )
        {
            av_free( extradata );
            return ret;
        }
        uint8_t *filtered_data = NULL;
        int      filtered_size;
        memcpy( extradata, ctx->extradata, ctx->extradata_size );   /* backup */
        ctx->extradata[4] |= 0x03;                                  /* Force 4 byte length size fields. */
        ret = av_bitstream_filter_filter( bsf, ctx, NULL,
                                          &filtered_data, &filtered_size,
                                          fake_idr, sizeof(fake_idr), 0 );
        av_bitstream_filter_close( bsf );
        av_freep( &ctx->extradata );
        restore_extradata( ctx, &extradata, extradata_size );
        if( ret <= 0 )
            return -1;
        initializer.data = filtered_data;
        initializer.size = filtered_size;
    }
    else
    {
        initializer.size = ctx->extradata_size + sizeof(fake_idr);
        initializer.data = (uint8_t *)av_mallocz( initializer.size + FF_INPUT_BUFFER_PADDING_SIZE );
        if( !initializer.data )
            return ret;
        memcpy( initializer.data, ctx->extradata, ctx->extradata_size );
        memcpy( initializer.data + ctx->extradata_size, fake_idr, sizeof(fake_idr) );
    }
    /* Initialize the decoder. */
    AVFrame *picture = av_frame_alloc();
    if( picture )
    {
        int got_picture;    /* unused */
        ret = avcodec_decode_video2( ctx, picture, &got_picture, &initializer );
        av_frame_free( &picture );
    }
    return ret;
}
