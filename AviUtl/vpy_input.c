/********************************************************************************
 * vpy_input.c
 ********************************************************************************
 * Copyright (C) 2013 L-SMASH Works project
 *
 * Authors: Yusuke Nakamura <muken.the.vfrmaniac@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *******************************************************************************/

/* This file is available under an MIT license.
 * However, when distributing its binary file, it will be under LGPL or GPL.
 * Don't distribute it if its license is GPL. */

#include "lwinput.h"

#include "VSScript.h"
#include "VSHelper.h"

typedef struct
{
    const VSAPI       *vsapi;
    VSScript          *vsscript;
    VSNodeRef         *node;
    const VSVideoInfo *vi;
    int                buf_linesize;
} vpy_handler_t;

static void *open_file
(
    char            *file_name,
    reader_option_t *opt
)
{
    /* Check file extension. */
    int file_name_length = strlen( file_name );
    if( file_name_length < 5 )
        return NULL;
    char *ext = &file_name[file_name_length - 4];
    if( ext[0] != '.' || ext[1] != 'v' || ext[2] != 'p' || ext[3] != 'y' )
        return NULL;
    /* Try to open the file as avisynth script. */
    vpy_handler_t *hp = lw_malloc_zero( sizeof(vpy_handler_t) );
    if( !hp )
        return NULL;
    if( vsscript_init() == 0 )
    {
        free( hp );
        return NULL;
    }
    hp->vsapi = vsscript_getVSApi();
    if( !hp->vsapi || vsscript_evaluateFile( &hp->vsscript, file_name ) )
        goto fail;
    hp->node = vsscript_getOutput( hp->vsscript, 0 );
    if( !hp->node )
        goto fail;
    hp->vi = hp->vsapi->getVideoInfo( hp->node );
    return hp;
fail:
    if( hp->node )
        hp->vsapi->freeNode( hp->node );
    if( hp->vsscript )
        vsscript_freeScript( hp->vsscript );
    vsscript_finalize();
    free( hp );
    return NULL;
}

static int get_video_track
(
    lsmash_handler_t *h
)
{
    vpy_handler_t *hp = (vpy_handler_t *)h->video_private;
    return isConstantFormat( hp->vi ) && hp->vi->numFrames > 0 ? 0 : -1;
}

static int prepare_video_decoding
(
    lsmash_handler_t *h,
    video_option_t   *opt
)
{
    vpy_handler_t *hp = (vpy_handler_t *)h->video_private;
    h->video_sample_count = hp->vi->numFrames;
    h->framerate_num      = hp->vi->fpsNum;
    h->framerate_den      = hp->vi->fpsDen;
    /* BITMAPINFOHEADER */
    const VSFormat *format = hp->vi->format;
    h->video_format.biSize        = sizeof( BITMAPINFOHEADER );
    h->video_format.biWidth       = hp->vi->width;
    h->video_format.biHeight      = hp->vi->height;
    switch( format->id )
    {
        case pfRGB24 :
            h->video_format.biBitCount    = RGB24_SIZE << 3;
            h->video_format.biCompression = OUTPUT_TAG_RGB;
            break;
        case pfYUV422P8 :
            h->video_format.biBitCount    = YUY2_SIZE << 3;
            h->video_format.biCompression = OUTPUT_TAG_YUY2;
            break;
        default :
            return -1;
    }
    hp->buf_linesize = MAKE_AVIUTL_PITCH( hp->vi->width * h->video_format.biBitCount );
    return 0;
}

static int read_video
(
    lsmash_handler_t *h,
    int               sample_number,
    void             *buf
)
{
    vpy_handler_t *hp = (vpy_handler_t *)h->video_private;
    const VSFrameRef *frame = hp->vsapi->getFrame( sample_number, hp->node, NULL, 0 );
    if( !frame )
        return 0;
    const VSFormat *format = hp->vi->format;
    if( h->video_format.biCompression == OUTPUT_TAG_RGB )
    {
        int src_stride   = hp->vsapi->getStride( frame, 0 );
        int src_offset   = 0;
        int dst_offset   = hp->buf_linesize * (hp->vi->height - 1);
        int dst_row_size = hp->vi->width * format->bytesPerSample * format->numPlanes;
        const uint8_t *r = hp->vsapi->getReadPtr( frame, 0 );
        const uint8_t *g = hp->vsapi->getReadPtr( frame, 1 );
        const uint8_t *b = hp->vsapi->getReadPtr( frame, 2 );
        for( int y = 0; y < hp->vi->height; y++ )
        {
            const uint8_t *src_data[3] = { b + src_offset, g + src_offset, r + src_offset };
            uint8_t *dst_data = buf + dst_offset;
            uint8_t *dst_end  = dst_data + dst_row_size;
            while( dst_data < dst_end )
            {
                (*dst_data++) = *(src_data[0]++);
                (*dst_data++) = *(src_data[1]++);
                (*dst_data++) = *(src_data[2]++);
            }
            src_offset += src_stride;
            dst_offset -= hp->buf_linesize;
        }
    }
    else
    {
        int src_y_stride  = hp->vsapi->getStride( frame, 0 );
        int src_uv_stride = hp->vsapi->getStride( frame, 1 );
        int src_y_offset  = 0;
        int src_uv_offset = 0;
        int dst_offset    = 0;
        int dst_row_size  = hp->vi->width * format->bytesPerSample * 2;
        const uint8_t *y = hp->vsapi->getReadPtr( frame, 0 );
        const uint8_t *u = hp->vsapi->getReadPtr( frame, 1 );
        const uint8_t *v = hp->vsapi->getReadPtr( frame, 2 );
        for( int i = 0; i < hp->vi->height; i++ )
        {
            const uint8_t *src_data[3] = { y + src_y_offset, u + src_uv_offset, v + src_uv_offset };
            uint8_t *dst_data = buf + dst_offset;
            uint8_t *dst_end  = dst_data + dst_row_size;
            while( dst_data < dst_end )
            {
                (*dst_data++) = *(src_data[0]++);
                (*dst_data++) = *(src_data[1]++);
                (*dst_data++) = *(src_data[0]++);
                (*dst_data++) = *(src_data[2]++);
            }
            src_y_offset  += src_y_stride;
            src_uv_offset += src_uv_stride;
            dst_offset    += hp->buf_linesize;
        }
    }
    hp->vsapi->freeFrame( frame );
    return hp->buf_linesize * hp->vi->height;
}

static void close_file
(
    void *private_stuff
)
{
    vpy_handler_t *hp = (vpy_handler_t *)private_stuff;
    if( !hp )
        return;
    if( hp->node )
        hp->vsapi->freeNode( hp->node );
    if( hp->vsscript )
        vsscript_freeScript( hp->vsscript );
    vsscript_finalize();
    free( hp );
}

lsmash_reader_t vpy_reader =
{
    VPY_READER,
    open_file,
    get_video_track,
    NULL,
    NULL,
    prepare_video_decoding,
    NULL,
    read_video,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    close_file
};
