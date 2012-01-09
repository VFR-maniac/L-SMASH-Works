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
#ifdef DEBUG_VIDEO
#include <libavutil/pixdesc.h>
#endif

/* for SSE2 intrinsic func */
#ifdef __GNUC__
#pragma GCC push_options
#pragma GCC target ("sse2")
#define AUI_ALIGN(x) __attribute__((aligned(x)))
static void __cpuid(int CPUInfo[4], int prm)
{
    asm volatile ( "cpuid" :"=a"(CPUInfo[0]), "=b"(CPUInfo[1]), "=c"(CPUInfo[2]), "=d"(CPUInfo[3]) :"a"(prm) );
    return;
}
#else
#define AUI_ALIGN(x) __declspec(align(x))
#include <intrin.h>
#endif /* __GNUC__ */
#include <emmintrin.h>
#ifdef __GNUC__
#pragma GCC pop_options
#endif /* __GNUC__ */

int check_sse2()
{
    int CPUInfo[4];
    __cpuid(CPUInfo, 1);
    return (CPUInfo[3] & 0x04000000) != 0;
}

static void avoid_yuv_scale_conversion( int *input_pixel_format )
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

output_colorspace determine_colorspace_conversion( int *input_pixel_format, int *output_pixel_format )
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
        case PIX_FMT_RGB24 :
        case PIX_FMT_BGR24 :
        case PIX_FMT_ARGB :
        case PIX_FMT_RGBA :
        case PIX_FMT_ABGR :
        case PIX_FMT_BGRA :
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
            *output_pixel_format = PIX_FMT_BGR24;       /* packed RGB 8:8:8, 24bpp, BGRBGR... */
            return OUTPUT_RGB24;
        default :
            *output_pixel_format = PIX_FMT_YUYV422;     /* packed YUV 4:2:2, 16bpp */
            return OUTPUT_YUY2;
    }
}

void convert_yuv16le_to_yc48( uint8_t *buf, int buf_linesize, uint8_t **dst_data, int dst_linesize, int output_height, int full_range )
{
    uint32_t offset = 0;
    while( output_height-- )
    {
        uint8_t *p_dst[3] = { dst_data[0] + offset, dst_data[1] + offset, dst_data[2] + offset };
        for( int i = 0; i < buf_linesize; i += YC48_SIZE )
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
            buf[0] = y;
            buf[1] = y >> 8;
            buf[2] = cb;
            buf[3] = cb >> 8;
            buf[4] = cr;
            buf[5] = cr >> 8;
            buf += YC48_SIZE;
        }
        offset += dst_linesize;
    }
}

#ifdef __GNUC__
/* Force SSE2 to use intrinsic SSE2 functions. */
#pragma GCC push_options
#pragma GCC target ("sse2")
#endif /* __GNUC__ */
/* SSE2 version of func convert_yuv16le_to_yc48
 * dst_data[0], dst_data[1], dst_data[2], buf, buf_linesize and dst_linesize need to be mod16. */
void convert_yuv16le_to_yc48_sse2( uint8_t *buf, int buf_linesize, uint8_t **dst_data, int dst_linesize, int output_height, int full_range )
{
    uint8_t *ycp = buf, *ycp_fin;
    uint8_t *p_dst_y, *p_dst_u, *p_dst_v;
    __m128i x0, x1, x2, x3;
#define Y_COEF         4788
#define Y_COEF_FULL    4770
#define UV_COEF        4682
#define UV_COEF_FULL   4662
#define Y_OFFSET       ((-299)+((Y_COEF)>>1))
#define Y_OFFSET_FULL  ((-299)+((Y_COEF_FULL)>>1))
#define UV_OFFSET      32768
#define UV_OFFSET_FULL 589824
    static const int AUI_ALIGN(16) aY_coef[2][4] = {
        { Y_COEF,      Y_COEF,      Y_COEF,      Y_COEF      },
        { Y_COEF_FULL, Y_COEF_FULL, Y_COEF_FULL, Y_COEF_FULL }
    };
    static const int AUI_ALIGN(16) aUV_coef[2][4] = {
        { UV_COEF,      UV_COEF,      UV_COEF,      UV_COEF      },
        { UV_COEF_FULL, UV_COEF_FULL, UV_COEF_FULL, UV_COEF_FULL }
    };
    static const int16_t AUI_ALIGN(16) aY_offest[2][8] = {
        { Y_OFFSET,      Y_OFFSET,      Y_OFFSET,      Y_OFFSET,      Y_OFFSET,      Y_OFFSET,      Y_OFFSET,      Y_OFFSET      },
        { Y_OFFSET_FULL, Y_OFFSET_FULL, Y_OFFSET_FULL, Y_OFFSET_FULL, Y_OFFSET_FULL, Y_OFFSET_FULL, Y_OFFSET_FULL, Y_OFFSET_FULL }
    };
    static const int AUI_ALIGN(16) aUV_offest[2][4] = {
        { UV_OFFSET,      UV_OFFSET,      UV_OFFSET,      UV_OFFSET      },
        { UV_OFFSET_FULL, UV_OFFSET_FULL, UV_OFFSET_FULL, UV_OFFSET_FULL }
    };
#undef Y_COEF
#undef Y_COEF_FULL
#undef UV_COEF
#undef UV_COEF_FULL
#undef Y_OFFSET
#undef Y_OFFSET_FULL
#undef UV_OFFSET
#undef UV_OFFSET_FULL
    x3 = _mm_setzero_si128();   /* Initialize to avoid warning and some weird gcc optimization. */
    /*  Y = ((( y - 32768 ) * coef)           >> 16 ) + (coef/2 - 299) */
    /* UV = (( uv - 32768 ) * coef + offset ) >> 16 */
    for( uint32_t offset = 0; output_height--; offset += dst_linesize )
    {
        p_dst_y = dst_data[0] + offset;
        p_dst_u = dst_data[1] + offset;
        p_dst_v = dst_data[2] + offset;
        for( ycp_fin = ycp + buf_linesize; ycp < ycp_fin; ycp += 48, p_dst_y += 16, p_dst_u += 16, p_dst_v += 16 )
        {
            /* make -32768 (0x8000) */
            x3 = _mm_cmpeq_epi32(x3, x3);   /* 0xffff */
            x3 = _mm_slli_epi16(x3, 15);    /* 0x8000 */
            /* load */
            x0 = _mm_load_si128((__m128i *)p_dst_y);
            x1 = _mm_load_si128((__m128i *)p_dst_u);
            x2 = _mm_load_si128((__m128i *)p_dst_v);
            /* change uint16 to int16 in order to use _mm_madd_epi16()
             * range 0 - 65535 to -32768 - 32767 */
            x0 = _mm_add_epi16(x0, x3);
            x1 = _mm_add_epi16(x1, x3);
            x2 = _mm_add_epi16(x2, x3);

            /* calc Y */
            x3 = x0;
            x3 = _mm_unpackhi_epi16(x3, x0);
            x0 = _mm_unpacklo_epi16(x0, x0);

            x3 = _mm_madd_epi16(x3, _mm_load_si128((__m128i *)aY_coef[full_range]));
            x0 = _mm_madd_epi16(x0, _mm_load_si128((__m128i *)aY_coef[full_range]));

            x3 = _mm_srai_epi32(x3, 16);
            x0 = _mm_srai_epi32(x0, 16);

            x0 = _mm_packs_epi32(x0, x3);

            x0 = _mm_add_epi16(x0, _mm_load_si128((__m128i *)aY_offest[full_range]));

            /* calc U */
            x3 = x1;
            x3 = _mm_unpackhi_epi16(x3, x1);
            x1 = _mm_unpacklo_epi16(x1, x1);

            x3 = _mm_madd_epi16(x3, _mm_load_si128((__m128i *)aUV_coef[full_range]));
            x1 = _mm_madd_epi16(x1, _mm_load_si128((__m128i *)aUV_coef[full_range]));

            x3 = _mm_add_epi32(x3, _mm_load_si128((__m128i *)aUV_offest[full_range]));
            x1 = _mm_add_epi32(x1, _mm_load_si128((__m128i *)aUV_offest[full_range]));

            x3 = _mm_srai_epi32(x3, 16);
            x1 = _mm_srai_epi32(x1, 16);

            x1 = _mm_packs_epi32(x1, x3);

            /* calc V */
            x3 = x2;
            x3 = _mm_unpackhi_epi16(x3, x2);
            x2 = _mm_unpacklo_epi16(x2, x2);

            x3 = _mm_madd_epi16(x3, _mm_load_si128((__m128i *)aUV_coef[full_range]));
            x2 = _mm_madd_epi16(x2, _mm_load_si128((__m128i *)aUV_coef[full_range]));

            x3 = _mm_add_epi32(x3, _mm_load_si128((__m128i *)aUV_offest[full_range]));
            x2 = _mm_add_epi32(x2, _mm_load_si128((__m128i *)aUV_offest[full_range]));

            x3 = _mm_srai_epi32(x3, 16);
            x2 = _mm_srai_epi32(x2, 16);

            x2 = _mm_packs_epi32(x2, x3);

            /* shuffle order 7,6,5,4,3,2,1,0 to 7,3,5,1,6,2,4,0 */
            x0 = _mm_shufflelo_epi16(x0, _MM_SHUFFLE(3,1,2,0)); /* 7,6,5,4,3,1,2,0 */
            x0 = _mm_shufflehi_epi16(x0, _MM_SHUFFLE(3,1,2,0)); /* 7,5,6,4,3,1,2,0 */
            x0 = _mm_shuffle_epi32(  x0, _MM_SHUFFLE(3,1,2,0)); /* 7,5,3,1,6,4,2,0 */
            x0 = _mm_shufflelo_epi16(x0, _MM_SHUFFLE(3,1,2,0)); /* 7,5,3,1,6,2,4,0 */
            x0 = _mm_shufflehi_epi16(x0, _MM_SHUFFLE(3,1,2,0)); /* 7,3,5,1,6,2,4,0 */

            x1 = _mm_shufflelo_epi16(x1, _MM_SHUFFLE(3,1,2,0));
            x1 = _mm_shufflehi_epi16(x1, _MM_SHUFFLE(3,1,2,0));
            x1 = _mm_shuffle_epi32(  x1, _MM_SHUFFLE(3,1,2,0));
            x1 = _mm_shufflelo_epi16(x1, _MM_SHUFFLE(3,1,2,0));
            x1 = _mm_shufflehi_epi16(x1, _MM_SHUFFLE(3,1,2,0));

            x2 = _mm_shufflelo_epi16(x2, _MM_SHUFFLE(3,1,2,0));
            x2 = _mm_shufflehi_epi16(x2, _MM_SHUFFLE(3,1,2,0));
            x2 = _mm_shuffle_epi32(  x2, _MM_SHUFFLE(3,1,2,0));
            x2 = _mm_shufflelo_epi16(x2, _MM_SHUFFLE(3,1,2,0));
            x2 = _mm_shufflehi_epi16(x2, _MM_SHUFFLE(3,1,2,0));

            /* shuffle to PIXEL_YC */
            x3 = _mm_shuffle_epi32(x0, _MM_SHUFFLE(3,2,3,2));
            x0 = _mm_unpacklo_epi16(x0, x1);
            x1 = _mm_unpackhi_epi16(x1, x2);
            x2 = _mm_unpacklo_epi16(x2, x3);

            x3 = _mm_shuffle_epi32(x0, _MM_SHUFFLE(3,2,3,2));
            x0 = _mm_unpacklo_epi32(x0, x2);
            x2 = _mm_unpackhi_epi32(x2, x1);
            x1 = _mm_unpacklo_epi32(x1, x3);

            x3 = _mm_shuffle_epi32(x0, _MM_SHUFFLE(3,2,3,2));
            x0 = _mm_unpacklo_epi64(x0, x1);
            x1 = _mm_unpackhi_epi64(x1, x2);
            x2 = _mm_unpacklo_epi64(x2, x3);

            /* store */
            _mm_stream_si128((__m128i *)&ycp[ 0], x0);
            _mm_stream_si128((__m128i *)&ycp[16], x2);
            _mm_stream_si128((__m128i *)&ycp[32], x1);
        }
    }
}
#ifdef __GNUC__
#pragma GCC pop_options
#endif /* __GNUC__ */

static void convert_packed_chroma_to_planar(uint8_t *packed_chroma, uint8_t *planar_chroma, int packed_linesize, int chroma_width, int chroma_height)
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

static void convert_yv12i_to_yuy2( uint8_t *buf, int buf_linesize, uint8_t **pic_data, int *pic_linesize, int height )
{
    uint8_t *pic_y = pic_data[0];
    uint8_t *pic_u = pic_data[1];
    uint8_t *pic_v = pic_data[2];
    int x_loopcount = buf_linesize / 4;
#define COPY_Y_PIXEL_TO_YUY2( line, x ) \
    { \
        buf[line*buf_linesize + 4*x + 0] = pic_y[line*pic_linesize[0] + 2*x]; \
        buf[line*buf_linesize + 4*x + 2] = pic_y[line*pic_linesize[0] + 2*x+1]; \
    }
#define AVG_UV_PIXEL_TO_YUY2( line, x, y1, y2 ) \
    { \
        buf[line*buf_linesize + 4*x + 1] = (pic_u[y1*pic_linesize[1] + x] + pic_u[y2*pic_linesize[1] + x] + 1) >> 1; \
        buf[line*buf_linesize + 4*x + 3] = (pic_v[y1*pic_linesize[1] + x] + pic_v[y2*pic_linesize[1] + x] + 1) >> 1; \
    }
#define COPY_PIXEL_TO_YUY2( line1, line2, x ) \
    { \
        COPY_Y_PIXEL_TO_YUY2( line1, x ); \
        buf[line1*buf_linesize + 4*x + 1] = pic_u[line1*pic_linesize[1] + x]; \
        buf[line1*buf_linesize + 4*x + 3] = pic_v[line1*pic_linesize[1] + x]; \
        COPY_Y_PIXEL_TO_YUY2( line2, x ); \
        buf[line2*buf_linesize + 4*x + 1] = pic_u[line2*pic_linesize[1] + x]; \
        buf[line2*buf_linesize + 4*x + 3] = pic_v[line2*pic_linesize[1] + x]; \
    }
    /* copy first 2 lines */
    for( int x = 0; x < x_loopcount; x++ )
        COPY_PIXEL_TO_YUY2( 0, 1, x );
    buf += buf_linesize * 2;

    /* interlaced yv12 to yuy2 with 2,3-interpolation */
    int pic_offset = 0;
    int y;
    for( y = 2; y < height - 2; y += 4 )
    {
        pic_y = pic_data[0] + pic_linesize[0] * y;
        pic_u = pic_data[1] + pic_offset;
        pic_v = pic_data[2] + pic_offset;
        for( int x = 0; x < x_loopcount; x++ )
        {
            COPY_Y_PIXEL_TO_YUY2( 0, x );
            AVG_UV_PIXEL_TO_YUY2( 0, x, 0, 2 );

            COPY_PIXEL_TO_YUY2( 1, 2, x );

            COPY_Y_PIXEL_TO_YUY2( 3, x );
            AVG_UV_PIXEL_TO_YUY2( 3, x, 1, 3 );
        }
        buf        += buf_linesize * 4;
        pic_offset += pic_linesize[1] * 2;
    }

    /* copy last 2 lines */
    pic_y = pic_data[0] + pic_linesize[0] * y;
    pic_u = pic_data[1] + pic_offset;
    pic_v = pic_data[2] + pic_offset;
    for( int x = 0; x < x_loopcount; x++ )
        COPY_PIXEL_TO_YUY2( 0, 1, x );
#undef COPY_PIXEL_TO_YUY2
#undef AVG_UV_PIXEL_TO_YUY2
#undef COPY_Y_PIXEL_TO_YUY2
}

int to_yuv16le_to_yc48( AVCodecContext *video_ctx, struct SwsContext *sws_ctx, AVFrame *picture, uint8_t *buf )
{
    int _dst_linesize = picture->linesize[0] << (video_ctx->pix_fmt == PIX_FMT_YUV444P || video_ctx->pix_fmt == PIX_FMT_YUV440P);
    if( _dst_linesize & 15 )
        _dst_linesize = (_dst_linesize & 0xfffffff0) + 16;  /* Make mod16. */
    uint8_t *dst_data[4];
    dst_data[0] = av_mallocz( _dst_linesize * video_ctx->height * 3 );
    if( !dst_data[0] )
    {
        MessageBox( HWND_DESKTOP, "Failed to av_mallocz for YC48 convertion.", "lsmashinput", MB_ICONERROR | MB_OK );
        return 0;
    }
    for( int i = 1; i < 3; i++ )
        dst_data[i] = dst_data[i - 1] + _dst_linesize * video_ctx->height;
    dst_data[3] = NULL;
    const int dst_linesize[4] = { _dst_linesize, _dst_linesize, _dst_linesize, 0 };
    int output_height = sws_scale( sws_ctx, (const uint8_t* const*)picture->data, picture->linesize, 0, video_ctx->height, dst_data, dst_linesize );
    int buf_linesize  = video_ctx->width * YC48_SIZE;
    int output_size   = buf_linesize * output_height;
    DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_OK, "dst linesize = %d, output_height = %d, output_size = %d",
                                     _dst_linesize, output_height, output_size );
    /* Convert planar YUV 4:4:4 48bpp little-endian into YC48. */
    static int sse2_available = -1;
    if( sse2_available == -1 )
        sse2_available = check_sse2();
    func_get_output *convert = (sse2_available && ((buf_linesize | (size_t)buf) & 15) == 0)
                             ? convert_yuv16le_to_yc48_sse2
                             : convert_yuv16le_to_yc48;
    convert( buf, buf_linesize, dst_data, _dst_linesize, output_height, video_ctx->color_range == AVCOL_RANGE_JPEG );
    av_free( dst_data[0] );
    return output_size;
}

int to_rgb24( AVCodecContext *video_ctx, struct SwsContext *sws_ctx, AVFrame *picture, uint8_t *buf )
{
    const int dst_linesize[4] = { picture->linesize[0] + picture->linesize[1] + picture->linesize[2] + picture->linesize[3], 0, 0, 0 };
    uint8_t  *dst_data    [4] = { NULL, NULL, NULL, NULL };
    dst_data[0] = av_mallocz( dst_linesize[0] * video_ctx->height );
    if( !dst_data[0] )
    {
        MessageBox( HWND_DESKTOP, "Failed to av_malloc.", "lsmashinput", MB_ICONERROR | MB_OK );
        return 0;
    }
    int output_height = sws_scale( sws_ctx, (const uint8_t* const*)picture->data, picture->linesize, 0, video_ctx->height, dst_data, dst_linesize );
    int buf_linesize  = video_ctx->width * RGB24_SIZE;
    int output_size   = buf_linesize * output_height;
    DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_OK, "dst linesize = %d, output_height = %d, output_size = %d",
                                     dst_linesize[0], output_height, output_size );
    uint8_t *dst = dst_data[0] + dst_linesize[0] * output_height;
    while( output_height-- )
    {
        dst -= dst_linesize[0];
        memcpy( buf, dst, buf_linesize );
        buf += buf_linesize;
    }
    av_free( dst_data[0] );
    return output_size;
}

int to_yuy2( AVCodecContext *video_ctx, struct SwsContext *sws_ctx, AVFrame *picture, uint8_t *buf )
{
    int output_size = 0;
    if( picture->interlaced_frame
        && ((video_ctx->pix_fmt == PIX_FMT_YUV420P)
         || (video_ctx->pix_fmt == PIX_FMT_NV12   )
         || (video_ctx->pix_fmt == PIX_FMT_NV21   )) )
    {
        uint8_t *another_chroma = NULL;
        if( (video_ctx->pix_fmt == PIX_FMT_NV12)
         || (video_ctx->pix_fmt == PIX_FMT_NV21) )
        {
            another_chroma = av_mallocz( (picture->linesize[1] / 2) * video_ctx->height );
            if( !another_chroma )
            {
                MessageBox( HWND_DESKTOP, "Failed to av_malloc.", "lsmashinput", MB_ICONERROR | MB_OK );
                return 0;
            }
            /* convert chroma nv12 to yv12 (split packed uv into planar u and v) */
            convert_packed_chroma_to_planar( picture->data[1], another_chroma, picture->linesize[1], video_ctx->width / 2, video_ctx->height / 2 );
            /* change data set as yv12 */
            picture->data[2] = another_chroma;
            picture->linesize[1] /= 2;
            picture->linesize[2] = picture->linesize[1];
            if( video_ctx->pix_fmt == PIX_FMT_NV21 )
            {
                /* swap chroma pointer */
                uint8_t *tmp     = picture->data[2];
                picture->data[2] = picture->data[1];
                picture->data[1] = tmp;
            }
        }
        /* interlaced yv12 to yuy2 convertion */
        convert_yv12i_to_yuy2( buf, video_ctx->width * YUY2_SIZE, picture->data, picture->linesize, video_ctx->height );
        output_size = video_ctx->width * video_ctx->height * YUY2_SIZE;

        if( another_chroma )
            av_free( another_chroma );
    }
    else
    {
        const int dst_linesize[4] = { picture->linesize[0] + picture->linesize[1] + picture->linesize[2] + picture->linesize[3], 0, 0, 0 };
        uint8_t  *dst_data    [4] = { NULL, NULL, NULL, NULL };
        dst_data[0] = av_mallocz( dst_linesize[0] * video_ctx->height );
        if( !dst_data[0] )
        {
            MessageBox( HWND_DESKTOP, "Failed to av_malloc.", "lsmashinput", MB_ICONERROR | MB_OK );
            return 0;
        }
        int output_height = sws_scale( sws_ctx, (const uint8_t* const*)picture->data, picture->linesize, 0, video_ctx->height, dst_data, dst_linesize );
        int buf_linesize  = video_ctx->width * YUY2_SIZE;
        output_size   = buf_linesize * output_height;
        DEBUG_VIDEO_MESSAGE_BOX_DESKTOP( MB_OK, "dst linesize = %d, output_height = %d, output_size = %d",
                                         dst_linesize[0], output_height, output_size );
        uint8_t *dst = dst_data[0];
        while( output_height-- )
        {
            memcpy( buf, dst, buf_linesize );
            buf += buf_linesize;
            dst += dst_linesize[0];
        }
        av_free( dst_data[0] );
    }
    return output_size;
}
