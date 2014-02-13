/*****************************************************************************
 * utils.c / utils.cpp
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

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>

#include "utils.h"

void *lw_malloc_zero( size_t size )
{
    void *p = malloc( size );
    if( !p )
        return NULL;
    memset( p, 0, size );
    return p;
}

void lw_freep( void *pointer )
{
    void **p = (void **)pointer;
    free( *p );
    *p = NULL;
}

void *lw_memdup
(
    void  *src,
    size_t size
)
{
    if( size == 0 )
        return NULL;
    void *dst = malloc( size );
    if( !dst )
        return NULL;
    memcpy( dst, src, size );
    return dst;
}

int lw_log_write_message
(
    lw_log_handler_t *lhp,
    lw_log_level      level,
    char             *message,
    const char       *format,
    va_list           args
)
{
    if( level < lhp->level )
        return 0;
    char *prefix;
    switch( level )
    {
        case LW_LOG_FATAL:
            prefix = "Fatal";
            break;
        case LW_LOG_ERROR:
            prefix = "Error";
            break;
        case LW_LOG_WARNING:
            prefix = "Warning";
            break;
        case LW_LOG_INFO:
            prefix = "Info";
            break;
        default:
            prefix = "Unknown";
            break;
    }
    char temp[512];
    vsprintf( temp, format, args );
    sprintf( message, "[%s]: %s", prefix, temp );
    return 1;
}

int lw_check_file_extension
(
    const char *file_name,
    const char *extension
)
{
    int extension_length = strlen( extension );
    int file_name_length = strlen( file_name );
    if( file_name_length - extension_length <= 1 )
        return -1;
    const char *ext = &file_name[file_name_length - extension_length];
    return ext[-1] != '.' || memcmp( extension, ext, extension_length ) ? -1 : 0;
}

static inline double lw_round
(
    double x
)
{
    return x > 0 ? floor( x + 0.5 ) : ceil( x - 0.5 );
}

static inline double sigexp10
(
    double  value,
    double *exponent
)
{
    /* This function separates significand and exp10 from double floating point. */
    *exponent = 1;
    while( value < 1 )
    {
        value *= 10;
        *exponent /= 10;
    }
    while( value >= 10 )
    {
        value /= 10;
        *exponent *= 10;
    }
    return value;
}

int lw_try_rational_framerate
(
    double   framerate,
    int64_t *framerate_num,
    int64_t *framerate_den
)
{
#define DOUBLE_EPSILON 5e-5
    if( framerate == 0 )
        return 0;
    uint64_t fps_den;
    uint64_t fps_num;
    double   exponent;
    double   fps_sig = sigexp10( framerate, &exponent );
    int      i = 1;
    uint64_t base[2] = { 1001, (uint64_t)1e9 };
    for( int j = 0; j < 2; j++ )
        while( 1 )
        {
            fps_den = i * base[j];
            fps_num = (uint64_t)(lw_round( fps_den * fps_sig ) * exponent);
            if( fps_num > INT_MAX )
                break;
            if( fabs( ((double)fps_num / fps_den) / exponent - fps_sig ) < DOUBLE_EPSILON )
            {
                *framerate_num = (int)fps_num;
                *framerate_den = (int)fps_den;
                return 1;
            }
            ++i;
        }
    return 0;
#undef DOUBLE_EPSILON
}
