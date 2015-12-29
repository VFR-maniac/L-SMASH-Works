/*****************************************************************************
 * lwlibav_dec.c / lwindex.cpp
 *****************************************************************************
 * Copyright (C) 2012-2015 L-SMASH Works project
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

#ifdef __cplusplus
extern "C"
{
#endif  /* __cplusplus */
#include <libavformat/avformat.h>   /* Demuxer */
#include <libavcodec/avcodec.h>     /* Decoder */
#ifdef __cplusplus
}
#endif  /* __cplusplus */

#include "utils.h"
#include "lwlibav_dec.h"
#include "qsv.h"

static AVCodec *find_decoder
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

static int open_decoder
(
    AVCodecContext *ctx,
    const AVCodec  *codec
)
{
    int ret = avcodec_open2( ctx, codec, NULL );
    if( is_qsv_decoder( ctx->codec ) )
        ret = do_qsv_decoder_workaround( ctx );
    return ret;
}

int find_and_open_decoder
(
    AVCodecContext *ctx,
    enum AVCodecID  codec_id,
    const char    **preferred_decoder_names,
    int             threads
)
{
    AVCodec *codec = find_decoder( codec_id, preferred_decoder_names );
    if( !codec )
        return -1;
    ctx->thread_count = threads;
    return (open_decoder( ctx, codec ) < 0) ? -1 : 0;
}

void lwlibav_flush_buffers
(
    lwlibav_decode_handler_t *dhp
)
{
    /* Close and reopen the decoder even if the decoder implements avcodec_flush_buffers().
     * It seems this brings about more stable composition when seeking. */
    AVCodecContext *ctx   = dhp->format->streams[ dhp->stream_index ]->codec;
    const AVCodec  *codec = ctx->codec;
    avcodec_close( ctx );
    ctx->codec_id = AV_CODEC_ID_NONE;   /* AVCodecContext.codec_id is supposed to be set properly in avcodec_open2().
                                         * This avoids avcodec_open2() failure by the difference of enum AVCodecID.
                                         * For instance, when stream is encoded as AC-3,
                                         * AVCodecContext.codec_id might have been set to AV_CODEC_ID_EAC3
                                         * while AVCodec.id is set to AV_CODEC_ID_AC3. */
    if( open_decoder( ctx, codec ) < 0 )
    {
        dhp->error = 1;
        lw_log_show( &dhp->lh, LW_LOG_FATAL,
                     "Failed to flush buffers.\n"
                     "It is recommended you reopen the file." );
    }
    dhp->exh.delay_count = 0;
}

void lwlibav_update_configuration
(
    lwlibav_decode_handler_t *dhp,
    uint32_t                  frame_number,
    int                       extradata_index,
    int64_t                   rap_pos
)
{
    lwlibav_extradata_handler_t *exhp = &dhp->exh;
    if( exhp->entry_count == 0 || extradata_index < 0 )
    {
        /* No need to update the extradata. */
        exhp->current_index = extradata_index;
        lwlibav_flush_buffers( dhp );
        return;
    }
    AVCodecContext *ctx = dhp->format->streams[ dhp->stream_index ]->codec;
    void *app_specific = ctx->opaque;
    avcodec_close( ctx );
    if( ctx->extradata )
    {
        av_freep( &ctx->extradata );
        ctx->extradata_size = 0;
    }
    /* Find an appropriate decoder. */
    char error_string[96] = { 0 };
    lwlibav_extradata_t *entry = &exhp->entries[extradata_index];
    const AVCodec *codec = find_decoder( entry->codec_id, dhp->preferred_decoder_names );
    if( !codec )
    {
        strcpy( error_string, "Failed to find the decoder.\n" );
        goto fail;
    }
    /* Get decoder default settings. */
    int thread_count = ctx->thread_count;
    if( avcodec_get_context_defaults3( ctx, codec ) < 0 )
    {
        strcpy( error_string, "Failed to get CODEC default.\n" );
        goto fail;
    }
    /* Set up decoder basic settings. */
    if( ctx->codec_type == AVMEDIA_TYPE_VIDEO )
        set_video_basic_settings( dhp, frame_number );
    else
        set_audio_basic_settings( dhp, frame_number );
    /* Update extradata. */
    if( entry->extradata_size > 0 )
    {
        ctx->extradata = (uint8_t *)av_malloc( entry->extradata_size + FF_INPUT_BUFFER_PADDING_SIZE );
        if( !ctx->extradata )
        {
            strcpy( error_string, "Failed to allocate extradata.\n" );
            goto fail;
        }
        ctx->extradata_size = entry->extradata_size;
        memcpy( ctx->extradata, entry->extradata, ctx->extradata_size );
        memset( ctx->extradata + ctx->extradata_size, 0, FF_INPUT_BUFFER_PADDING_SIZE );
    }
    /* AVCodecContext.codec_id is supposed to be set properly in avcodec_open2().
     * See lwlibav_flush_buffers(), why this is needed. */
    ctx->codec_id  = AV_CODEC_ID_NONE;
    /* This is needed by some CODECs such as UtVideo and raw video. */
    ctx->codec_tag = entry->codec_tag;
    /* Open an appropriate decoder.
     * Here, we force single threaded decoding since some decoder doesn't do its proper initialization with multi-threaded decoding. */
    ctx->thread_count = 1;
    if( open_decoder( ctx, codec ) < 0 )
    {
        strcpy( error_string, "Failed to open decoder.\n" );
        goto fail;
    }
    exhp->current_index = extradata_index;
    exhp->delay_count   = 0;
    /* Set up decoder basic settings by actual decoding. */
    if( ctx->codec_type == AVMEDIA_TYPE_VIDEO
      ? try_decode_video_frame( dhp, frame_number, rap_pos, error_string ) < 0
      : try_decode_audio_frame( dhp, frame_number, error_string ) < 0 )
        goto fail;
    /* Reopen/flush with the requested number of threads. */
    ctx->thread_count = thread_count;
    int width  = ctx->width;
    int height = ctx->height;
    lwlibav_flush_buffers( dhp );
    ctx->get_buffer2 = exhp->get_buffer ? exhp->get_buffer : avcodec_default_get_buffer2;
    ctx->opaque      = app_specific;
    /* avcodec_open2() may have changed resolution unexpectedly. */
    ctx->width       = width;
    ctx->height      = height;
    return;
fail:
    exhp->delay_count = 0;
    dhp->error = 1;
    lw_log_show( &dhp->lh, LW_LOG_FATAL,
                 "%sIt is recommended you reopen the file.", error_string );
}

int lwlibav_get_av_frame
(
    AVFormatContext *format_ctx,
    int              stream_index,
    uint32_t         frame_number,
    AVPacket        *pkt
)
{
    av_packet_unref( pkt );
    av_init_packet( pkt );
    /* Get a packet as the requested frame physically. */
    while( read_av_frame( format_ctx, pkt ) >= 0 )
    {
        if( pkt->stream_index != stream_index )
        {
            av_packet_unref( pkt );
            continue;
        }
        /* libavformat seek results might be inaccurate.
         * So, you might get a returnable packet exceeding frame_count and, if present, return it. */
        return 0;
    }
    /* Return a null packet. */
    pkt->data = NULL;
    pkt->size = 0;
    return 1;
}

int lw_copy_av_packet
(
    AVPacket *dst,
    AVPacket *src
)
{
#if LIBAVUTIL_VERSION_MICRO >= 100
    return av_copy_packet( dst, src );
#else
    int ret;
    if( (ret = av_new_packet( dst, src->size )) != 0
     || (ret = av_packet_copy_props( dst, src )) != 0 )
    {
        av_packet_unref( dst );
        return ret;
    }
    memcpy( dst->data, src->data, src->size );
    return 0;
#endif
}
