/*****************************************************************************
 * lwindex.c / lwindex.cpp
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

#ifdef __cplusplus
extern "C"
{
#endif  /* __cplusplus */
#include <libavformat/avformat.h>       /* Demuxer */
#include <libavcodec/avcodec.h>         /* Decoder */
#include <libavresample/avresample.h>   /* Resampler/Buffer */
#include <libavutil/mathematics.h>      /* Timebase rescaler */
#include <libavutil/pixdesc.h>
#ifdef __cplusplus
}
#endif  /* __cplusplus */

#include "utils.h"
#include "video_output.h"
#include "audio_output.h"
#include "lwlibav_dec.h"
#include "lwlibav_video.h"
#include "lwlibav_audio.h"
#include "progress.h"
#include "lwindex.h"

typedef struct
{
    lwlibav_extradata_handler_t exh;
    AVCodecParserContext       *parser_ctx;
    AVBitStreamFilterContext   *bsf;
    AVFrame                    *picture;
    uint32_t                    delay_count;
    lw_field_info_t             last_field_info;
    int                         mpeg12_video;   /* 0: neither MPEG-1 Video nor MPEG-2 Video
                                                 * 1: either MPEG-1 Video or MPEG-2 Video */
    int                         vc1_wmv3;       /* 0: neither VC-1 nor WMV3
                                                 * 1: either VC-1 or WMV3
                                                 * 2: either VC-1 or WMV3 encapsulated in ASF */
    int                         buffer_size;
    uint8_t                    *buffer;
#if LIBAVCODEC_VERSION_MICRO < 100
    int (*decode)(AVCodecContext *, AVFrame *, int *, AVPacket * );
#else
    int (*decode)(AVCodecContext *, AVFrame *, int *, const AVPacket * );
#endif
} lwindex_helper_t;

typedef struct
{
    int64_t pts;
    int64_t dts;
} video_timestamp_t;

typedef struct
{
    video_timestamp_t temp;
    video_timestamp_t core;
} video_timestamp_temp_t;

static inline int check_frame_reordering
(
    video_frame_info_t *info,
    uint32_t            sample_count
)
{
    for( uint32_t i = 2; i <= sample_count; i++ )
        if( info[i].pts < info[i - 1].pts )
            return 1;
    return 0;
}

static int compare_info_pts
(
    const video_frame_info_t *a,
    const video_frame_info_t *b
)
{
    int64_t diff = (int64_t)(a->pts - b->pts);
    return diff > 0 ? 1 : (diff == 0 ? 0 : -1);
}

static int compare_pts
(
    const video_timestamp_t *a,
    const video_timestamp_t *b
)
{
    int64_t diff = (int64_t)(a->pts - b->pts);
    return diff > 0 ? 1 : (diff == 0 ? 0 : -1);
}

static int compare_dts
(
    const video_timestamp_t *a,
    const video_timestamp_t *b
)
{
    int64_t diff = (int64_t)(a->dts - b->dts);
    return diff > 0 ? 1 : (diff == 0 ? 0 : -1);
}

static inline void sort_info_presentation_order
(
    video_frame_info_t *info,
    uint32_t            sample_count
)
{
    qsort( info, sample_count, sizeof(video_frame_info_t), (int(*)( const void *, const void * ))compare_info_pts );
}

static inline void sort_presentation_order
(
    video_timestamp_t *timestamp,
    uint32_t           sample_count,
    size_t             size
)
{
    qsort( timestamp, sample_count, size, (int(*)( const void *, const void * ))compare_pts );
}

static inline void sort_decoding_order
(
    video_timestamp_t *timestamp,
    uint32_t           sample_count,
    size_t             size
)
{
    qsort( timestamp, sample_count, size, (int(*)( const void *, const void * ))compare_dts );
}

static inline int lineup_seek_base_candidates
(
    lwlibav_file_handler_t *lwhp
)
{
    return !strcmp( lwhp->format_name, "mpeg" )
        || !strcmp( lwhp->format_name, "mpegts" )
        || lwhp->raw_demuxer
         ? SEEK_DTS_BASED | SEEK_PTS_BASED | SEEK_POS_BASED | SEEK_POS_CORRECTION
         : SEEK_DTS_BASED | SEEK_PTS_BASED | SEEK_POS_CORRECTION;
}

static void mpeg12_video_vc1_genarate_pts
(
    lwlibav_video_decode_handler_t *vdhp
)
{
    video_frame_info_t *info = vdhp->frame_list;
    int      reordered_stream  = 0;
    uint32_t num_consecutive_b = 0;
    for( uint32_t i = 1; i <= vdhp->frame_count; i++ )
    {
        /* In the case where B-pictures exist
         * Decode order
         *      I[1]P[2]P[3]B[4]B[5]P[6]...
         * DTS
         *        0   1   2   3   4   5 ...
         * Presentation order
         *      I[1]P[2]B[4]B[5]P[3]P[6]...
         * PTS
         *        1   2   3   4   5   6 ...
         * We assume B-pictures always be present in the stream here. */
        if( (enum AVPictureType)info[i].pict_type == AV_PICTURE_TYPE_B )
        {
            /* B-pictures shall be output or displayed in the same order as they are encoded. */
            info[i].pts = info[i].dts;
            ++num_consecutive_b;
            reordered_stream = 1;
        }
        else
        {
            /* Apply DTS of the current picture to PTS of the last I- or P-picture. */
            if( i > num_consecutive_b + 1 )
                info[i - num_consecutive_b - 1].pts = info[i].dts;
            num_consecutive_b = 0;
        }
    }
    if( reordered_stream && num_consecutive_b != vdhp->frame_count )
    {
        /* Check if any duplicated PTS. */
        uint32_t flush_number = vdhp->frame_count - num_consecutive_b;
        int64_t *last_pts = &info[flush_number].pts;
        if( *last_pts != AV_NOPTS_VALUE )
            for( uint32_t i = vdhp->frame_count; i && *last_pts >= info[i].dts; i-- )
                if( *last_pts == info[i].pts && i != flush_number )
                    *last_pts = AV_NOPTS_VALUE;
        if( *last_pts == AV_NOPTS_VALUE )
        {
            /* Estimate PTS of the last displayed picture. */
            int64_t duration = info[ vdhp->frame_count ].dts - info[ vdhp->frame_count - 1 ].dts;
            *last_pts = info[ vdhp->frame_count ].dts + duration;
        }
        /* Check leading B-pictures. */
        int64_t last_keyframe_pts = AV_NOPTS_VALUE;
        for( uint32_t i = 1; i <= vdhp->frame_count; i++ )
        {
            if( info[i].pts       != AV_NOPTS_VALUE
             && last_keyframe_pts != AV_NOPTS_VALUE
             && info[i].pts < last_keyframe_pts )
                info[i].flags |= LW_VFRAME_FLAG_LEADING;
            if( info[i].flags & LW_VFRAME_FLAG_KEY )
                last_keyframe_pts = info[i].pts;
        }
    }
    else
        for( uint32_t i = 1; i <= vdhp->frame_count; i++ )
            info[i].pts = info[i].dts;
}

static void interpolate_pts
(
    video_frame_info_t     *info,       /* 0-origin */
    video_timestamp_temp_t *timestamp,  /* 0-origin */
    uint32_t                frame_count,
    AVRational              time_base,
    uint32_t                max_composition_delay
)
{
    /* Find the first valid PTS. */
    uint32_t valid_start = UINT32_MAX;
    for( uint32_t i = 0; i < frame_count; i++ )
        if( timestamp[i].core.pts != AV_NOPTS_VALUE )
            valid_start = i;
    if( valid_start != UINT32_MAX )
    {
        /* Generate PTSs. */
        for( uint32_t i = valid_start; i; i-- )
            timestamp[i - 1].core.pts = timestamp[i].core.pts - time_base.num;
        while( valid_start < frame_count )
        {
            /* Find the next valid PTS. */
            uint32_t valid_end = UINT32_MAX;
            for( uint32_t i = valid_start + 1; i < frame_count; i++ )
                if( timestamp[i].core.pts != AV_NOPTS_VALUE
                 && timestamp[i].core.pts != timestamp[i - 1].core.pts )
                    valid_end = i;
            /* Interpolate PTSs roughly. */
            if( valid_end != UINT32_MAX )
                for( uint32_t i = valid_end; i > valid_start + 1; i-- )
                    timestamp[i - 1].core.pts = timestamp[i].core.pts - time_base.num;
            else
                for( uint32_t i = valid_start + 1; i < frame_count; i++ )
                    timestamp[i].core.pts = timestamp[i - 1].core.pts + time_base.num;
            valid_start = valid_end;
        }
    }
    else
    {
        if( max_composition_delay )
            /* Get the maximum composition delay derived from reordering. */
            for( uint32_t i = 0; i < frame_count; i++ )
                if( i < timestamp[i].core.dts )
                {
                    uint32_t composition_delay = timestamp[i].core.dts - i;
                    max_composition_delay = MAX( max_composition_delay, composition_delay );
                }
        /* Generate PTSs. */
        timestamp[0].core.pts = max_composition_delay * time_base.num;
        for( uint32_t i = 1; i < frame_count; i++ )
            timestamp[i].core.pts = timestamp[i - 1].core.pts + (info[i - 1].repeat_pict == 0 ? 1 : 2) * time_base.num;
    }
}

static void interpolate_dts
(
    video_frame_info_t *info,       /* 0-origin */
    uint32_t            frame_count,
    AVRational          time_base
)
{
    /* Find the first valid DTS. */
    uint32_t valid_start = UINT32_MAX;
    for( uint32_t i = 0; i < frame_count; i++ )
        if( info[i].dts != AV_NOPTS_VALUE )
            valid_start = i;
    if( valid_start != UINT32_MAX )
    {
        /* Generate DTSs. */
        for( uint32_t i = valid_start; i; i-- )
            info[i - 1].dts = info[i].dts - time_base.num;
        while( valid_start < frame_count )
        {
            /* Find the next valid DTS. */
            uint32_t valid_end = UINT32_MAX;
            for( uint32_t i = valid_start + 1; i < frame_count; i++ )
                if( info[i].dts != AV_NOPTS_VALUE
                 && info[i].dts != info[i - 1].dts )
                    valid_end = i;
            /* Interpolate DTSs roughly. */
            if( valid_end != UINT32_MAX )
                for( uint32_t i = valid_end; i > valid_start + 1; i-- )
                    info[i - 1].dts = info[i].dts - time_base.num;
            else
                for( uint32_t i = valid_start + 1; i < frame_count; i++ )
                    info[i].dts = info[i - 1].dts + time_base.num;
            valid_start = valid_end;
        }
    }
    else
    {
        /* Generate DTSs. */
        info[0].dts = 0;
        for( uint32_t i = 1; i < frame_count; i++ )
            info[i].dts = info[i - 1].dts + (info[i - 1].repeat_pict == 0 ? 1 : 2) * time_base.num;
    }
}

static int poc_genarate_pts
(
    lwlibav_video_decode_handler_t *vdhp,
    AVRational                      time_base,
    int                             max_num_reorder_pics
)
{
    video_frame_info_t *info = &vdhp->frame_list[1];
    /* Deduplicate POCs. */
    int64_t  poc_offset            = 0;
    int64_t  poc_min               = 0;
    int64_t  invalid_poc_min       = 0;
    uint32_t last_idr              = UINT32_MAX;
    uint32_t invalid_poc_start     = 0;
    uint32_t max_composition_delay = 0;
    int      invalid_poc_present   = 0;
    for( uint32_t i = 0; ; i++ )
    {
        if( i < vdhp->frame_count && info[i].poc != 0 )
        {
            /* poc_offset is not added to each POC here.
             * It is done when we encounter the next coded video sequence. */
            if( info[i].poc < 0 )
            {
                /* Pictures with negative POC shall precede IDR-picture in composition order.
                 * The minimum POC is added to poc_offset when we encounter the next coded video sequence. */
                if( last_idr == UINT32_MAX || i > last_idr + max_num_reorder_pics )
                {
                    if( !invalid_poc_present )
                    {
                        invalid_poc_present = 1;
                        invalid_poc_start   = i;
                    }
                    if( invalid_poc_min > info[i].poc )
                        invalid_poc_min = info[i].poc;
                }
                else if( poc_min > info[i].poc )
                {
                    poc_min = info[i].poc;
                    max_composition_delay = MAX( max_composition_delay, i - last_idr );
                }
            }
            continue;
        }
        /* Encountered a new coded video sequence or no more POCs.
         * Add poc_offset to each POC of the previous coded video sequence. */
        poc_offset -= poc_min;
        int64_t poc_max = 0;
        for( uint32_t j = last_idr; j < i; j++ )
            if( info[j].poc >= 0 || (j <= last_idr + max_num_reorder_pics) )
            {
                info[j].poc += poc_offset;
                if( poc_max < info[j].poc )
                    poc_max = info[j].poc;
            }
        poc_offset = poc_max + 1;
        if( invalid_poc_present )
        {
            /* Pictures with invalid negative POC is probably supposed to be composited
             * both before the next coded video sequence and after the current one. */
            poc_offset -= invalid_poc_min;
            for( uint32_t j = invalid_poc_start; j < i; j++ )
                if( info[j].poc < 0 )
                {
                    info[j].poc += poc_offset;
                    if( poc_max < info[j].poc )
                        poc_max = info[j].poc;
                }
            invalid_poc_present = 0;
            invalid_poc_start   = 0;
            invalid_poc_min     = 0;
            poc_offset = poc_max + 1;
        }
        if( i < vdhp->frame_count )
        {
            poc_min = 0;
            last_idr = i;
        }
        else
            break;      /* no more POCs */
    }
    /* Check if composition delay derived from reordering is present. */
    int composition_reordering_present;
    if( max_composition_delay == 0 )
    {
        composition_reordering_present = 0;
        for( uint32_t i = 1; i < vdhp->frame_count; i++ )
            if( info[i].poc < info[i - 1].poc )
            {
                composition_reordering_present = 1;
                break;
            }
    }
    else
        composition_reordering_present = 1;
    /* Generate timestamps. */
    video_timestamp_temp_t *timestamp = (video_timestamp_temp_t *)malloc( vdhp->frame_count * sizeof(video_timestamp_temp_t) );
    if( !timestamp )
        return -1;
    for( uint32_t i = 0; i < vdhp->frame_count; i++ )
    {
        timestamp[i].core.pts = info[i].pts;
        timestamp[i].core.dts = info[i].dts;
    }
    if( composition_reordering_present )
    {
        /* Here, PTSs are temporary values for sort. */
        for( uint32_t i = 0; i < vdhp->frame_count; i++ )
        {
            timestamp[i].temp.pts = info[i].poc;
            timestamp[i].temp.dts = i;
        }
        sort_presentation_order( &timestamp[0].temp, vdhp->frame_count, sizeof(video_timestamp_temp_t) );
        interpolate_pts( info, timestamp, vdhp->frame_count, time_base, max_composition_delay );
        sort_decoding_order( &timestamp[0].temp, vdhp->frame_count, sizeof(video_timestamp_temp_t) );
        /* Check leading pictures. */
        int64_t last_keyframe_pts = AV_NOPTS_VALUE;
        for( uint32_t i = 0; i < vdhp->frame_count; i++ )
        {
            if( last_keyframe_pts != AV_NOPTS_VALUE && timestamp[i].core.pts < last_keyframe_pts )
                info[i].flags |= LW_VFRAME_FLAG_LEADING;
            if( info[i].flags & LW_VFRAME_FLAG_KEY )
                last_keyframe_pts = timestamp[i].core.pts;
        }
    }
    else
        interpolate_pts( info, timestamp, vdhp->frame_count, time_base, 0 );
    /* Set generated timestamps. */
    for( uint32_t i = 0; i < vdhp->frame_count; i++ )
    {
        info[i].pts = timestamp[i].core.pts;
        info[i].dts = timestamp[i].core.dts;
    }
    free( timestamp );
    return 0;
}

static int decide_video_seek_method
(
    lwlibav_file_handler_t         *lwhp,
    lwlibav_video_decode_handler_t *vdhp,
    uint32_t                        sample_count,
    AVRational                      time_base
)
{
    vdhp->lw_seek_flags = lineup_seek_base_candidates( lwhp );
    video_frame_info_t *info = vdhp->frame_list;
    /* Decide seek base. */
    for( uint32_t i = 1; i <= sample_count; i++ )
        if( info[i].pts == AV_NOPTS_VALUE )
        {
            vdhp->lw_seek_flags &= ~SEEK_PTS_BASED;
            break;
        }
    if( info[1].dts == AV_NOPTS_VALUE )
        vdhp->lw_seek_flags &= ~SEEK_DTS_BASED;
    else
        for( uint32_t i = 2; i <= sample_count; i++ )
            if( info[i].dts == AV_NOPTS_VALUE || info[i].dts <= info[i - 1].dts )
            {
                vdhp->lw_seek_flags &= ~SEEK_DTS_BASED;
                break;
            }
    if( info[1].file_offset == -1 )
        vdhp->lw_seek_flags &= ~SEEK_POS_CORRECTION;
    else
        for( uint32_t i = 2; i <= sample_count; i++ )
            if( info[i].file_offset == -1 || info[i].file_offset <= info[i - 1].file_offset )
            {
                vdhp->lw_seek_flags &= ~SEEK_POS_CORRECTION;
                break;
            }
    if( vdhp->lw_seek_flags & SEEK_POS_BASED )
    {
        if( lwhp->format_flags & AVFMT_NO_BYTE_SEEK )
            vdhp->lw_seek_flags &= ~SEEK_POS_BASED;
        else
        {
            uint32_t error_count = 0;
            for( uint32_t i = 1; i <= sample_count; i++ )
                error_count += (info[i].file_offset == -1);
            if( error_count == sample_count )
                vdhp->lw_seek_flags &= ~SEEK_POS_BASED;
        }
    }
    /* Construct frame info about timestamp. */
    int no_pts_loss = !!(vdhp->lw_seek_flags & SEEK_PTS_BASED);
    if( (lwhp->raw_demuxer || ((vdhp->lw_seek_flags & SEEK_DTS_BASED) && !(vdhp->lw_seek_flags & SEEK_PTS_BASED)))
     && (vdhp->codec_id == AV_CODEC_ID_MPEG1VIDEO || vdhp->codec_id == AV_CODEC_ID_MPEG2VIDEO
      || vdhp->codec_id == AV_CODEC_ID_VC1        || vdhp->codec_id == AV_CODEC_ID_WMV3
      || vdhp->codec_id == AV_CODEC_ID_VC1IMAGE   || vdhp->codec_id == AV_CODEC_ID_WMV3IMAGE) )
    {
        /* Generate or interpolate DTS if any invalid DTS for each frame. */
        if( !(vdhp->lw_seek_flags & SEEK_DTS_BASED) )
            interpolate_dts( &info[1], vdhp->frame_count, time_base );
        /* Generate PTS from DTS. */
        mpeg12_video_vc1_genarate_pts( vdhp );
        vdhp->lw_seek_flags |= SEEK_PTS_GENERATED;
        no_pts_loss = 1;
    }
    else if( (lwhp->raw_demuxer || !no_pts_loss)
          && (vdhp->codec_id == AV_CODEC_ID_H264 || vdhp->codec_id == AV_CODEC_ID_HEVC) )
    {
        /* Generate PTS. */
        if( poc_genarate_pts( vdhp, time_base, vdhp->codec_id == AV_CODEC_ID_H264 ? 32 : 15 ) < 0 )
        {
            if( vdhp->lh.show_log )
                vdhp->lh.show_log( &vdhp->lh, LW_LOG_FATAL, "Failed to allocate memory for PTS generation." );
            return -1;
        }
        vdhp->lw_seek_flags |= SEEK_PTS_GENERATED;
        no_pts_loss = 1;
    }
    /* Reorder in presentation order. */
    if( no_pts_loss && check_frame_reordering( info, sample_count ) )
    {
        /* Consider presentation order for keyframe detection.
         * Note: sample number is 1-origin. */
        vdhp->order_converter = (order_converter_t *)lw_malloc_zero( (sample_count + 1) * sizeof(order_converter_t) );
        if( !vdhp->order_converter )
        {
            if( vdhp->lh.show_log )
                vdhp->lh.show_log( &vdhp->lh, LW_LOG_FATAL, "Failed to allocate memory." );
            return -1;
        }
        sort_info_presentation_order( &info[1], sample_count );
        video_timestamp_t *timestamp = (video_timestamp_t *)lw_malloc_zero( (sample_count + 1) * sizeof(video_timestamp_t) );
        if( !timestamp )
        {
            if( vdhp->lh.show_log )
                vdhp->lh.show_log( &vdhp->lh, LW_LOG_FATAL, "Failed to allocate memory of video timestamps." );
            return -1;
        }
        for( uint32_t i = 1; i <= sample_count; i++ )
        {
            timestamp[i].pts = (int64_t)i;
            timestamp[i].dts = (int64_t)info[i].sample_number;
        }
        sort_decoding_order( &timestamp[1], sample_count, sizeof(video_timestamp_t) );
        for( uint32_t i = 1; i <= sample_count; i++ )
            vdhp->order_converter[i].decoding_to_presentation = (uint32_t)timestamp[i].pts;
        free( timestamp );
    }
    else if( vdhp->lw_seek_flags & SEEK_DTS_BASED )
        for( uint32_t i = 1; i <= sample_count; i++ )
            info[i].pts = info[i].dts;
    /* Treat video frames with unique value as keyframe. */
    if( vdhp->lw_seek_flags & SEEK_POS_BASED )
    {
        if( info[ info[1].sample_number ].file_offset == -1 )
            info[ info[1].sample_number ].flags &= ~LW_VFRAME_FLAG_KEY;
        for( uint32_t i = 2; i <= sample_count; i++ )
        {
            uint32_t j = info[i    ].sample_number;
            uint32_t k = info[i - 1].sample_number;
            if( info[j].file_offset == -1 )
                info[j].flags &= ~LW_VFRAME_FLAG_KEY;
            else if( info[j].file_offset == info[k].file_offset )
            {
                info[j].flags &= ~LW_VFRAME_FLAG_KEY;
                info[k].flags &= ~LW_VFRAME_FLAG_KEY;
            }
        }
    }
    else if( vdhp->lw_seek_flags & SEEK_PTS_BASED )
    {
        if( info[ info[1].sample_number ].pts == AV_NOPTS_VALUE )
            info[ info[1].sample_number ].flags &= ~LW_VFRAME_FLAG_KEY;
        for( uint32_t i = 2; i <= sample_count; i++ )
        {
            uint32_t j = info[i    ].sample_number;
            uint32_t k = info[i - 1].sample_number;
            if( info[j].pts == AV_NOPTS_VALUE )
                info[j].flags &= ~LW_VFRAME_FLAG_KEY;
            else if( info[j].pts == info[k].pts )
            {
                info[j].flags &= ~LW_VFRAME_FLAG_KEY;
                info[k].flags &= ~LW_VFRAME_FLAG_KEY;
            }
        }
    }
    else if( vdhp->lw_seek_flags & SEEK_DTS_BASED )
    {
        if( info[ info[1].sample_number ].dts == AV_NOPTS_VALUE )
            info[ info[1].sample_number ].flags &= ~LW_VFRAME_FLAG_KEY;
        for( uint32_t i = 2; i <= sample_count; i++ )
        {
            uint32_t j = info[i    ].sample_number;
            uint32_t k = info[i - 1].sample_number;
            if( info[j].dts == AV_NOPTS_VALUE )
                info[j].flags &= ~LW_VFRAME_FLAG_KEY;
            else if( info[j].dts == info[k].dts )
            {
                info[j].flags &= ~LW_VFRAME_FLAG_KEY;
                info[k].flags &= ~LW_VFRAME_FLAG_KEY;
            }
        }
    }
    /* Set up keyframe list: presentation order (info) -> decoding order (keyframe_list) */
    for( uint32_t i = 1; i <= sample_count; i++ )
        vdhp->keyframe_list[ info[i].sample_number ] = !!(info[i].flags & LW_VFRAME_FLAG_KEY);
    return 0;
}

static void decide_audio_seek_method
(
    lwlibav_file_handler_t         *lwhp,
    lwlibav_audio_decode_handler_t *adhp,
    uint32_t                        sample_count
)
{
    adhp->lw_seek_flags = lineup_seek_base_candidates( lwhp );
    audio_frame_info_t *info = adhp->frame_list;
    for( uint32_t i = 1; i <= sample_count; i++ )
        if( info[i].pts == AV_NOPTS_VALUE )
        {
            adhp->lw_seek_flags &= ~SEEK_PTS_BASED;
            break;
        }
    for( uint32_t i = 1; i <= sample_count; i++ )
        if( info[i].dts == AV_NOPTS_VALUE )
        {
            adhp->lw_seek_flags &= ~SEEK_DTS_BASED;
            break;
        }
    if( adhp->lw_seek_flags & SEEK_POS_BASED )
    {
        if( lwhp->format_flags & AVFMT_NO_BYTE_SEEK )
            adhp->lw_seek_flags &= ~SEEK_POS_BASED;
        else
        {
            uint32_t error_count = 0;
            for( uint32_t i = 1; i <= sample_count; i++ )
                error_count += (info[i].file_offset == -1);
            if( error_count == sample_count )
                adhp->lw_seek_flags &= ~SEEK_POS_BASED;
        }
    }
    if( !(adhp->lw_seek_flags & SEEK_PTS_BASED) && (adhp->lw_seek_flags & SEEK_DTS_BASED) )
        for( uint32_t i = 1; i <= sample_count; i++ )
            info[i].pts = info[i].dts;
    /* Treat audio frames with unique value as a keyframe. */
    if( adhp->lw_seek_flags & SEEK_POS_BASED )
    {
        info[1].keyframe = (info[1].file_offset != -1);
        for( uint32_t i = 2; i <= sample_count; i++ )
            if( info[i].file_offset == -1 )
                info[i].keyframe = 0;
            else if( info[i].file_offset == info[i - 1].file_offset )
                info[i].keyframe = info[i - 1].keyframe = 0;
            else
                info[i].keyframe = 1;
    }
    else if( adhp->lw_seek_flags & SEEK_PTS_BASED )
    {
        info[1].keyframe = (info[1].pts != AV_NOPTS_VALUE);
        for( uint32_t i = 2; i <= sample_count; i++ )
            if( info[i].pts == AV_NOPTS_VALUE )
                info[i].keyframe = 0;
            else if( info[i].pts == info[i - 1].pts )
                info[i].keyframe = info[i - 1].keyframe = 0;
            else
                info[i].keyframe = 1;
    }
    else if( adhp->lw_seek_flags & SEEK_DTS_BASED )
    {
        info[1].keyframe = (info[1].dts != AV_NOPTS_VALUE);
        for( uint32_t i = 2; i <= sample_count; i++ )
            if( info[i].dts == AV_NOPTS_VALUE )
                info[i].keyframe = 0;
            else if( info[i].dts == info[i - 1].dts )
                info[i].keyframe = info[i - 1].keyframe = 0;
            else
                info[i].keyframe = 1;
    }
    else
        for( uint32_t i = 1; i <= sample_count; i++ )
            info[i].keyframe = 1;
}

static int64_t calculate_av_gap
(
    lwlibav_video_decode_handler_t *vdhp,
    lwlibav_video_output_handler_t *vohp,
    lwlibav_audio_decode_handler_t *adhp,
    AVRational                      video_time_base,
    AVRational                      audio_time_base,
    int                             sample_rate
)
{
    /* Pick the first video timestamp.
     * If invalid, skip A/V gap calculation. */
    int64_t video_ts = (vdhp->lw_seek_flags & SEEK_PTS_BASED) ? vdhp->frame_list[1].pts : vdhp->frame_list[1].dts;
    if( video_ts == AV_NOPTS_VALUE )
        return 0;
    /* Pick the first valid audio timestamp.
     * If not found, skip A/V gap calculation. */
    int64_t  audio_ts        = 0;
    uint32_t audio_ts_number = 0;
    if( adhp->lw_seek_flags & SEEK_PTS_BASED )
    {
        for( uint32_t i = 1; i <= adhp->frame_count; i++ )
            if( adhp->frame_list[i].pts != AV_NOPTS_VALUE )
            {
                audio_ts        = adhp->frame_list[i].pts;
                audio_ts_number = i;
                break;
            }
    }
    else
        for( uint32_t i = 1; i <= adhp->frame_count; i++ )
            if( adhp->frame_list[i].dts != AV_NOPTS_VALUE )
            {
                audio_ts        = adhp->frame_list[i].dts;
                audio_ts_number = i;
                break;
            }
    if( audio_ts_number == 0 )
        return 0;
    /* Estimate the first audio timestamp if invalid. */
    AVRational audio_sample_base = { 1, sample_rate };
    for( uint32_t i = 1, delay_count = 0; i < MIN( audio_ts_number + delay_count, adhp->frame_count ); i++ )
        if( adhp->frame_list[i].length != -1 )
            audio_ts -= av_rescale_q( adhp->frame_list[i].length, audio_sample_base, audio_time_base );
        else
            ++delay_count;
    /* Calculate A/V gap in audio samplerate. */
    if( video_ts || audio_ts )
    {
        int64_t av_gap = av_rescale_q( audio_ts, audio_time_base, audio_sample_base )
                       - av_rescale_q( video_ts, video_time_base, audio_sample_base );
        if( vohp->repeat_control && vohp->repeat_correction_ts )
            av_gap += av_rescale_q( vohp->repeat_correction_ts, video_time_base, audio_sample_base );
        return av_gap;
    }
    return 0;
}

static void create_video_frame_order_list
(
    lwlibav_video_decode_handler_t *vdhp,
    lwlibav_video_output_handler_t *vohp,
    lwlibav_option_t               *opt
)
{
    if( !(vdhp->lw_seek_flags & (SEEK_PTS_BASED | SEEK_PTS_GENERATED)) )
        goto disable_repeat;
    video_frame_info_t *info                      = vdhp->frame_list;
    uint32_t            frame_count               = vdhp->frame_count;
    uint32_t            order_count               = 0;
    int                 no_support_frame_tripling = (vdhp->codec_id != AV_CODEC_ID_MPEG2VIDEO);
    int                 specified_field_dominance = opt->field_dominance == 0 ? LW_FIELD_INFO_UNKNOWN   /* Obey source flags. */
                                                  : opt->field_dominance == 1 ? LW_FIELD_INFO_TOP       /* TFF: Top -> Bottom */
                                                  :                             LW_FIELD_INFO_BOTTOM;   /* BFF: Bottom -> Top */
    /* Check repeat_pict and order_count. */
    if( specified_field_dominance > 0 && (lw_field_info_t)specified_field_dominance != info[1].field_info )
        ++order_count;
    int             enable_repeat   = 0;
    int             complete_frame  = 1;
    int             repeat_field    = 1;
    lw_field_info_t next_field_info = info[1].field_info;
    for( uint32_t i = 1; i <= frame_count; i++, order_count++ )
    {
        int             repeat_pict = info[i].repeat_pict;
        lw_field_info_t field_info  = info[i].field_info;
        int             field_shift = !(repeat_pict & 1);
        if( field_info == LW_FIELD_INFO_UNKNOWN )
        {
            /* Override with TFF or BFF. */
            field_info = next_field_info;
            info[i].field_info = field_info;
        }
        else if( field_info != next_field_info && (!repeat_field || !complete_frame) )
            goto disable_repeat;
        if( opt->apply_repeat_flag )
            switch( repeat_pict )
            {
                case 5 :    /* frame tripling */
                    if( no_support_frame_tripling )
                        ++order_count;
                case 3 :    /* frame doubling */
                    ++order_count;
                    enable_repeat |= 1;
                    break;
                case 2 :    /* field tripling */
                    repeat_field  ^= 1;
                    order_count   += repeat_field;
                    enable_repeat |= 1;
                    break;
                default :
                    break;
            }
        if( repeat_pict == 0 && !(info[i].flags & LW_VFRAME_FLAG_CORRUPT) )
        {
            /* PAFF field coded picture */
            complete_frame ^= 1;
            order_count    -= complete_frame;
            enable_repeat  |= 1;
        }
        if( field_shift )
            next_field_info = field_info == LW_FIELD_INFO_TOP ? LW_FIELD_INFO_BOTTOM : LW_FIELD_INFO_TOP;
    }
    if( !enable_repeat )
        goto disable_repeat;
    /* Allocate frame cache buffers. */
    for( int i = 0; i < REPEAT_CONTROL_CACHE_NUM; i++ )
    {
        vohp->frame_cache_buffers[i] = av_frame_alloc();
        if( !vohp->frame_cache_buffers[i] )
            goto disable_repeat;
        vohp->frame_cache_numbers[i] = 0;
    }
    /* Create order list. */
    lw_video_frame_order_t *order_list = (lw_video_frame_order_t *)lw_malloc_zero( (order_count + 2) * sizeof(lw_video_frame_order_t) );
    if( !order_list )
    {
        if( vdhp->lh.show_log )
            vdhp->lh.show_log( &vdhp->lh, LW_LOG_FATAL, "Failed to allocate memory to the frame order list for video." );
        goto disable_repeat;
    }
    int64_t  correction_ts = 0;
    uint32_t t_count       = 1;
    uint32_t b_count       = 1;
    if( specified_field_dominance > 0 )
    {
        if( (lw_field_info_t)specified_field_dominance == LW_FIELD_INFO_TOP && info[1].field_info == LW_FIELD_INFO_BOTTOM )
            order_list[t_count++].top = 1;
        else if( (lw_field_info_t)specified_field_dominance == LW_FIELD_INFO_BOTTOM && info[1].field_info == LW_FIELD_INFO_TOP )
            order_list[b_count++].bottom = 1;
        if( t_count > 1 || b_count > 1 )
            correction_ts = (info[2].pts - info[1].pts) / (info[1].repeat_pict + 1);
    }
    complete_frame  = 1;
    for( uint32_t i = 1; i <= frame_count; i++ )
    {
        /* Check repeat_pict and field dominance. */
        int             repeat_pict = info[i].repeat_pict;
        lw_field_info_t field_info  = info[i].field_info;
        order_list[t_count++].top    = i;
        order_list[b_count++].bottom = i;
        if( opt->apply_repeat_flag )
            switch( repeat_pict )
            {
                case 5 :    /* frame tripling */
                    if( no_support_frame_tripling )
                    {
                        order_list[t_count++].top    = i;
                        order_list[b_count++].bottom = i;
                    }
                case 3 :    /* frame doubling */
                    order_list[t_count++].top    = i;
                    order_list[b_count++].bottom = i;
                    break;
                case 2 :    /* field tripling */
                    if( field_info == LW_FIELD_INFO_TOP )
                        order_list[t_count++].top = i;
                    else if( field_info == LW_FIELD_INFO_BOTTOM )
                        order_list[b_count++].bottom = i;
                    break;
                default :
                    break;
            }
        if( repeat_pict == 0 && !(info[i].flags & LW_VFRAME_FLAG_CORRUPT) )
        {
            /* PAFF field coded picture */
            if( field_info == LW_FIELD_INFO_BOTTOM )
                --t_count;
            else
                --b_count;
            complete_frame ^= 1;
        }
    }
    --t_count;
    --b_count;
    if( t_count != b_count )
    {
        order_list[order_count].top    = frame_count;
        order_list[order_count].bottom = frame_count;
    }
    memset( &order_list[order_count + 1], 0, sizeof(lw_video_frame_order_t) );
    /* Set up repeat control info. */
    if( vdhp->lh.show_log )
        vdhp->lh.show_log( &vdhp->lh, LW_LOG_INFO,
                           "Enable repeat control. frame_count = %u, order_count = %u, t_count = %u, b_count = %u",
                           frame_count, order_count, t_count, b_count );
    vohp->repeat_control       = 1;
    vohp->repeat_correction_ts = correction_ts;
    vohp->frame_order_count    = order_count;
    vohp->frame_order_list     = order_list;
    vohp->frame_count          = vohp->frame_order_count;
    return;
disable_repeat:
    if( vdhp->lh.show_log && opt->apply_repeat_flag )
        vdhp->lh.show_log( &vdhp->lh, LW_LOG_INFO, "Disable repeat control." );
    vohp->repeat_control       = 0;
    vohp->repeat_correction_ts = 0;
    vohp->frame_order_count    = 0;
    vohp->frame_order_list     = NULL;
    vohp->frame_count          = vdhp->frame_count;
    return;
}

static lwlibav_extradata_t *alloc_extradata_entries
(
    lwlibav_extradata_handler_t *exhp,
    int                          count
)
{
    assert( count > 0 && count > exhp->entry_count );
    lwlibav_extradata_t *temp = (lwlibav_extradata_t *)realloc( exhp->entries, count * sizeof(lwlibav_extradata_t) );
    if( !temp )
        return NULL;
    exhp->entries = temp;
    temp = &exhp->entries[ exhp->entry_count ];
    for( int i = exhp->entry_count; i < count; i++ )
    {
        lwlibav_extradata_t *entry = &exhp->entries[i];
        entry->extradata       = NULL;
        entry->extradata_size  = 0;
        entry->codec_id        = AV_CODEC_ID_NONE;
        entry->codec_tag       = 0;
        entry->width           = 0;
        entry->height          = 0;
        entry->pixel_format    = AV_PIX_FMT_NONE;
        entry->channel_layout  = 0;
        entry->sample_format   = AV_SAMPLE_FMT_NONE;
        entry->sample_rate     = 0;
        entry->bits_per_sample = 0;
        entry->block_align     = 0;
    }
    exhp->entry_count = count;
    return temp;
}

static uint8_t *make_vc1_ebdu
(
    lwindex_helper_t *helper,
    AVPacket         *pkt,
    int              *size,
    uint8_t           bdu_type,
    int               is_vc1
)
{
    uint8_t *data = helper->buffer;
    int buffer_size = (1 + !is_vc1) * (pkt->size + 4);
    if( helper->buffer_size < buffer_size + FF_INPUT_BUFFER_PADDING_SIZE )
    {
        data = (uint8_t *)av_realloc( helper->buffer, buffer_size + FF_INPUT_BUFFER_PADDING_SIZE );
        if( !data )
            return NULL;
        helper->buffer      = data;
        helper->buffer_size = buffer_size + FF_INPUT_BUFFER_PADDING_SIZE;
    }
    /* start code */
    data[0] = 0x00;
    data[1] = 0x00;
    data[2] = 0x01;
    data[3] = bdu_type;
    if( is_vc1 )
    {
        *size = pkt->size + 4;
        memcpy( data + 4, pkt->data, pkt->size );
    }
    else
    {
        /* RBDU to EBDU */
        uint8_t *pos = pkt->data;
        uint8_t *end = pkt->data + pkt->size;
        *size = 4;
        if( pos < end )
            data[ (*size)++ ] = *(pos++);
        if( pos < end )
            data[ (*size)++ ] = *(pos++);   /* No need to check emulation since bdu_type == 0 is reserved. */
        while( pos < end )
        {
            if( pos[-2] == 0x00 && pos[-1] == 0x00 && pos[0] <= 0x03 )
                data[ (*size)++ ] = 0x03;
            data[ (*size)++ ] = (pos++)[0];
        }
    }
    memset( data + *size, 0, FF_INPUT_BUFFER_PADDING_SIZE );
    return data;
}

static lwindex_helper_t *get_index_helper
(
    const char     *format_name,
    AVCodecContext *ctx,
    AVStream       *stream
)
{
    lwindex_helper_t *helper = (lwindex_helper_t *)ctx->opaque;
    if( !helper )
    {
        /* Allocate the index helper. */
        helper = (lwindex_helper_t *)lw_malloc_zero( sizeof(lwindex_helper_t) );
        if( !helper )
            return NULL;
        ctx->opaque = (void *)helper;
        helper->mpeg12_video = (ctx->codec_id == AV_CODEC_ID_MPEG1VIDEO || ctx->codec_id == AV_CODEC_ID_MPEG2VIDEO);
        helper->vc1_wmv3     = (ctx->codec_id == AV_CODEC_ID_VC1  || ctx->codec_id == AV_CODEC_ID_VC1IMAGE
                             || ctx->codec_id == AV_CODEC_ID_WMV3 || ctx->codec_id == AV_CODEC_ID_WMV3IMAGE);
        if( helper->vc1_wmv3 && !strcmp( format_name, "asf" ) )
            helper->vc1_wmv3 = 2;
        /* Set up the parser externally.
         * We don't trust parameters returned by the internal parser. */
        helper->parser_ctx = av_parser_init( helper->vc1_wmv3 ? AV_CODEC_ID_VC1 : ctx->codec_id );
        if( helper->parser_ctx )
        {
            helper->parser_ctx->flags |= PARSER_FLAG_COMPLETE_FRAMES;
            /* Set up bitstream filter if needed. */
            if( ctx->codec_id == AV_CODEC_ID_H264
             && ctx->extradata_size >= 8    /* 8 is the offset of the first byte of the first SPS in AVCConfigurationRecord. */
             && ctx->extradata[0] == 1      /* configurationVersion == 1 */
             && helper->parser_ctx->parser
             && helper->parser_ctx->parser->split
             && helper->parser_ctx->parser->split( ctx, ctx->extradata + 8, ctx->extradata_size - 8 ) <= 0 )
            {
                /* Since a SPS shall have no start code and no its emulation,
                 * therefore, this stream is not encapsulated as byte stream format. */
                helper->bsf = av_bitstream_filter_init( "h264_mp4toannexb" );
                if( !helper->bsf )
                    return NULL;
            }
        }
        /* For audio, prepare the decoder and the parser to get frame length.
         * For MPEG-1/2 Video and VC-1/WMV3, prepare the decoder to get picture type properly. */
        if( ctx->codec_type == AVMEDIA_TYPE_AUDIO || helper->mpeg12_video || helper->vc1_wmv3 )
        {
            helper->decode  = ctx->codec_type == AVMEDIA_TYPE_AUDIO ? avcodec_decode_audio4 : avcodec_decode_video2;
            helper->picture = av_frame_alloc();
            if( !helper->picture )
                return NULL;
        }
        if( helper->parser_ctx && helper->vc1_wmv3 == 2 )
        {
            /* Initialize the VC-1/WMV3 parser by extradata. */
            uint8_t *data;
            int      size;
            if( ctx->codec_id == AV_CODEC_ID_WMV3 || ctx->codec_id == AV_CODEC_ID_WMV3IMAGE )
            {
                /* Make a sequence header EBDU (0x0000010F). */
                AVPacket packet = { 0 };
                packet.data = ctx->extradata;
                packet.size = ctx->extradata_size;
                data = make_vc1_ebdu( helper, &packet, &size, 0x0F, 0 );
                if( !data )
                    return NULL;
            }
            else
            {
                /* For WVC1, the first byte is its size. */
                data = ctx->extradata      + 1;
                size = ctx->extradata_size - 1;
            }
            uint8_t *dummy;
            int      dummy_size;
            av_parser_parse2( helper->parser_ctx, ctx,
                              &dummy, &dummy_size, data, size,
                              AV_NOPTS_VALUE, AV_NOPTS_VALUE, -1 );
        }
    }
    return helper;
}

static int append_extradata_if_new
(
    lwindex_helper_t *helper,
    AVCodecContext   *ctx,
    AVPacket         *pkt
)
{
    lwlibav_extradata_handler_t *list = &((lwindex_helper_t *)ctx->opaque)->exh;
    if( !(pkt->flags & AV_PKT_FLAG_KEY) && list->entry_count > 0 )
        /* Some decoders might not change AVCodecContext.extradata even if a new extradata occurs.
         * Here, we assume non-keyframes reference the latest extradata. */
        return list->current_index;
    /* Anyway, import extradata from AVCodecContext. */
    lwlibav_extradata_t current = { ctx->extradata, ctx->extradata_size };
    /* Import extradata from a side data in the packet if present. */
    for( int i = 0; i < pkt->side_data_elems; i++ )
        if( pkt->side_data[i].type == AV_PKT_DATA_NEW_EXTRADATA )
        {
            current.extradata      = pkt->side_data[i].data;
            current.extradata_size = pkt->side_data[i].size;
            break;
        }
    /* Try to import extradata from the packet by splitting if no extradata is present in side data. */
    if( current.extradata == ctx->extradata )
    {
        AVCodecParserContext *parser_ctx = helper->parser_ctx;
        if( parser_ctx && parser_ctx->parser && parser_ctx->parser->split )
        {
            /* For H.264 stream without start codes, don't split extradata from pkt->data.
             * Its extradata is stored as global header. so, pkt->data shall contain no extradata. */
            int extradata_size = helper->bsf ? 0 : parser_ctx->parser->split( ctx, pkt->data, pkt->size );
            if( extradata_size > 0 )
            {
                current.extradata      = pkt->data;
                current.extradata_size = extradata_size;
            }
            else if( list->entry_count > 0 )
                /* Probably, this frame is a keyframe in CODEC level
                 * but should not be a random accessible point in container level.
                 * For instance, an IDR-picture which corresponding SPSs and PPSs
                 * do not precede immediately might not be decodable correctly
                 * when decoding from there in MPEG-2 transport stream. */
                return list->current_index;
        }
    }
    if( list->entry_count == 0 )
    {
        lwlibav_extradata_t *entry = alloc_extradata_entries( list, 1 );
        if( !entry )
            return -1;
        list->current_index = 0;
        if( current.extradata && current.extradata_size > 0 )
        {
            entry->extradata_size = current.extradata_size;
            entry->extradata      = (uint8_t *)av_malloc( current.extradata_size + FF_INPUT_BUFFER_PADDING_SIZE );
            if( !entry->extradata )
                return -1;
            memcpy( entry->extradata, current.extradata, entry->extradata_size );
            memset( entry->extradata + entry->extradata_size, 0, FF_INPUT_BUFFER_PADDING_SIZE );
        }
    }
    else
    {
        lwlibav_extradata_t *entry = &list->entries[ list->current_index ];
        if( current.extradata_size != entry->extradata_size
         || memcmp( current.extradata, entry->extradata, current.extradata_size ) )
        {
            /* Check if this extradata is a new one. If so, append it to the list. */
            for( int i = 0; i < list->entry_count; i++ )
            {
                if( i == list->current_index )
                    continue;   /* already compared */
                entry = &list->entries[i];
                if( current.extradata_size == entry->extradata_size
                 && (current.extradata_size == 0 || !memcmp( current.extradata, entry->extradata, current.extradata_size )) )
                {
                    /* The same extradata is found. */
                    list->current_index = i;
                    return list->current_index;
                }
            }
            /* Append a new extradata. */
            entry = alloc_extradata_entries( list, list->entry_count + 1 );
            if( !entry )
                return -1;
            if( current.extradata && current.extradata_size > 0 )
            {
                entry->extradata_size = current.extradata_size;
                entry->extradata      = (uint8_t *)av_malloc( current.extradata_size + FF_INPUT_BUFFER_PADDING_SIZE );
                if( !entry->extradata )
                    return -1;
                memcpy( entry->extradata, current.extradata, entry->extradata_size );
                memset( entry->extradata + entry->extradata_size, 0, FF_INPUT_BUFFER_PADDING_SIZE );
            }
            list->current_index = list->entry_count - 1;
        }
    }
    return list->current_index;
}

static void investigate_pix_fmt_by_decoding
(
    AVCodecContext *video_ctx,
    AVPacket       *pkt,
    AVFrame        *picture
)
{
    int got_picture;
    avcodec_decode_video2( video_ctx, picture, &got_picture, pkt );
}

static inline uint8_t *make_parsable_format
(
    lwindex_helper_t *helper,
    AVCodecContext   *ctx,
    AVPacket         *pkt,
    int              *size
)
{
    if( !helper->bsf )
    {
        *size = pkt->size;
        return pkt->data;
    }
    /* Convert frame data into parsable bitstream format. */
    if( helper->buffer )
        av_freep( &helper->buffer );
    helper->buffer_size = 0;
    if( av_bitstream_filter_filter( helper->bsf, ctx, NULL,
                                    &helper->buffer, &helper->buffer_size,
                                    pkt->data, pkt->size, 0 ) < 0 )
    {
        *size = 0;
        return NULL;
    }
    *size = helper->buffer_size;
    return helper->buffer;
}

static int get_picture_type
(
    lwindex_helper_t *helper,
    AVCodecContext   *ctx,
    AVPacket         *pkt
)
{
    if( !helper->parser_ctx )
        return 0;
    /* Get by the parser. */
    int      size;
    uint8_t *data;
    if( helper->vc1_wmv3 == 2 )
        /* Make a frame EBDU (0x0000010D). */
        data = make_vc1_ebdu( helper, pkt, &size, 0x0D, ctx->codec_id == AV_CODEC_ID_VC1 || ctx->codec_id == AV_CODEC_ID_VC1IMAGE );
    else
        data = make_parsable_format( helper, ctx, pkt, &size );
    if( !data )
        return -1;
    uint8_t *dummy;
    int      dummy_size;
    av_parser_parse2( helper->parser_ctx, ctx,
                      &dummy, &dummy_size, data, size,
                      pkt->pts, pkt->dts, pkt->pos );
    /* One frame decoding.
     * Sometimes, the parser returns a picture type other than I-picture and BI-picture even if the frame is a keyframe.
     * Actual decoding fixes this issue.
     * In addition, it seems the libavcodec VC-1 decoder returns an error when feeding BI-picture at the first.
     * So, we treat only I-picture as a keyframe. */
    if( (helper->mpeg12_video || helper->vc1_wmv3)
     && (pkt->flags & AV_PKT_FLAG_KEY)
     && (enum AVPictureType)helper->parser_ctx->pict_type != AV_PICTURE_TYPE_I )
    {
        int decode_complete;
        helper->decode( ctx, helper->picture, &decode_complete, pkt );
        if( !decode_complete )
        {
            AVPacket null_pkt = { 0 };
            av_init_packet( &null_pkt );
            null_pkt.data = NULL;
            null_pkt.size = 0;
            helper->decode( ctx, helper->picture, &decode_complete, pkt );
        }
        if( (enum AVPictureType)helper->picture->pict_type != AV_PICTURE_TYPE_I )
            pkt->flags &= ~AV_PKT_FLAG_KEY;
        return helper->picture->pict_type > 0 ? helper->picture->pict_type : 0;
    }
    return helper->parser_ctx->pict_type > 0 ? helper->parser_ctx->pict_type : 0;
}

static int get_audio_frame_length
(
    lwindex_helper_t *helper,
    AVCodecContext   *ctx,
    AVPacket         *pkt
)
{
    int frame_length;
    if( helper->parser_ctx )
    {
        /* Try to get from the parser. */
        uint8_t *dummy;
        int      dummy_size;
        av_parser_parse2( helper->parser_ctx, ctx,
                          &dummy, &dummy_size, pkt->data, pkt->size,
                          pkt->pts, pkt->dts, pkt->pos );
        frame_length = helper->parser_ctx->duration;
    }
    else
        frame_length = 0;
    if( frame_length == 0 && helper->delay_count == 0 )
        frame_length = ctx->frame_size;
    if( frame_length == 0 )
    {
        /* Try to get by actual decoding. */
        AVPacket temp = *pkt;
        int output_audio = 0;
        while( temp.size > 0 )
        {
            int decode_complete;
            int consumed_bytes = helper->decode( ctx, helper->picture, &decode_complete, &temp );
            if( consumed_bytes < 0 )
            {
                ctx->channels    = av_get_channel_layout_nb_channels( helper->picture->channel_layout );
                ctx->sample_rate = helper->picture->sample_rate;
                break;
            }
            temp.size -= consumed_bytes;
            temp.data += consumed_bytes;
            if( decode_complete )
            {
                frame_length += helper->picture->nb_samples;
                output_audio = 1;
            }
        }
        if( !output_audio )
        {
            frame_length = -1;
            ++ helper->delay_count;
            if( helper->parser_ctx )
            {
                /* Don't use the parser anymore because of asynchronization. */
                av_parser_close( helper->parser_ctx );
                helper->parser_ctx = NULL;
            }
        }
    }
    return frame_length;
}

static enum AVSampleFormat select_better_sample_format
(
    enum AVSampleFormat a,
    enum AVSampleFormat b
)
{
    switch( a )
    {
        case AV_SAMPLE_FMT_NONE :
            if( b != AV_SAMPLE_FMT_NONE )
                a = b;
            break;
        case AV_SAMPLE_FMT_U8 :
        case AV_SAMPLE_FMT_U8P :
            if( b != AV_SAMPLE_FMT_U8 && b != AV_SAMPLE_FMT_U8P )
                a = b;
            break;
        case AV_SAMPLE_FMT_S16 :
        case AV_SAMPLE_FMT_S16P :
            if( b != AV_SAMPLE_FMT_U8  && b != AV_SAMPLE_FMT_U8P
             && b != AV_SAMPLE_FMT_S16 && b != AV_SAMPLE_FMT_S16P )
                a = b;
            break;
        case AV_SAMPLE_FMT_S32 :
        case AV_SAMPLE_FMT_S32P :
            if( b != AV_SAMPLE_FMT_U8  && b != AV_SAMPLE_FMT_U8P
             && b != AV_SAMPLE_FMT_S16 && b != AV_SAMPLE_FMT_S16P
             && b != AV_SAMPLE_FMT_S32 && b != AV_SAMPLE_FMT_S32P )
                a = b;
            break;
        case AV_SAMPLE_FMT_FLT :
        case AV_SAMPLE_FMT_FLTP :
            if( b == AV_SAMPLE_FMT_DBL || b == AV_SAMPLE_FMT_DBLP )
                a = b;
            break;
        default :
            break;
    }
    return a;
}

static inline void print_index
(
    FILE       *index,
    const char *format,
    ...
)
{
    if( !index )
        return;
    va_list args;
    va_start( args, format );
    vfprintf( index, format, args );
    va_end( args );
}

static inline void write_av_index_entry
(
    FILE         *index,
    AVIndexEntry *ie
)
{
    print_index( index, "POS=%"PRId64",TS=%"PRId64",Flags=%x,Size=%d,Distance=%d\n",
                 ie->pos, ie->timestamp, ie->flags, ie->size, ie->min_distance );
}

static void write_video_extradata
(
    FILE                *index,
    lwlibav_extradata_t *entry
)
{
    if( !index )
        return;
    fprintf( index, "Size=%d,Codec=%d,4CC=0x%x,Width=%d,Height=%d,Format=%s,BPS=%d\n",
             entry->extradata_size, entry->codec_id, entry->codec_tag, entry->width, entry->height,
             av_get_pix_fmt_name( entry->pixel_format ) ? av_get_pix_fmt_name( entry->pixel_format ) : "none",
             entry->bits_per_sample );
    if( entry->extradata_size > 0 )
        fwrite( entry->extradata, 1, entry->extradata_size, index );
    fprintf( index, "\n" );
}

static void write_audio_extradata
(
    FILE                *index,
    lwlibav_extradata_t *entry
)
{
    if( !index )
        return;
    fprintf( index, "Size=%d,Codec=%d,4CC=0x%x,Layout=0x%"PRIx64",Rate=%d,Format=%s,BPS=%d,Align=%d\n",
             entry->extradata_size, entry->codec_id, entry->codec_tag, entry->channel_layout, entry->sample_rate,
             av_get_sample_fmt_name( entry->sample_format ) ? av_get_sample_fmt_name( entry->sample_format ) : "none",
             entry->bits_per_sample, entry->block_align );
    if( entry->extradata_size > 0 )
        fwrite( entry->extradata, 1, entry->extradata_size, index );
    fprintf( index, "\n" );
}

static void disable_video_stream( lwlibav_video_decode_handler_t *vdhp )
{
    if( vdhp->frame_list )
        lw_freep( &vdhp->frame_list );
    if( vdhp->keyframe_list )
        lw_freep( &vdhp->keyframe_list );
    if( vdhp->order_converter )
        lw_freep( &vdhp->order_converter );
    if( vdhp->index_entries )
        av_freep( &vdhp->index_entries );
    vdhp->stream_index        = -1;
    vdhp->index_entries_count = 0;
    vdhp->frame_count         = 0;
}

static void cleanup_index_helpers( AVFormatContext *format_ctx )
{
    for( unsigned int stream_index = 0; stream_index < format_ctx->nb_streams; stream_index++ )
    {
        lwindex_helper_t *helper = (lwindex_helper_t *)format_ctx->streams[stream_index]->codec->opaque;
        if( !helper )
            continue;
        if( helper->parser_ctx )
            av_parser_close( helper->parser_ctx );
        if( helper->bsf )
            av_bitstream_filter_close( helper->bsf );
        if( helper->picture )
            av_frame_free( &helper->picture );
        if( helper->buffer )
            av_free( helper->buffer );
        lwlibav_extradata_handler_t *list = &helper->exh;
        if( list->entries )
        {
            for( int i = 0; i < list->entry_count; i++ )
                if( list->entries[i].extradata )
                    av_free( list->entries[i].extradata );
            free( list->entries );
        }
        /* Free an index helper. */
        lw_freep( &format_ctx->streams[stream_index]->codec->opaque );
    }
}

static void create_index
(
    lwlibav_file_handler_t         *lwhp,
    lwlibav_video_decode_handler_t *vdhp,
    lwlibav_video_output_handler_t *vohp,
    lwlibav_audio_decode_handler_t *adhp,
    lwlibav_audio_output_handler_t *aohp,
    AVFormatContext                *format_ctx,
    lwlibav_option_t               *opt,
    progress_indicator_t           *indicator,
    progress_handler_t             *php
)
{
    uint32_t video_info_count = 1 << 16;
    uint32_t audio_info_count = 1 << 16;
    video_frame_info_t *video_info = (video_frame_info_t *)lw_malloc_zero( video_info_count * sizeof(video_frame_info_t) );
    if( !video_info )
        return;
    audio_frame_info_t *audio_info = (audio_frame_info_t *)lw_malloc_zero( audio_info_count * sizeof(audio_frame_info_t) );
    if( !audio_info )
    {
        free( video_info );
        return;
    }
    /*
        # Structure of Libav reader index file
        <LibavReaderIndexFile=12>
        <InputFilePath>foobar.omo</InputFilePath>
        <LibavReaderIndex=0x00000208,0,marumoska>
        <ActiveVideoStreamIndex>+0000000000</ActiveVideoStreamIndex>
        <ActiveAudioStreamIndex>-0000000001</ActiveAudioStreamIndex>
        Index=0,Type=0,Codec=2,TimeBase=1001/24000,POS=0,PTS=2002,DTS=0,EDI=0
        Key=1,Pic=1,POC=0,Repeat=1,Field=0,Width=1920,Height=1080,Format=yuv420p,ColorSpace=5
        </LibavReaderIndex>
        <StreamIndexEntries=0,0,1>
        POS=0,TS=2002,Flags=1,Size=1024,Distance=0
        </StreamIndexEntries>
        <ExtraDataList=0,0,1>
        Size=252,Codec=28,4CC=0x564d4448,Width=1920,Height=1080,Format=yuv420p,BPS=0
        ... binary string ...
        </ExtraDataList>
        </LibavReaderIndexFile>
     */
    char index_path[512] = { 0 };
    sprintf( index_path, "%s.lwi", lwhp->file_path );
    FILE *index = !opt->no_create_index ? fopen( index_path, "wb" ) : NULL;
    if( !index && !opt->no_create_index )
    {
        free( video_info );
        free( audio_info );
        return;
    }
    lwhp->format_name  = (char *)format_ctx->iformat->name;
    lwhp->format_flags = format_ctx->iformat->flags;
    lwhp->raw_demuxer  = !!format_ctx->iformat->raw_codec_id;
    vdhp->format       = format_ctx;
    adhp->format       = format_ctx;
    adhp->dv_in_avi    = !strcmp( lwhp->format_name, "avi" ) ? -1 : 0;
    int32_t video_index_pos = 0;
    int32_t audio_index_pos = 0;
    if( index )
    {
        /* Write Index file header. */
        fprintf( index, "<LibavReaderIndexFile=%d>\n", INDEX_FILE_VERSION );
        fprintf( index, "<InputFilePath>%s</InputFilePath>\n", lwhp->file_path );
        fprintf( index, "<LibavReaderIndex=0x%08x,%d,%s>\n", lwhp->format_flags, lwhp->raw_demuxer, lwhp->format_name );
        video_index_pos = ftell( index );
        fprintf( index, "<ActiveVideoStreamIndex>%+011d</ActiveVideoStreamIndex>\n", -1 );
        audio_index_pos = ftell( index );
        fprintf( index, "<ActiveAudioStreamIndex>%+011d</ActiveAudioStreamIndex>\n", -1 );
    }
    AVPacket pkt = { 0 };
    av_init_packet( &pkt );
    int       video_resolution      = 0;
    int       is_attached_pic       = 0;
    uint32_t  video_sample_count    = 0;
    int64_t   last_keyframe_pts     = AV_NOPTS_VALUE;
    uint32_t  audio_sample_count    = 0;
    int       audio_sample_rate     = 0;
    int       constant_frame_length = 1;
    uint64_t  audio_duration        = 0;
    int64_t   first_dts             = AV_NOPTS_VALUE;
    int64_t   filesize              = avio_size( format_ctx->pb );
    if( indicator->open )
        indicator->open( php );
    /* Start to read frames and write the index file. */
    while( read_av_frame( format_ctx, &pkt ) >= 0 )
    {
        AVStream       *stream  = format_ctx->streams[ pkt.stream_index ];
        AVCodecContext *pkt_ctx = stream->codec;
        if( pkt_ctx->codec_type != AVMEDIA_TYPE_VIDEO
         && pkt_ctx->codec_type != AVMEDIA_TYPE_AUDIO )
            continue;
        if( pkt_ctx->codec_id == AV_CODEC_ID_NONE )
            continue;
        if( !av_codec_is_decoder( pkt_ctx->codec ) && open_decoder( pkt_ctx, pkt_ctx->codec_id, lwhp->threads ) )
            continue;
        lwindex_helper_t *helper = get_index_helper( lwhp->format_name, pkt_ctx, stream );
        if( !helper )
        {
            av_free_packet( &pkt );
            goto fail_index;
        }
        int extradata_index = append_extradata_if_new( helper, pkt_ctx, &pkt );
        if( extradata_index < 0 )
        {
            av_free_packet( &pkt );
            goto fail_index;
        }
        if( pkt_ctx->codec_type == AVMEDIA_TYPE_VIDEO )
        {
            if( pkt_ctx->pix_fmt == AV_PIX_FMT_NONE )
                investigate_pix_fmt_by_decoding( pkt_ctx, &pkt, vdhp->frame_buffer );
            int dv_in_avi_init = 0;
            if( adhp->dv_in_avi    == -1
             && vdhp->stream_index == -1
             && pkt_ctx->codec_id  == AV_CODEC_ID_DVVIDEO
             && opt->force_audio   == 0 )
            {
                dv_in_avi_init     = 1;
                adhp->dv_in_avi    = 1;
                vdhp->stream_index = pkt.stream_index;
            }
            /* Replace lower resolution stream with higher. Override attached picture. */
            int higher_priority = ((pkt_ctx->width * pkt_ctx->height > video_resolution)
                                || (is_attached_pic && !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC)));
            if( dv_in_avi_init
             || (!opt->force_video && (vdhp->stream_index == -1 || (pkt.stream_index != vdhp->stream_index && higher_priority)))
             || (opt->force_video && vdhp->stream_index == -1 && pkt.stream_index == opt->force_video_index) )
            {
                /* Update active video stream. */
                if( index )
                {
                    int32_t current_pos = ftell( index );
                    fseek( index, video_index_pos, SEEK_SET );
                    fprintf( index, "<ActiveVideoStreamIndex>%+011d</ActiveVideoStreamIndex>\n", pkt.stream_index );
                    fseek( index, current_pos, SEEK_SET );
                }
                memset( video_info, 0, (video_sample_count + 1) * sizeof(video_frame_info_t) );
                vdhp->ctx                = pkt_ctx;
                vdhp->codec_id           = pkt_ctx->codec_id;
                vdhp->stream_index       = pkt.stream_index;
                video_resolution         = pkt_ctx->width * pkt_ctx->height;
                is_attached_pic          = !!(stream->disposition & AV_DISPOSITION_ATTACHED_PIC);
                video_sample_count       = 0;
                last_keyframe_pts        = AV_NOPTS_VALUE;
                vdhp->max_width          = pkt_ctx->width;
                vdhp->max_height         = pkt_ctx->height;
                vdhp->initial_width      = pkt_ctx->width;
                vdhp->initial_height     = pkt_ctx->height;
                vdhp->initial_colorspace = pkt_ctx->colorspace;
            }
            /* Get picture type. */
            int pict_type = get_picture_type( helper, pkt_ctx, &pkt );
            if( pict_type < 0 )
            {
                av_free_packet( &pkt );
                goto fail_index;
            }
            /* Get Picture Order Count. */
            int poc = helper->parser_ctx ? helper->parser_ctx->output_picture_number : 0;
            /* Get field information. */
            int             repeat_pict;
            lw_field_info_t field_info;
            if( helper->parser_ctx )
            {
                repeat_pict = pkt_ctx->ticks_per_frame == 2
                            ? helper->parser_ctx->repeat_pict
                            : 2 * helper->parser_ctx->repeat_pict + 1;
                if( helper->parser_ctx->picture_structure == AV_PICTURE_STRUCTURE_TOP_FIELD )
                    field_info = LW_FIELD_INFO_TOP;
                else if( helper->parser_ctx->picture_structure == AV_PICTURE_STRUCTURE_BOTTOM_FIELD )
                    field_info = LW_FIELD_INFO_BOTTOM;
                else
                {
                    if( helper->parser_ctx->field_order == AV_FIELD_TT
                     || helper->parser_ctx->field_order == AV_FIELD_TB )
                        field_info = LW_FIELD_INFO_TOP;
                    else if( helper->parser_ctx->field_order == AV_FIELD_BB
                          || helper->parser_ctx->field_order == AV_FIELD_BT )
                        field_info = LW_FIELD_INFO_BOTTOM;
                    else
                        field_info = helper->last_field_info;
                }
                helper->last_field_info = field_info;
            }
            else
            {
                repeat_pict = 1;
                field_info = helper->last_field_info;
            }
            /* Set video frame info if this stream is active. */
            if( pkt.stream_index == vdhp->stream_index )
            {
                ++video_sample_count;
                video_info[video_sample_count].pts             = pkt.pts;
                video_info[video_sample_count].dts             = pkt.dts;
                video_info[video_sample_count].file_offset     = pkt.pos;
                video_info[video_sample_count].sample_number   = video_sample_count;
                video_info[video_sample_count].extradata_index = extradata_index;
                video_info[video_sample_count].pict_type       = pict_type;
                video_info[video_sample_count].poc             = poc;
                video_info[video_sample_count].repeat_pict     = repeat_pict;
                video_info[video_sample_count].field_info      = field_info;
                if( pkt.pts != AV_NOPTS_VALUE && last_keyframe_pts != AV_NOPTS_VALUE && pkt.pts < last_keyframe_pts )
                    video_info[video_sample_count].flags |= LW_VFRAME_FLAG_LEADING;
                if( pkt.flags & AV_PKT_FLAG_KEY )
                {
                    /* For the present, treat this frame as a keyframe. */
                    video_info[video_sample_count].flags |= LW_VFRAME_FLAG_KEY;
                    last_keyframe_pts = pkt.pts;
                }
                if( repeat_pict == 0 && field_info == LW_FIELD_INFO_UNKNOWN && pkt_ctx->pix_fmt == AV_PIX_FMT_NONE
                 && (pkt_ctx->codec_id == AV_CODEC_ID_H264 || pkt_ctx->codec_id == AV_CODEC_ID_HEVC)
                 && (pkt_ctx->width == 0 || pkt_ctx->height == 0) )
                    video_info[video_sample_count].flags |= LW_VFRAME_FLAG_CORRUPT;
                /* Set maximum resolution. */
                if( vdhp->max_width  < pkt_ctx->width )
                    vdhp->max_width  = pkt_ctx->width;
                if( vdhp->max_height < pkt_ctx->height )
                    vdhp->max_height = pkt_ctx->height;
                if( video_sample_count + 1 == video_info_count )
                {
                    video_info_count <<= 1;
                    video_frame_info_t *temp = (video_frame_info_t *)realloc( video_info, video_info_count * sizeof(video_frame_info_t) );
                    if( !temp )
                    {
                        av_free_packet( &pkt );
                        goto fail_index;
                    }
                    video_info = temp;
                }
            }
            /* Set width, height and pixel_format for the current extradata. */
            if( extradata_index >= 0 )
            {
                lwlibav_extradata_handler_t *list = &helper->exh;
                lwlibav_extradata_t *entry = &list->entries[ list->current_index ];
                if( entry->width < pkt_ctx->width )
                    entry->width = pkt_ctx->width;
                if( entry->height < pkt_ctx->height )
                    entry->height = pkt_ctx->height;
                if( entry->pixel_format == AV_PIX_FMT_NONE )
                    entry->pixel_format = pkt_ctx->pix_fmt;
                if( entry->bits_per_sample == 0 )
                    entry->bits_per_sample = pkt_ctx->bits_per_coded_sample;
                if( entry->codec_id == AV_CODEC_ID_NONE )
                    entry->codec_id = pkt_ctx->codec_id;
                if( entry->codec_tag == 0 )
                    entry->codec_tag = pkt_ctx->codec_tag;
            }
            /* Write a video packet info to the index file. */
            print_index( index, "Index=%d,Type=%d,Codec=%d,TimeBase=%d/%d,POS=%"PRId64",PTS=%"PRId64",DTS=%"PRId64",EDI=%d\n"
                         "Key=%d,Pic=%d,POC=%d,Repeat=%d,Field=%d,Width=%d,Height=%d,Format=%s,ColorSpace=%d\n",
                         pkt.stream_index, AVMEDIA_TYPE_VIDEO, pkt_ctx->codec_id,
                         stream->time_base.num, stream->time_base.den,
                         pkt.pos, pkt.pts, pkt.dts, extradata_index,
                         !!(pkt.flags & AV_PKT_FLAG_KEY), pict_type, poc, repeat_pict, field_info,
                         pkt_ctx->width, pkt_ctx->height,
                         av_get_pix_fmt_name( pkt_ctx->pix_fmt ) ? av_get_pix_fmt_name( pkt_ctx->pix_fmt ) : "none",
                         pkt_ctx->colorspace );
        }
        else
        {
            if( adhp->stream_index == -1 && (!opt->force_audio || (opt->force_audio && pkt.stream_index == opt->force_audio_index)) )
            {
                /* Update active audio stream. */
                if( index )
                {
                    int32_t current_pos = ftell( index );
                    fseek( index, audio_index_pos, SEEK_SET );
                    fprintf( index, "<ActiveAudioStreamIndex>%+011d</ActiveAudioStreamIndex>\n", pkt.stream_index );
                    fseek( index, current_pos, SEEK_SET );
                }
                adhp->ctx          = pkt_ctx;
                adhp->codec_id     = pkt_ctx->codec_id;
                adhp->stream_index = pkt.stream_index;
            }
            int bits_per_sample = pkt_ctx->bits_per_raw_sample   > 0 ? pkt_ctx->bits_per_raw_sample
                                : pkt_ctx->bits_per_coded_sample > 0 ? pkt_ctx->bits_per_coded_sample
                                : av_get_bytes_per_sample( pkt_ctx->sample_fmt ) << 3;
            /* Get audio frame_length. */
            int frame_length = get_audio_frame_length( helper, pkt_ctx, &pkt );
            /* Set audio frame info if this stream is active. */
            if( pkt.stream_index == adhp->stream_index )
            {
                if( frame_length != -1 )
                    audio_duration += frame_length;
                if( audio_duration <= INT32_MAX )
                {
                    /* Set up audio frame info. */
                    ++audio_sample_count;
                    audio_info[audio_sample_count].pts             = pkt.pts;
                    audio_info[audio_sample_count].dts             = pkt.dts;
                    audio_info[audio_sample_count].file_offset     = pkt.pos;
                    audio_info[audio_sample_count].sample_number   = audio_sample_count;
                    audio_info[audio_sample_count].extradata_index = extradata_index;
                    audio_info[audio_sample_count].sample_rate     = pkt_ctx->sample_rate;
                    if( frame_length != -1 && audio_sample_count > helper->delay_count )
                    {
                        uint32_t audio_frame_number = audio_sample_count - helper->delay_count;
                        audio_info[audio_frame_number].length = frame_length;
                        if( audio_frame_number > 1 && audio_info[audio_frame_number].length != audio_info[audio_frame_number - 1].length )
                            constant_frame_length = 0;
                    }
                    if( audio_sample_rate == 0 )
                        audio_sample_rate = pkt_ctx->sample_rate;
                    if( audio_sample_count + 1 == audio_info_count )
                    {
                        audio_info_count <<= 1;
                        audio_frame_info_t *temp = (audio_frame_info_t *)realloc( audio_info, audio_info_count * sizeof(audio_frame_info_t) );
                        if( !temp )
                        {
                            av_free_packet( &pkt );
                            goto fail_index;
                        }
                        audio_info = temp;
                    }
                    if( pkt_ctx->channel_layout == 0 )
                        pkt_ctx->channel_layout = av_get_default_channel_layout( pkt_ctx->channels );
                    if( av_get_channel_layout_nb_channels( pkt_ctx->channel_layout )
                      > av_get_channel_layout_nb_channels( aohp->output_channel_layout ) )
                        aohp->output_channel_layout = pkt_ctx->channel_layout;
                    aohp->output_sample_format   = select_better_sample_format( aohp->output_sample_format, pkt_ctx->sample_fmt );
                    aohp->output_sample_rate     = MAX( aohp->output_sample_rate, audio_sample_rate );
                    aohp->output_bits_per_sample = MAX( aohp->output_bits_per_sample, bits_per_sample );
                }
            }
            /* Set channel_layout, sample_rate, sample_format and bits_per_sample for the current extradata. */
            if( extradata_index >= 0 )
            {
                lwlibav_extradata_handler_t *list = &helper->exh;
                lwlibav_extradata_t *entry = &list->entries[ list->current_index ];
                if( entry->channel_layout == 0 )
                    entry->channel_layout = pkt_ctx->channel_layout;
                if( entry->sample_rate == 0 )
                    entry->sample_rate = pkt_ctx->sample_rate;
                if( entry->sample_format == AV_SAMPLE_FMT_NONE )
                    entry->sample_format = pkt_ctx->sample_fmt;
                if( entry->bits_per_sample == 0 )
                    entry->bits_per_sample = bits_per_sample;
                if( entry->block_align == 0 )
                    entry->block_align = pkt_ctx->block_align;
                if( entry->codec_id == AV_CODEC_ID_NONE )
                    entry->codec_id = pkt_ctx->codec_id;
                if( entry->codec_tag == 0 )
                    entry->codec_tag = pkt_ctx->codec_tag;
            }
            /* Write an audio packet info to the index file. */
            print_index( index, "Index=%d,Type=%d,Codec=%d,TimeBase=%d/%d,POS=%"PRId64",PTS=%"PRId64",DTS=%"PRId64",EDI=%d\n"
                         "Channels=%d:0x%"PRIx64",Rate=%d,Format=%s,BPS=%d,Length=%d\n",
                         pkt.stream_index, AVMEDIA_TYPE_AUDIO, pkt_ctx->codec_id,
                         stream->time_base.num, stream->time_base.den,
                         pkt.pos, pkt.pts, pkt.dts, extradata_index,
                         pkt_ctx->channels, pkt_ctx->channel_layout, pkt_ctx->sample_rate,
                         av_get_sample_fmt_name( pkt_ctx->sample_fmt ) ? av_get_sample_fmt_name( pkt_ctx->sample_fmt ) : "none",
                         bits_per_sample, frame_length );
        }
        if( indicator->update )
        {
            /* Update progress dialog. */
            int percent = 0;
            if( first_dts == AV_NOPTS_VALUE )
                first_dts = pkt.dts;
            if( filesize > 0 && pkt.pos > 0 )
                /* Update if packet's file offset is valid. */
                percent = (int)(100.0 * ((double)pkt.pos / filesize) + 0.5);
            else if( format_ctx->duration > 0 && first_dts != AV_NOPTS_VALUE && pkt.dts != AV_NOPTS_VALUE )
                /* Update if packet's DTS is valid. */
                percent = (int)(100.0
                             * (pkt.dts - first_dts) * (stream->time_base.num / (double)stream->time_base.den)
                             / (format_ctx->duration / AV_TIME_BASE)
                             + 0.5);
            const char *message = index ? "Creating Index file" : "Parsing input file";
            int abort = indicator->update( php, message, percent );
            av_free_packet( &pkt );
            if( abort )
                goto fail_index;
        }
        else
            av_free_packet( &pkt );
    }
    /* Handle delay derived from the audio decoder. */
    for( unsigned int stream_index = 0; stream_index < format_ctx->nb_streams; stream_index++ )
    {
        AVStream         *stream  = format_ctx->streams[stream_index];
        AVCodecContext   *pkt_ctx = stream->codec;
        lwindex_helper_t *helper  = (lwindex_helper_t *)pkt_ctx->opaque;
        if( !helper || !helper->decode || pkt_ctx->codec_type != AVMEDIA_TYPE_AUDIO )
            continue;
        /* Flush if decoding is delayed. */
        for( uint32_t i = 1; i <= helper->delay_count; i++ )
        {
            AVPacket null_pkt = { 0 };
            av_init_packet( &null_pkt );
            null_pkt.data = NULL;
            null_pkt.size = 0;
            int decode_complete;
            if( helper->decode( pkt_ctx, helper->picture, &decode_complete, &null_pkt ) >= 0 )
            {
                int frame_length = decode_complete ? helper->picture->nb_samples : 0;
                if( stream_index == adhp->stream_index )
                {
                    audio_duration += frame_length;
                    if( audio_duration > INT32_MAX )
                        break;
                    uint32_t audio_frame_number = audio_sample_count - helper->delay_count + i;
                    audio_info[audio_frame_number].length = frame_length;
                    if( audio_frame_number > 1
                     && audio_info[audio_frame_number].length != audio_info[audio_frame_number - 1].length )
                        constant_frame_length = 0;
                }
                print_index( index, "Index=%d,Type=%d,Codec=%d,TimeBase=%d/%d,POS=-1,PTS=%"PRId64",DTS=%"PRId64",EDI=-1\n"
                             "Channels=0:0x0,Rate=0,Format=none,BPS=0,Length=%d\n",
                             stream_index, AVMEDIA_TYPE_AUDIO, pkt_ctx->codec_id,
                             format_ctx->streams[stream_index]->time_base.num,
                             format_ctx->streams[stream_index]->time_base.den,
                             AV_NOPTS_VALUE, AV_NOPTS_VALUE, frame_length );
            }
        }
    }
    print_index( index, "</LibavReaderIndex>\n" );
    /* Deallocate video frame info if no active video stream. */
    if( vdhp->stream_index < 0 )
        lw_freep( &video_info );
    /* Deallocate audio frame info if no active audio stream. */
    if( adhp->stream_index < 0 )
        lw_freep( &audio_info );
    else
    {
        /* Check the active stream is DV in AVI Type-1 or not. */
        if( adhp->dv_in_avi == 1 && format_ctx->streams[ adhp->stream_index ]->nb_index_entries == 0 )
        {
            /* DV in AVI Type-1 */
            audio_sample_count = video_info ? MIN( video_sample_count, audio_sample_count ) : 0;
            for( uint32_t i = 1; i <= audio_sample_count; i++ )
            {
                audio_info[i].keyframe        = !!(video_info[i].flags & LW_VFRAME_FLAG_KEY);
                audio_info[i].sample_number   = video_info[i].sample_number;
                audio_info[i].pts             = video_info[i].pts;
                audio_info[i].dts             = video_info[i].dts;
                audio_info[i].file_offset     = video_info[i].file_offset;
                audio_info[i].extradata_index = video_info[i].extradata_index;
            }
        }
        else
        {
            if( adhp->dv_in_avi == 1 && opt->force_video && opt->force_video_index == -1 )
            {
                /* Disable DV video stream. */
                disable_video_stream( vdhp );
                video_info = NULL;
            }
            adhp->dv_in_avi = 0;
        }
    }
    for( unsigned int stream_index = 0; stream_index < format_ctx->nb_streams; stream_index++ )
    {
        AVStream *stream = format_ctx->streams[stream_index];
        if( stream->codec->codec_type == AVMEDIA_TYPE_VIDEO )
        {
            print_index( index, "<StreamIndexEntries=%d,%d,%d>\n", stream_index, AVMEDIA_TYPE_VIDEO, stream->nb_index_entries );
            if( vdhp->stream_index != stream_index )
                for( int i = 0; i < stream->nb_index_entries; i++ )
                    write_av_index_entry( index, &stream->index_entries[i] );
            else if( stream->nb_index_entries > 0 )
            {
                vdhp->index_entries = (AVIndexEntry *)av_malloc( stream->index_entries_allocated_size );
                if( !vdhp->index_entries )
                    goto fail_index;
                for( int i = 0; i < stream->nb_index_entries; i++ )
                {
                    AVIndexEntry *ie = &stream->index_entries[i];
                    vdhp->index_entries[i] = *ie;
                    write_av_index_entry( index, ie );
                }
                vdhp->index_entries_count = stream->nb_index_entries;
            }
            print_index( index, "</StreamIndexEntries>\n" );
        }
        else if( stream->codec->codec_type == AVMEDIA_TYPE_AUDIO )
        {
            print_index( index, "<StreamIndexEntries=%d,%d,%d>\n", stream_index, AVMEDIA_TYPE_AUDIO, stream->nb_index_entries );
            if( adhp->stream_index != stream_index )
                for( int i = 0; i < stream->nb_index_entries; i++ )
                    write_av_index_entry( index, &stream->index_entries[i] );
            else if( stream->nb_index_entries > 0 )
            {
                /* Audio stream in matroska container requires index_entries for seeking.
                 * This avoids for re-reading the file to create index_entries since the file will be closed once. */
                adhp->index_entries = (AVIndexEntry *)av_malloc( stream->index_entries_allocated_size );
                if( !adhp->index_entries )
                    goto fail_index;
                for( int i = 0; i < stream->nb_index_entries; i++ )
                {
                    AVIndexEntry *ie = &stream->index_entries[i];
                    adhp->index_entries[i] = *ie;
                    write_av_index_entry( index, ie );
                }
                adhp->index_entries_count = stream->nb_index_entries;
            }
            print_index( index, "</StreamIndexEntries>\n" );
        }
    }
    for( unsigned int stream_index = 0; stream_index < format_ctx->nb_streams; stream_index++ )
    {
        AVStream *stream = format_ctx->streams[stream_index];
        if( stream->codec->codec_type == AVMEDIA_TYPE_VIDEO || stream->codec->codec_type == AVMEDIA_TYPE_AUDIO )
        {
            lwindex_helper_t *helper = (lwindex_helper_t *)stream->codec->opaque;
            if( !helper )
                continue;
            lwlibav_extradata_handler_t *list = &helper->exh;
            void (*write_av_extradata)( FILE *, lwlibav_extradata_t * ) = stream->codec->codec_type == AVMEDIA_TYPE_VIDEO
                                                                        ? write_video_extradata
                                                                        : write_audio_extradata;
            print_index( index, "<ExtraDataList=%d,%d,%d>\n", stream_index, stream->codec->codec_type, list->entry_count );
            if( (stream->codec->codec_type == AVMEDIA_TYPE_VIDEO && stream_index == vdhp->stream_index)
             || (stream->codec->codec_type == AVMEDIA_TYPE_AUDIO && stream_index == adhp->stream_index) )
            {
                for( int i = 0; i < list->entry_count; i++ )
                    write_av_extradata( index, &list->entries[i] );
                lwlibav_extradata_handler_t *exhp = stream->codec->codec_type == AVMEDIA_TYPE_VIDEO ? &vdhp->exh : &adhp->exh;
                exhp->entry_count   = list->entry_count;
                exhp->entries       = list->entries;
                exhp->current_index = stream->codec->codec_type == AVMEDIA_TYPE_VIDEO
                                    ? video_info[1].extradata_index
                                    : audio_info[1].extradata_index;
                /* Avoid freeing entries. */
                list->entry_count = 0;
                list->entries     = NULL;
            }
            else
                for( int i = 0; i < list->entry_count; i++ )
                    write_av_extradata( index, &list->entries[i] );
            print_index( index, "</ExtraDataList>\n" );
        }
    }
    print_index( index, "</LibavReaderIndexFile>\n" );
    if( vdhp->stream_index >= 0 )
    {
        vdhp->keyframe_list = (uint8_t *)lw_malloc_zero( (video_sample_count + 1) * sizeof(uint8_t) );
        if( !vdhp->keyframe_list )
            goto fail_index;
        vdhp->frame_list      = video_info;
        vdhp->frame_count     = video_sample_count;
        vdhp->initial_pix_fmt = vdhp->ctx->pix_fmt;
        if( decide_video_seek_method( lwhp, vdhp, video_sample_count, format_ctx->streams[ vdhp->stream_index ]->time_base ) )
            goto fail_index;
        /* Create the repeat control info. */
        create_video_frame_order_list( vdhp, vohp, opt );
    }
    if( adhp->stream_index >= 0 )
    {
        adhp->frame_list   = audio_info;
        adhp->frame_count  = audio_sample_count;
        adhp->frame_length = constant_frame_length ? adhp->frame_list[1].length : 0;
        decide_audio_seek_method( lwhp, adhp, audio_sample_count );
        if( opt->av_sync && vdhp->stream_index >= 0 )
            lwhp->av_gap = calculate_av_gap( vdhp, vohp, adhp,
                                             format_ctx->streams[ vdhp->stream_index ]->time_base,
                                             format_ctx->streams[ adhp->stream_index ]->time_base,
                                             audio_sample_rate );
    }
    cleanup_index_helpers( format_ctx );
    if( index )
        fclose( index );
    if( indicator->close )
        indicator->close( php );
    vdhp->format = NULL;
    adhp->format = NULL;
    return;
fail_index:
    cleanup_index_helpers( format_ctx );
    free( video_info );
    free( audio_info );
    if( index )
        fclose( index );
    if( indicator->close )
        indicator->close( php );
    vdhp->format = NULL;
    adhp->format = NULL;
    return;
}

static int parse_index
(
    lwlibav_file_handler_t         *lwhp,
    lwlibav_video_decode_handler_t *vdhp,
    lwlibav_video_output_handler_t *vohp,
    lwlibav_audio_decode_handler_t *adhp,
    lwlibav_audio_output_handler_t *aohp,
    lwlibav_option_t               *opt,
    FILE                           *index
)
{
    /* Test to open the target file. */
    char file_path[512] = { 0 };
    if( fscanf( index, "<InputFilePath>%[^\n<]</InputFilePath>\n", file_path ) != 1 )
        return -1;
    FILE *target = fopen( file_path, "rb" );
    if( !target )
        return -1;
    fclose( target );
    int file_path_length = strlen( file_path );
    lwhp->file_path = (char *)lw_malloc_zero( file_path_length + 1 );
    if( !lwhp->file_path )
        return -1;
    memcpy( lwhp->file_path, file_path, file_path_length );
    /* Parse the index file. */
    char format_name[256];
    int active_video_index;
    int active_audio_index;
    if( fscanf( index, "<LibavReaderIndex=0x%x,%d,%[^>]>\n", &lwhp->format_flags, &lwhp->raw_demuxer, format_name ) != 3 )
        return -1;
    int32_t active_index_pos = ftell( index );
    if( fscanf( index, "<ActiveVideoStreamIndex>%d</ActiveVideoStreamIndex>\n", &active_video_index ) != 1
     || fscanf( index, "<ActiveAudioStreamIndex>%d</ActiveAudioStreamIndex>\n", &active_audio_index ) != 1 )
        return -1;
    lwhp->format_name = format_name;
    adhp->dv_in_avi = !strcmp( lwhp->format_name, "avi" ) ? -1 : 0;
    int video_present = (active_video_index >= 0);
    int audio_present = (active_audio_index >= 0);
    vdhp->stream_index = opt->force_video ? opt->force_video_index : active_video_index;
    adhp->stream_index = opt->force_audio ? opt->force_audio_index : active_audio_index;
    uint32_t video_info_count = 1 << 16;
    uint32_t audio_info_count = 1 << 16;
    video_frame_info_t *video_info = NULL;
    audio_frame_info_t *audio_info = NULL;
    if( vdhp->stream_index >= 0 )
    {
        video_info = (video_frame_info_t *)lw_malloc_zero( video_info_count * sizeof(video_frame_info_t) );
        if( !video_info )
            goto fail_parsing;
    }
    if( adhp->stream_index >= 0 )
    {
        audio_info = (audio_frame_info_t *)lw_malloc_zero( audio_info_count * sizeof(audio_frame_info_t) );
        if( !audio_info )
            goto fail_parsing;
    }
    vdhp->codec_id             = AV_CODEC_ID_NONE;
    adhp->codec_id             = AV_CODEC_ID_NONE;
    vdhp->initial_pix_fmt      = AV_PIX_FMT_NONE;
    vdhp->initial_colorspace   = AVCOL_SPC_NB;
    aohp->output_sample_format = AV_SAMPLE_FMT_NONE;
    uint32_t video_sample_count    = 0;
    int64_t  last_keyframe_pts     = AV_NOPTS_VALUE;
    uint32_t audio_sample_count    = 0;
    int      audio_sample_rate     = 0;
    int      constant_frame_length = 1;
    uint64_t audio_duration        = 0;
    AVRational video_time_base     = { 0, 0 };
    AVRational audio_time_base     = { 0, 0 };
    char buf[1024];
    while( fgets( buf, sizeof(buf), index ) )
    {
        int stream_index;
        int codec_type;
        int codec_id;
        int extradata_index;
        AVRational time_base;
        int64_t pos;
        int64_t pts;
        int64_t dts;
        if( sscanf( buf, "Index=%d,Type=%d,Codec=%d,TimeBase=%d/%d,POS=%"SCNd64",PTS=%"SCNd64",DTS=%"SCNd64",EDI=%d",
                    &stream_index, &codec_type, &codec_id, &time_base.num, &time_base.den, &pos, &pts, &dts, &extradata_index ) != 9 )
            break;
        if( codec_type == AVMEDIA_TYPE_VIDEO )
        {
            if( !fgets( buf, sizeof(buf), index ) )
                goto fail_parsing;
            if( adhp->dv_in_avi == -1 && codec_id == AV_CODEC_ID_DVVIDEO && !opt->force_audio )
            {
                adhp->dv_in_avi = 1;
                if( vdhp->stream_index == -1 )
                {
                    vdhp->stream_index = stream_index;
                    video_info = (video_frame_info_t *)lw_malloc_zero( video_info_count * sizeof(video_frame_info_t) );
                    if( !video_info )
                        goto fail_parsing;
                }
            }
            if( stream_index == vdhp->stream_index )
            {
                int pict_type;
                int poc;
                int repeat_pict;
                int field_info;
                int key;
                int width;
                int height;
                int colorspace;
                char pix_fmt[64];
                if( sscanf( buf, "Key=%d,Pic=%d,POC=%d,Repeat=%d,Field=%d,Width=%d,Height=%d,Format=%[^,],ColorSpace=%d",
                            &key, &pict_type, &poc, &repeat_pict, &field_info, &width, &height, pix_fmt, &colorspace ) != 9 )
                    goto fail_parsing;
                if( vdhp->codec_id == AV_CODEC_ID_NONE )
                    vdhp->codec_id = (enum AVCodecID)codec_id;
                if( (key | width | height) || pict_type == -1 || colorspace != AVCOL_SPC_NB )
                {
                    if( vdhp->initial_width == 0 || vdhp->initial_height == 0 )
                    {
                        vdhp->initial_width  = width;
                        vdhp->initial_height = height;
                        vdhp->max_width      = width;
                        vdhp->max_height     = height;
                    }
                    else
                    {
                        if( vdhp->max_width  < width )
                            vdhp->max_width  = width;
                        if( vdhp->max_height < width )
                            vdhp->max_height = height;
                    }
                    if( vdhp->initial_pix_fmt == AV_PIX_FMT_NONE )
                        vdhp->initial_pix_fmt = av_get_pix_fmt( (const char *)pix_fmt );
                    if( vdhp->initial_colorspace == AVCOL_SPC_NB )
                        vdhp->initial_colorspace = (enum AVColorSpace)colorspace;
                    if( video_time_base.num == 0 || video_time_base.den == 0 )
                    {
                        video_time_base.num = time_base.num;
                        video_time_base.den = time_base.den;
                    }
                    ++video_sample_count;
                    video_info[video_sample_count].pts             = pts;
                    video_info[video_sample_count].dts             = dts;
                    video_info[video_sample_count].file_offset     = pos;
                    video_info[video_sample_count].sample_number   = video_sample_count;
                    video_info[video_sample_count].extradata_index = extradata_index;
                    video_info[video_sample_count].pict_type       = pict_type;
                    video_info[video_sample_count].poc             = poc;
                    video_info[video_sample_count].repeat_pict     = repeat_pict;
                    video_info[video_sample_count].field_info      = (lw_field_info_t)field_info;
                    if( pts != AV_NOPTS_VALUE && last_keyframe_pts != AV_NOPTS_VALUE && pts < last_keyframe_pts )
                        video_info[video_sample_count].flags |= LW_VFRAME_FLAG_LEADING;
                    if( key )
                    {
                        video_info[video_sample_count].flags |= LW_VFRAME_FLAG_KEY;
                        last_keyframe_pts = pts;
                    }
                    if( repeat_pict == 0 && field_info == LW_FIELD_INFO_UNKNOWN
                     && av_get_pix_fmt( (const char *)pix_fmt ) == AV_PIX_FMT_NONE
                     && ((enum AVCodecID)codec_id == AV_CODEC_ID_H264 || (enum AVCodecID)codec_id == AV_CODEC_ID_HEVC)
                     && (width == 0 || height == 0) )
                        video_info[video_sample_count].flags |= LW_VFRAME_FLAG_CORRUPT;
                }
                if( video_sample_count + 1 == video_info_count )
                {
                    video_info_count <<= 1;
                    video_frame_info_t *temp = (video_frame_info_t *)realloc( video_info, video_info_count * sizeof(video_frame_info_t) );
                    if( !temp )
                        goto fail_parsing;
                    video_info = temp;
                }
            }
        }
        else if( codec_type == AVMEDIA_TYPE_AUDIO )
        {
            if( !fgets( buf, sizeof(buf), index ) )
                goto fail_parsing;
            if( stream_index == adhp->stream_index )
            {
                uint64_t layout;
                int      channels;
                int      sample_rate;
                char     sample_fmt[64];
                int      bits_per_sample;
                int      frame_length;
                if( sscanf( buf, "Channels=%d:0x%"SCNx64",Rate=%d,Format=%[^,],BPS=%d,Length=%d",
                            &channels, &layout, &sample_rate, sample_fmt, &bits_per_sample, &frame_length ) != 6 )
                    goto fail_parsing;
                if( adhp->codec_id == AV_CODEC_ID_NONE )
                    adhp->codec_id = (enum AVCodecID)codec_id;
                if( (channels | layout | sample_rate | bits_per_sample) && audio_duration <= INT32_MAX )
                {
                    if( audio_sample_rate == 0 )
                        audio_sample_rate = sample_rate;
                    if( audio_time_base.num == 0 || audio_time_base.den == 0 )
                    {
                        audio_time_base.num = time_base.num;
                        audio_time_base.den = time_base.den;
                    }
                    if( layout == 0 )
                        layout = av_get_default_channel_layout( channels );
                    if( av_get_channel_layout_nb_channels( layout )
                      > av_get_channel_layout_nb_channels( aohp->output_channel_layout ) )
                        aohp->output_channel_layout = layout;
                    aohp->output_sample_format   = select_better_sample_format( aohp->output_sample_format,
                                                                                av_get_sample_fmt( (const char *)sample_fmt ) );
                    aohp->output_sample_rate     = MAX( aohp->output_sample_rate, audio_sample_rate );
                    aohp->output_bits_per_sample = MAX( aohp->output_bits_per_sample, bits_per_sample );
                    ++audio_sample_count;
                    audio_info[audio_sample_count].pts             = pts;
                    audio_info[audio_sample_count].dts             = dts;
                    audio_info[audio_sample_count].file_offset     = pos;
                    audio_info[audio_sample_count].sample_number   = audio_sample_count;
                    audio_info[audio_sample_count].extradata_index = extradata_index;
                    audio_info[audio_sample_count].sample_rate     = sample_rate;
                }
                else
                    for( uint32_t i = 1; i <= adhp->exh.delay_count; i++ )
                    {
                        uint32_t audio_frame_number = audio_sample_count - adhp->exh.delay_count + i;
                        if( audio_frame_number > audio_sample_count )
                            goto fail_parsing;
                        audio_info[audio_frame_number].length = frame_length;
                        if( audio_frame_number > 1 && audio_info[audio_frame_number].length != audio_info[audio_frame_number - 1].length )
                            constant_frame_length = 0;
                        audio_duration += frame_length;
                    }
                if( audio_sample_count + 1 == audio_info_count )
                {
                    audio_info_count <<= 1;
                    audio_frame_info_t *temp = (audio_frame_info_t *)realloc( audio_info, audio_info_count * sizeof(audio_frame_info_t) );
                    if( !temp )
                        goto fail_parsing;
                    audio_info = temp;
                }
                if( frame_length == -1 )
                    ++ adhp->exh.delay_count;
                else if( audio_sample_count > adhp->exh.delay_count )
                {
                    uint32_t audio_frame_number = audio_sample_count - adhp->exh.delay_count;
                    audio_info[audio_frame_number].length = frame_length;
                    if( audio_frame_number > 1 && audio_info[audio_frame_number].length != audio_info[audio_frame_number - 1].length )
                        constant_frame_length = 0;
                    audio_duration += frame_length;
                }
            }
        }
    }
    if( video_present && opt->force_video && opt->force_video_index != -1
     && (video_sample_count == 0 || vdhp->initial_pix_fmt == AV_PIX_FMT_NONE || vdhp->initial_width == 0 || vdhp->initial_height == 0) )
        goto fail_parsing;  /* Need to re-create the index file. */
    if( audio_present && opt->force_audio && opt->force_audio_index != -1 && (audio_sample_count == 0 || audio_duration == 0) )
        goto fail_parsing;  /* Need to re-create the index file. */
    if( strncmp( buf, "</LibavReaderIndex>", strlen( "</LibavReaderIndex>" ) ) )
        goto fail_parsing;
    /* Parse AVIndexEntry. */
    if( !fgets( buf, sizeof(buf), index ) )
        goto fail_parsing;
    while( !strncmp( buf, "<StreamIndexEntries=", strlen( "<StreamIndexEntries=" ) ) )
    {
        int stream_index;
        int codec_type;
        int index_entries_count;
        if( sscanf( buf, "<StreamIndexEntries=%d,%d,%d>", &stream_index, &codec_type, &index_entries_count ) != 3 )
            goto fail_parsing;
        if( !fgets( buf, sizeof(buf), index ) )
            goto fail_parsing;
        if( index_entries_count > 0 )
        {
            if( codec_type == AVMEDIA_TYPE_VIDEO && stream_index == vdhp->stream_index )
            {
                vdhp->index_entries_count = index_entries_count;
                vdhp->index_entries = (AVIndexEntry *)av_malloc( vdhp->index_entries_count * sizeof(AVIndexEntry) );
                if( !vdhp->index_entries )
                    goto fail_parsing;
                for( int i = 0; i < vdhp->index_entries_count; i++ )
                {
                    AVIndexEntry ie;
                    int size;
                    int flags;
                    if( sscanf( buf, "POS=%"SCNd64",TS=%"SCNd64",Flags=%x,Size=%d,Distance=%d",
                                &ie.pos, &ie.timestamp, &flags, &size, &ie.min_distance ) != 5 )
                        break;
                    ie.size  = size;
                    ie.flags = flags;
                    vdhp->index_entries[i] = ie;
                    if( !fgets( buf, sizeof(buf), index ) )
                        goto fail_parsing;
                }
            }
            else if( codec_type == AVMEDIA_TYPE_AUDIO && stream_index == adhp->stream_index )
            {
                adhp->index_entries_count = index_entries_count;
                adhp->index_entries = (AVIndexEntry *)av_malloc( adhp->index_entries_count * sizeof(AVIndexEntry) );
                if( !adhp->index_entries )
                    goto fail_parsing;
                for( int i = 0; i < adhp->index_entries_count; i++ )
                {
                    AVIndexEntry ie;
                    int size;
                    int flags;
                    if( sscanf( buf, "POS=%"SCNd64",TS=%"SCNd64",Flags=%x,Size=%d,Distance=%d",
                                &ie.pos, &ie.timestamp, &flags, &size, &ie.min_distance ) != 5 )
                        break;
                    ie.size  = size;
                    ie.flags = flags;
                    adhp->index_entries[i] = ie;
                    if( !fgets( buf, sizeof(buf), index ) )
                        goto fail_parsing;
                }
            }
            else
                for( int i = 0; i < index_entries_count; i++ )
                    if( !fgets( buf, sizeof(buf), index ) )
                        goto fail_parsing;
        }
        if( strncmp( buf, "</StreamIndexEntries>", strlen( "</StreamIndexEntries>" ) ) )
            goto fail_parsing;
        if( !fgets( buf, sizeof(buf), index ) )
            goto fail_parsing;
    }
    /* Parse extradata. */
    while( !strncmp( buf, "<ExtraDataList=", strlen( "<ExtraDataList=" ) ) )
    {
        int stream_index;
        int codec_type;
        int entry_count;
        if( sscanf( buf, "<ExtraDataList=%d,%d,%d>", &stream_index, &codec_type, &entry_count ) != 3 )
            goto fail_parsing;
        if( !fgets( buf, sizeof(buf), index ) )
            goto fail_parsing;
        if( entry_count > 0 )
        {
            if( (codec_type == AVMEDIA_TYPE_VIDEO && stream_index == vdhp->stream_index)
             || (codec_type == AVMEDIA_TYPE_AUDIO && stream_index == adhp->stream_index) )
            {
                lwlibav_extradata_handler_t *exhp = codec_type == AVMEDIA_TYPE_VIDEO ? &vdhp->exh : &adhp->exh;
                if( !alloc_extradata_entries( exhp, entry_count ) )
                    goto fail_parsing;
                exhp->current_index = codec_type == AVMEDIA_TYPE_VIDEO
                                    ? video_info[1].extradata_index
                                    : audio_info[1].extradata_index;
                for( int i = 0; i < exhp->entry_count; i++ )
                {
                    lwlibav_extradata_t *entry = &exhp->entries[i];
                    /* Get extradata size and others. */
                    int codec_id;
                    if( codec_type == AVMEDIA_TYPE_VIDEO )
                    {
                        char pix_fmt[64];
                        if( sscanf( buf, "Size=%d,Codec=%d,4CC=0x%x,Width=%d,Height=%d,Format=%[^,],BPS=%d",
                                    &entry->extradata_size, &codec_id, &entry->codec_tag,
                                    &entry->width, &entry->height,
                                    pix_fmt, &entry->bits_per_sample ) != 7 )
                            break;
                        entry->pixel_format = av_get_pix_fmt( (const char *)pix_fmt );
                    }
                    else
                    {
                        char sample_fmt[64];
                        if( sscanf( buf, "Size=%d,Codec=%d,4CC=0x%x,Layout=0x%"SCNx64",Rate=%d,Format=%[^,],BPS=%d,Align=%d",
                                    &entry->extradata_size, &codec_id, &entry->codec_tag,
                                    &entry->channel_layout, &entry->sample_rate,
                                    sample_fmt, &entry->bits_per_sample, &entry->block_align ) != 8 )
                            break;
                        entry->sample_format = av_get_sample_fmt( (const char *)sample_fmt );
                    }
                    entry->codec_id = (enum AVCodecID)codec_id;
                    /* Get extradata. */
                    if( entry->extradata_size > 0 )
                    {
                        entry->extradata = (uint8_t *)av_malloc( entry->extradata_size + FF_INPUT_BUFFER_PADDING_SIZE );
                        if( !entry->extradata )
                            goto fail_parsing;
                        if( fread( entry->extradata, 1, entry->extradata_size, index ) != entry->extradata_size )
                        {
                            av_free( entry->extradata );
                            goto fail_parsing;
                        }
                        memset( entry->extradata + entry->extradata_size, 0, FF_INPUT_BUFFER_PADDING_SIZE );
                    }
                    if( !fgets( buf, sizeof(buf), index )   /* new line ('\n') */
                     || !fgets( buf, sizeof(buf), index ) ) /* the first line of the next entry */
                        goto fail_parsing;
                }
            }
            else
                for( int i = 0; i < entry_count; i++ )
                {
                    /* extradata size */
                    int extradata_size;
                    if( sscanf( buf, "Size=%d", &extradata_size ) != 1 )
                        goto fail_parsing;
                    /* extradata */
                    for( int i = 0; i < extradata_size; i++ )
                        if( fgetc( index ) == EOF )
                            goto fail_parsing;
                    if( !fgets( buf, sizeof(buf), index )   /* new line ('\n') */
                     || !fgets( buf, sizeof(buf), index ) ) /* the first line of the next entry */
                        goto fail_parsing;
                }
        }
        if( strncmp( buf, "</ExtraDataList>", strlen( "</ExtraDataList>" ) ) )
            goto fail_parsing;
        if( !fgets( buf, sizeof(buf), index ) )
            goto fail_parsing;
    }
    if( !strncmp( buf, "</LibavReaderIndexFile>", strlen( "</LibavReaderIndexFile>" ) ) )
    {
        if( vdhp->stream_index >= 0 )
        {
            vdhp->keyframe_list = (uint8_t *)lw_malloc_zero( (video_sample_count + 1) * sizeof(uint8_t) );
            if( !vdhp->keyframe_list )
                goto fail_parsing;
            vdhp->frame_list  = video_info;
            vdhp->frame_count = video_sample_count;
            if( decide_video_seek_method( lwhp, vdhp, video_sample_count, video_time_base ) )
                goto fail_parsing;
            /* Create the repeat control info. */
            create_video_frame_order_list( vdhp, vohp, opt );
        }
        if( adhp->stream_index >= 0 )
        {
            if( adhp->dv_in_avi == 1 && adhp->index_entries_count == 0 )
            {
                /* DV in AVI Type-1 */
                audio_sample_count = MIN( video_sample_count, audio_sample_count );
                for( uint32_t i = 0; i <= audio_sample_count; i++ )
                {
                    audio_info[i].keyframe        = !!(video_info[i].flags & LW_VFRAME_FLAG_KEY);
                    audio_info[i].sample_number   = video_info[i].sample_number;
                    audio_info[i].pts             = video_info[i].pts;
                    audio_info[i].dts             = video_info[i].dts;
                    audio_info[i].file_offset     = video_info[i].file_offset;
                    audio_info[i].extradata_index = video_info[i].extradata_index;
                }
            }
            else
            {
                if( adhp->dv_in_avi == 1 && ((!opt->force_video && active_video_index == -1) || (opt->force_video && opt->force_video_index == -1)) )
                {
                    /* Disable DV video stream. */
                    disable_video_stream( vdhp );
                    video_info = NULL;
                }
                adhp->dv_in_avi = 0;
            }
            adhp->frame_list   = audio_info;
            adhp->frame_count  = audio_sample_count;
            adhp->frame_length = constant_frame_length ? audio_info[1].length : 0;
            decide_audio_seek_method( lwhp, adhp, audio_sample_count );
            if( opt->av_sync && vdhp->stream_index >= 0 )
                lwhp->av_gap = calculate_av_gap( vdhp, vohp, adhp, video_time_base, audio_time_base, audio_sample_rate );
        }
        if( vdhp->stream_index != active_video_index || adhp->stream_index != active_audio_index )
        {
            /* Update the active stream indexes when specifying different stream indexes. */
            fseek( index, active_index_pos, SEEK_SET );
            fprintf( index, "<ActiveVideoStreamIndex>%+011d</ActiveVideoStreamIndex>\n", vdhp->stream_index );
            fprintf( index, "<ActiveAudioStreamIndex>%+011d</ActiveAudioStreamIndex>\n", adhp->stream_index );
        }
        return 0;
    }
fail_parsing:
    vdhp->frame_list = NULL;
    adhp->frame_list = NULL;
    if( video_info )
        free( video_info );
    if( audio_info )
        free( audio_info );
    return -1;
}

int lwlibav_construct_index
(
    lwlibav_file_handler_t         *lwhp,
    lwlibav_video_decode_handler_t *vdhp,
    lwlibav_video_output_handler_t *vohp,
    lwlibav_audio_decode_handler_t *adhp,
    lwlibav_audio_output_handler_t *aohp,
    lw_log_handler_t               *lhp,
    lwlibav_option_t               *opt,
    progress_indicator_t           *indicator,
    progress_handler_t             *php
)
{
    /* Allocate frame buffer. */
    vdhp->frame_buffer = av_frame_alloc();
    if( !vdhp->frame_buffer )
        return -1;
    adhp->frame_buffer = av_frame_alloc();
    if( !adhp->frame_buffer )
    {
        av_frame_free( &vdhp->frame_buffer );
        return -1;
    }
    /* Try to open the index file. */
    int file_path_length = strlen( opt->file_path );
    char *index_file_path = (char *)lw_malloc_zero(file_path_length + 5);
    if( !index_file_path )
    {
        av_frame_free( &vdhp->frame_buffer );
        av_frame_free( &adhp->frame_buffer );
        return -1;
    }
    memcpy( index_file_path, opt->file_path, file_path_length );
    const char *ext = file_path_length >= 5 ? &opt->file_path[file_path_length - 4] : NULL;
    if( ext && !strncmp( ext, ".lwi", strlen( ".lwi" ) ) )
        index_file_path[file_path_length] = '\0';
    else
    {
        memcpy( index_file_path + file_path_length, ".lwi", strlen( ".lwi" ) );
        index_file_path[file_path_length + 4] = '\0';
    }
    FILE *index = fopen( index_file_path, (opt->force_video || opt->force_audio) ? "r+b" : "rb" );
    free( index_file_path );
    if( index )
    {
        int version = 0;
        int ret = fscanf( index, "<LibavReaderIndexFile=%d>\n", &version );
        if( ret == 1
         && version == INDEX_FILE_VERSION
         && parse_index( lwhp, vdhp, vohp, adhp, aohp, opt, index ) == 0 )
        {
            /* Opening and parsing the index file succeeded. */
            fclose( index );
            av_register_all();
            avcodec_register_all();
            lwhp->threads = opt->threads;
            return 0;
        }
        fclose( index );
    }
    /* Open file. */
    if( !lwhp->file_path )
    {
        lwhp->file_path = (char *)lw_malloc_zero( file_path_length + 1 );
        if( !lwhp->file_path )
            goto fail;
        memcpy( lwhp->file_path, opt->file_path, file_path_length );
    }
    av_register_all();
    avcodec_register_all();
    AVFormatContext *format_ctx = NULL;
    if( lavf_open_file( &format_ctx, opt->file_path, lhp ) )
    {
        if( format_ctx )
            lavf_close_file( &format_ctx );
        goto fail;
    }
    lwhp->threads      = opt->threads;
    vdhp->stream_index = -1;
    adhp->stream_index = -1;
    /* Create the index file. */
    create_index( lwhp, vdhp, vohp, adhp, aohp, format_ctx, opt, indicator, php );
    /* Close file.
     * By opening file for video and audio separately, indecent work about frame reading can be avoidable. */
    lavf_close_file( &format_ctx );
    vdhp->ctx = NULL;
    adhp->ctx = NULL;
    return 0;
fail:
    if( vdhp->frame_buffer )
        av_frame_free( &vdhp->frame_buffer );
    if( adhp->frame_buffer )
        av_frame_free( &adhp->frame_buffer );
    if( lwhp->file_path )
        lw_freep( &lwhp->file_path );
    return -1;
}

int lwlibav_import_av_index_entry
(
    lwlibav_decode_handler_t *dhp
)
{
    if( dhp->index_entries )
    {
        AVStream *stream = dhp->format->streams[ dhp->stream_index ];
        for( int i = 0; i < dhp->index_entries_count; i++ )
        {
            AVIndexEntry *ie = &dhp->index_entries[i];
            if( av_add_index_entry( stream, ie->pos, ie->timestamp, ie->size, ie->min_distance, ie->flags ) < 0 )
            {
                if( dhp->lh.show_log )
                    dhp->lh.show_log( &dhp->lh, LW_LOG_FATAL, "Failed to import AVIndexEntrys." );
                return -1;
            }
        }
        av_freep( &dhp->index_entries );
    }
    return 0;
}
