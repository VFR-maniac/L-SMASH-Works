/*****************************************************************************
 * video_output.c
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

#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

#include "colorspace.h"
#include "video_output.h"

int convert_colorspace
(
    video_output_handler_t *vohp,
    AVCodecContext         *ctx,
    AVFrame                *picture,
    uint8_t                *buf
)
{
    /* Convert color space. We don't change the presentation resolution. */
    au_video_output_handler_t *au_vohp = (au_video_output_handler_t *)vohp->private_handler;
    enum AVPixelFormat *input_pixel_format = (enum AVPixelFormat *)&picture->format;
    avoid_yuv_scale_conversion( input_pixel_format );
    video_scaler_handler_t *vshp = &vohp->scaler;
    if( !vshp->sws_ctx
     || vshp->input_width        != picture->width
     || vshp->input_height       != picture->height
     || vshp->input_pixel_format != *input_pixel_format )
    {
        vshp->sws_ctx = sws_getCachedContext( vshp->sws_ctx,
                                              picture->width, picture->height, picture->format,
                                              picture->width, picture->height, vshp->output_pixel_format,
                                              vshp->flags, NULL, NULL, NULL );
        if( !vshp->sws_ctx )
            return 0;
        vshp->input_width        = picture->width;
        vshp->input_height       = picture->height;
        vshp->input_pixel_format = *input_pixel_format;
        memcpy( buf, au_vohp->back_ground, au_vohp->output_frame_size );
    }
    if( au_vohp->convert_colorspace( ctx, vshp->sws_ctx, picture, buf, au_vohp->output_linesize ) < 0 )
        return 0;
    return au_vohp->output_frame_size;
}

void free_au_video_output_handler( void *private_handler )
{
    au_video_output_handler_t *au_vohp = (au_video_output_handler_t *)private_handler;
    if( !au_vohp )
        return;
    if( au_vohp->back_ground )
        free( au_vohp->back_ground );
    free( au_vohp );
}
