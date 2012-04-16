/********************************************************************************
 * ffms_input.c
 ********************************************************************************
 * Copyright (C) 2011-2012 L-SMASH Works project
 *
 * Authors: Yusuke Nakamura <muken.the.vfrmaniac@gmail.com>
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

#include <ffms.h>

#include "lsmashinput.h"

typedef struct
{
    FFMS_VideoSource *video_source;
    FFMS_AudioSource *audio_source;
    FFMS_Index       *index;
    func_get_output  *get_output;
    char             *file_name;
    int               threads;
    int               out_linesize;
} ffms_handler_t;

func_get_output convert_yuv16le_to_yc48;
func_get_output convert_yuv16le_to_yc48_sse2;

static void *open_file( char *file_name, int threads )
{
    ffms_handler_t *hp = malloc_zero( sizeof(ffms_handler_t) );
    if( !hp )
        return NULL;
    FFMS_Init( 0, 0 );
    FFMS_ErrorInfo e = { 0 };
    hp->file_name = file_name;
    hp->threads   = threads;
    hp->index     = FFMS_MakeIndex( hp->file_name, -1, 0, NULL, NULL, FFMS_IEH_ABORT, NULL, NULL, &e );
    if( !hp->index )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to create the index." );
        free( hp );
        return NULL;
    }
    return hp;
}

static int get_first_video_track( lsmash_handler_t *h, int seek_mode, int forward_seek_threshold )
{
    ffms_handler_t *hp = (ffms_handler_t *)h->video_private;
    FFMS_ErrorInfo e = { 0 };
    int video_track_number = FFMS_GetFirstTrackOfType( hp->index, FFMS_TYPE_VIDEO, &e );
    if( video_track_number < 0 )
        return -1;
    hp->video_source = FFMS_CreateVideoSource( hp->file_name, video_track_number, hp->index, hp->threads, FFMS_SEEK_NORMAL + seek_mode, &e );
    return 0;
}

static int get_first_audio_track( lsmash_handler_t *h )
{
    ffms_handler_t *hp = (ffms_handler_t *)h->audio_private;
    FFMS_ErrorInfo e = { 0 };
    int audio_track_number = FFMS_GetFirstTrackOfType( hp->index, FFMS_TYPE_AUDIO, &e );
    if( audio_track_number < 0 )
        return -1;
    hp->audio_source = FFMS_CreateAudioSource( hp->file_name, audio_track_number, hp->index, FFMS_DELAY_FIRST_VIDEO_TRACK, &e );
    return 0;
}

static void destroy_disposable( void *private_stuff )
{
    ffms_handler_t *hp = (ffms_handler_t *)private_stuff;
    FFMS_DestroyIndex( hp->index );
    hp->index = NULL;
}

static void yuv16le_to_yc48( uint8_t *out_data, int out_linesize, uint8_t **in_data, int in_linesize, int height, int full_range )
{
    static int sse2_available = -1;
    if( sse2_available == -1 )
        sse2_available = check_sse2();
    if( sse2_available && ((out_linesize | (size_t)out_data) & 15) == 0 )
        convert_yuv16le_to_yc48_sse2( out_data, out_linesize, in_data, in_linesize, height, full_range );
    else
        convert_yuv16le_to_yc48( out_data, out_linesize, in_data, in_linesize, height, full_range );
}

static void flip_vertical( uint8_t *out_data, int out_linesize, uint8_t **in_data, int in_linesize, int height, int full_range )
{
    in_data[0] += in_linesize * height;
    while( height-- )
    {
        in_data[0] -= in_linesize;
        memcpy( out_data, in_data[0], out_linesize );
        out_data += out_linesize;
    }
}

static void just_copy( uint8_t *out_data, int out_linesize, uint8_t **in_data, int in_linesize, int height, int full_range )
{
    memcpy( out_data, in_data[0], out_linesize * height );
}

static int prepare_video_decoding( lsmash_handler_t *h, video_option_t *opt )
{
    ffms_handler_t *hp = (ffms_handler_t *)h->video_private;
    if( !hp->video_source )
        return 0;
    const FFMS_VideoProperties *vp = FFMS_GetVideoProperties( hp->video_source );
    if( !vp )
        return -1;
    h->video_sample_count = vp->NumFrames;
    h->framerate_num      = vp->FPSNumerator;
    h->framerate_den      = vp->FPSDenominator;
    /* Get frame to determine colorspace conversion. */
    FFMS_ErrorInfo e = { 0 };
    const FFMS_Frame *frame = FFMS_GetFrame( hp->video_source, 0, &e );
    if( !frame )
        return -1;
    int input_pixel_format = frame->EncodedPixelFormat;
    int output_pixel_format;
    output_colorspace_index index = determine_colorspace_conversion( &input_pixel_format, &output_pixel_format );
    static const struct
    {
        func_get_output      *get_output;
        int                   pixel_size;
        output_colorspace_tag compression;
    } colorspace_table[3] =
        {
            { yuv16le_to_yc48, YC48_SIZE,  OUTPUT_TAG_YC48 },
            { flip_vertical,   RGB24_SIZE, OUTPUT_TAG_RGB  },
            { just_copy,       YUY2_SIZE,  OUTPUT_TAG_YUY2 }
        };
    hp->get_output = colorspace_table[index].get_output;
    int pixelformat[2] = { output_pixel_format, -1 };
    if( FFMS_SetOutputFormatV2( hp->video_source, pixelformat, frame->EncodedWidth, frame->EncodedHeight, FFMS_RESIZER_POINT, &e ) )
    {
        MessageBox( HWND_DESKTOP, "Couldn't convert colorspace", "lsmashinput", MB_ICONERROR | MB_OK );
        FFMS_DestroyVideoSource( hp->video_source );
        hp->video_source = NULL;
        return 0;
    }
    frame = FFMS_GetFrame( hp->video_source, 0, &e );
    hp->out_linesize = frame->ScaledWidth * colorspace_table[index].pixel_size;
    /* BITMAPINFOHEADER */
    h->video_format.biSize        = sizeof( BITMAPINFOHEADER );
    h->video_format.biWidth       = frame->ScaledWidth;
    h->video_format.biHeight      = frame->ScaledHeight;
    h->video_format.biBitCount    = colorspace_table[index].pixel_size * 8;
    h->video_format.biCompression = colorspace_table[index].compression;
    return 0;
}

static int prepare_audio_decoding( lsmash_handler_t *h )
{
    ffms_handler_t *hp = (ffms_handler_t *)h->audio_private;
    if( !hp->audio_source )
        return 0;
    const FFMS_AudioProperties *ap = FFMS_GetAudioProperties( hp->audio_source );
    if( !ap )
        return -1;
    h->audio_pcm_sample_count = ap->NumSamples;
    /* WAVEFORMATEXTENSIBLE (WAVEFORMATEX) */
    WAVEFORMATEX *Format = &h->audio_format.Format;
    Format->nChannels       = ap->Channels;
    Format->nSamplesPerSec  = ap->SampleRate;
    Format->wBitsPerSample  = ap->BitsPerSample;
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
    DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_OK, "channels = %d, sampling_rate = %d, bits_per_sample = %d, block_align = %d, avg_bps = %d",
                                     Format->nChannels, Format->nSamplesPerSec,
                                     Format->wBitsPerSample, Format->nBlockAlign, Format->nAvgBytesPerSec );
    return 0;
}

static int read_video( lsmash_handler_t *h, int sample_number, void *buf )
{
    ffms_handler_t *hp = (ffms_handler_t *)h->video_private;
    FFMS_ErrorInfo e = { 0 };
    const FFMS_Frame *frame = FFMS_GetFrame( hp->video_source, sample_number, &e );
    if( frame )
    {
        hp->get_output( buf, hp->out_linesize, (uint8_t **)frame->Data, frame->Linesize[0], frame->ScaledHeight, frame->ColorRange == FFMS_CR_JPEG );
        return hp->out_linesize * frame->ScaledHeight;
    }
    return 0;
}

static int read_audio( lsmash_handler_t *h, int start, int wanted_length, void *buf )
{
    ffms_handler_t *hp = (ffms_handler_t *)h->audio_private;
    FFMS_ErrorInfo e = { 0 };
    if( !FFMS_GetAudio( hp->audio_source, buf, start, wanted_length, &e ) )
        return wanted_length;
    return 0;
}

static int is_keyframe( lsmash_handler_t *h, int sample_number )
{
    ffms_handler_t *hp = (ffms_handler_t *)h->video_private;
    FFMS_Track *video_track = FFMS_GetTrackFromVideo( hp->video_source );
    if( !video_track )
        return 0;
    const FFMS_FrameInfo *info = FFMS_GetFrameInfo( video_track, sample_number );
    if( info )
        return info->KeyFrame ? 1 : 0;
    return 1;
}

static void video_cleanup( lsmash_handler_t *h )
{
    ffms_handler_t *hp = (ffms_handler_t *)h->video_private;
    if( !hp )
        return;
    if( hp->video_source )
        FFMS_DestroyVideoSource( hp->video_source );
}

static void audio_cleanup( lsmash_handler_t *h )
{
    ffms_handler_t *hp = (ffms_handler_t *)h->audio_private;
    if( !hp )
        return;
    if( hp->audio_source )
        FFMS_DestroyAudioSource( hp->audio_source );
}

static void close_file( void *private_stuff )
{
    ffms_handler_t *hp = (ffms_handler_t *)private_stuff;
    if( !hp )
        return;
    if( hp->index )
        FFMS_DestroyIndex( hp->index );
    free( hp );
}

lsmash_reader_t ffms_reader =
{
    FFMS_READER,
    open_file,
    get_first_video_track,
    get_first_audio_track,
    destroy_disposable,
    prepare_video_decoding,
    prepare_audio_decoding,
    read_video,
    read_audio,
    is_keyframe,
    video_cleanup,
    audio_cleanup,
    close_file
};
