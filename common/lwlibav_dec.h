/*****************************************************************************
 * lwlibav_dec.h
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

/* This file is available under an ISC license. */

#define SEEK_DTS_BASED         0x00000001
#define SEEK_PTS_BASED         0x00000002
#define SEEK_FILE_OFFSET_BASED 0x00000004

typedef struct
{
    int    error;
    void  *message_priv;
    void (*error_message)( void *message_priv, const char *message, ... );
} error_handler_t;

static inline int lavf_open_file
(
    AVFormatContext **format_ctx,
    const char       *file_path,
    error_handler_t  *ehp
)
{
    if( avformat_open_input( format_ctx, file_path, NULL, NULL ) )
    {
        if( ehp->error_message )
            ehp->error_message( ehp->message_priv, "Failed to avformat_open_input." );
        return -1;
    }
    if( avformat_find_stream_info( *format_ctx, NULL ) < 0 )
    {
        if( ehp->error_message )
            ehp->error_message( ehp->message_priv, "Failed to avformat_find_stream_info." );
        return -1;
    }
    return 0;
}

static inline void lavf_close_file( AVFormatContext **format_ctx )
{
    for( unsigned int index = 0; index < (*format_ctx)->nb_streams; index++ )
        if( avcodec_is_open( (*format_ctx)->streams[index]->codec ) )
            avcodec_close( (*format_ctx)->streams[index]->codec );
    avformat_close_input( format_ctx );
}

static inline int open_decoder
(
    AVCodecContext *ctx,
    enum AVCodecID  codec_id,
    int             threads
)
{
    AVCodec *codec = avcodec_find_decoder( codec_id );
    if( !codec )
        return -1;
    ctx->thread_count = threads;
    return (avcodec_open2( ctx, codec, NULL ) < 0) ? -1 : 0;
}

static inline uint32_t get_decoder_delay( AVCodecContext *ctx )
{
    return ctx->has_b_frames + ((ctx->active_thread_type & FF_THREAD_FRAME) ? ctx->thread_count - 1 : 0);
}

static inline int read_av_frame
(
    AVFormatContext *format_ctx,
    AVPacket        *pkt
)
{
    int ret = av_read_frame( format_ctx, pkt );
    if( ret == AVERROR( EAGAIN ) )
    {
        av_usleep( 10000 );
        return read_av_frame( format_ctx, pkt );
    }
    return ret;
}

static inline void flush_buffers
(
    AVCodecContext  *ctx,
    error_handler_t *ehp
)
{
    /* Close and reopen the decoder even if the decoder implements avcodec_flush_buffers().
     * It seems this brings about more stable composition when seeking. */
    const AVCodec *codec = ctx->codec;
    avcodec_close( ctx );
    if( avcodec_open2( ctx, codec, NULL ) < 0 )
    {
        ehp->error = 1;
        if( ehp->error_message )
            ehp->error_message( ehp->message_priv,
                                "Failed to flush buffers.\n"
                                "It is recommended you reopen the file." );
    }
}

int lw_get_av_frame
(
    AVFormatContext *format_ctx,
    int              stream_index,
    AVPacket        *pkt
);
