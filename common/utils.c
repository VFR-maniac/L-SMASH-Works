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
