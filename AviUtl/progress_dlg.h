/*****************************************************************************
 * progress_dlg.h
 *****************************************************************************
 * Copyright (C) 2012-2015 L-SMASH Works project
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

#ifndef _PROGRESS_DLG_H_
#define _PROGRESS_DLG_H_

typedef struct
{
    HWND hnd;
    int  progress_percent;
    int  abort;
} progress_dlg_t;

void init_progress_dlg( progress_dlg_t *dlg, const char *module_name, int template_id );
void close_progress_dlg( progress_dlg_t *dlg );
int update_progress_dlg( progress_dlg_t *dlg, const char *mes, int progress_percent );

#endif /* _PROGRESS_DLG_H_ */
