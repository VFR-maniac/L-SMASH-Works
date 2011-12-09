/*****************************************************************************
 * lsmashexport.c
 *****************************************************************************
 * Copyright (C) 2011 Libav-SMASH project
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

#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <windows.h>

/* L-SMASH */
#define LSMASH_DEMUXER_ENABLED
#include <lsmash.h>

#include "filter.h"

/* Macros for debug */
#ifdef DEBUG
#define DEBUG_MESSAGE_BOX_DESKTOP( uType, ... ) \
do \
{ \
    char temp[256]; \
    wsprintf( temp, __VA_ARGS__ ); \
    MessageBox( HWND_DESKTOP, temp, "lsmashexport", uType ); \
} while( 0 )
#else
#define DEBUG_MESSAGE_BOX_DESKTOP( uType, ... )
#endif

/* File filter */
#define MPEG4_FILE_EXT "All Support Formats (*.*)\0*.mp4;*.mov;*.3gp;*.3g2\0" \
                       "MP4 file (*.mp4)\0*.mp4\0" \
                       "M4V file (*.m4v)\0*.m4v\0" \
                       "MOV file (*.mov)\0*.mov\0" \
                       "3GPP file (*.3gp)\0*.3gp\0" \
                       "3GPP2 file (*.3g2)\0*.3g2\0"

FILTER_DLL filter =
{
    FILTER_FLAG_EXPORT|FILTER_FLAG_NO_CONFIG|FILTER_FLAG_ALWAYS_ACTIVE|FILTER_FLAG_PRIORITY_LOWEST|FILTER_FLAG_EX_INFORMATION,
    0,0,                        /* Size of configuration window */
    "Libav-SMASH Exporter",     /* Name of filter plugin */
    0,                          /* Number of trackbars */
    NULL,                       /* Pointer to group of names of trackbar */
    NULL,                       /* Pointer to group of initial values of trackbar */
    NULL,                       /* Minimum of trackbar */
    NULL,                       /* Maximum of trackbar */
    0,                          /* Number of checkboxes */
    NULL,                       /* Pointer to group of names of checkbox */
    NULL,                       /* Pointer to group of initial values of checkbox */
    NULL,                       /* Pointer to filter process function (If NULL, won't be called.) */
    NULL,                       /* Pointer to function called when beginning (If NULL, won't be called.) */
    NULL,                       /* Pointer to function called when ending (If NULL, won't be called.) */
    NULL,                       /* Pointer to function called when its configuration is updated (If NULL, won't be called.) */
    func_WndProc,               /* Pointer to function called when window message comes on configuration window (If NULL, won't be called.) */
    NULL,                       /* Pointer to group of set points of trackbar */
    NULL,                       /* Pointer to group of set points of checkbox */
    NULL,                       /* Pointer to extended data region (Valid only if FILTER_FLAG_EX_DATA is enabled.) */
    0,                          /* Size of extended data (Valid only if FILTER_FLAG_EX_DATA is enabled.) */
    "Libav-SMASH Exporter",     /* Information of filter plugin */
};

EXTERN_C FILTER_DLL __declspec(dllexport) * __stdcall GetFilterTable( void )
{
    return &filter;
}

#define VIDEO_TRACK 0
#define AUDIO_TRACK 1

typedef struct
{
    int                       active;
    uint32_t                  track_ID;
    uint32_t                  current_sample_number;
    uint32_t                  last_sample_delta;
    uint32_t                  end_sample_number;
    uint64_t                  composition_delay;
    uint64_t                  skip_duration;
    uint64_t                  sync_delay;
    double                    dts;
    lsmash_sample_t          *sample;
    lsmash_track_parameters_t track_param;
    lsmash_media_parameters_t media_param;
} input_track_t;

typedef struct
{
    lsmash_root_t *root;
    input_track_t  track[2];
    lsmash_movie_parameters_t movie_param;
} input_movie_t;

typedef struct
{
    uint32_t                  track_ID;
    uint32_t                  current_sample_number;
    uint32_t                  last_sample_delta;
    uint64_t                  last_sample_dts;
    uint64_t                  skip_dt_interval;
    lsmash_track_parameters_t track_param;
    lsmash_media_parameters_t media_param;
} output_track_t;

typedef struct
{
    lsmash_root_t *root;
    output_track_t track[2];
    uint32_t       number_of_tracks;
} output_movie_t;

typedef struct
{
    input_movie_t  *input;
    output_movie_t *output;
    int             source_file_id;
} lsmash_handler_t;

static int get_first_track_of_type( input_movie_t *input, uint32_t number_of_tracks, uint32_t type )
{
    uint32_t track_ID;
    uint32_t i;
    for( i = 1; i <= number_of_tracks; i++ )
    {
        track_ID = lsmash_get_track_ID( input->root, i );
        if( track_ID == 0 )
            return -1;
        lsmash_media_parameters_t media_param;
        lsmash_initialize_media_parameters( &media_param );
        if( lsmash_get_media_parameters( input->root, track_ID, &media_param ) )
        {
            DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to get media parameters." );
            return -1;
        }
        if( media_param.handler_type == type )
            break;
    }
    if( i > number_of_tracks )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to find %s track.",
                                   type == ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK ? "video" : "audio" );
        return -1;
    }
    type = type == ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK ? VIDEO_TRACK : AUDIO_TRACK;
    input->track[type].track_ID = track_ID;
    input->track[type].active = 1;
    lsmash_initialize_track_parameters( &input->track[type].track_param );
    if( lsmash_get_track_parameters( input->root, track_ID, &input->track[type].track_param ) )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to get track parameters." );
        return -1;
    }
    lsmash_initialize_media_parameters( &input->track[type].media_param );
    if( lsmash_get_media_parameters( input->root, track_ID, &input->track[type].media_param ) )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to get media parameters." );
        return -1;
    }
    if( lsmash_construct_timeline( input->root, track_ID ) )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to get construct timeline." );
        return -1;
    }
    if( lsmash_get_last_sample_delta_from_media_timeline( input->root, track_ID, &input->track[type].last_sample_delta ) )
    {
        DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to get the last sample delta." );
        return -1;
    }
    return 0;
}

static int open_input_movie( lsmash_handler_t *hp, char *file_name )
{
    input_movie_t *input = hp->input;
    input->root = lsmash_open_movie( file_name, LSMASH_FILE_MODE_READ );
    if( !input->root )
        return 0;
    lsmash_initialize_movie_parameters( &input->movie_param );
    lsmash_get_movie_parameters( input->root, &input->movie_param );
    uint32_t number_of_tracks = input->movie_param.number_of_tracks;
    if( number_of_tracks == 0 )
        return -1;
    /* Get video track. */
    if( get_first_track_of_type( input, number_of_tracks, ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK ) )
        return -1;
    /* Get audio track. If absent, ignore audio track. */
    get_first_track_of_type( input, number_of_tracks, ISOM_MEDIA_HANDLER_TYPE_AUDIO_TRACK );
    lsmash_discard_boxes( input->root );
    return 0;
}

static int open_output_file( lsmash_handler_t *hp, FILTER *fp )
{
    char file_name[MAX_PATH];
    if( !fp->exfunc->dlg_get_save_name( (LPSTR)file_name, MPEG4_FILE_EXT, NULL ) )
        return FALSE;
    output_movie_t *output = hp->output;
    output->root = lsmash_open_movie( file_name, LSMASH_FILE_MODE_WRITE );
    if( !output->root )
        return -1;
    lsmash_movie_parameters_t movie_param;
    lsmash_initialize_movie_parameters( &movie_param );
    input_movie_t *input = hp->input;
    movie_param.major_brand      = input->movie_param.major_brand;
    movie_param.minor_version    = input->movie_param.minor_version;
    movie_param.number_of_brands = input->movie_param.number_of_brands;
    movie_param.brands           = input->movie_param.brands;
    lsmash_set_movie_parameters( output->root, &movie_param );
    output->number_of_tracks = input->track[VIDEO_TRACK].active + input->track[AUDIO_TRACK].active;
    for( uint32_t i = 0; i < output->number_of_tracks; i++ )
    {
        input_track_t *in_track = &input->track[i];
        if( !in_track->active )
            continue;
        output_track_t *out_track = &output->track[i];
        out_track->track_ID = lsmash_create_track( output->root, in_track->media_param.handler_type );
        if( !out_track->track_ID )
        {
            DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to create a track." );
            return -1;
        }
        /* Copy track and media parameters except for track_ID. */
        out_track->track_param = in_track->track_param;
        out_track->media_param = in_track->media_param;
        /* Set track and media parameters specified by users */
        out_track->track_param.track_ID = out_track->track_ID;
        if( lsmash_set_track_parameters( output->root, out_track->track_ID, &out_track->track_param ) )
        {
            DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to set track parameters." );
            return -1;
        }
        if( lsmash_set_media_parameters( output->root, out_track->track_ID, &out_track->media_param ) )
        {
            DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to set media parameters." );
            return -1;
        }
        if( lsmash_copy_decoder_specific_info( output->root, out_track->track_ID, input->root, in_track->track_ID ) )
        {
            DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to copy a Decoder Specific Info." );
            return -1;
        }
        out_track->last_sample_delta = in_track->last_sample_delta;
    }
    return 0;
}

static int get_composition_delay( input_movie_t *input, input_track_t *in_track, uint32_t start_sample )
{
    uint32_t rap_number;
    if( lsmash_get_closest_random_accessible_point_from_media_timeline( input->root, in_track->track_ID, start_sample, &rap_number ) )
        return 0;
    uint64_t rap_dts;
    uint64_t rap_cts;
    uint32_t ctd_shift;
    if( lsmash_get_dts_from_media_timeline( input->root, in_track->track_ID, rap_number, &rap_dts ) )
        return -1;
    if( lsmash_get_cts_from_media_timeline( input->root, in_track->track_ID, rap_number, &rap_cts ) )
        return -1;
    if( lsmash_get_composition_to_decode_shift_from_media_timeline( input->root, in_track->track_ID, &ctd_shift ) )
        return -1;
    in_track->composition_delay = rap_cts - rap_dts + ctd_shift;
    /* Check if starting point is random accessible. */
    if( lsmash_get_closest_random_accessible_point_from_media_timeline( input->root, in_track->track_ID, start_sample, &rap_number ) )
        return -1;
    if( rap_number != start_sample )
    {
        /* Get duration that should be skipped. */
        if( lsmash_get_cts_from_media_timeline( input->root, in_track->track_ID, rap_number, &rap_cts ) )
            return -1;
        uint64_t seek_cts;
        if( lsmash_get_cts_from_media_timeline( input->root, in_track->track_ID, start_sample, &seek_cts ) )
            return -1;
        if( rap_cts < seek_cts )
            in_track->skip_duration = seek_cts - rap_cts;
    }
    return 0;
}

static int setup_export_range( lsmash_handler_t *hp, uint32_t start_sample, uint32_t end_sample )
{
    input_movie_t  *in_movie  = hp->input;
    output_movie_t *out_movie = hp->output;
    input_track_t *in_video_track = &in_movie->track[VIDEO_TRACK];
    in_video_track->current_sample_number = start_sample;
    in_video_track->end_sample_number     = end_sample;
    if( lsmash_get_dts_from_media_timeline( in_movie->root, in_video_track->track_ID,
                                            start_sample, &out_movie->track[VIDEO_TRACK].skip_dt_interval ) )
        return -1;
    if( get_composition_delay( in_movie, in_video_track, start_sample ) )
        return -1;
    uint64_t video_end_time;
    if( end_sample < lsmash_get_sample_count_in_media_timeline( in_movie->root, in_video_track->track_ID ) )
    {
        if( lsmash_get_dts_from_media_timeline( in_movie->root, in_video_track->track_ID, end_sample + 1, &video_end_time ) )
            return -1;
    }
    else
    {
        if( lsmash_get_dts_from_media_timeline( in_movie->root, in_video_track->track_ID, end_sample, &video_end_time ) )
            return -1;
        video_end_time += in_video_track->last_sample_delta;
    }
    input_track_t *in_audio_track = &in_movie->track[AUDIO_TRACK];
    if( in_video_track->media_param.timescale == 0 )
        return -1;
    double timescale_convert_multiplier = (double)in_audio_track->media_param.timescale / in_video_track->media_param.timescale;
    uint64_t audio_end_time = video_end_time * timescale_convert_multiplier + 0.5;
    uint64_t audio_start_time = out_movie->track[VIDEO_TRACK].skip_dt_interval * timescale_convert_multiplier;
    uint32_t sample_number = lsmash_get_sample_count_in_media_timeline( in_movie->root, in_audio_track->track_ID );
    uint64_t dts;
    do
    {
        if( lsmash_get_dts_from_media_timeline( in_movie->root, in_audio_track->track_ID, sample_number, &dts ) )
            return -1;
        if( dts > audio_end_time )
            in_audio_track->end_sample_number = sample_number;
        if( dts <= audio_start_time )
            break;
        --sample_number;
    } while( 1 );
    in_audio_track->current_sample_number = sample_number;
    in_audio_track->skip_duration = in_video_track->sync_delay = audio_start_time - dts;
    return lsmash_get_dts_from_media_timeline( in_movie->root, in_audio_track->track_ID,
                                               sample_number, &out_movie->track[AUDIO_TRACK].skip_dt_interval );
}

static int do_mux( lsmash_handler_t *hp, void *editp, FILTER *fp )
{
    input_movie_t  *in_movie  = hp->input;
    output_movie_t *out_movie = hp->output;
    double   largest_dts = 0;
    uint32_t num_consecutive_sample_skip = 0;
    uint32_t num_active_input_tracks = out_movie->number_of_tracks;
    uint64_t total_media_size = 0;
    int      type = 0;
    while( 1 )
    {

        input_track_t *in_track = &in_movie->track[type];
        /* Try append a sample in an input track where we didn't reach the end of media timeline. */
        if( in_track->active )
        {
            lsmash_sample_t *sample = in_track->sample;
            /* Get a new sample data if the track doesn't hold any one. */
            if( !sample )
            {
                if( type == VIDEO_TRACK )
                {
                    FRAME_STATUS fs;
                    if( !fp->exfunc->get_frame_status( editp, in_track->current_sample_number - 1, &fs ) )
                        return -1;
                    int source_file_id;
                    int source_video_number;
                    if( !fp->exfunc->get_source_video_number( editp, fs.video, &source_file_id, &source_video_number ) )
                        return -1;
                    if( source_file_id != hp->source_file_id )
                    {
                        MessageBox( HWND_DESKTOP, "Multiple input files is not supported yet.", "lsmashexport", MB_ICONERROR | MB_OK );
                        break;
                    }
                }
                sample = lsmash_get_sample_from_media_timeline( in_movie->root, in_track->track_ID, in_track->current_sample_number );
                if( sample )
                {
                    in_track->sample = sample;
                    in_track->dts = (double)sample->dts / in_track->media_param.timescale;
                }
                else
                {
                    if( lsmash_check_sample_existence_in_media_timeline( in_movie->root, in_track->track_ID, in_track->current_sample_number ) )
                    {
                        MessageBox( HWND_DESKTOP, "Failed to get a sample.", "lsmashexport", MB_ICONERROR | MB_OK );
                        break;
                    }
                    /* No more appendable samples in this track. */
                    in_track->sample = NULL;
                    in_track->active = 0;
                    if( --num_active_input_tracks == 0 )
                        break;      /* end of muxing */
                }
            }
            if( sample )
            {
                /* Append a sample if meeting a condition. */
                if( in_track->dts <= largest_dts || num_consecutive_sample_skip == num_active_input_tracks )
                {
                    /* The first DTS must be 0. */
                    output_track_t *out_track = &out_movie->track[type];
                    if( out_track->skip_dt_interval )
                    {
                        sample->dts -= out_track->skip_dt_interval;
                        sample->cts -= out_track->skip_dt_interval;
                    }
                    uint64_t sample_size     = sample->length;      /* sample might be deleted internally after appending. */
                    uint64_t last_sample_dts = sample->dts;         /* same as above */
                    /* Append a sample into output movie. */
                    if( lsmash_append_sample( out_movie->root, out_track->track_ID, sample ) )
                    {
                        lsmash_delete_sample( sample );
                        return -1;
                    }
                    largest_dts                       = max( largest_dts, in_track->dts );
                    in_track->sample                  = NULL;
                    out_track->last_sample_dts        = last_sample_dts;
                    num_consecutive_sample_skip       = 0;
                    total_media_size                 += sample_size;
                    if( in_track->current_sample_number == in_track->end_sample_number )
                    {
                        in_track->active = 0;
                        if( --num_active_input_tracks == 0 )
                            break;      /* end of muxing */
                    }
                    in_track->current_sample_number  += 1;
                    out_track->current_sample_number += 1;
                }
                else
                    ++num_consecutive_sample_skip;      /* Skip appendig sample. */
            }
        }
        type ^= 0x01;
    }
    for( uint32_t i = 0; i < out_movie->number_of_tracks; i++ )
        if( lsmash_flush_pooled_samples( out_movie->root, out_movie->track[i].track_ID, out_movie->track[i].last_sample_delta ) )
            MessageBox( HWND_DESKTOP, "Failed to flush samples.", "lsmashexport", MB_ICONERROR | MB_OK );
    return 0;
}

static int construct_timeline_maps( lsmash_handler_t *hp )
{
    output_movie_t *output = hp->output;
    input_movie_t  *input  = hp->input;
    for( uint32_t i = 0; i < output->number_of_tracks; i++ )
    {
        output_track_t *out_track = &output->track[i];
        input_track_t  *in_track  = &input->track[i];
        uint32_t movie_timescale = lsmash_get_movie_timescale( output->root );
        uint32_t media_timescale = lsmash_get_media_timescale( output->root, out_track->track_ID );
        if( !media_timescale )
            return -1;
        double timescale_convert_multiplier = (double)movie_timescale / media_timescale;
        if( in_track->skip_duration || in_track->sync_delay )
        {
            uint64_t empty_duration = in_track->sync_delay + in_track->skip_duration + lsmash_get_composition_to_decode_shift( output->root, out_track->track_ID );
            empty_duration = empty_duration * timescale_convert_multiplier + 0.5;
            if( lsmash_create_explicit_timeline_map( output->root, out_track->track_ID, empty_duration, ISOM_EDIT_MODE_EMPTY, ISOM_EDIT_MODE_NORMAL ) )
                return -1;
        }
        uint64_t start_time = in_track->composition_delay + in_track->skip_duration;
        uint64_t duration = (out_track->last_sample_dts + out_track->last_sample_delta - in_track->skip_duration) * timescale_convert_multiplier;
        if( lsmash_create_explicit_timeline_map( output->root, out_track->track_ID, duration, start_time, ISOM_EDIT_MODE_NORMAL ) )
            return -1;
    }
    return 0;
}

static void finish_movie( output_movie_t *output )
{
    lsmash_adhoc_remux_t moov_to_front;
    moov_to_front.func        = NULL;
    moov_to_front.buffer_size = 4*1024*1024;    /* 4MiB */
    moov_to_front.param       = NULL;
    lsmash_finish_movie( output->root, &moov_to_front );
}

static void cleanup_handler( lsmash_handler_t *hp )
{
    if( !hp )
        return;
    output_movie_t *output = hp->output;
    if( output )
        lsmash_destroy_root( output->root );
    input_movie_t *input = hp->input;
    if( input )
        lsmash_destroy_root( input->root );
}

static BOOL exporter_error( lsmash_handler_t *hp )
{
    cleanup_handler( hp );
    return FALSE;
}

BOOL func_WndProc( HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, void *editp, FILTER *fp )
{
    if( fp->exfunc->is_editing( editp ) != TRUE )
        return FALSE;
    if( message != WM_FILTER_EXPORT )
        return FALSE;
    int frame_s;
    int frame_e;
    if( !fp->exfunc->get_select_frame( editp, &frame_s, &frame_e ) )
    {
        MessageBox( HWND_DESKTOP, "Failed to get the selection range.", "lsmashexport", MB_ICONERROR | MB_OK );
        return FALSE;
    }
    FRAME_STATUS fs;
    if( !fp->exfunc->get_frame_status( editp, frame_s, &fs ) )
    {
        MessageBox( HWND_DESKTOP, "Failed to get the status of the first frame.", "lsmashexport", MB_ICONERROR | MB_OK );
        return FALSE;
    }
    int source_file_id;
    int source_video_number;
    if( !fp->exfunc->get_source_video_number( editp, fs.video, &source_file_id, &source_video_number ) )
    {
        MessageBox( HWND_DESKTOP, "Failed to get the number of the source video.", "lsmashexport", MB_ICONERROR | MB_OK );
        return FALSE;
    }
    FILE_INFO fi;
    if( !fp->exfunc->get_source_file_info( editp, &fi, source_file_id ) )
    {
        MessageBox( HWND_DESKTOP, "Failed to get the information of the source file.", "lsmashexport", MB_ICONERROR | MB_OK );
        return FALSE;
    }
    DEBUG_MESSAGE_BOX_DESKTOP( MB_OK, "Source file name: %s, source_file_id = %d, frames = %d.",
                               fi.name, source_file_id, fi.frame_n );
    lsmash_handler_t h       = { 0 };
    input_movie_t in_movie   = { 0 };
    output_movie_t out_movie = { 0 };
    h.input  = &in_movie;
    h.output = &out_movie;
    h.source_file_id = source_file_id;
    if( open_input_movie( &h, fi.name ) )
    {
        MessageBox( HWND_DESKTOP, "Failed to open the input file.", "lsmashexport", MB_ICONERROR | MB_OK );
        return exporter_error( &h );
    }
    if( open_output_file( &h, fp ) )
    {
        MessageBox( HWND_DESKTOP, "Failed to open the output file.", "lsmashexport", MB_ICONERROR | MB_OK );
        return exporter_error( &h );
    }
    if( setup_export_range( &h, frame_s + 1, frame_e + 1 ) )
    {
        MessageBox( HWND_DESKTOP, "Failed to set up export range.", "lsmashexport", MB_ICONERROR | MB_OK );
        return exporter_error( &h );
    }
    if( do_mux( &h, editp, fp ) )
    {
        MessageBox( HWND_DESKTOP, "Failed to do muxing.", "lsmashexport", MB_ICONERROR | MB_OK );
        return exporter_error( &h );
    }
    if( construct_timeline_maps( &h ) )
    {
        MessageBox( HWND_DESKTOP, "Failed to costruct timeline maps.", "lsmashexport", MB_ICONERROR | MB_OK );
        return exporter_error( &h );
    }
    finish_movie( h.output );
    cleanup_handler( &h );
    MessageBox( HWND_DESKTOP, "Muxing completed!", "lsmashexport", MB_OK );
    return FALSE;
}
