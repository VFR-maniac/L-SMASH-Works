/********************************************************************************
 * avs_input.c
 ********************************************************************************
 * Copyright (C) 2012 L-SMASH Works project
 *
 * Authors: Oka Motofumi <chikuzen.mo@gmail.com>
 *          Yusuke Nakamura <muken.the.vfrmaniac@gmail.com>
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

#include "lsmashinput.h"

#define AVSC_NO_DECLSPEC
#undef EXTERN_C
#include "avisynth_c.h"

#define AVS_INTERFACE_25 2

typedef struct {
    AVS_Clip              *clip;
    AVS_ScriptEnvironment *env;
    const AVS_VideoInfo   *vi;
    HMODULE                library;
    struct
    {
#define AVSC_DECLARE_FUNC( name ) name##_func name
        AVSC_DECLARE_FUNC( avs_clip_get_error );
        AVSC_DECLARE_FUNC( avs_create_script_environment );
        AVSC_DECLARE_FUNC( avs_delete_script_environment );
        AVSC_DECLARE_FUNC( avs_get_error );
        AVSC_DECLARE_FUNC( avs_get_frame );
        AVSC_DECLARE_FUNC( avs_get_audio );
        AVSC_DECLARE_FUNC( avs_get_video_info );
        AVSC_DECLARE_FUNC( avs_invoke );
        AVSC_DECLARE_FUNC( avs_bit_blt );
        AVSC_DECLARE_FUNC( avs_release_clip );
        AVSC_DECLARE_FUNC( avs_release_value );
        AVSC_DECLARE_FUNC( avs_release_video_frame );
        AVSC_DECLARE_FUNC( avs_take_clip );
#undef AVSC_DECLARE_FUNC
    } func;
} avs_handler_t;

static int load_avisynth_dll( avs_handler_t *hp )
{
#define LOAD_AVS_FUNC( name, ignore_fail ) \
{ \
    hp->func.name = (name##_func)GetProcAddress( hp->library, #name ); \
    if( !ignore_fail && !hp->func.name ) \
        goto fail; \
}
    hp->library = LoadLibrary( "avisynth" );
    if( !hp->library )
        return -1;
    LOAD_AVS_FUNC( avs_clip_get_error,            0 );
    LOAD_AVS_FUNC( avs_create_script_environment, 0 );
    LOAD_AVS_FUNC( avs_delete_script_environment, 1 );
    LOAD_AVS_FUNC( avs_get_error,                 1 );
    LOAD_AVS_FUNC( avs_get_frame,                 0 );
    LOAD_AVS_FUNC( avs_get_audio,                 0 );
    LOAD_AVS_FUNC( avs_get_video_info,            0 );
    LOAD_AVS_FUNC( avs_invoke,                    0 );
    LOAD_AVS_FUNC( avs_bit_blt,                   0 );
    LOAD_AVS_FUNC( avs_release_clip,              0 );
    LOAD_AVS_FUNC( avs_release_value,             0 );
    LOAD_AVS_FUNC( avs_release_video_frame,       0 );
    LOAD_AVS_FUNC( avs_take_clip,                 0 );
    return 0;
fail:
    FreeLibrary( hp->library );
    return -1;
#undef LOAD_AVS_FUNC
}

static AVS_Value invoke_filter( avs_handler_t *hp, AVS_Value before, const char *filter )
{
    hp->func.avs_release_clip( hp->clip );
    AVS_Value after = hp->func.avs_invoke( hp->env, filter, before, NULL );
    hp->func.avs_release_value( before );
    hp->clip = hp->func.avs_take_clip( after, hp->env );
    hp->vi = hp->func.avs_get_video_info( hp->clip );
    return after;
}

static AVS_Value initialize_avisynth( avs_handler_t *hp, char *input )
{
    if( load_avisynth_dll( hp ) )
        return avs_void;
    hp->env = hp->func.avs_create_script_environment( AVS_INTERFACE_25 );
    if( hp->func.avs_get_error && hp->func.avs_get_error( hp->env ) )
        return avs_void;
    AVS_Value arg = avs_new_value_string( input );
    AVS_Value res = hp->func.avs_invoke( hp->env, "Import", arg, NULL );
    if( avs_is_error( res ) )
        return res;
    AVS_Value mt_test = hp->func.avs_invoke( hp->env, "GetMTMode", avs_new_value_bool( 0 ), NULL );
    int mt_mode = avs_is_int( mt_test ) ? avs_as_int( mt_test ) : 0;
    hp->func.avs_release_value( mt_test );
    if( mt_mode > 0 && mt_mode < 5 )
    {
        AVS_Value temp = hp->func.avs_invoke( hp->env, "Distributor", res, NULL );
        hp->func.avs_release_value( res );
        res = temp;
    }
    hp->clip = hp->func.avs_take_clip( res, hp->env );
    hp->vi = hp->func.avs_get_video_info( hp->clip );
    if( avs_is_planar( hp->vi ) )
    {
        if( !(hp->vi->width & 1) )
            res = invoke_filter( hp, res, "ConvertToYUY2" );
        else
        {
            hp->func.avs_release_value( res );
            return avs_void;
        }
    }
    return avs_is_rgb32( hp->vi ) ? invoke_filter( hp, res, "ConvertToRGB24" ) : res;
}

static void close_avisynth_dll( avs_handler_t *hp )
{
    if( hp->clip )
        hp->func.avs_release_clip( hp->clip );
    if( hp->func.avs_delete_script_environment )
        hp->func.avs_delete_script_environment( hp->env );
    FreeLibrary( hp->library );
}

static void *open_file( char *file_name, int threads )
{
    /* Check file extension. */
    int file_name_length = strlen( file_name );
    if( file_name_length < 5 )
        return NULL;
    char *ext = &file_name[file_name_length - 4];
    if( ext[0] != '.' || ext[1] != 'a' || ext[2] != 'v' || ext[3] != 's' )
        return NULL;
    /* Try to open the file as avisynth script. */
    avs_handler_t *hp = malloc_zero( sizeof(avs_handler_t) );
    if( !hp )
        return NULL;
    AVS_Value res = initialize_avisynth( hp, file_name );
    if( !avs_is_clip( res ) )
    {
        if( hp->library )
            close_avisynth_dll( hp );
        free( hp );
        return NULL;
    }
    hp->func.avs_release_value( res );
    return hp;
}

static int get_video_track( lsmash_handler_t *h )
{
    avs_handler_t *hp = (avs_handler_t *)h->video_private;
    return hp->vi->num_frames > 0 ? 0 : -1;
}

static int get_audio_track( lsmash_handler_t *h )
{
    avs_handler_t *hp = (avs_handler_t *)h->audio_private;
    return hp->vi->num_audio_samples > 0 ? 0 : -1;
}

static int prepare_video_decoding( lsmash_handler_t *h, video_option_t *opt )
{
    avs_handler_t *hp = (avs_handler_t *)h->video_private;
    h->video_sample_count = hp->vi->num_frames;
    h->framerate_num      = hp->vi->fps_numerator;
    h->framerate_den      = hp->vi->fps_denominator;
    /* BITMAPINFOHEADER */
    h->video_format.biSize        = sizeof( BITMAPINFOHEADER );
    h->video_format.biWidth       = hp->vi->width;
    h->video_format.biHeight      = hp->vi->height;
    h->video_format.biBitCount    = avs_bits_per_pixel( hp->vi );
    h->video_format.biCompression = avs_is_rgb( hp->vi ) ? OUTPUT_TAG_RGB : OUTPUT_TAG_YUY2;
    return 0;
}

static int prepare_audio_decoding( lsmash_handler_t *h )
{
    avs_handler_t *hp = (avs_handler_t *)h->audio_private;
    h->audio_pcm_sample_count = hp->vi->num_audio_samples;
    /* WAVEFORMATEXTENSIBLE (WAVEFORMATEX) */
    WAVEFORMATEX *Format = &h->audio_format.Format;
    Format->nChannels       = hp->vi->nchannels;
    Format->nSamplesPerSec  = hp->vi->audio_samples_per_second;
    Format->wBitsPerSample  = avs_bytes_per_audio_sample( hp->vi ) * 8;
    Format->nBlockAlign     = (Format->nChannels * Format->wBitsPerSample) / 8;
    Format->nAvgBytesPerSec = Format->nSamplesPerSec * Format->nBlockAlign;
    Format->wFormatTag      = Format->wBitsPerSample == 8 || Format->wBitsPerSample == 16 ? WAVE_FORMAT_PCM : WAVE_FORMAT_EXTENSIBLE;
    if( Format->wFormatTag == WAVE_FORMAT_EXTENSIBLE )
    {
        Format->cbSize = 22;
        h->audio_format.Samples.wValidBitsPerSample = Format->wBitsPerSample;
        h->audio_format.SubFormat                   = KSDATAFORMAT_SUBTYPE_PCM;
    }
    else
        Format->cbSize = 0;
    return 0;
}

static int read_video( lsmash_handler_t *h, int sample_number, void *buf )
{
    avs_handler_t *hp = (avs_handler_t *)h->video_private;
    int width = hp->vi->width * avs_bits_per_pixel( hp->vi ) >> 3;
    AVS_VideoFrame *frame = hp->func.avs_get_frame( hp->clip, sample_number );
    if( hp->func.avs_clip_get_error( hp->clip ) )
        return 0;
    hp->func.avs_bit_blt( hp->env, buf, width, avs_get_read_ptr( frame ),
                          avs_get_pitch( frame ), width, hp->vi->height );
    hp->func.avs_release_video_frame( frame );
    return avs_bmp_size( hp->vi );
}

static int read_audio( lsmash_handler_t *h, int start, int wanted_length, void *buf )
{
    avs_handler_t *hp = (avs_handler_t *)h->audio_private;
    hp->func.avs_get_audio( hp->clip, buf, start, wanted_length );
    return wanted_length;
}

static int delay_audio( lsmash_handler_t *h, int *start, int wanted_length, int audio_delay )
{
    if( *start < audio_delay )
    {
        if( *start + wanted_length < audio_delay )
            return 0;
        *start = audio_delay - *start;
    }
    else
        *start -= audio_delay;
    return 1;
}

static void close_file( void *private_stuff )
{
    avs_handler_t *hp = (avs_handler_t *)private_stuff;
    if( !hp )
        return;
    if( hp->library )
        close_avisynth_dll( hp );
    free( hp );
}

lsmash_reader_t avs_reader =
{
    AVS_READER,
    open_file,
    get_video_track,
    get_audio_track,
    NULL,
    prepare_video_decoding,
    prepare_audio_decoding,
    read_video,
    read_audio,
    NULL,
    delay_audio,
    NULL,
    NULL,
    close_file
};
