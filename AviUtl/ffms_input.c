/********************************************************************************
 * ffms_input.c
 ********************************************************************************
 * Copyright (C) 2011 L-SMASH Works project
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

#include "lsmashinput.h"

#include <ffms.h>
#include <libavutil/pixfmt.h>

typedef struct
{
    FFMS_VideoSource *video_source;
    FFMS_AudioSource *audio_source;
    func_get_output  *get_output;
    int               out_linesize;
} ffms_handler_t;

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

static int prepare_video_decoding( lsmash_handler_t *h )
{
    ffms_handler_t *hp = (ffms_handler_t *)h->reader.private_stuff;
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
    /* Hack for avoiding YUV-scale conversion */
    int in_pix_fmt = frame->EncodedPixelFormat;
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
        if( in_pix_fmt == range_hack_table[i].full )
            in_pix_fmt = range_hack_table[i].limited;
    /* Determine colorspace conversion. */
    enum PixelFormat out_pix_fmt;
    uint32_t pixel_size;
    uint32_t compression;
    switch( frame->EncodedPixelFormat )
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
            hp->get_output = yuv16le_to_yc48;
            pixel_size     = YC48_SIZE;
            out_pix_fmt    = PIX_FMT_YUV444P16LE;   /* planar YUV 4:4:4, 48bpp little-endian -> YC48 */
            compression    = MAKEFOURCC( 'Y', 'C', '4', '8' );
            break;
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
            hp->get_output = flip_vertical;
            pixel_size     = RGB24_SIZE;
            out_pix_fmt    = PIX_FMT_BGR24;     /* packed RGB 8:8:8, 24bpp, BGRBGR... */
            compression    = 0;
            break;
        default :
            hp->get_output = just_copy;
            pixel_size     = YUY2_SIZE;
            out_pix_fmt    = PIX_FMT_YUYV422;   /* packed YUV 4:2:2, 16bpp */
            compression    = MAKEFOURCC( 'Y', 'U', 'Y', '2' );
            break;
    }
    int pixelformat[2] = { out_pix_fmt, -1 };
    if( FFMS_SetOutputFormatV2( hp->video_source, pixelformat, frame->EncodedWidth, frame->EncodedHeight, FFMS_RESIZER_POINT, &e ) )
    {
        MessageBox( HWND_DESKTOP, "Couldn't convert colorspace", "lsmashinput", MB_ICONERROR | MB_OK );
        FFMS_DestroyVideoSource( hp->video_source );
        hp->video_source = NULL;
        return 0;
    }
    frame = FFMS_GetFrame( hp->video_source, 0, &e );
    hp->out_linesize = frame->ScaledWidth * pixel_size;
    /* BITMAPINFOHEADER */
    h->video_format.biSize        = sizeof( BITMAPINFOHEADER );
    h->video_format.biWidth       = frame->ScaledWidth;
    h->video_format.biHeight      = frame->ScaledHeight;
    h->video_format.biBitCount    = pixel_size * 8;
    h->video_format.biCompression = compression;
    return 0;
}

static int prepare_audio_decoding( lsmash_handler_t *h )
{
    ffms_handler_t *hp = (ffms_handler_t *)h->reader.private_stuff;
    if( !hp->audio_source )
        return 0;
    const FFMS_AudioProperties *ap = FFMS_GetAudioProperties( hp->audio_source );
    if( !ap )
        return -1;
    h->audio_pcm_sample_count = ap->NumSamples;
    /* WAVEFORMATEX */
    h->audio_format.nChannels       = ap->Channels;
    h->audio_format.nSamplesPerSec  = ap->SampleRate;
    h->audio_format.wBitsPerSample  = ap->BitsPerSample;
    h->audio_format.nBlockAlign     = (h->audio_format.nChannels * h->audio_format.wBitsPerSample) / 8;
    h->audio_format.nAvgBytesPerSec = h->audio_format.nSamplesPerSec * h->audio_format.nBlockAlign;
    h->audio_format.wFormatTag      = WAVE_FORMAT_PCM;      /* AviUtl doesn't support WAVE_FORMAT_EXTENSIBLE even if the input audio is 24bit PCM. */
    h->audio_format.cbSize          = 0;
    DEBUG_AUDIO_MESSAGE_BOX_DESKTOP( MB_OK, "channels = %d, sampling_rate = %d, bits_per_sample = %d, block_align = %d, avg_bps = %d",
                                     h->audio_format.nChannels, h->audio_format.nSamplesPerSec,
                                     h->audio_format.wBitsPerSample, h->audio_format.nBlockAlign, h->audio_format.nAvgBytesPerSec );
    return 0;
}

static void cleanup( lsmash_handler_t *h )
{
    ffms_handler_t *hp = (ffms_handler_t *)h->reader.private_stuff;
    if( !hp )
        return;
    if( hp->video_source )
        FFMS_DestroyVideoSource( hp->video_source );
    if( hp->audio_source )
        FFMS_DestroyAudioSource( hp->audio_source );
    free( hp );
}

static BOOL open_file( lsmash_handler_t *h, char *file_name, int threads )
{
    ffms_handler_t *hp = malloc_zero( sizeof(ffms_handler_t) );
    if( !hp )
        return FALSE;
    h->reader.private_stuff = hp;
    FFMS_Init( 0, 0 );
    FFMS_ErrorInfo e = { 0 };
    FFMS_Index *index = FFMS_MakeIndex( file_name, -1, 0, NULL, NULL, FFMS_IEH_ABORT, NULL, NULL, &e );
    if( !index )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to create the index." );
        return FALSE;
    }
    int video_track_number = FFMS_GetFirstTrackOfType( index, FFMS_TYPE_VIDEO, &e );
    if( video_track_number >= 0 )
        hp->video_source = FFMS_CreateVideoSource( file_name, video_track_number, index, threads, FFMS_SEEK_NORMAL, &e );
    int audio_track_number = FFMS_GetFirstTrackOfType( index, FFMS_TYPE_AUDIO, &e );
    if( audio_track_number >= 0 )
        hp->audio_source = FFMS_CreateAudioSource( file_name, audio_track_number, index, FFMS_DELAY_FIRST_VIDEO_TRACK, &e );
    FFMS_DestroyIndex( index );
    if( !hp->video_source && !hp->audio_source )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "No readable video and/or audio streams." );
        return FALSE;
    }
    /* Prepare decoding. */
    if( prepare_video_decoding( h )
     || prepare_audio_decoding( h ) )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to prepare decodings." );
        return FALSE;
    }
    return TRUE;
}

static int read_video( lsmash_handler_t *h, int sample_number, void *buf )
{
    ffms_handler_t *hp = (ffms_handler_t *)h->reader.private_stuff;
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
    ffms_handler_t *hp = (ffms_handler_t *)h->reader.private_stuff;
    FFMS_ErrorInfo e = { 0 };
    if( !FFMS_GetAudio( hp->audio_source, buf, start, wanted_length, &e ) )
        return wanted_length;
    return 0;
}

static BOOL is_keyframe( lsmash_handler_t *h, int sample_number )
{
    ffms_handler_t *hp = (ffms_handler_t *)h->reader.private_stuff;
    FFMS_Track *video_track = FFMS_GetTrackFromVideo( hp->video_source );
    if( !video_track )
        return FALSE;
    const FFMS_FrameInfo *info = FFMS_GetFrameInfo( video_track, sample_number );
    if( info )
        return info->KeyFrame == 0 ? FALSE : TRUE;
    return FALSE;
}

lsmash_reader_t ffms_reader =
{
    NULL,
    open_file,
    read_video,
    read_audio,
    is_keyframe,
    cleanup
};
