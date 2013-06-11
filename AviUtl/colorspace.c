/*****************************************************************************
 * colorspace.c
 *****************************************************************************
 * Copyright (C) 2011-2013 L-SMASH Works project
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

#include "lwinput.h"

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

static int check_sse41()
{
    int CPUInfo[4];
    __cpuid(CPUInfo, 1);
    return (CPUInfo[2] & 0x00080000) != 0;
}

static void convert_yuv16le_to_lw48( uint8_t *buf, int buf_linesize, uint8_t **dst_data, int *dst_linesize, int output_linesize, int output_height, int full_range )
{
    uint32_t offset = 0;
    while( output_height-- )
    {
        uint8_t *p_buf = buf;
        uint8_t *p_dst[3] = { dst_data[0] + offset, dst_data[1] + offset, dst_data[2] + offset };
        for( int i = 0; i < output_linesize; i += YC48_SIZE )
        {
            p_buf[0] = p_dst[0][0];
            p_buf[1] = p_dst[0][1];
            p_buf[2] = p_dst[1][0];
            p_buf[3] = p_dst[1][1];
            p_buf[4] = p_dst[2][0];
            p_buf[5] = p_dst[2][1];
            p_buf += LW48_SIZE;
            p_dst[0] += 2;
            p_dst[1] += 2;
            p_dst[2] += 2;
        }
        buf    += buf_linesize;
        offset += dst_linesize[0];
    }
}

static void convert_yuv16le_to_yc48( uint8_t *buf, int buf_linesize, uint8_t **dst_data, int *dst_linesize, int output_linesize, int output_height, int full_range )
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
        offset += dst_linesize[0];
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

static void inline __attribute__((always_inline)) convert_yuv420ple_i_to_yuv444p16le( uint8_t **dst, const int *dst_linesize, uint8_t **pic_data, int *pic_linesize, int output_linesize, int height, int bit_depth )
{
    const int lshft = 16 - bit_depth;
    /* copy luma */
    {
        uint16_t *ptr_src_line = (uint16_t *)pic_data[0];
        uint16_t *ptr_dst_line = (uint16_t *)dst[0];
        const int dst_line_len = dst_linesize[0] / sizeof(uint16_t);
        const int src_line_len = pic_linesize[0] / sizeof(uint16_t);
        const int luma_width = (output_linesize / sizeof(uint16_t));
        for( int y = 0; y < height; y++ )
            for( int x = 0; x < luma_width; x++ )
                ptr_dst_line[y*dst_line_len+x] = ptr_src_line[y*src_line_len+x] << lshft;
    }
    /* chroma upsampling for interlaced yuv420 */
    const int src_chroma_width = (output_linesize / sizeof(uint16_t)) / 2;
    for( int i_color = 1; i_color < 3; i_color++ )
    {
        uint16_t *ptr_src_line = (uint16_t *)pic_data[i_color];
        uint16_t *ptr_dst_line = (uint16_t *)dst[i_color];
        const int dst_line_len = dst_linesize[i_color] / sizeof(uint16_t);
        const int src_line_len = pic_linesize[i_color] / sizeof(uint16_t);
        /* first 2 lines */
        int x;
        uint16_t tmp[2][4];
        uint64_t *tmp0 = (uint64_t *)tmp[0], *tmp1 = (uint64_t *)tmp[1];

    /* this inner loop branch should be deleted by forced inline expansion and "lshft" constant propagation. */
#define INTERPOLATE_CHROMA( k, x ) \
    { \
        int chroma0 = (5 * ptr_src_line[0 * src_line_len + x] + 3 * ptr_src_line[2 * src_line_len + x]); \
        int chroma1 = (7 * ptr_src_line[1 * src_line_len + x] + 1 * ptr_src_line[3 * src_line_len + x]); \
        int chroma2 = (1 * ptr_src_line[0 * src_line_len + x] + 7 * ptr_src_line[2 * src_line_len + x]); \
        int chroma3 = (3 * ptr_src_line[1 * src_line_len + x] + 5 * ptr_src_line[3 * src_line_len + x]); \
        if( lshft - 3 < 0 ) \
        { \
            tmp[k][0] = (chroma0 + (1<<(2-lshft))) >> (3-lshft); \
            tmp[k][1] = (chroma1 + (1<<(2-lshft))) >> (3-lshft); \
            tmp[k][2] = (chroma2 + (1<<(2-lshft))) >> (3-lshft); \
            tmp[k][3] = (chroma3 + (1<<(2-lshft))) >> (3-lshft); \
        } \
        else if( lshft - 3 > 0 ) \
        { \
            tmp[k][0] = chroma0 << (lshft-3); \
            tmp[k][1] = chroma1 << (lshft-3); \
            tmp[k][2] = chroma2 << (lshft-3); \
            tmp[k][3] = chroma3 << (lshft-3); \
        } \
        else \
        { \
            tmp[k][0] = chroma0; \
            tmp[k][1] = chroma1; \
            tmp[k][2] = chroma2; \
            tmp[k][3] = chroma3; \
        } \
    }
#define PUT_CHROMA( x, line ) \
    { \
        ptr_dst_line[dst_line_len * line + 2 * x + 0] = tmp[0][line]; \
        ptr_dst_line[dst_line_len * line + 2 * x + 1] = (tmp[0][line] + tmp[1][line]) >> 1; \
    }

        tmp[0][0] = ptr_src_line[0] << lshft;
        tmp[0][1] = ptr_src_line[src_line_len] << lshft;
        for( x = 0; x < src_chroma_width - 1; x++ )
        {
            tmp[1][0] = ptr_src_line[x+1] << lshft;
            tmp[1][1] = ptr_src_line[x+1 + src_line_len] << lshft;
            for( int i = 0; i < 2; i++ )
                PUT_CHROMA( x, i );
            *tmp0 = *tmp1;
        }
        for( int i = 0; i < 2; i++ )
            PUT_CHROMA( x, i );
        ptr_dst_line += (dst_line_len << 1);

        /* 5,3,7,1 - interlaced yuv420 to yuv422 interpolation with 1,1 - yuv422 to yuv444 interpolation. */
        for( int y = 2; y < height - 2; y += 4, ptr_dst_line += (dst_line_len << 2), ptr_src_line += (src_line_len << 1) )
        {
            INTERPOLATE_CHROMA( 0, 0 );
            for( x = 0; x < src_chroma_width - 1; x++ )
            {
                INTERPOLATE_CHROMA( 1, x+1 );
                for( int i = 0; i < 4; i++ )
                    PUT_CHROMA( x, i );
                *tmp0 = *tmp1;
            }
            for( int i = 0; i < 4; i++ )
                PUT_CHROMA( x, i );
        }

        /* last 2 lines */
        tmp[0][0] = ptr_src_line[0] << lshft;
        tmp[0][1] = ptr_src_line[src_line_len] << lshft;
        for( x = 0; x < src_chroma_width - 1; x++ )
        {
            tmp[1][0] = ptr_src_line[x+1] << lshft;
            tmp[1][1] = ptr_src_line[x+1 + src_line_len] << lshft;
            for( int i = 0; i < 2; i++ )
                PUT_CHROMA( x, i );
            *tmp0 = *tmp1;
        }
        for( int i = 0; i < 2; i++ )
            PUT_CHROMA( x, i );
#undef INTERPOLATE_CHROMA
#undef PUT_CHROMA
    }
}

static void convert_yuv420p9le_i_to_yuv444p16le( uint8_t **dst, const int *dst_linesize, uint8_t **pic_data, int *pic_linesize, int output_linesize, int height )
{
    convert_yuv420ple_i_to_yuv444p16le( dst, dst_linesize, pic_data, pic_linesize, output_linesize, height, 9 );
}

static void convert_yuv420p10le_i_to_yuv444p16le( uint8_t **dst, const int *dst_linesize, uint8_t **pic_data, int *pic_linesize, int output_linesize, int height )
{
    convert_yuv420ple_i_to_yuv444p16le( dst, dst_linesize, pic_data, pic_linesize, output_linesize, height, 10 );
}

static void convert_yuv420p16le_i_to_yuv444p16le( uint8_t **dst, const int *dst_linesize, uint8_t **pic_data, int *pic_linesize, int output_linesize, int height )
{
    convert_yuv420ple_i_to_yuv444p16le( dst, dst_linesize, pic_data, pic_linesize, output_linesize, height, 16 );
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

static int to_yuv16le( AVCodecContext *ctx, struct SwsContext *sws_ctx, AVFrame *picture, uint8_t *dst_data[4], int dst_linesize[4] )
{
    static const struct
    {
        enum AVPixelFormat px_fmt;
        func_convert_yuv420ple_i_to_yuv444p16le convert[2];
    } yuv420_list[] = {
        { AV_PIX_FMT_YUV420P9LE,  { convert_yuv420p9le_i_to_yuv444p16le,  convert_yuv420p9le_i_to_yuv444p16le_sse41  } },
        { AV_PIX_FMT_YUV420P10LE, { convert_yuv420p10le_i_to_yuv444p16le, convert_yuv420p10le_i_to_yuv444p16le_sse41 } },
        { AV_PIX_FMT_YUV420P16LE, { convert_yuv420p16le_i_to_yuv444p16le, convert_yuv420p16le_i_to_yuv444p16le_sse41 } },
    };
    int yuv420_index = -1;
    if( picture->interlaced_frame )
        for( int idx = 0; idx < _countof(yuv420_list); idx++ )
            if( picture->format == yuv420_list[idx].px_fmt )
            {
                yuv420_index = idx;
                break;
            }
    if( yuv420_index != -1 )
    {
        static int sse41_available = -1;
        if( sse41_available == -1 )
            sse41_available = check_sse41();
        yuv420_list[yuv420_index].convert[sse41_available]( dst_data, dst_linesize, picture->data, picture->linesize, ctx->width * sizeof(uint16_t), ctx->height );
        return ctx->height;
    }
    else
        return sws_scale( sws_ctx, (const uint8_t* const*)picture->data, picture->linesize, 0, ctx->height, dst_data, dst_linesize );
}

int to_yuv16le_to_lw48( AVCodecContext *ctx, struct SwsContext *sws_ctx, AVFrame *picture, uint8_t *buf, int buf_linesize, int buf_height )
{
    uint8_t *dst_data    [4];
    int      dst_linesize[4];
    if( av_image_alloc( dst_data, dst_linesize, ctx->width, ctx->height, AV_PIX_FMT_YUV444P16LE, 16 ) < 0 )
    {
        MessageBox( HWND_DESKTOP, "Failed to av_image_alloc for LW48 convertion.", "lsmashinput", MB_ICONERROR | MB_OK );
        return 0;
    }
    int output_linesize = ctx->width * LW48_SIZE;
    int output_height   = to_yuv16le( ctx, sws_ctx, picture, dst_data, dst_linesize );
    /* Convert planar YUV 4:4:4 48bpp little-endian into LW48. */
    convert_yuv16le_to_lw48( buf, buf_linesize, dst_data, dst_linesize, output_linesize, output_height, ctx->color_range == AVCOL_RANGE_JPEG );
    av_free( dst_data[0] );
    return MAKE_AVIUTL_PITCH( output_linesize << 3 ) * output_height;
}

int to_yuv16le_to_yc48( AVCodecContext *ctx, struct SwsContext *sws_ctx, AVFrame *picture, uint8_t *buf, int buf_linesize, int buf_height )
{
    uint8_t *dst_data    [4];
    int      dst_linesize[4];
    if( av_image_alloc( dst_data, dst_linesize, ctx->width, ctx->height, AV_PIX_FMT_YUV444P16LE, 16 ) < 0 )
    {
        MessageBox( HWND_DESKTOP, "Failed to av_image_alloc for YC48 convertion.", "lsmashinput", MB_ICONERROR | MB_OK );
        return 0;
    }
    int output_linesize = ctx->width * YC48_SIZE;
    int output_height   = to_yuv16le( ctx, sws_ctx, picture, dst_data, dst_linesize );
    /* Convert planar YUV 4:4:4 48bpp little-endian into YC48. */
    static int simd_available = -1;
    if( simd_available == -1 )
        simd_available = check_sse2() + ( check_sse2() && check_sse41() );
    static void (*func_yuv16le_to_yc48[3])( uint8_t *, int, uint8_t **, int *, int, int, int ) = { convert_yuv16le_to_yc48, convert_yuv16le_to_yc48_sse2, convert_yuv16le_to_yc48_sse4_1 };
    func_yuv16le_to_yc48[simd_available * (((buf_linesize | (size_t)buf) & 15) == 0)]
        ( buf, buf_linesize, dst_data, dst_linesize, output_linesize, output_height, ctx->color_range == AVCOL_RANGE_JPEG );
    av_free( dst_data[0] );
    return MAKE_AVIUTL_PITCH( output_linesize << 3 ) * output_height;
}

int to_rgba( AVCodecContext *ctx, struct SwsContext *sws_ctx, AVFrame *picture, uint8_t *buf, int buf_linesize, int buf_height )
{
    uint8_t *dst_data    [4] = { buf + buf_linesize * (buf_height - 1), NULL, NULL, NULL };
    int      dst_linesize[4] = { -buf_linesize, 0, 0, 0 };
    int output_height   = sws_scale( sws_ctx, (const uint8_t* const*)picture->data, picture->linesize, 0, ctx->height, dst_data, dst_linesize );
    int output_linesize = ctx->width * RGBA_SIZE;
    return MAKE_AVIUTL_PITCH( output_linesize << 3 ) * output_height;
}

int to_rgb24( AVCodecContext *ctx, struct SwsContext *sws_ctx, AVFrame *picture, uint8_t *buf, int buf_linesize, int buf_height )
{
    uint8_t *dst_data    [4] = { buf + buf_linesize * (buf_height - 1), NULL, NULL, NULL };
    int      dst_linesize[4] = { -buf_linesize, 0, 0, 0 };
    int output_height   = sws_scale( sws_ctx, (const uint8_t* const*)picture->data, picture->linesize, 0, ctx->height, dst_data, dst_linesize );
    int output_linesize = ctx->width * RGB24_SIZE;
    return MAKE_AVIUTL_PITCH( output_linesize << 3 ) * output_height;
}

int to_yuy2( AVCodecContext *ctx, struct SwsContext *sws_ctx, AVFrame *picture, uint8_t *buf, int buf_linesize, int buf_height )
{
    int output_linesize = 0;
    if( picture->interlaced_frame
     && ((picture->format == AV_PIX_FMT_YUV420P)
     ||  (picture->format == AV_PIX_FMT_YUVJ420P)
     ||  (picture->format == AV_PIX_FMT_NV12)
     ||  (picture->format == AV_PIX_FMT_NV21)) )
    {
        uint8_t *another_chroma = NULL;
        if( (picture->format == AV_PIX_FMT_NV12)
         || (picture->format == AV_PIX_FMT_NV21) )
        {
            another_chroma = av_mallocz( (picture->linesize[1] / 2) * ctx->height );
            if( !another_chroma )
            {
                MessageBox( HWND_DESKTOP, "Failed to av_malloc.", "lsmashinput", MB_ICONERROR | MB_OK );
                return -1;
            }
            /* convert chroma nv12 to yv12 (split packed uv into planar u and v) */
            convert_packed_chroma_to_planar( picture->data[1], another_chroma, picture->linesize[1], ctx->width / 2, ctx->height / 2 );
            /* change data set as yv12 */
            picture->data[2] = picture->data[1];
            picture->data[1+(picture->format == AV_PIX_FMT_NV12)] = another_chroma;
            picture->linesize[1] /= 2;
            picture->linesize[2] = picture->linesize[1];
        }
        /* interlaced yv12 to yuy2 convertion */
        output_linesize = ctx->width * YUY2_SIZE;
        static int ssse3_available = -1;
        if( ssse3_available == -1 )
            ssse3_available = check_ssse3();
        static void (*func_yv12i_to_yuy2[2])( uint8_t*, int, uint8_t**, int*, int, int ) = { convert_yv12i_to_yuy2, convert_yv12i_to_yuy2_ssse3 };
        func_yv12i_to_yuy2[ssse3_available]( buf, buf_linesize, picture->data, picture->linesize, output_linesize, ctx->height );
        if( another_chroma )
            av_free( another_chroma );
    }
    else
    {
        uint8_t *dst_data    [4] = { buf, NULL, NULL, NULL };
        int      dst_linesize[4] = { buf_linesize, 0, 0, 0 };
        sws_scale( sws_ctx, (const uint8_t* const*)picture->data, picture->linesize, 0, ctx->height, dst_data, dst_linesize );
        output_linesize = ctx->width * YUY2_SIZE;
    }
    return MAKE_AVIUTL_PITCH( output_linesize << 3 ) * buf_height;
}
