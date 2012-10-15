/*****************************************************************************
 * colorspace.c
 *****************************************************************************
 * Copyright (C) 2011-2012 L-SMASH Works project
 *
 * Authors: Yusuke Nakamura <muken.the.vfrmaniac@gmail.com>
 *          Oka Motofumi <chikuzen.mo@gmail.com>
 *          rigaya <rigaya34589@live.jp>
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

#include "lsmashinput.h"

#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>

#include "colorspace_simd.h"

#ifdef __GNUC__
static void __cpuid(int CPUInfo[4], int prm)
{
    asm volatile ( "cpuid" :"=a"(CPUInfo[0]), "=b"(CPUInfo[1]), "=c"(CPUInfo[2]), "=d"(CPUInfo[3]) :"a"(prm) );
    return;
}
#else
#include <intrin.h>
#endif /* __GNUC__ */

static int check_sse2()
{
    int CPUInfo[4];
    __cpuid(CPUInfo, 1);
    return (CPUInfo[3] & 0x04000000) != 0;
}

static int check_ssse3()
{
    int CPUInfo[4];
    __cpuid(CPUInfo, 1);
    return (CPUInfo[2] & 0x00000200) != 0;
}

void avoid_yuv_scale_conversion( int *input_pixel_format )
{
    static const struct
    {
        enum PixelFormat full;
        enum PixelFormat limited;
    } range_hack_table[]
        = {
            { PIX_FMT_YUVJ420P, PIX_FMT_YUV420P },
            { PIX_FMT_YUVJ422P, PIX_FMT_YUV422P },
            { PIX_FMT_YUVJ444P, PIX_FMT_YUV444P },
            { PIX_FMT_YUVJ440P, PIX_FMT_YUV440P },
            { PIX_FMT_NONE,     PIX_FMT_NONE    }
          };
    for( int i = 0; range_hack_table[i].full != PIX_FMT_NONE; i++ )
        if( *input_pixel_format == range_hack_table[i].full )
            *input_pixel_format = range_hack_table[i].limited;
}

output_colorspace_index determine_colorspace_conversion( int *input_pixel_format, int *output_pixel_format )
{
    DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_OK, "input_pixel_format = %s", av_pix_fmt_descriptors[*input_pixel_format].name );
    avoid_yuv_scale_conversion( input_pixel_format );
    switch( *input_pixel_format )
    {
        case PIX_FMT_YUV444P :
        case PIX_FMT_YUV440P :
        case PIX_FMT_YUV420P9LE :
        case PIX_FMT_YUV420P9BE :
        case PIX_FMT_YUV422P9LE :
        case PIX_FMT_YUV422P9BE :
        case PIX_FMT_YUV444P9LE :
        case PIX_FMT_YUV444P9BE :
        case PIX_FMT_YUV420P10LE :
        case PIX_FMT_YUV420P10BE :
        case PIX_FMT_YUV422P10LE :
        case PIX_FMT_YUV422P10BE :
        case PIX_FMT_YUV444P10LE :
        case PIX_FMT_YUV444P10BE :
        case PIX_FMT_YUV420P16LE :
        case PIX_FMT_YUV420P16BE :
        case PIX_FMT_YUV422P16LE :
        case PIX_FMT_YUV422P16BE :
        case PIX_FMT_YUV444P16LE :
        case PIX_FMT_YUV444P16BE :
        case PIX_FMT_RGB48LE :
        case PIX_FMT_RGB48BE :
        case PIX_FMT_BGR48LE :
        case PIX_FMT_BGR48BE :
        case PIX_FMT_GBRP9LE :
        case PIX_FMT_GBRP9BE :
        case PIX_FMT_GBRP10LE :
        case PIX_FMT_GBRP10BE :
        case PIX_FMT_GBRP16LE :
        case PIX_FMT_GBRP16BE :
            *output_pixel_format = PIX_FMT_YUV444P16LE; /* planar YUV 4:4:4, 48bpp little-endian -> YC48 */
            return OUTPUT_YC48;
        case PIX_FMT_ARGB :
        case PIX_FMT_RGBA :
        case PIX_FMT_ABGR :
        case PIX_FMT_BGRA :
            *output_pixel_format = PIX_FMT_BGRA;        /* packed BGRA 8:8:8:8, 32bpp, BGRABGRA... */
            return OUTPUT_RGBA;
        case PIX_FMT_RGB24 :
        case PIX_FMT_BGR24 :
        case PIX_FMT_BGR8 :
        case PIX_FMT_BGR4 :
        case PIX_FMT_BGR4_BYTE :
        case PIX_FMT_RGB8 :
        case PIX_FMT_RGB4 :
        case PIX_FMT_RGB4_BYTE :
        case PIX_FMT_RGB565LE :
        case PIX_FMT_RGB565BE :
        case PIX_FMT_RGB555LE :
        case PIX_FMT_RGB555BE :
        case PIX_FMT_BGR565LE :
        case PIX_FMT_BGR565BE :
        case PIX_FMT_BGR555LE :
        case PIX_FMT_BGR555BE :
        case PIX_FMT_RGB444LE :
        case PIX_FMT_RGB444BE :
        case PIX_FMT_BGR444LE :
        case PIX_FMT_BGR444BE :
        case PIX_FMT_GBRP :
        case PIX_FMT_PAL8 :
            *output_pixel_format = PIX_FMT_BGR24;       /* packed RGB 8:8:8, 24bpp, BGRBGR... */
            return OUTPUT_RGB24;
        default :
            *output_pixel_format = PIX_FMT_YUYV422;     /* packed YUV 4:2:2, 16bpp */
            return OUTPUT_YUY2;
    }
}

static void convert_yuv16le_to_yc48( uint8_t *buf, int buf_linesize, uint8_t **dst_data, int dst_linesize, int output_linesize, int output_height, int full_range )
{
    uint32_t offset = 0;
    while( output_height-- )
    {
        uint8_t *p_buf = buf;
        uint8_t *p_dst[3] = { dst_data[0] + offset, dst_data[1] + offset, dst_data[2] + offset };
        for( int i = 0; i < output_linesize; i += YC48_SIZE )
        {
            static const uint32_t y_coef   [2] = {  1197,   4770 };
            static const uint32_t y_shift  [2] = {    14,     16 };
            static const uint32_t uv_coef  [2] = {  4682,   4662 };
            static const uint32_t uv_offset[2] = { 32768, 589824 };
            uint16_t y  = (((int32_t)((p_dst[0][0] | (p_dst[0][1] << 8)) * y_coef[full_range])) >> y_shift[full_range]) - 299;
            uint16_t cb = ((int32_t)(((p_dst[1][0] | (p_dst[1][1] << 8)) - 32768) * uv_coef[full_range] + uv_offset[full_range])) >> 16;
            uint16_t cr = ((int32_t)(((p_dst[2][0] | (p_dst[2][1] << 8)) - 32768) * uv_coef[full_range] + uv_offset[full_range])) >> 16;
            p_dst[0] += 2;
            p_dst[1] += 2;
            p_dst[2] += 2;
            p_buf[0] = y;
            p_buf[1] = y >> 8;
            p_buf[2] = cb;
            p_buf[3] = cb >> 8;
            p_buf[4] = cr;
            p_buf[5] = cr >> 8;
            p_buf += YC48_SIZE;
        }
        buf    += buf_linesize;
        offset += dst_linesize;
    }
}

static void convert_packed_chroma_to_planar( uint8_t *packed_chroma, uint8_t *planar_chroma, int packed_linesize, int chroma_width, int chroma_height )
{
    int planar_linesize = packed_linesize / 2;
    for( int y = 0; y < chroma_height; y++ )
    {
        uint8_t *src   = packed_chroma + packed_linesize * y;
        uint8_t *dst_u = packed_chroma + planar_linesize * y;
        uint8_t *dst_v = planar_chroma + planar_linesize * y;
        for( int x = 0; x < chroma_width; x++ )
        {
            dst_u[x] = src[2*x];
            dst_v[x] = src[2*x+1];
        }
    }
}

static void convert_yv12i_to_yuy2( uint8_t *buf, int buf_linesize, uint8_t **pic_data, int *pic_linesize, int output_linesize, int height )
{
    uint8_t *pic_y = pic_data[0];
    uint8_t *pic_u = pic_data[1];
    uint8_t *pic_v = pic_data[2];
#define INTERPOLATE_CHROMA( x, chroma, offset ) \
    { \
        buf[0 * buf_linesize + 4 * x + offset] = (5 * chroma[0*pic_linesize[1] + x] + 3 * chroma[2 * pic_linesize[1] + x] + 4) >> 3; \
        buf[1 * buf_linesize + 4 * x + offset] = (7 * chroma[1*pic_linesize[1] + x] + 1 * chroma[3 * pic_linesize[1] + x] + 4) >> 3; \
        buf[2 * buf_linesize + 4 * x + offset] = (1 * chroma[0*pic_linesize[1] + x] + 7 * chroma[2 * pic_linesize[1] + x] + 4) >> 3; \
        buf[3 * buf_linesize + 4 * x + offset] = (3 * chroma[1*pic_linesize[1] + x] + 5 * chroma[3 * pic_linesize[1] + x] + 4) >> 3; \
    }
#define COPY_CHROMA( x, chroma, offset ) \
    { \
        buf[               4 * x + offset] = chroma[                  x]; \
        buf[buf_linesize + 4 * x + offset] = chroma[pic_linesize[1] + x]; \
    }
    /* Copy all luma (Y). */
    int luma_width = output_linesize / YUY2_SIZE;
    for( int y = 0; y < height; y++ )
    {
        for( int x = 0; x < luma_width; x++ )
            buf[y * buf_linesize + 2 * x] = pic_y[x];
        pic_y += pic_linesize[0];
    }
    /* Copy first 2 lines */
    int chroma_width = luma_width / 2;
    for( int x = 0; x < chroma_width; x++ )
    {
        COPY_CHROMA( x, pic_u, 1 );
        COPY_CHROMA( x, pic_v, 3 );
    }
    buf += buf_linesize * 2;
    /* Interpolate interlaced yv12 to yuy2 with suggestion in MPEG-2 spec. */
    int four_buf_linesize = buf_linesize * 4;
    int four_chroma_linesize = pic_linesize[1] * 2;
    for( int y = 2; y < height - 2; y += 4 )
    {
        for( int x = 0; x < chroma_width; x++ )
        {
            INTERPOLATE_CHROMA( x, pic_u, 1 );      /* U */
            INTERPOLATE_CHROMA( x, pic_v, 3 );      /* V */
        }
        buf   += four_buf_linesize;
        pic_u += four_chroma_linesize;
        pic_v += four_chroma_linesize;
    }
    /* Copy last 2 lines. */
    for( int x = 0; x < chroma_width; x++ )
    {
        COPY_CHROMA( x, pic_u, 1 );
        COPY_CHROMA( x, pic_v, 3 );
    }
#undef INTERPOLATE_CHROMA
#undef COPY_CHROMA
}

static int get_conversion_multiplier( enum PixelFormat dst_pix_fmt, enum PixelFormat src_pix_fmt, int width )
{
    int src_size = 0;
    const AVPixFmtDescriptor *desc = &av_pix_fmt_descriptors[src_pix_fmt];
    int used_plane[4] = { 0, 0, 0, 0 };
    for( int i = 0; i < desc->nb_components; i++ )
    {
        int plane = desc->comp[i].plane;
        if( used_plane[plane] )
            continue;
        src_size += av_image_get_linesize( src_pix_fmt, width, plane );
        used_plane[plane] = 1;
    }
    if( src_size == 0 )
        return 1;
    int dst_size = 0;
    desc = &av_pix_fmt_descriptors[dst_pix_fmt];
    used_plane[0] = used_plane[1] = used_plane[2] = used_plane[3] = 0;
    for( int i = 0; i < desc->nb_components; i++ )
    {
        int plane = desc->comp[i].plane;
        if( used_plane[plane] )
            continue;
        dst_size += av_image_get_linesize( dst_pix_fmt, width, plane );
        used_plane[plane] = 1;
    }
    return (dst_size - 1) / src_size + 1;
}

int to_yuv16le_to_yc48( AVCodecContext *video_ctx, struct SwsContext *sws_ctx, AVFrame *picture, uint8_t *buf, int buf_linesize )
{
    int abs_dst_linesize = picture->linesize[0] > 0 ? picture->linesize[0] : -picture->linesize[0];
    abs_dst_linesize *= get_conversion_multiplier( PIX_FMT_YUV444P16LE, picture->format, picture->width );
    if( abs_dst_linesize & 15 )
        abs_dst_linesize = (abs_dst_linesize & 0xfffffff0) + 16;  /* Make mod16. */
    uint8_t *dst_data[4];
    dst_data[0] = av_mallocz( abs_dst_linesize * picture->height * 3 );
    if( !dst_data[0] )
    {
        MessageBox( HWND_DESKTOP, "Failed to av_mallocz for YC48 convertion.", "lsmashinput", MB_ICONERROR | MB_OK );
        return 0;
    }
    for( int i = 1; i < 3; i++ )
        dst_data[i] = dst_data[i - 1] + abs_dst_linesize * picture->height;
    dst_data[3] = NULL;
    const int dst_linesize[4] = { abs_dst_linesize, abs_dst_linesize, abs_dst_linesize, 0 };
    int output_height   = sws_scale( sws_ctx, (const uint8_t* const*)picture->data, picture->linesize, 0, picture->height, dst_data, dst_linesize );
    int output_linesize = picture->width * YC48_SIZE;
    /* Convert planar YUV 4:4:4 48bpp little-endian into YC48. */
    static int sse2_available = -1;
    if( sse2_available == -1 )
        sse2_available = check_sse2();
    static void (*func_yuv16le_to_yc48[2])( uint8_t*, int, uint8_t**, int, int, int, int ) = { convert_yuv16le_to_yc48, convert_yuv16le_to_yc48_sse2 };
    func_yuv16le_to_yc48[sse2_available && ((buf_linesize | (size_t)buf) & 15) == 0 && buf_linesize == output_linesize ]
        ( buf, buf_linesize, dst_data, abs_dst_linesize, output_linesize, output_height, video_ctx->color_range == AVCOL_RANGE_JPEG );
    av_free( dst_data[0] );
    return MAKE_AVIUTL_PITCH( output_linesize << 3 ) * output_height;
}

int to_rgba( AVCodecContext *video_ctx, struct SwsContext *sws_ctx, AVFrame *picture, uint8_t *buf, int buf_linesize )
{
    int abs_dst_linesize = picture->linesize[0] + picture->linesize[1] + picture->linesize[2] + picture->linesize[3];
    if( abs_dst_linesize < 0 )
        abs_dst_linesize = -abs_dst_linesize;
    abs_dst_linesize *= get_conversion_multiplier( PIX_FMT_BGRA, picture->format, picture->width );
    const int dst_linesize[4] = { abs_dst_linesize, 0, 0, 0 };
    uint8_t  *dst_data    [4] = { NULL, NULL, NULL, NULL };
    dst_data[0] = av_mallocz( dst_linesize[0] * picture->height );
    if( !dst_data[0] )
    {
        MessageBox( HWND_DESKTOP, "Failed to av_malloc.", "lsmashinput", MB_ICONERROR | MB_OK );
        return 0;
    }
    int output_height   = sws_scale( sws_ctx, (const uint8_t* const*)picture->data, picture->linesize, 0, picture->height, dst_data, dst_linesize );
    int output_linesize = picture->width * RGBA_SIZE;
    uint8_t *dst = dst_data[0] + dst_linesize[0] * output_height;
    while( output_height-- )
    {
        dst -= dst_linesize[0];
        memcpy( buf, dst, output_linesize );
        buf += buf_linesize;
    }
    av_free( dst_data[0] );
    return MAKE_AVIUTL_PITCH( output_linesize << 3 ) * output_height;
}

int to_rgb24( AVCodecContext *video_ctx, struct SwsContext *sws_ctx, AVFrame *picture, uint8_t *buf, int buf_linesize )
{
    int abs_dst_linesize = picture->linesize[0] + picture->linesize[1] + picture->linesize[2] + picture->linesize[3];
    if( abs_dst_linesize < 0 )
        abs_dst_linesize = -abs_dst_linesize;
    abs_dst_linesize *= get_conversion_multiplier( PIX_FMT_BGR24, picture->format, picture->width );
    const int dst_linesize[4] = { abs_dst_linesize, 0, 0, 0 };
    uint8_t  *dst_data    [4] = { NULL, NULL, NULL, NULL };
    dst_data[0] = av_mallocz( dst_linesize[0] * picture->height );
    if( !dst_data[0] )
    {
        MessageBox( HWND_DESKTOP, "Failed to av_malloc.", "lsmashinput", MB_ICONERROR | MB_OK );
        return -1;
    }
    int output_height   = sws_scale( sws_ctx, (const uint8_t* const*)picture->data, picture->linesize, 0, picture->height, dst_data, dst_linesize );
    int output_linesize = picture->width * RGB24_SIZE;
    uint8_t *dst = dst_data[0] + dst_linesize[0] * output_height;
    while( output_height-- )
    {
        dst -= dst_linesize[0];
        memcpy( buf, dst, output_linesize );
        buf += buf_linesize;
    }
    av_free( dst_data[0] );
    return MAKE_AVIUTL_PITCH( output_linesize << 3 ) * output_height;
}

int to_yuy2( AVCodecContext *video_ctx, struct SwsContext *sws_ctx, AVFrame *picture, uint8_t *buf, int buf_linesize )
{
    int output_linesize = 0;
    if( picture->interlaced_frame
     && ((picture->format == PIX_FMT_YUV420P)
     ||  (picture->format == PIX_FMT_YUVJ420P)
     ||  (picture->format == PIX_FMT_NV12)
     ||  (picture->format == PIX_FMT_NV21)) )
    {
        uint8_t *another_chroma = NULL;
        if( (picture->format == PIX_FMT_NV12)
         || (picture->format == PIX_FMT_NV21) )
        {
            another_chroma = av_mallocz( (picture->linesize[1] / 2) * picture->height );
            if( !another_chroma )
            {
                MessageBox( HWND_DESKTOP, "Failed to av_malloc.", "lsmashinput", MB_ICONERROR | MB_OK );
                return -1;
            }
            /* convert chroma nv12 to yv12 (split packed uv into planar u and v) */
            convert_packed_chroma_to_planar( picture->data[1], another_chroma, picture->linesize[1], picture->width / 2, video_ctx->height / 2 );
            /* change data set as yv12 */
            picture->data[2] = picture->data[1];
            picture->data[1+(picture->format == PIX_FMT_NV12)] = another_chroma;
            picture->linesize[1] /= 2;
            picture->linesize[2] = picture->linesize[1];
        }
        /* interlaced yv12 to yuy2 convertion */
        output_linesize = picture->width * YUY2_SIZE;
        static int ssse3_available = -1;
        if( ssse3_available == -1 )
            ssse3_available = check_ssse3();
        static void (*func_yv12i_to_yuy2[2])( uint8_t*, int, uint8_t**, int*, int, int ) = { convert_yv12i_to_yuy2, convert_yv12i_to_yuy2_ssse3 };
        func_yv12i_to_yuy2[ssse3_available && buf_linesize == output_linesize]( buf, buf_linesize, picture->data, picture->linesize, output_linesize, picture->height );
        if( another_chroma )
            av_free( another_chroma );
    }
    else
    {
        int abs_dst_linesize = picture->linesize[0] + picture->linesize[1] + picture->linesize[2] + picture->linesize[3];
        if( abs_dst_linesize < 0 )
            abs_dst_linesize = -abs_dst_linesize;
        abs_dst_linesize *= get_conversion_multiplier( PIX_FMT_YUYV422, picture->format, picture->width );
        const int dst_linesize[4] = { abs_dst_linesize, 0, 0, 0 };
        uint8_t  *dst_data    [4] = { NULL, NULL, NULL, NULL };
        dst_data[0] = av_mallocz( dst_linesize[0] * picture->height );
        if( !dst_data[0] )
        {
            MessageBox( HWND_DESKTOP, "Failed to av_malloc.", "lsmashinput", MB_ICONERROR | MB_OK );
            return -1;
        }
        int output_height = sws_scale( sws_ctx, (const uint8_t* const*)picture->data, picture->linesize, 0, picture->height, dst_data, dst_linesize );
        output_linesize = picture->width * YUY2_SIZE;
        uint8_t *dst = dst_data[0];
        while( output_height-- )
        {
            memcpy( buf, dst, output_linesize );
            buf += buf_linesize;
            dst += dst_linesize[0];
        }
        av_free( dst_data[0] );
    }
    return MAKE_AVIUTL_PITCH( output_linesize << 3 ) * picture->height;
}
