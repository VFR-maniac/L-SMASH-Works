/*****************************************************************************
 * colorspace_simd.c
 *****************************************************************************
 * Copyright (C) 2012-2013 L-SMASH Works project
 *
 * Authors: rigaya <rigaya34589@live.jp>
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

#include <stdint.h>

#include "../common/utils.h"

#ifdef __GNUC__
#define AUI_ALIGN(x) __attribute__((aligned(x)))
#define AUI_FUNC_ALIGN __attribute__((force_align_arg_pointer))
#else
#define AUI_ALIGN(x) __declspec(align(x))
#define AUI_FUNC_ALIGN
#endif

#ifdef __GNUC__
#pragma GCC target ("sse2")
#endif
#include <emmintrin.h>
/* SSE2 version of func convert_yuv16le_to_yc48
 * dst_data[0], dst_data[1], dst_data[2], buf, buf_linesize and dst_linesize need to be mod16. */
void AUI_FUNC_ALIGN convert_yuv16le_to_yc48_sse2( uint8_t *buf, int buf_linesize, uint8_t **dst_data, int *dst_linesize, int output_linesize, int output_height, int full_range )
{
    uint8_t *ycp, *ycp_fin;
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
    for( uint32_t h = 0; h < output_height; h++ )
    {
        p_dst_y = dst_data[0] + dst_linesize[0] * h;
        p_dst_u = dst_data[1] + dst_linesize[1] * h;
        p_dst_v = dst_data[2] + dst_linesize[2] * h;
        ycp = buf + buf_linesize * h;
        for( ycp_fin = ycp + output_linesize; ycp < ycp_fin; ycp += 48, p_dst_y += 16, p_dst_u += 16, p_dst_v += 16 )
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
    const int background_fill_count = ((48 - (output_linesize % 48)) % 48) >> 1;
    if( background_fill_count )
        for( int j = 0; j < output_height; j++ )
        {
            int16_t *ptr = (int16_t *)(buf + buf_linesize * j + output_linesize);
            for( int i = 0; i < background_fill_count; i++ )
                ptr[i] = 0;
        }
}

#ifdef __GNUC__
#pragma GCC target ("ssse3")
#endif
#include <tmmintrin.h>
/* SSSE3 version of func convert_yv12i_to_yuy2 */
void AUI_FUNC_ALIGN convert_yv12i_to_yuy2_ssse3( uint8_t *buf, int buf_linesize, uint8_t **pic_data, int *pic_linesize, int output_linesize, int height )
{
    static const uint8_t AUI_ALIGN(16) Array_5371[4][16] = {
        { 5, 3, 5, 3, 5, 3, 5, 3, 5, 3, 5, 3, 5, 3, 5, 3 }, { 7, 1, 7, 1, 7, 1, 7, 1, 7, 1, 7, 1, 7, 1, 7, 1 },
        { 1, 7, 1, 7, 1, 7, 1, 7, 1, 7, 1, 7, 1, 7, 1, 7 }, { 3, 5, 3, 5, 3, 5, 3, 5, 3, 5, 3, 5, 3, 5, 3, 5 },
    };
    static const int16_t AUI_ALIGN(16) Array_4[8] = { 4, 4, 4, 4, 4, 4, 4, 4 };
    uint8_t *ptr_y, *ptr_u, *ptr_v, *ptr_dst, *ptr_dst_fin;
    __m128i x0, x1, x2, x3, x4, x5, x6, x7;
    const int y_pitch = pic_linesize[0];
    const int uv_pitch = y_pitch >> 1;
    uint8_t *y_line = pic_data[0];
    uint8_t *u_line = pic_data[1];
    uint8_t *v_line = pic_data[2];
    uint8_t *dst_line = buf;
    /* copy first 2 lines */
    uint8_t *dst_line_fin = buf + buf_linesize * 2;
    for( ; dst_line < dst_line_fin; dst_line += buf_linesize, y_line += y_pitch, u_line += uv_pitch, v_line += uv_pitch )
    {
        ptr_dst = dst_line;
        ptr_y = y_line;
        ptr_u = u_line;
        ptr_v = v_line;
        ptr_dst_fin = ptr_dst + output_linesize;
        for( ; ptr_dst < ptr_dst_fin; ptr_dst += 64, ptr_y += 32, ptr_u += 16, ptr_v += 16 )
        {
            x0 = _mm_loadu_si128((__m128i *)ptr_u);
            x1 = _mm_loadu_si128((__m128i *)ptr_v);
            x2 = _mm_unpacklo_epi8(x0, x1);
            x3 = _mm_unpackhi_epi8(x0, x1);

            x0 = _mm_loadu_si128((__m128i *)(ptr_y +  0));
            x1 = _mm_loadu_si128((__m128i *)(ptr_y + 16));
            x4 = _mm_unpacklo_epi8(x0, x2);
            x5 = _mm_unpackhi_epi8(x0, x2);
            x6 = _mm_unpacklo_epi8(x1, x3);
            x7 = _mm_unpackhi_epi8(x1, x3);
            _mm_storeu_si128((__m128i *)(ptr_dst +  0), x4);
            _mm_storeu_si128((__m128i *)(ptr_dst + 16), x5);
            _mm_storeu_si128((__m128i *)(ptr_dst + 32), x6);
            _mm_storeu_si128((__m128i *)(ptr_dst + 48), x7);
        }
    }

    /* 5,3,7,1 interlaced yv12 to yuy2 interpolation */
    u_line = pic_data[1];
    v_line = pic_data[2];
    dst_line_fin = buf + buf_linesize * (height - 2);
    for( ; dst_line < dst_line_fin; dst_line += (buf_linesize << 2), y_line += (y_pitch << 2), u_line += (uv_pitch << 1), v_line += (uv_pitch << 1)  )
    {
        for( int i = 0; i < 4; i++ )
        {
            ptr_dst = dst_line + buf_linesize * i;
            ptr_y = y_line + y_pitch * i;
            ptr_u = u_line + uv_pitch * (i & 0x01);
            ptr_v = v_line + uv_pitch * (i & 0x01);
            ptr_dst_fin = ptr_dst + output_linesize;
            for( ; ptr_dst < ptr_dst_fin; ptr_dst += 64, ptr_y += 32, ptr_u += 16, ptr_v += 16 )
            {
                x0 = _mm_loadu_si128((__m128i *)(ptr_u));
                x1 = _mm_loadu_si128((__m128i *)(ptr_v));
                x2 = _mm_unpacklo_epi8(x0, x1);
                x3 = _mm_unpackhi_epi8(x0, x1);

                x0 = _mm_loadu_si128((__m128i *)(ptr_u + (uv_pitch << 1)));
                x1 = _mm_loadu_si128((__m128i *)(ptr_v + (uv_pitch << 1)));
                x4 = _mm_unpacklo_epi8(x0, x1);
                x5 = _mm_unpackhi_epi8(x0, x1);

                x6 = _mm_unpacklo_epi8(x2, x4);
                x7 = _mm_unpackhi_epi8(x2, x4);

                x6 = _mm_maddubs_epi16(x6, _mm_load_si128((__m128i *)Array_5371[i]));
                x7 = _mm_maddubs_epi16(x7, _mm_load_si128((__m128i *)Array_5371[i]));

                x6 = _mm_add_epi16(x6, _mm_load_si128((__m128i *)Array_4));
                x7 = _mm_add_epi16(x7, _mm_load_si128((__m128i *)Array_4));

                x6 = _mm_srai_epi16(x6, 3);
                x7 = _mm_srai_epi16(x7, 3);

                x6 = _mm_packus_epi16(x6, x7);
                x7 = _mm_loadu_si128((__m128i *)(ptr_y + 0));

                x0 = _mm_unpacklo_epi8(x7, x6);
                x1 = _mm_unpackhi_epi8(x7, x6);

                _mm_storeu_si128((__m128i *)(ptr_dst +  0), x0);
                _mm_storeu_si128((__m128i *)(ptr_dst + 16), x1);

                x6 = _mm_unpacklo_epi8(x3, x5);
                x7 = _mm_unpackhi_epi8(x3, x5);

                x6 = _mm_maddubs_epi16(x6, _mm_load_si128((__m128i *)Array_5371[i]));
                x7 = _mm_maddubs_epi16(x7, _mm_load_si128((__m128i *)Array_5371[i]));

                x6 = _mm_add_epi16(x6, _mm_load_si128((__m128i *)Array_4));
                x7 = _mm_add_epi16(x7, _mm_load_si128((__m128i *)Array_4));

                x6 = _mm_srai_epi16(x6, 3);
                x7 = _mm_srai_epi16(x7, 3);

                x6 = _mm_packus_epi16(x6, x7);
                x7 = _mm_loadu_si128((__m128i *)(ptr_y + 16));

                x0 = _mm_unpacklo_epi8(x7, x6);
                x1 = _mm_unpackhi_epi8(x7, x6);

                _mm_storeu_si128((__m128i *)(ptr_dst + 32), x0);
                _mm_storeu_si128((__m128i *)(ptr_dst + 48), x1);
            }
        }
    }

    /* copy last 2 lines */
    dst_line_fin = buf + buf_linesize * height;
    for( ; dst_line < dst_line_fin; dst_line += buf_linesize, y_line += y_pitch, u_line += uv_pitch, v_line += uv_pitch )
    {
        ptr_dst = dst_line;
        ptr_y = y_line;
        ptr_u = u_line;
        ptr_v = v_line;
        ptr_dst_fin = ptr_dst + (output_linesize & ~63);
        for( ; ptr_dst < ptr_dst_fin; ptr_dst += 64, ptr_y += 32, ptr_u += 16, ptr_v += 16 )
        {
            x0 = _mm_loadu_si128((__m128i *)ptr_u);
            x1 = _mm_loadu_si128((__m128i *)ptr_v);
            x2 = _mm_unpacklo_epi8(x0, x1);
            x3 = _mm_unpackhi_epi8(x0, x1);

            x0 = _mm_loadu_si128((__m128i *)(ptr_y +  0));
            x1 = _mm_loadu_si128((__m128i *)(ptr_y + 16));
            x4 = _mm_unpacklo_epi8(x0, x2);
            x5 = _mm_unpackhi_epi8(x0, x2);
            x6 = _mm_unpacklo_epi8(x1, x3);
            x7 = _mm_unpackhi_epi8(x1, x3);
            _mm_storeu_si128((__m128i *)(ptr_dst +  0), x4);
            _mm_storeu_si128((__m128i *)(ptr_dst + 16), x5);
            _mm_storeu_si128((__m128i *)(ptr_dst + 32), x6);
            _mm_storeu_si128((__m128i *)(ptr_dst + 48), x7);
        }
        ptr_dst_fin = ptr_dst + (output_linesize & 63);
        for( ; ptr_dst < ptr_dst_fin; ptr_dst += 4, ptr_y += 2, ptr_u += 1, ptr_v += 1 )
        {
            ptr_dst[0] = ptr_y[0];
            ptr_dst[1] = ptr_u[0];
            ptr_dst[2] = ptr_y[1];
            ptr_dst[3] = ptr_v[0];
        }
    }
    const int background_fill_count = MIN((64 - (output_linesize & 63)) & 63, buf_linesize - output_linesize) >> 2;
    if( background_fill_count )
    {
        static const uint32_t yuy2_background = (128<<24) + (128<<8);
        /* background_fill are not needed for last 2 lines, since the copying of them won't overwrite. */
        for( int j = 0; j < height - 2; j++ )
        {
            uint32_t *ptr = (uint32_t *)(buf + buf_linesize * j + output_linesize);
            for( int i = 0; i < background_fill_count; i++ )
                ptr[i] = yuy2_background;
        }
    }
}
