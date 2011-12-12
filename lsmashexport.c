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
    char temp[512]; \
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
    uint32_t start_sample_number;       /* start time of media, not presentation */
    uint32_t end_sample_number;         /* end time of media */
    uint32_t skip_samples;
    uint32_t last_sample_delta;
    uint64_t last_sample_dts;
    uint64_t first_sample_dts;
    uint64_t presentation_start_time;
    uint64_t start_skip_duration;
    uint64_t end_skip_duration;
    uint64_t skip_dt_interval;
} input_sequence_t;

typedef struct
{
    int                       active;
    uint32_t                  track_ID;
    uint32_t                  last_sample_delta;
    lsmash_sample_t          *sample;
    double                    dts;
    lsmash_track_parameters_t track_param;
    lsmash_media_parameters_t media_param;
} input_track_t;

typedef struct
{
    int                       file_id;
    lsmash_root_t            *root;
    input_track_t             track[2];
    lsmash_movie_parameters_t movie_param;
} input_movie_t;

typedef struct
{
    uint32_t                  track_ID;
    uint64_t                  edit_offset;
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
    input_movie_t *input;
    uint32_t       sequence_number;
    uint32_t       sample_number;
} sent_sample_t;

typedef struct
{
    input_movie_t   **input;
    output_movie_t   *output;
    sent_sample_t    *sent[2];
    input_sequence_t *sequence[2];
    uint32_t          number_of_sequences;
    uint32_t          number_of_inputs;
    uint32_t          number_of_samples[2];
    int               with_audio;
#ifdef DEBUG
    FILE             *log_file;
#endif
} lsmash_handler_t;

static void *malloc_zero( size_t size )
{
    void *p = malloc( size );
    if( !p )
        return NULL;
    memset( p, 0, size );
    return p;
}

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
    type = (type == ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK) ? VIDEO_TRACK : AUDIO_TRACK;
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

static int open_input_movie( lsmash_handler_t *hp, char *file_name, int file_id )
{
    input_movie_t **input_array = realloc( hp->input, (hp->number_of_inputs + 1) * sizeof(input_movie_t *) );
    if( !input_array )
        return -1;
    hp->input = input_array;
    input_movie_t *input = malloc_zero( sizeof(input_movie_t) );
    if( !input )
    {
        input_array[hp->number_of_inputs] = NULL;
        return -1;
    }
    input_array[hp->number_of_inputs] = input;
    input->root = lsmash_open_movie( file_name, LSMASH_FILE_MODE_READ );
    if( !input->root )
        return -1;
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
    input->file_id = file_id;
    hp->with_audio |= input->track[VIDEO_TRACK].active;
    ++ hp->number_of_inputs;
    return 0;
}

static int check_file_id_duplication( lsmash_handler_t *hp, int file_id )
{
    for( uint32_t i = 0; i < hp->number_of_inputs; i++ )
        if( file_id == hp->input[i]->file_id )
            return 1;
    return 0;
}

static int get_input_number_from_file_id( lsmash_handler_t *hp, int file_id )
{
    for( int i = 0; i < hp->number_of_inputs; i++ )
        if( file_id == hp->input[i]->file_id )
            return i;
    return -1;
}

static int set_media_starting_point( input_movie_t *input, uint32_t track_ID, input_sequence_t *sequence, uint32_t start_sample )
{
    /* Check if starting point is random accessible. */
    uint32_t rap_number;
    if( lsmash_get_closest_random_accessible_point_from_media_timeline( input->root, track_ID, start_sample, &rap_number ) )
    {
        sequence->start_sample_number = start_sample;
        return 0;
    }
    if( rap_number != start_sample )
    {
        /* Get duration that should be skipped. */
        uint64_t rap_cts;
        if( lsmash_get_cts_from_media_timeline( input->root, track_ID, rap_number, &rap_cts ) )
            return -1;
        uint64_t seek_cts;
        if( lsmash_get_cts_from_media_timeline( input->root, track_ID, start_sample, &seek_cts ) )
            return -1;
        if( rap_cts < seek_cts )
            sequence->end_skip_duration = seek_cts - rap_cts;   /* Cut off presentation duration. */
    }
    /* Media starts from random accessible point.
     * Presentation does NOT always start from random accessible point. */
    sequence->start_sample_number = rap_number;
    sequence->skip_samples = start_sample - rap_number;
    return 0;
}

static int setup_exported_range_of_sequence( lsmash_handler_t *hp, input_movie_t *input, uint32_t sequence_number, uint32_t start_sample, uint32_t end_sample )
{
    input_track_t    *in_video_track  = &input->track[VIDEO_TRACK];
    input_sequence_t *video_sequence  = &hp->sequence[VIDEO_TRACK][sequence_number - 1];
    video_sequence->end_sample_number = end_sample;
    if( set_media_starting_point( input, in_video_track->track_ID, video_sequence, start_sample ) )
        return -1;
    if( lsmash_get_dts_from_media_timeline( input->root, in_video_track->track_ID,
                                            video_sequence->start_sample_number, &video_sequence->skip_dt_interval ) )
        return -1;
    input_track_t *in_audio_track = &input->track[AUDIO_TRACK];
    if( !hp->with_audio )
        return 0;
    uint64_t video_start_time;  /* start time of video for presentation */
    if( lsmash_get_dts_from_media_timeline( input->root, in_video_track->track_ID, start_sample, &video_start_time ) )
        return -1;
    uint64_t video_end_time;
    if( end_sample < lsmash_get_sample_count_in_media_timeline( input->root, in_video_track->track_ID ) )
    {
        if( lsmash_get_dts_from_media_timeline( input->root, in_video_track->track_ID, end_sample + 1, &video_end_time ) )
            return -1;
    }
    else
    {
        if( lsmash_get_dts_from_media_timeline( input->root, in_video_track->track_ID, end_sample, &video_end_time ) )
            return -1;
        video_end_time += in_video_track->last_sample_delta;
    }
    if( in_video_track->media_param.timescale == 0 )
        return -1;
    double timescale_convert_multiplier = (double)in_audio_track->media_param.timescale / in_video_track->media_param.timescale;
    uint64_t          audio_start_time = video_start_time * timescale_convert_multiplier;
    uint64_t          audio_end_time   = video_end_time   * timescale_convert_multiplier + 0.5;
    uint32_t          sample_number    = lsmash_get_sample_count_in_media_timeline( input->root, in_audio_track->track_ID );
    input_sequence_t *audio_sequence   = &hp->sequence[AUDIO_TRACK][sequence_number - 1];
    uint64_t          dts;
    do
    {
        if( lsmash_get_dts_from_media_timeline( input->root, in_audio_track->track_ID, sample_number, &dts ) )
            return -1;
        if( dts > audio_end_time )
        {
            audio_sequence->end_sample_number = sample_number;
            audio_sequence->end_skip_duration = dts - audio_end_time;
        }
        if( dts <= audio_start_time )
        {
            if( audio_sequence->end_sample_number == 0 )
                audio_sequence->end_sample_number = sample_number;
            break;
        }
        --sample_number;
    } while( 1 );
    audio_sequence->start_sample_number = sample_number;
    audio_sequence->start_skip_duration = audio_start_time - dts;
    uint32_t number_of_samples = hp->number_of_samples[AUDIO_TRACK] + audio_sequence->end_sample_number - audio_sequence->start_sample_number + 1;
    sent_sample_t *temp = realloc( hp->sent[AUDIO_TRACK], number_of_samples * sizeof(sent_sample_t) );
    if( !temp )
        return -1;
    hp->sent[AUDIO_TRACK] = temp;
    for( uint32_t i = hp->number_of_samples[AUDIO_TRACK]; i < number_of_samples; i++ )
    {
        hp->sent[AUDIO_TRACK][i].input           = input;
        hp->sent[AUDIO_TRACK][i].sequence_number = sequence_number;
        hp->sent[AUDIO_TRACK][i].sample_number   = sample_number;
        ++sample_number;
    }
    hp->number_of_samples[AUDIO_TRACK] = number_of_samples;
    return lsmash_get_dts_from_media_timeline( input->root, in_audio_track->track_ID,
                                               audio_sequence->start_sample_number, &audio_sequence->skip_dt_interval );
}

static int get_input_movies( lsmash_handler_t *hp, void *editp, FILTER *fp, int frame_s, int frame_e )
{
    uint32_t number_of_samples = frame_e - frame_s + 1;
    hp->number_of_samples[VIDEO_TRACK] = number_of_samples;
    hp->sent[VIDEO_TRACK] = malloc( number_of_samples * sizeof(sent_sample_t) );
    if( !hp->sent[VIDEO_TRACK] )
        return -1;
    int prev_source_file_id      = -1;
    int prev_source_video_number = -1;
    int current_input_number     = 0;
    for( int i = 0; i < number_of_samples; i++ )
    {
        FRAME_STATUS fs;
        if( !fp->exfunc->get_frame_status( editp, i + frame_s, &fs ) )
        {
            MessageBox( HWND_DESKTOP, "Failed to get the status of the first frame.", "lsmashexport", MB_ICONERROR | MB_OK );
            return -1;
        }
        int source_file_id;
        int source_video_number;
        if( !fp->exfunc->get_source_video_number( editp, fs.video, &source_file_id, &source_video_number ) )
        {
            MessageBox( HWND_DESKTOP, "Failed to get the number of the source video.", "lsmashexport", MB_ICONERROR | MB_OK );
            return -1;
        }
        FILE_INFO fi;
        if( !fp->exfunc->get_source_file_info( editp, &fi, source_file_id ) )
        {
            MessageBox( HWND_DESKTOP, "Failed to get the information of the source file.", "lsmashexport", MB_ICONERROR | MB_OK );
            return -1;
        }
        /* Check if encountered a new sequence. */
        if( source_file_id != prev_source_file_id || source_video_number != (prev_source_video_number + 1) )
        {
            if( !check_file_id_duplication( hp, source_file_id )
             && open_input_movie( hp, fi.name, source_file_id ) )
                return -1;
            current_input_number = get_input_number_from_file_id( hp, source_file_id );
            if( current_input_number == -1 )
                return -1;  /* unknown error */
            ++ hp->number_of_sequences;
            prev_source_file_id = source_file_id;
        }
        prev_source_video_number = source_video_number;
        hp->sent[VIDEO_TRACK][i].input           = hp->input[current_input_number];
        hp->sent[VIDEO_TRACK][i].sequence_number = hp->number_of_sequences;
        hp->sent[VIDEO_TRACK][i].sample_number   = source_video_number + 1;
    }
    hp->sequence[VIDEO_TRACK] = malloc_zero( hp->number_of_sequences * sizeof(input_sequence_t) );
    if( !hp->sequence[VIDEO_TRACK] )
        return -1;
    if( hp->with_audio )
    {
        hp->sequence[AUDIO_TRACK] = malloc_zero( hp->number_of_sequences * sizeof(input_sequence_t) );
        if( !hp->sequence[AUDIO_TRACK] )
            return -1;
    }
    /* Set up exported range of each sequence.
     * Also count number of audio samples for exporting if audio stream is present. */
    for( int i = 0; i < number_of_samples; )
    {
        input_movie_t *input               = hp->sent[VIDEO_TRACK][i].input;
        uint32_t       sequence_number     = hp->sent[VIDEO_TRACK][i].sequence_number;
        uint32_t       start_sample_number = hp->sent[VIDEO_TRACK][i].sample_number;
        for( i += 1; i < number_of_samples && sequence_number == hp->sent[VIDEO_TRACK][i].sequence_number; i++ );
        uint32_t end_sample_number = hp->sent[VIDEO_TRACK][i - 1].sample_number;
        if( setup_exported_range_of_sequence( hp, input, sequence_number, start_sample_number, end_sample_number ) )
            return -1;
    }
    return 0;
}

static int open_output_file( lsmash_handler_t *hp, FILTER *fp )
{
    char file_name[MAX_PATH];
    if( !fp->exfunc->dlg_get_save_name( (LPSTR)file_name, MPEG4_FILE_EXT, NULL ) )
        return -1;
    output_movie_t *output = hp->output;
    output->root = lsmash_open_movie( file_name, LSMASH_FILE_MODE_WRITE );
    if( !output->root )
        return -1;
#ifdef DEBUG
    char log_file_name[MAX_PATH + 10];
    sprintf( log_file_name, "%s_log.txt", file_name );
    hp->log_file = fopen( log_file_name, "wb" );
    if( !hp->log_file )
        return -1;
#endif
    lsmash_movie_parameters_t movie_param;
    lsmash_initialize_movie_parameters( &movie_param );
    sent_sample_t *sent  = &hp->sent[VIDEO_TRACK][0];
    input_movie_t *input = sent->input;
    movie_param.major_brand      = input->movie_param.major_brand;
    movie_param.minor_version    = input->movie_param.minor_version;
    movie_param.number_of_brands = input->movie_param.number_of_brands;
    movie_param.brands           = input->movie_param.brands;
    lsmash_set_movie_parameters( output->root, &movie_param );
    output->number_of_tracks = 1 + hp->with_audio;
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
        /* FIXME: support multiple sample descriptions for multiple input files. */
        if( lsmash_copy_decoder_specific_info( output->root, out_track->track_ID, input->root, in_track->track_ID ) )
        {
            DEBUG_MESSAGE_BOX_DESKTOP( MB_ICONERROR | MB_OK, "Failed to copy a Decoder Specific Info." );
            return -1;
        }
    }
    return 0;
}

static int do_mux( lsmash_handler_t *hp, void *editp, FILTER *fp )
{
    int               type                        = VIDEO_TRACK;
    int               active[2]                   = { 1, hp->with_audio };
    output_movie_t   *out_movie                   = hp->output;
    input_sequence_t *sequence[2]                 = { NULL, NULL };
    lsmash_sample_t  *sample[2]                   = { NULL, NULL };
    uint32_t          output_sample_count[2]      = { 0, 0 };
    uint32_t          sequence_number[2]          = { 0, 0 };
    double            dts[2]                      = { 0, 0 };
    double            largest_dts                 = 0;
    uint32_t          num_consecutive_sample_skip = 0;
    uint32_t          num_active_input_tracks     = out_movie->number_of_tracks;
    uint64_t          total_media_size            = 0;
    while( 1 )
    {
        /* Try append a sample in an input track where we didn't reach the end of media timeline. */
        if( !active[type] )
        {
            type ^= 0x01;
            continue;
        }
        sent_sample_t *sent     = &hp->sent[type][output_sample_count[type]];
        input_movie_t *input    = sent->input;
        input_track_t *in_track = &input->track[type];
        /* Get a new sample data if the track doesn't hold any one. */
        if( !sample[type] )
        {
            output_track_t *out_track = &out_movie->track[type];
            if( sequence_number[type] != sent->sequence_number )
            {
                /* Change sequence. */
                if( sequence[type] )
                    out_track->edit_offset = sequence[type]->last_sample_dts + sequence[type]->last_sample_delta;
                sequence[type] = &hp->sequence[type][sent->sequence_number - 1];
                sequence_number[type] = sent->sequence_number;
                /* Append all sample should be skipped by edit list. */
                for( uint32_t i = 0; i < sequence[type]->skip_samples; i++ )
                {
                    lsmash_sample_t *skip_sample = lsmash_get_sample_from_media_timeline( input->root, in_track->track_ID, sequence[type]->start_sample_number + i );
                    if( skip_sample )
                    {
                        /* The first DTS must be 0. */
                        skip_sample->dts += out_track->edit_offset - sequence[type]->skip_dt_interval;
                        skip_sample->cts += out_track->edit_offset - sequence[type]->skip_dt_interval;
                        dts[type] = (double)skip_sample->dts / in_track->media_param.timescale;
                    }
                    else
                    {
                        MessageBox( HWND_DESKTOP, "Failed to get a sample.", "lsmashexport", MB_ICONERROR | MB_OK );
                        goto abort;
                    }
                    uint32_t sample_delta;
                    if( lsmash_get_sample_delta_from_media_timeline( input->root, in_track->track_ID, sequence[type]->start_sample_number + i, &sample_delta ) )
                    {
                        lsmash_delete_sample( sample[type] );
                        MessageBox( HWND_DESKTOP, "Failed to get sample delta.", "lsmashexport", MB_ICONERROR | MB_OK );
                        goto abort;
                    }
                    uint64_t sample_size     = skip_sample->length;         /* sample might be deleted internally after appending. */
                    uint64_t last_sample_dts = skip_sample->dts;            /* same as above */
                    /* Let's append a sample that should be skipped by edit list. */
                    if( lsmash_append_sample( out_movie->root, out_track->track_ID, skip_sample ) )
                    {
                        lsmash_delete_sample( skip_sample );
                        MessageBox( HWND_DESKTOP, "Failed to append a sample.", "lsmashexport", MB_ICONERROR | MB_OK );
                        goto abort;
                    }
                    largest_dts                       = max( largest_dts, dts[type] );
                    sequence[type]->last_sample_dts   = last_sample_dts;
                    sequence[type]->last_sample_delta = sample_delta;
                    total_media_size                 += sample_size;
                    if( i == 0 )
                        sequence[type]->first_sample_dts = last_sample_dts;
                }
            }
            sample[type] = lsmash_get_sample_from_media_timeline( input->root, in_track->track_ID, sent->sample_number );
            if( sample[type] )
            {
                /* The first DTS must be 0. */
                sample[type]->dts += out_track->edit_offset - sequence[type]->skip_dt_interval;
                sample[type]->cts += out_track->edit_offset - sequence[type]->skip_dt_interval;
                dts[type] = (double)sample[type]->dts / in_track->media_param.timescale;
            }
            else
            {
                if( lsmash_check_sample_existence_in_media_timeline( input->root, in_track->track_ID, sent->sample_number ) )
                {
                    MessageBox( HWND_DESKTOP, "Failed to get a sample.", "lsmashexport", MB_ICONERROR | MB_OK );
                    break;
                }
                /* no more samples in this track */
                active[type] = 0;
                if( --num_active_input_tracks == 0 )
                    break;      /* end of muxing */
                type ^= 0x01;
                continue;
            }
        }
        /* Append a sample if meeting a condition. */
        if( dts[type] <= largest_dts || num_consecutive_sample_skip == num_active_input_tracks )
        {
            uint32_t sample_delta;
            if( lsmash_get_sample_delta_from_media_timeline( input->root, in_track->track_ID, sent->sample_number, &sample_delta ) )
            {
                lsmash_delete_sample( sample[type] );
                MessageBox( HWND_DESKTOP, "Failed to get sample delta.", "lsmashexport", MB_ICONERROR | MB_OK );
                break;
            }
            uint64_t sample_size     = sample[type]->length;        /* sample might be deleted internally after appending. */
            uint64_t last_sample_dts = sample[type]->dts;           /* same as above */
            uint64_t last_sample_cts = sample[type]->cts;           /* same as above */
            output_track_t *out_track = &out_movie->track[type];
#ifdef DEBUG
            char log_data[1024];
            sprintf( log_data, "sequence_number=%"PRIu32", file_id=%d, type=%s, sample_number=%"PRIu32", source_sample_number=%"PRIu32", "
                     "edit_offset=%"PRIu64", skip_dt_interval=%"PRIu64", DTS=%"PRIu64", CTS=%"PRIu64", sample_delta=%"PRIu32"\n",
                     sequence_number[type], input->file_id, type ? "audio" : "video", output_sample_count[type] + 1, sent->sample_number,
                     out_track->edit_offset, sequence[type]->skip_dt_interval, sample[type]->dts, sample[type]->cts, sample_delta );
            fwrite( log_data, 1, strlen( log_data ), hp->log_file );
#endif
            /* Append a sample into output movie. */
            if( lsmash_append_sample( out_movie->root, out_track->track_ID, sample[type] ) )
            {
                lsmash_delete_sample( sample[type] );
                MessageBox( HWND_DESKTOP, "Failed to append a sample.", "lsmashexport", MB_ICONERROR | MB_OK );
                break;
            }
            largest_dts                       = max( largest_dts, dts[type] );
            sample[type]                      = NULL;
            sequence[type]->last_sample_dts   = last_sample_dts;
            sequence[type]->last_sample_delta = sample_delta;
            num_consecutive_sample_skip       = 0;
            total_media_size                 += sample_size;
            if( sent->sample_number == sequence[type]->start_sample_number )
                sequence[type]->first_sample_dts = last_sample_dts;
            /* Check if this sample is the first sample in the presentation of this sequence.
             * If so, set its CTS into start time of the presentation of this sequence. */
            if( sent->sample_number == sequence[type]->start_sample_number + sequence[type]->skip_samples )
                sequence[type]->presentation_start_time = last_sample_cts;
            /* Check if this track has no more samples to be appended. */
            if( ++output_sample_count[type] == hp->number_of_samples[type] )
            {
                active[type] = 0;
                if( --num_active_input_tracks == 0 )
                    break;      /* end of muxing */
            }
        }
        else
            ++num_consecutive_sample_skip;      /* Skip appendig sample. */
        type ^= 0x01;
    }
abort:
    for( uint32_t i = 0; i < out_movie->number_of_tracks; i++ )
        if( lsmash_flush_pooled_samples( out_movie->root, out_movie->track[i].track_ID, sequence[i]->last_sample_delta ) )
            MessageBox( HWND_DESKTOP, "Failed to flush samples.", "lsmashexport", MB_ICONERROR | MB_OK );
    return 0;
}

static int construct_timeline_maps( lsmash_handler_t *hp )
{
    output_movie_t *output = hp->output;
    for( uint32_t i = 0; i < output->number_of_tracks; i++ )
    {
        if( !hp->sequence[i] )
            continue;
        output_track_t *out_track = &output->track[i];
        uint32_t media_timescale = lsmash_get_media_timescale( output->root, out_track->track_ID );
        if( media_timescale == 0 )
            continue;
        uint32_t movie_timescale = lsmash_get_movie_timescale( output->root );
        double timescale_convert_multiplier = (double)movie_timescale / media_timescale;
        for( int j = 0; j < hp->number_of_sequences; j++ )
        {
            /* Create edits per sequence. */
            input_sequence_t *sequence = &hp->sequence[i][j];
            int64_t start_time = sequence->presentation_start_time + sequence->start_skip_duration;
            uint64_t skip_duration = sequence->start_skip_duration + sequence->end_skip_duration;
            uint64_t duration = (sequence->last_sample_dts + sequence->last_sample_delta - sequence->first_sample_dts - skip_duration)
                              * timescale_convert_multiplier;
            if( lsmash_create_explicit_timeline_map( output->root, out_track->track_ID, duration, start_time, ISOM_EDIT_MODE_NORMAL ) )
                return -1;
#ifdef DEBUG
            char log_data[1024];
            sprintf( log_data, "type = %s, last_sample_dts = %"PRIu64", last_sample_delta=%"PRIu32", first_sample_dts=%"PRIu64", "
                     "start_skip_duration=%"PRIu64", end_skip_duration=%"PRIu64"\n",
                     i ? "audio" : "video", sequence->last_sample_dts, sequence->last_sample_delta, sequence->first_sample_dts,
                     sequence->start_skip_duration, sequence->end_skip_duration );
            fwrite( log_data, 1, strlen( log_data ), hp->log_file );
#endif
        }
    }
    return 0;
}

static int finish_movie( output_movie_t *output )
{
    lsmash_adhoc_remux_t moov_to_front;
    moov_to_front.func        = NULL;
    moov_to_front.buffer_size = 4*1024*1024;    /* 4MiB */
    moov_to_front.param       = NULL;
    return lsmash_finish_movie( output->root, &moov_to_front );
}

static void cleanup_handler( lsmash_handler_t *hp )
{
    if( !hp )
        return;
    output_movie_t *output = hp->output;
    if( output )
        lsmash_destroy_root( output->root );
    if( hp->input )
        for( uint32_t i = 0; i < hp->number_of_inputs; i++ )
            if( hp->input[i] )
                lsmash_destroy_root( hp->input[i]->root );
    if( hp->sequence[VIDEO_TRACK] )
        free( hp->sequence[VIDEO_TRACK] );
    if( hp->sequence[AUDIO_TRACK] )
        free( hp->sequence[AUDIO_TRACK] );
#ifdef DEBUG
    if( hp->log_file )
        fclose( hp->log_file );
#endif
}

static BOOL exporter_error( lsmash_handler_t *hp )
{
    cleanup_handler( hp );
    return FALSE;
}

BOOL func_WndProc( HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, void *editp, FILTER *fp )
{
    if( !fp->exfunc->is_editing( editp ) )
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
    lsmash_handler_t h       = { 0 };
    output_movie_t out_movie = { 0 };
    h.output = &out_movie;
    if( get_input_movies( &h, editp, fp, frame_s, frame_e ) )
    {
        MessageBox( HWND_DESKTOP, "Failed to open the input files.", "lsmashexport", MB_ICONERROR | MB_OK );
        return exporter_error( &h );
    }
    if( open_output_file( &h, fp ) )
    {
        MessageBox( HWND_DESKTOP, "Failed to open the output file.", "lsmashexport", MB_ICONERROR | MB_OK );
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
    if( finish_movie( h.output ) )
    {
        MessageBox( HWND_DESKTOP, "Failed to finish movie.", "lsmashexport", MB_ICONERROR | MB_OK );
        return exporter_error( &h );
    }
    cleanup_handler( &h );
    MessageBox( HWND_DESKTOP, "Muxing completed!", "lsmashexport", MB_OK );
    return FALSE;
}
