/********************************************************************************
 * vpy_input.c
 ********************************************************************************
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

/* This file is available under an ISC license.
 * However, when distributing its binary file, it will be under LGPL or GPL.
 * Don't distribute it if its license is GPL. */

#include "lwinput.h"

#include "VSHelper.h"

#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

#include "colorspace.h"
#include "video_output.h"

typedef struct VSScript VSScript;

typedef enum VSEvalFlags
{
    efSetWorkingDir = 1,
} VSEvalFlags;

#define VSSCRIPT_API( ret, name ) typedef ret (VS_CC *name##_func)
VSSCRIPT_API( int,           vsscript_init         )( void );
VSSCRIPT_API( int,           vsscript_finalize     )( void );
VSSCRIPT_API( int,           vsscript_evaluateFile )( VSScript **handle, const char *scriptFilename, int flags );
VSSCRIPT_API( void,          vsscript_freeScript   )( VSScript *handle );
VSSCRIPT_API( VSNodeRef *,   vsscript_getOutput    )( VSScript *handle, int index );
VSSCRIPT_API( const VSAPI *, vsscript_getVSApi     )( void );
#undef VSSCRIPT_API

typedef struct
{
    /* VSScript */
    HMODULE                   library;
    VSScript                 *vsscript;
    struct
    {
#define VSSCRIPT_DECLARE_FUNC( name ) name##_func name
        VSSCRIPT_DECLARE_FUNC( vsscript_init         );
        VSSCRIPT_DECLARE_FUNC( vsscript_finalize     );
        VSSCRIPT_DECLARE_FUNC( vsscript_evaluateFile );
        VSSCRIPT_DECLARE_FUNC( vsscript_freeScript   );
        VSSCRIPT_DECLARE_FUNC( vsscript_getOutput    );
        VSSCRIPT_DECLARE_FUNC( vsscript_getVSApi     );
#undef VSSCRIPT_DECLARE_FUNC
    } func;
    /* VapourSynth */
    const VSAPI              *vsapi;
    VSNodeRef                *node;
    const VSVideoInfo        *vi;
    /* Video stuff */
    AVFrame                  *av_frame;
    AVCodecContext           *ctx;
    lw_video_output_handler_t voh;
} vpy_handler_t;

static int load_vsscript_dll
(
    vpy_handler_t *hp
)
{
    hp->library = LoadLibrary( "vsscript" );
    if( !hp->library )
        return 0;
    hp->func.vsscript_init = (vsscript_init_func)GetProcAddress( hp->library, "_vsscript_init@0" );
    if( !hp->func.vsscript_init )
        goto fail;
    hp->func.vsscript_finalize = (vsscript_finalize_func)GetProcAddress( hp->library, "_vsscript_finalize@0" );
    if( !hp->func.vsscript_finalize )
        goto fail;
    hp->func.vsscript_evaluateFile = (vsscript_evaluateFile_func)GetProcAddress( hp->library, "_vsscript_evaluateFile@12" );
    if( !hp->func.vsscript_evaluateFile )
        goto fail;
    hp->func.vsscript_freeScript = (vsscript_freeScript_func)GetProcAddress( hp->library, "_vsscript_freeScript@4" );
    if( !hp->func.vsscript_freeScript )
        goto fail;
    hp->func.vsscript_getOutput = (vsscript_getOutput_func)GetProcAddress( hp->library, "_vsscript_getOutput@8" );
    if( !hp->func.vsscript_getOutput )
        goto fail;
    hp->func.vsscript_getVSApi = (vsscript_getVSApi_func)GetProcAddress( hp->library, "_vsscript_getVSApi@0" );
    if( !hp->func.vsscript_getVSApi )
        goto fail;
    return 0;
fail:
    FreeLibrary( hp->library );
    return -1;
}

static void close_vsscript_dll
(
    vpy_handler_t *hp
)
{
    assert( hp->library );
    if( hp->node )
        hp->vsapi->freeNode( hp->node );
    if( hp->vsscript )
        hp->func.vsscript_freeScript( hp->vsscript );
    hp->func.vsscript_finalize();
    FreeLibrary( hp->library );
}

static void *open_file
(
    char            *file_name,
    reader_option_t *opt
)
{
    /* Check file extension. */
    if( lw_check_file_extension( file_name, "vpy" ) < 0 )
        return NULL;
    /* Try to open the file as VapourSynth script. */
    vpy_handler_t *hp = lw_malloc_zero( sizeof(vpy_handler_t) );
    if( !hp )
        return NULL;
    if( load_vsscript_dll( hp ) < 0 )
    {
        free( hp );
        return NULL;
    }
    if( hp->func.vsscript_init() == 0 )
        goto fail;
    hp->vsapi = hp->func.vsscript_getVSApi();
    if( !hp->vsapi || hp->func.vsscript_evaluateFile( &hp->vsscript, file_name, efSetWorkingDir ) )
        goto fail;
    hp->node = hp->func.vsscript_getOutput( hp->vsscript, 0 );
    if( !hp->node )
        goto fail;
    hp->vi = hp->vsapi->getVideoInfo( hp->node );
    /* */
    hp->ctx = avcodec_alloc_context3( NULL );
    if( !hp->ctx )
        goto fail;
    return hp;
fail:
    if( hp->library )
        close_vsscript_dll( hp );
    free( hp );
    return NULL;
}

static int get_video_track
(
    lsmash_handler_t *h
)
{
    vpy_handler_t *hp = (vpy_handler_t *)h->video_private;
    if( !isConstantFormat( hp->vi ) || hp->vi->numFrames <= 0 )
        return -1;
    hp->av_frame = av_frame_alloc();
    return hp->av_frame ? 0 : -1;
}

static enum AVPixelFormat vs_to_av_input_pixel_format
(
    VSPresetFormat vs_input_pixel_format
)
{
    static const struct
    {
        VSPresetFormat     vs_input_pixel_format;
        enum AVPixelFormat av_input_pixel_format;
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
            { pfRGB24,     AV_PIX_FMT_GBRP        },
            { pfRGB27,     AV_PIX_FMT_GBRP9LE     },
            { pfRGB30,     AV_PIX_FMT_GBRP10LE    },
            { pfRGB48,     AV_PIX_FMT_GBRP16LE    },
            { pfGray8,     AV_PIX_FMT_GRAY8       },
            { pfGray16,    AV_PIX_FMT_GRAY16LE    },
            { pfNone,      AV_PIX_FMT_NONE        }
        };
    for( int i = 0; format_table[i].vs_input_pixel_format != pfNone; i++ )
        if( vs_input_pixel_format == format_table[i].vs_input_pixel_format )
            return format_table[i].av_input_pixel_format;
    return AV_PIX_FMT_NONE;
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
    /* Set up the initial input format. */
    hp->ctx->width       = hp->vi->width;
    hp->ctx->height      = hp->vi->height;
    hp->ctx->pix_fmt     = vs_to_av_input_pixel_format( hp->vi->format->id );
    hp->ctx->color_range = AVCOL_RANGE_UNSPECIFIED;
    hp->ctx->colorspace  = AVCOL_SPC_UNSPECIFIED;
    /* Set up video rendering. */
    if( !au_setup_video_rendering( &hp->voh, hp->ctx, opt, &h->video_format, hp->vi->width, hp->vi->height ) )
        return -1;
    return 0;
}

static inline int vs_is_rgb_format
(
    int color_family
)
{
    return color_family >= cmRGB && color_family < cmRGB + 1000000 ? 1 : 0;
}

static int read_video
(
    lsmash_handler_t *h,
    int               sample_number,
    void             *buf
)
{
    vpy_handler_t *hp = (vpy_handler_t *)h->video_private;
    const VSFrameRef *vs_frame = hp->vsapi->getFrame( sample_number, hp->node, NULL, 0 );
    if( !vs_frame )
        return 0;
    int is_rgb = vs_is_rgb_format( hp->vi->format->colorFamily );
    for( int i = 0; i < hp->vi->format->numPlanes; i++ )
    {
        static const int component_reorder[2][3] =
            {
                { 0, 1, 2 },    /* YUV -> YUV */
                { 2, 0, 1 }     /* RGB -> GBR */
            };
        int j = component_reorder[is_rgb][i];
        hp->av_frame->data    [j] = (uint8_t *)hp->vsapi->getReadPtr( vs_frame, i );
        hp->av_frame->linesize[j] = hp->vsapi->getStride( vs_frame, i );
    }
    const VSMap *props = hp->vsapi->getFramePropsRO( vs_frame );
    hp->ctx->color_range = hp->vsapi->propGetInt( props, "_ColorRange", 0, NULL ) ? AVCOL_RANGE_MPEG : AVCOL_RANGE_JPEG;
    hp->ctx->colorspace  = hp->vsapi->propGetInt( props, "_ColorSpace", 0, NULL );
    hp->av_frame->format = hp->ctx->pix_fmt;
    int frame_size = convert_colorspace( &hp->voh, hp->ctx, hp->av_frame, buf );
    hp->vsapi->freeFrame( vs_frame );
    return frame_size;
}

static void video_cleanup
(
    lsmash_handler_t *h
)
{
    vpy_handler_t *hp = (vpy_handler_t *)h->video_private;
    if( !hp )
        return;
    if( hp->av_frame )
        av_frame_free( &hp->av_frame );
    lw_cleanup_video_output_handler( &hp->voh );
}

static void close_file
(
    void *private_stuff
)
{
    vpy_handler_t *hp = (vpy_handler_t *)private_stuff;
    if( !hp )
        return;
    if( hp->ctx )
        avcodec_close( hp->ctx );
    if( hp->library )
        close_vsscript_dll( hp );
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
    video_cleanup,
    NULL,
    close_file
};
