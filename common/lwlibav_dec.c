/*****************************************************************************
 * lwlibav_dec.c / lwindex.cpp
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

#include "lwlibav_dec.h"

int get_av_frame
(
    AVFormatContext *format_ctx,
    int              stream_index,
    uint8_t        **buffer,
    uint32_t        *buffer_size,
    AVPacket        *pkt
)
{
    AVPacket temp = { 0 };
    av_init_packet( &temp );
    while( read_av_frame( format_ctx, &temp ) >= 0 )
    {
        if( temp.stream_index != stream_index )
        {
            av_free_packet( &temp );
            continue;
        }
        /* Don't trust the first survey of the maximum packet size. It seems various by seeking. */
        if( ((unsigned int)temp.size + FF_INPUT_BUFFER_PADDING_SIZE) > *buffer_size )
        {
            uint8_t *new_buffer = (uint8_t *)av_realloc( *buffer, temp.size + FF_INPUT_BUFFER_PADDING_SIZE );
            if( !new_buffer )
            {
                av_free_packet( &temp );
                continue;
            }
            *buffer      = new_buffer;
            *buffer_size = temp.size + FF_INPUT_BUFFER_PADDING_SIZE;
        }
        *pkt = temp;
        pkt->data = *buffer;
        memcpy( pkt->data, temp.data, temp.size );
        av_free_packet( &temp );
        return 0;
    }
    *pkt = temp;
    pkt->data = NULL;
    pkt->size = 0;
    return 1;
}
