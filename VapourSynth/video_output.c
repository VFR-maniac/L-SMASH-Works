/*****************************************************************************
 * video_output.c
 *****************************************************************************
 * Copyright (C) 2013 L-SMASH Works project
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

#include <string.h>

/* Libav */
#include <libavcodec/avcodec.h>         /* Decoder */
#include <libswscale/swscale.h>         /* Colorspace converter */
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>

#include "VapourSynth.h"
#include "video_output.h"

static inline void bit_blt
(
    uint8_t *dst_data,
    int      dst_linesize,
    uint8_t *src_data,
    int      src_linesize,
    int      row_size,
    int      height
)
{
    if( src_linesize == dst_linesize && src_linesize == row_size )
    {
        memcpy( dst_data, src_data, src_linesize * height );
        return;
    }
    for( int i = 0; i < height; i++ )
    {
        memcpy( dst_data, src_data, row_size );
        dst_data += dst_linesize;
        src_data += src_linesize;
    }
}

static void make_black_background_planar_yuv8
(
    VSFrameRef  *vs_frame,
    const VSAPI *vsapi
)
{
    for( int i = 0; i < 3; i++ )
        memset( vsapi->getWritePtr( vs_frame, i ), i ? 0x80 : 0x00, vsapi->getStride( vs_frame, i ) * vsapi->getFrameHeight( vs_frame, i ) );
}

static void make_black_background_planar_yuv16
(
    VSFrameRef  *vs_frame,
    const VSAPI *vsapi
)
{
    int shift = vsapi->getFrameFormat( vs_frame )->bitsPerSample - 8;
    for( int i = 0; i < 3; i++ )
    {
        int v = i ? 0x00000080 << shift : 0x00000000;
        uint8_t *data = vsapi->getWritePtr( vs_frame, i );
        uint8_t *end  = data + vsapi->getStride( vs_frame, i ) * vsapi->getFrameHeight( vs_frame, i );
        while( data < end )
        {
            /* Assume little endianess. */
            data[0] = v;
            data[1] = v >> 8;
            data += 2;
        }
    }
}

static void make_black_background_planar_rgb
(
    VSFrameRef  *vs_frame,
    const VSAPI *vsapi
)
{
    for( int i = 0; i < 3; i++ )
        memset( vsapi->getWritePtr( vs_frame, i ), 0x00, vsapi->getStride( vs_frame, i ) * vsapi->getFrameHeight( vs_frame, i ) );
}

static void make_frame_planar_yuv
(
    AVPicture                 *av_picture,
    int                        width,
    int                        height,
    const component_reorder_t *component_reorder,
    VSFrameRef                *vs_frame,
    VSFrameContext            *frame_ctx,
    const VSAPI               *vsapi
)
{
    const VSFormat *vs_format = vsapi->getFrameFormat( vs_frame );
    int av_row_size_y = vs_format->bytesPerSample * width;
    for( int i = 0; i < 3; i++ )
    {
        int      vs_frame_linesize = vsapi->getStride  ( vs_frame, i );
        uint8_t *vs_frame_data     = vsapi->getWritePtr( vs_frame, i );
        int      av_plane          = component_reorder[i];
        int      av_frame_linesize = av_picture->linesize[av_plane];
        uint8_t *av_frame_data     = av_picture->data    [av_plane];
        int      av_row_size       = av_row_size_y >> (i ? vs_format->subSamplingW : 0);
        int      av_height         = height        >> (i ? vs_format->subSamplingH : 0);
        bit_blt( vs_frame_data, vs_frame_linesize,
                 av_frame_data, av_frame_linesize,
                 av_row_size,   av_height );
    }
}

static void make_frame_planar_rgb8
(
    AVPicture                 *av_picture,
    int                        width,
    int                        height,
    const component_reorder_t *component_reorder,
    VSFrameRef                *vs_frame,
    VSFrameContext            *frame_ctx,
    const VSAPI               *vsapi
)
{
    uint8_t *vs_frame_data[3] =
        {
            vsapi->getWritePtr( vs_frame, 0 ),
            vsapi->getWritePtr( vs_frame, 1 ),
            vsapi->getWritePtr( vs_frame, 2 )
        };
    const VSFormat *vs_format = vsapi->getFrameFormat( vs_frame );
    int av_num_components = vs_format->numPlanes + (component_reorder[3] == -1 ? 0 : 1);
    int vs_frame_linesize = vsapi->getStride( vs_frame, 0 );
    int vs_pixel_offset   = 0;
    int av_pixel_offset   = 0;
    for( int i = 0; i < height; i++ )
    {
        uint8_t *av_pixel   = av_picture->data[0] + av_pixel_offset;
        uint8_t *av_pixel_r = av_pixel + component_reorder[0];
        uint8_t *av_pixel_g = av_pixel + component_reorder[1];
        uint8_t *av_pixel_b = av_pixel + component_reorder[2];
        uint8_t *vs_pixel_r = vs_frame_data[0] + vs_pixel_offset;
        uint8_t *vs_pixel_g = vs_frame_data[1] + vs_pixel_offset;
        uint8_t *vs_pixel_b = vs_frame_data[2] + vs_pixel_offset;
        for( int j = 0; j < width; j++ )
        {
            *(vs_pixel_r++) = *av_pixel_r;
            *(vs_pixel_g++) = *av_pixel_g;
            *(vs_pixel_b++) = *av_pixel_b;
            av_pixel_r += av_num_components;
            av_pixel_g += av_num_components;
            av_pixel_b += av_num_components;
        }
        av_pixel_offset += av_picture->linesize[0];
        vs_pixel_offset += vs_frame_linesize;
    }
}

static void make_frame_planar_rgb16
(
    AVPicture                 *av_picture,
    int                        width,
    int                        height,
    const component_reorder_t *component_reorder,
    VSFrameRef                *vs_frame,
    VSFrameContext            *frame_ctx,
    const VSAPI               *vsapi
)
{
    uint8_t *vs_frame_data[3] =
        {
            vsapi->getWritePtr( vs_frame, 0 ),
            vsapi->getWritePtr( vs_frame, 1 ),
            vsapi->getWritePtr( vs_frame, 2 )
        };
    const VSFormat *vs_format = vsapi->getFrameFormat( vs_frame );
    int av_num_components = vs_format->numPlanes + (component_reorder[3] == -1 ? 0 : 1);
    int vs_frame_linesize = vsapi->getStride( vs_frame, 0 );
    int vs_pixel_offset   = 0;
    int av_pixel_offset   = 0;
    for( int i = 0; i < height; i++ )
    {
        uint16_t *av_pixel   = (uint16_t *)(av_picture->data[0] + av_pixel_offset);
        uint16_t *av_pixel_r = av_pixel + component_reorder[0];
        uint16_t *av_pixel_g = av_pixel + component_reorder[1];
        uint16_t *av_pixel_b = av_pixel + component_reorder[2];
        uint16_t *vs_pixel_r = (uint16_t *)(vs_frame_data[0] + vs_pixel_offset);
        uint16_t *vs_pixel_g = (uint16_t *)(vs_frame_data[1] + vs_pixel_offset);
        uint16_t *vs_pixel_b = (uint16_t *)(vs_frame_data[2] + vs_pixel_offset);
        for( int j = 0; j < width; j++ )
        {
            *(vs_pixel_r++) = *av_pixel_r;
            *(vs_pixel_g++) = *av_pixel_g;
            *(vs_pixel_b++) = *av_pixel_b;
            av_pixel_r += av_num_components;
            av_pixel_g += av_num_components;
            av_pixel_b += av_num_components;
        }
        av_pixel_offset += av_picture->linesize[0];
        vs_pixel_offset += vs_frame_linesize;
    }
}

VSPresetFormat get_vs_output_pixel_format( const char *format_name )
{
    if( !format_name )
        return pfNone;
    static const struct
    {
        const char     *format_name;
        VSPresetFormat  vs_output_pixel_format;
    } format_table[] =
        {
            { "YUV420P8",  pfYUV420P8  },
            { "YUV422P8",  pfYUV422P8  },
            { "YUV444P8",  pfYUV444P8  },
            { "YUV410P8",  pfYUV410P8  },
            { "YUV411P8",  pfYUV411P8  },
            { "YUV440P8",  pfYUV440P8  },
            { "YUV420P9",  pfYUV420P9  },
            { "YUV422P9",  pfYUV422P9  },
            { "YUV444P9",  pfYUV444P9  },
            { "YUV420P10", pfYUV420P10 },
            { "YUV422P10", pfYUV422P10 },
            { "YUV444P10", pfYUV444P10 },
            { "YUV420P16", pfYUV420P16 },
            { "YUV422P16", pfYUV422P16 },
            { "YUV444P16", pfYUV444P16 },
            { "RGB24",     pfRGB24     },
            { "RGB48",     pfRGB48     },
            { NULL,        pfNone      }
        };
    for( int i = 0; format_table[i].format_name; i++ )
        if( strcasecmp( format_name, format_table[i].format_name ) == 0 )
            return format_table[i].vs_output_pixel_format;
    return pfNone;
}

static enum AVPixelFormat vs_to_av_output_pixel_format( VSPresetFormat vs_output_pixel_format )
{
    static const struct
    {
        VSPresetFormat     vs_output_pixel_format;
        enum AVPixelFormat av_output_pixel_format;
    } format_table[] =
        {
            { pfYUV420P8,  AV_PIX_FMT_YUV420P     },
            { pfYUV422P8,  AV_PIX_FMT_YUV422P     },
            { pfYUV444P8,  AV_PIX_FMT_YUV444P     },
            { pfYUV410P8,  AV_PIX_FMT_YUV410P     },
            { pfYUV411P8,  AV_PIX_FMT_YUV411P     },
            { pfYUV440P8,  AV_PIX_FMT_YUV440P     },
            { pfYUV420P9,  AV_PIX_FMT_YUV420P9LE  },
            { pfYUV422P9,  AV_PIX_FMT_YUV422P9LE  },
            { pfYUV444P9,  AV_PIX_FMT_YUV444P9LE  },
            { pfYUV420P10, AV_PIX_FMT_YUV420P10LE },
            { pfYUV422P10, AV_PIX_FMT_YUV422P10LE },
            { pfYUV444P10, AV_PIX_FMT_YUV444P10LE },
            { pfYUV420P16, AV_PIX_FMT_YUV420P16LE },
            { pfYUV422P16, AV_PIX_FMT_YUV422P16LE },
            { pfYUV444P16, AV_PIX_FMT_YUV444P16LE },
            { pfRGB24,     AV_PIX_FMT_RGB24       },
            { pfRGB48,     AV_PIX_FMT_BGR48LE     },
            { pfNone,      AV_PIX_FMT_NONE        }
        };
    for( int i = 0; format_table[i].vs_output_pixel_format != pfNone; i++ )
        if( vs_output_pixel_format == format_table[i].vs_output_pixel_format )
            return format_table[i].av_output_pixel_format;
    return AV_PIX_FMT_NONE;
}

static const component_reorder_t *get_component_reorder( enum AVPixelFormat av_output_pixel_format )
{
    static const struct
    {
        enum AVPixelFormat  av_output_pixel_format;
        component_reorder_t component_reorder[4];
    } reorder_table[] =
        {
            { AV_PIX_FMT_YUV420P,     {  0,  1,  2, -1 } },
            { AV_PIX_FMT_YUV422P,     {  0,  1,  2, -1 } },
            { AV_PIX_FMT_YUV444P,     {  0,  1,  2, -1 } },
            { AV_PIX_FMT_YUV410P,     {  0,  1,  2, -1 } },
            { AV_PIX_FMT_YUV411P,     {  0,  1,  2, -1 } },
            { AV_PIX_FMT_YUV440P,     {  0,  1,  2, -1 } },
            { AV_PIX_FMT_YUV420P9LE,  {  0,  1,  2, -1 } },
            { AV_PIX_FMT_YUV422P9LE,  {  0,  1,  2, -1 } },
            { AV_PIX_FMT_YUV444P9LE,  {  0,  1,  2, -1 } },
            { AV_PIX_FMT_YUV420P10LE, {  0,  1,  2, -1 } },
            { AV_PIX_FMT_YUV422P10LE, {  0,  1,  2, -1 } },
            { AV_PIX_FMT_YUV444P10LE, {  0,  1,  2, -1 } },
            { AV_PIX_FMT_YUV420P16LE, {  0,  1,  2, -1 } },
            { AV_PIX_FMT_YUV422P16LE, {  0,  1,  2, -1 } },
            { AV_PIX_FMT_YUV444P16LE, {  0,  1,  2, -1 } },
            { AV_PIX_FMT_RGB24,       {  0,  1,  2, -1 } },
            { AV_PIX_FMT_ARGB,        {  1,  2,  3,  0 } },
            { AV_PIX_FMT_RGBA,        {  0,  1,  2,  3 } },
            { AV_PIX_FMT_ABGR,        {  3,  2,  1,  0 } },
            { AV_PIX_FMT_BGRA,        {  2,  1,  0,  3 } },
            { AV_PIX_FMT_BGR48LE,     {  2,  1,  0, -1 } },
            { AV_PIX_FMT_NONE,        {  0,  1,  2,  3 } }
        };
    int i = 0;
    while( reorder_table[i].av_output_pixel_format != AV_PIX_FMT_NONE )
    {
        if( av_output_pixel_format == reorder_table[i].av_output_pixel_format )
            break;
        ++i;
    }
    return reorder_table[i].component_reorder;
}

static inline int set_frame_maker( vs_video_output_handler_t *vs_vohp )
{
    static const struct
    {
        VSPresetFormat              vs_output_pixel_format;
        func_make_black_background *func_make_black_background;
        func_make_frame            *func_make_frame;
    } frame_maker_table[] =
        {
            { pfYUV420P8,  make_black_background_planar_yuv8,  make_frame_planar_yuv   },
            { pfYUV422P8,  make_black_background_planar_yuv8,  make_frame_planar_yuv   },
            { pfYUV444P8,  make_black_background_planar_yuv8,  make_frame_planar_yuv   },
            { pfYUV410P8,  make_black_background_planar_yuv8,  make_frame_planar_yuv   },
            { pfYUV411P8,  make_black_background_planar_yuv8,  make_frame_planar_yuv   },
            { pfYUV440P8,  make_black_background_planar_yuv8,  make_frame_planar_yuv   },
            { pfYUV420P9,  make_black_background_planar_yuv16, make_frame_planar_yuv   },
            { pfYUV422P9,  make_black_background_planar_yuv16, make_frame_planar_yuv   },
            { pfYUV444P9,  make_black_background_planar_yuv16, make_frame_planar_yuv   },
            { pfYUV420P10, make_black_background_planar_yuv16, make_frame_planar_yuv   },
            { pfYUV422P10, make_black_background_planar_yuv16, make_frame_planar_yuv   },
            { pfYUV444P10, make_black_background_planar_yuv16, make_frame_planar_yuv   },
            { pfYUV420P16, make_black_background_planar_yuv16, make_frame_planar_yuv   },
            { pfYUV422P16, make_black_background_planar_yuv16, make_frame_planar_yuv   },
            { pfYUV444P16, make_black_background_planar_yuv16, make_frame_planar_yuv   },
            { pfRGB24,     make_black_background_planar_rgb,   make_frame_planar_rgb8  },
            { pfRGB48,     make_black_background_planar_rgb,   make_frame_planar_rgb16 },
            { pfNone,      NULL,                               NULL                    }
        };
    for( int i = 0; frame_maker_table[i].vs_output_pixel_format != pfNone; i++ )
        if( vs_vohp->vs_output_pixel_format == frame_maker_table[i].vs_output_pixel_format )
        {
            vs_vohp->make_black_background = frame_maker_table[i].func_make_black_background;
            vs_vohp->make_frame            = frame_maker_table[i].func_make_frame;
            return 0;
        }
    vs_vohp->make_black_background = NULL;
    vs_vohp->make_frame            = NULL;
    return -1;
}

int determine_colorspace_conversion
(
    lw_video_output_handler_t *vohp,
    enum AVPixelFormat         input_pixel_format
)
{
    avoid_yuv_scale_conversion( &input_pixel_format );
    static const struct
    {
        enum AVPixelFormat  av_input_pixel_format;
        VSPresetFormat      vs_output_pixel_format;
        int                 enable_scaler;
    } conversion_table[] =
        {
            { AV_PIX_FMT_YUV420P,     pfYUV420P8,  0 },
            { AV_PIX_FMT_NV12,        pfYUV420P8,  1 },
            { AV_PIX_FMT_NV21,        pfYUV420P8,  1 },
            { AV_PIX_FMT_YUV422P,     pfYUV422P8,  0 },
            { AV_PIX_FMT_UYVY422,     pfYUV422P8,  1 },
            { AV_PIX_FMT_YUYV422,     pfYUV422P8,  1 },
            { AV_PIX_FMT_YUV444P,     pfYUV444P8,  0 },
            { AV_PIX_FMT_YUV410P,     pfYUV410P8,  0 },
            { AV_PIX_FMT_YUV411P,     pfYUV411P8,  0 },
            { AV_PIX_FMT_UYYVYY411,   pfYUV411P8,  1 },
            { AV_PIX_FMT_YUV440P,     pfYUV440P8,  0 },
            { AV_PIX_FMT_YUV420P9LE,  pfYUV420P9,  0 },
            { AV_PIX_FMT_YUV420P9BE,  pfYUV420P9,  1 },
            { AV_PIX_FMT_YUV422P9LE,  pfYUV422P9,  0 },
            { AV_PIX_FMT_YUV422P9BE,  pfYUV422P9,  1 },
            { AV_PIX_FMT_YUV444P9LE,  pfYUV444P9,  0 },
            { AV_PIX_FMT_YUV444P9BE,  pfYUV444P9,  1 },
            { AV_PIX_FMT_YUV420P10LE, pfYUV420P10, 0 },
            { AV_PIX_FMT_YUV420P10BE, pfYUV420P10, 1 },
            { AV_PIX_FMT_YUV422P10LE, pfYUV422P10, 0 },
            { AV_PIX_FMT_YUV422P10BE, pfYUV422P10, 1 },
            { AV_PIX_FMT_YUV444P10LE, pfYUV444P10, 0 },
            { AV_PIX_FMT_YUV444P10BE, pfYUV444P10, 1 },
            { AV_PIX_FMT_YUV420P16LE, pfYUV420P16, 0 },
            { AV_PIX_FMT_YUV420P16BE, pfYUV420P16, 1 },
            { AV_PIX_FMT_YUV422P16LE, pfYUV422P16, 0 },
            { AV_PIX_FMT_YUV422P16BE, pfYUV422P16, 1 },
            { AV_PIX_FMT_YUV444P16LE, pfYUV444P16, 0 },
            { AV_PIX_FMT_YUV444P16BE, pfYUV444P16, 1 },
            { AV_PIX_FMT_BGR24,       pfRGB24,     0 },
            { AV_PIX_FMT_RGB24,       pfRGB24,     0 },
            { AV_PIX_FMT_ARGB,        pfRGB24,     0 },
            { AV_PIX_FMT_RGBA,        pfRGB24,     0 },
            { AV_PIX_FMT_ABGR,        pfRGB24,     0 },
            { AV_PIX_FMT_BGRA,        pfRGB24,     0 },
            { AV_PIX_FMT_BGR48LE,     pfRGB48,     0 },
            { AV_PIX_FMT_BGR48BE,     pfRGB48,     1 },
            { AV_PIX_FMT_NONE,        pfNone,      1 }
        };
    vs_video_output_handler_t *vs_vohp = (vs_video_output_handler_t *)vohp->private_handler;
    if( vs_vohp->variable_info || vs_vohp->vs_output_pixel_format == pfNone )
    {
        /* Determine by input pixel format. */
        for( int i = 0; conversion_table[i].vs_output_pixel_format != pfNone; i++ )
            if( input_pixel_format == conversion_table[i].av_input_pixel_format )
            {
                vs_vohp->vs_output_pixel_format = conversion_table[i].vs_output_pixel_format;
                vohp->scaler.enabled            = conversion_table[i].enable_scaler;
                break;
            }
    }
    else
    {
        /* Determine by both input pixel format and output pixel format. */
        int i = 0;
        while( conversion_table[i].vs_output_pixel_format != pfNone )
        {
            if( input_pixel_format              == conversion_table[i].av_input_pixel_format
             && vs_vohp->vs_output_pixel_format == conversion_table[i].vs_output_pixel_format )
            {
                vohp->scaler.enabled = conversion_table[i].enable_scaler;
                break;
            }
            ++i;
        }
        if( conversion_table[i].vs_output_pixel_format == pfNone )
            vohp->scaler.enabled = 1;
    }
    vohp->scaler.output_pixel_format = vohp->scaler.enabled
                                     ? vs_to_av_output_pixel_format( vs_vohp->vs_output_pixel_format )
                                     : input_pixel_format;
    vs_vohp->component_reorder = get_component_reorder( vohp->scaler.output_pixel_format );
    return set_frame_maker( vs_vohp );
}

VSFrameRef *new_output_video_frame
(
    lw_video_output_handler_t *vohp,
    AVFrame                   *av_frame,
    VSFrameContext            *frame_ctx,
    VSCore                    *core,
    const VSAPI               *vsapi
)
{
    vs_video_output_handler_t *vs_vohp = (vs_video_output_handler_t *)vohp->private_handler;
    VSFrameRef                *vs_frame;
    if( vs_vohp->variable_info )
    {
        if( determine_colorspace_conversion( vohp, (enum AVPixelFormat)av_frame->format ) )
        {
            if( frame_ctx )
                vsapi->setFilterError( "lsmas: failed to determin colorspace conversion.", frame_ctx );
            return NULL;
        }
        const VSFormat *vs_format = vsapi->getFormatPreset( vs_vohp->vs_output_pixel_format, core );
        vs_frame = vsapi->newVideoFrame( vs_format, av_frame->width, av_frame->height, NULL, core );
    }
    else
    {
        if( av_frame->format != vohp->scaler.input_pixel_format
         && determine_colorspace_conversion( vohp, (enum AVPixelFormat)av_frame->format ) )
        {
            if( frame_ctx )
                vsapi->setFilterError( "lsmas: failed to determin colorspace conversion.", frame_ctx );
            return NULL;
        }
        vs_frame = vsapi->copyFrame( vs_vohp->background_frame, core );
    }
    return vs_frame;
}

static inline int convert_av_pixel_format
(
    lw_video_scaler_handler_t *vshp,
    AVFrame                   *av_frame,
    AVPicture                 *av_picture
)
{
    if( !vshp->enabled )
    {
        for( int i = 0; i < 4; i++ )
        {
            av_picture->data    [i] = av_frame->data    [i];
            av_picture->linesize[i] = av_frame->linesize[i];
        }
        return 0;
    }
    if( av_image_alloc( av_picture->data, av_picture->linesize, av_frame->width, av_frame->height, vshp->output_pixel_format, 16 ) < 0 )
        return -1;
    int ret = sws_scale( vshp->sws_ctx,
                         (const uint8_t * const *)av_frame->data, av_frame->linesize,
                         0, av_frame->height,
                         av_picture->data, av_picture->linesize );
    if( ret > 0 )
        return ret;
    av_freep( &av_picture->data[0] );
    return -1;
}

VSFrameRef *make_frame
(
    lw_video_output_handler_t *vohp,
    AVFrame                   *av_frame,
    enum AVColorSpace          colorspace
)
{
    vs_video_output_handler_t *vs_vohp = (vs_video_output_handler_t *)vohp->private_handler;
    VSFrameContext *frame_ctx = vs_vohp->frame_ctx;
    VSCore         *core      = vs_vohp->core;
    const VSAPI    *vsapi     = vs_vohp->vsapi;
    if( vs_vohp->direct_rendering && !vohp->scaler.enabled )
    {
        /* Render from the decoder directly. */
        VSFrameRef *vs_frame_buffer = (VSFrameRef *)av_frame->opaque;
        return vs_frame_buffer;
    }
    if( !vs_vohp->make_frame )
        return NULL;
    /* Convert pixel format if needed. We don't change the presentation resolution. */
    enum AVPixelFormat *input_pixel_format = (enum AVPixelFormat *)&av_frame->format;
    int yuv_range = avoid_yuv_scale_conversion( input_pixel_format );
    lw_video_scaler_handler_t *vshp = &vohp->scaler;
    if( !vshp->sws_ctx
     || vshp->input_width        != av_frame->width
     || vshp->input_height       != av_frame->height
     || vshp->input_pixel_format != *input_pixel_format
     || vshp->input_colorspace   != colorspace
     || vshp->input_yuv_range    != yuv_range )
    {
        /* Update scaler. */
        vshp->sws_ctx = update_scaler_configuration( vshp->sws_ctx, vshp->flags,
                                                     av_frame->width, av_frame->height,
                                                     *input_pixel_format, vshp->output_pixel_format,
                                                     colorspace, yuv_range );
        if( !vshp->sws_ctx )
        {
            if( frame_ctx )
                vsapi->setFilterError( "lsmas: failed to update scaler settings.", frame_ctx );
            return NULL;
        }
        vshp->input_width        = av_frame->width;
        vshp->input_height       = av_frame->height;
        vshp->input_pixel_format = *input_pixel_format;
        vshp->input_colorspace   = colorspace;
        vshp->input_yuv_range    = yuv_range;
    }
    /* Make video frame. */
    AVPicture av_picture;
    int ret = convert_av_pixel_format( vshp, av_frame, &av_picture );
    if( ret < 0 )
    {
        if( frame_ctx )
            vsapi->setFilterError( "lsmas: failed to av_image_alloc.", frame_ctx );
        return NULL;
    }
    VSFrameRef *vs_frame = new_output_video_frame( vohp, av_frame, frame_ctx, core, vsapi );
    if( vs_frame )
        vs_vohp->make_frame( &av_picture, av_frame->width, av_frame->height, vs_vohp->component_reorder, vs_frame, frame_ctx, vsapi );
    else if( frame_ctx )
        vsapi->setFilterError( "lsmas: failed to alloc a output video frame.", frame_ctx );
    if( ret > 0 )
        av_free( av_picture.data[0] );
    return vs_frame;
}

int vs_check_dr_support_format( enum AVPixelFormat decoded_pixel_format )
{
    static enum AVPixelFormat dr_support_pix_fmt[] =
        {
            AV_PIX_FMT_YUV420P,
            AV_PIX_FMT_YUV422P,
            AV_PIX_FMT_YUV444P,
            AV_PIX_FMT_YUV410P,
            AV_PIX_FMT_YUV411P,
            AV_PIX_FMT_YUV440P,
            AV_PIX_FMT_YUV420P9LE,
            AV_PIX_FMT_YUV422P9LE,
            AV_PIX_FMT_YUV444P9LE,
            AV_PIX_FMT_YUV420P10LE,
            AV_PIX_FMT_YUV422P10LE,
            AV_PIX_FMT_YUV444P10LE,
            AV_PIX_FMT_YUV420P16LE,
            AV_PIX_FMT_YUV422P16LE,
            AV_PIX_FMT_YUV444P16LE,
            AV_PIX_FMT_NONE
        };
    for( int i = 0; dr_support_pix_fmt[i] != AV_PIX_FMT_NONE; i++ )
        if( dr_support_pix_fmt[i] == decoded_pixel_format )
            return 1;
    return 0;
}

int vs_video_get_buffer
(
    AVCodecContext *ctx,
    AVFrame        *av_frame
)
{
    lw_video_output_handler_t *lw_vohp = (lw_video_output_handler_t *)ctx->opaque;
    vs_video_output_handler_t *vs_vohp = (vs_video_output_handler_t *)lw_vohp->private_handler;
    enum AVPixelFormat pix_fmt = ctx->pix_fmt;
    avoid_yuv_scale_conversion( &pix_fmt );
    if( (!vs_vohp->variable_info && lw_vohp->scaler.input_pixel_format != pix_fmt)
     || !vs_check_dr_support_format( pix_fmt ) )
    {
        lw_vohp->scaler.enabled = 1;
        return avcodec_default_get_buffer( ctx, av_frame );
    }
    else
        lw_vohp->scaler.enabled = 0;
    /* New VapourSynth video frame buffer. */
    av_frame->width  = ctx->width;
    av_frame->height = ctx->height;
    av_frame->format = ctx->pix_fmt;
    avcodec_align_dimensions2( ctx, &av_frame->width, &av_frame->height, av_frame->linesize );
    VSFrameRef *vs_frame_buffer = new_output_video_frame( lw_vohp, av_frame, vs_vohp->frame_ctx, vs_vohp->core, vs_vohp->vsapi );
    if( !vs_frame_buffer )
        return -1;
    av_frame->opaque = vs_frame_buffer;
    /* Set data address and linesize. */
    vs_vohp->component_reorder = get_component_reorder( pix_fmt );
    for( int i = 0; i < 3; i++ )
    {
        int plane = vs_vohp->component_reorder[i];
        av_frame->base    [i] = vs_vohp->vsapi->getWritePtr( vs_frame_buffer, plane );
        av_frame->data    [i] = av_frame->base[i];
        av_frame->linesize[i] = vs_vohp->vsapi->getStride  ( vs_frame_buffer, plane );
    }
    /* Don't use extended_data. */
    av_frame->extended_data       = av_frame->data;
    /* Set fundamental fields. */
    av_frame->type                = FF_BUFFER_TYPE_USER;
    av_frame->pkt_pts             = ctx->pkt ? ctx->pkt->pts : AV_NOPTS_VALUE;
    av_frame->width               = ctx->width;
    av_frame->height              = ctx->height;
    av_frame->format              = ctx->pix_fmt;
    av_frame->sample_aspect_ratio = ctx->sample_aspect_ratio;
    return 0;
}

void vs_video_release_buffer
(
    AVCodecContext *ctx,
    AVFrame        *av_frame
)
{
    /* Delete VapourSynth video frame buffer. */
    if( av_frame->type == FF_BUFFER_TYPE_USER )
    {
        for( int i = 0; i < 3; i++ )
        {
            av_frame->base    [i] = NULL;
            av_frame->data    [i] = NULL;
            av_frame->linesize[i] = 0;
        }
        if( av_frame->opaque )
        {
            lw_video_output_handler_t *lw_vohp = (lw_video_output_handler_t *)ctx->opaque;
            if( !lw_vohp )
                return;
            vs_video_output_handler_t *vs_vohp = (vs_video_output_handler_t *)lw_vohp->private_handler;
            if( !vs_vohp || !vs_vohp->vsapi )
                return;
            vs_vohp->vsapi->freeFrame( (VSFrameRef *)av_frame->opaque );
            av_frame->opaque = NULL;
        }
        return;
    }
    avcodec_default_release_buffer( ctx, av_frame );
}
