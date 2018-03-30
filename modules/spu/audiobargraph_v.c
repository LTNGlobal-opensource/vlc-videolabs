/*****************************************************************************
 * audiobargraph_v.c : audiobargraph video plugin for vlc
 *****************************************************************************
 * Copyright (C) 2003-2006 VLC authors and VideoLAN
 *
 * Authors: Clement CHESNIN <clement.chesnin@gmail.com>
 *          Philippe COENT <philippe.coent@tdf.fr>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif
#include <string.h>
#include <math.h>
#include <assert.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_filter.h>
#include <vlc_subpicture.h>
#include <vlc_image.h>
#include <vlc_memstream.h>

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/

#define POSX_TEXT N_("X coordinate")
#define POSX_LONGTEXT N_("X coordinate of the bargraph.")
#define POSY_TEXT N_("Y coordinate")
#define POSY_LONGTEXT N_("Y coordinate of the bargraph.")
#define TRANS_TEXT N_("Transparency of the bargraph")
#define TRANS_LONGTEXT N_("Bargraph transparency value " \
  "(from 0 for full transparency to 255 for full opacity).")
#define POS_TEXT N_("Bargraph position")
#define POS_LONGTEXT N_(\
  "Enforce the bargraph position on the video " \
  "(0=center, 1=left, 2=right, 4=top, 8=bottom, you can " \
  "also use combinations of these values, eg 6 = top-right).")
#define BARWIDTH_TEXT N_("Bar width in pixel")
#define BARWIDTH_LONGTEXT N_("Width in pixel of each bar in the BarGraph to be displayed." )
#define BARHEIGHT_TEXT N_("Bar Height in pixel")
#define BARHEIGHT_LONGTEXT N_("Height in pixel of BarGraph to be displayed." )

#define CFG_PREFIX "audiobargraph_v-"

static const int pi_pos_values[] = { 0, 1, 2, 4, 8, 5, 6, 9, 10 };
static const char *const ppsz_pos_descriptions[] =
{ N_("Center"), N_("Left"), N_("Right"), N_("Top"), N_("Bottom"),
  N_("Top-Left"), N_("Top-Right"), N_("Bottom-Left"), N_("Bottom-Right") };

static int  OpenSub  (vlc_object_t *);
static void Close    (vlc_object_t *);

vlc_module_begin ()

    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_SUBPIC)

    set_capability("sub source", 0)
    set_callbacks(OpenSub, Close)
    set_description(N_("Audio Bar Graph Video sub source"))
    set_shortname(N_("Audio Bar Graph Video"))
    add_shortcut("audiobargraph_v")

    add_obsolete_string(CFG_PREFIX "i_values")
    add_integer(CFG_PREFIX "x", 0, POSX_TEXT, POSX_LONGTEXT, true)
    add_integer(CFG_PREFIX "y", 0, POSY_TEXT, POSY_LONGTEXT, true)
    add_integer_with_range(CFG_PREFIX "transparency", 255, 0, 255,
        TRANS_TEXT, TRANS_LONGTEXT, false)
    add_integer(CFG_PREFIX "position", -1, POS_TEXT, POS_LONGTEXT, false)
        change_integer_list(pi_pos_values, ppsz_pos_descriptions)
    add_obsolete_integer(CFG_PREFIX "alarm")
    add_integer(CFG_PREFIX "barWidth", 30, BARWIDTH_TEXT, BARWIDTH_LONGTEXT, true)
    add_integer(CFG_PREFIX "barHeight", 300, BARHEIGHT_TEXT, BARHEIGHT_LONGTEXT, true)

vlc_module_end ()


/*****************************************************************************
 * Local prototypes
 *****************************************************************************/

/*****************************************************************************
 * Structure to hold the Bar Graph properties
 ****************************************************************************/

typedef struct  {
    char* psz_stream_name;
    int i_stream_id;
    int i_nb_channels;
    float channels_peaks[AOUT_CHAN_MAX];
} bargraph_data_t;

typedef struct {
    vlc_mutex_t mutex;
    int i_count_channels;
    int i_streams;
    int i_refcount;
    bargraph_data_t** p_streams;
} shared_bargraph_data_t;



typedef struct
{
    int i_alpha;       /* -1 means use default alpha */
    shared_bargraph_data_t *p_data;
    picture_t *p_pic;
    mtime_t date;
    int barHeight;
    bool alarm;
    int barWidth;

} BarGraph_t;

static void shared_bargraph_data_unref( shared_bargraph_data_t* p_shared_data )
{
    if (!p_shared_data)
        return;
    vlc_mutex_lock(&p_shared_data->mutex);
    assert(p_shared_data->i_refcount >= 1);
    p_shared_data->i_refcount--;
    if (p_shared_data->i_refcount == 0)
    {
        TAB_CLEAN( p_shared_data->i_streams, p_shared_data->p_streams );
        vlc_mutex_unlock(&p_shared_data->mutex);
        free( p_shared_data );
    }
    else
        vlc_mutex_unlock(&p_shared_data->mutex);
}

static void shared_bargraph_data_ref( shared_bargraph_data_t* p_shared_data )
{
    vlc_mutex_lock(&p_shared_data->mutex);
    assert(p_shared_data->i_refcount >= 1);
    p_shared_data->i_refcount++;
    vlc_mutex_unlock(&p_shared_data->mutex);
}

/**
 * Private data holder
 */
struct filter_sys_t
{
    vlc_mutex_t lock;

    BarGraph_t p_BarGraph;

    int i_pos;
    int i_pos_x;
    int i_pos_y;
    bool b_absolute;

    /* On the fly control variable */
    bool b_spu_update;
};

static const char *const ppsz_filter_options[] = {
    "x", "y", "transparency", "position", "barWidth", "barHeight", NULL
};

static const char *const ppsz_filter_callbacks[] = {
    "audiobargraph_v-x",
    "audiobargraph_v-y",
    "audiobargraph_v-transparency",
    "audiobargraph_v-position",
    "audiobargraph_v-barWidth",
    "audiobargraph_v-barHeight",
    NULL
};

/*****************************************************************************
 * IEC 268-18  Source: meterbridge
 *****************************************************************************/
static float iec_scale(float dB)
{
    if (dB < -70.0f)
        return 0.0f;
    if (dB < -60.0f)
        return (dB + 70.0f) * 0.0025f;
    if (dB < -50.0f)
        return (dB + 60.0f) * 0.005f + 0.025f;
    if (dB < -40.0f)
        return (dB + 50.0f) * 0.0075f + 0.075f;
    if (dB < -30.0f)
        return (dB + 40.0f) * 0.015f + 0.15f;
    if (dB < -20.0f)
        return (dB + 30.0f) * 0.02f + 0.3f;
    if (dB < -0.001f || dB > 0.001f)  /* if (dB < 0.0f) */
        return (dB + 20.0f) * 0.025f + 0.5f;
    return 1.0f;
}

/* Drawing */

static const uint8_t bright_red[4]   = { 76, 85, 0xff, 0xff };
static const uint8_t black[4] = { 0x00, 0x80, 0x80, 0xff };
static const uint8_t white[4] = { 0xff, 0x80, 0x80, 0xff };
static const uint8_t bright_green[4] = { 150, 44, 21, 0xff };
static const uint8_t bright_yellow[4] = { 226, 1, 148, 0xff };
static const uint8_t green[4] = { 74, 85, 74, 0xff };
static const uint8_t yellow[4] = { 112, 64, 138, 0xff };
static const uint8_t red[4] = { 37, 106, 191, 0xff };

static inline void DrawHLine(plane_t *p, int line, int col, const uint8_t color[4], int w)
{
    for (int j = 0; j < 4; j++)
        memset(&p[j].p_pixels[line * p[j].i_pitch + col], color[j], w);
}

static void Draw2VLines(plane_t *p, int barheight, int col, const uint8_t color[4])
{
    for (int i = 10; i < barheight + 10; i++)
        DrawHLine(p, i, col, color, 2);
}

static void DrawHLines(plane_t *p, int line, int col, const uint8_t color[4], int h, int w)
{
    for (int i = line; i < line + h; i++)
        DrawHLine(p, i, col, color, w);
}

/*****************************************************************************
 * Draw: creates and returns the bar graph image
 *****************************************************************************/
static void Draw(BarGraph_t *b)
{
    int barHeight      = b->barHeight;
    int barWidth   = b->barWidth;

    shared_bargraph_data_t *p_values  = b->p_data;
    if (p_values == NULL)
        return;

    int level[6];
    for (int i = 0; i < 6; i++)
        level[i] = iec_scale(-(i+1) * 10) * barHeight + 20;

    if (b->p_pic)
        picture_Release(b->p_pic);

    vlc_mutex_lock(&p_values->mutex);
    int w = barWidth;
    for (int i_stream = 0; i_stream < p_values->i_streams; i_stream++ )
    {
        w += (p_values->p_streams[i_stream]->i_nb_channels + 1) * (5 + barWidth);
    }
    int h = barHeight + 30;

    b->p_pic = picture_New(VLC_FOURCC('Y','U','V','A'), w, h, 1, 1);
    if (!b->p_pic)
        goto end;
    picture_t *p_pic = b->p_pic;
    plane_t *p = p_pic->p;

    for (int i = 0 ; i < p_pic->i_planes ; i++)
        memset(p[i].p_pixels, 0x00, p[i].i_visible_lines * p[i].i_pitch);

    Draw2VLines(p, barHeight, barWidth - 10, black);
    Draw2VLines(p, barHeight, barWidth - 8, white);

    for (int i = 0; i < 6; i++) {
        DrawHLines(p, h - 1 - level[i] - 1, barWidth - 6, white, 1, 3);
        DrawHLines(p, h - 1 - level[i],     barWidth - 6, black, 2, 3);
    }

    int minus8  = iec_scale(- 8) * barHeight + 20;
    int minus18 = iec_scale(-18) * barHeight + 20;

    const uint8_t *indicator_color = b->alarm ? bright_red : black;

    int pi = barWidth;
    for (int i_stream = 0; i_stream < p_values->i_streams; i_stream++ )
    {
        bargraph_data_t* p_data = p_values->p_streams[i_stream];
        for (int i = 0; i < p_data->i_nb_channels; i++) {
            DrawHLines(p, h - 20 - 1, pi, indicator_color, 8, barWidth);
            float db = log10(p_data->channels_peaks[i]) * 20;
            db = VLC_CLIP(iec_scale(db)*b->barHeight, 0, b->barHeight);

            for (int line = 20; line < db + 20; line++) {
                if (line < minus18)
                    DrawHLines(p, h - line - 1, pi, bright_green, 1, barWidth);
                else if (line < minus8)
                    DrawHLines(p, h - line - 1, pi, bright_yellow, 1, barWidth);
                else
                    DrawHLines(p, h - line - 1, pi, bright_red, 1, barWidth);
            }

            for (int line = db + 20; line < barHeight + 20; line++) {
                if (line < minus18)
                    DrawHLines(p, h - line - 1, pi, green, 1, barWidth);
                else if (line < minus8)
                    DrawHLines(p, h - line - 1, pi, yellow, 1, barWidth);
                else
                    DrawHLines(p, h - line - 1, pi, red, 1, barWidth);
            }
            pi += 5 + barWidth;
        }
        pi += 5 + barWidth;
    }
end:
    vlc_mutex_unlock(&p_values->mutex);
}

/*****************************************************************************
 * Callback to update params on the fly
 *****************************************************************************/
static int BarGraphCallback(vlc_object_t *p_this, char const *psz_var,
                         vlc_value_t oldval, vlc_value_t newval, void *p_data)
{
    VLC_UNUSED(p_this); VLC_UNUSED(oldval);
    filter_sys_t *p_sys = p_data;
    BarGraph_t *p_BarGraph = &p_sys->p_BarGraph;

    vlc_mutex_lock(&p_sys->lock);
    if (!strcmp(psz_var, CFG_PREFIX "x"))
        p_sys->i_pos_x = newval.i_int;
    else if (!strcmp(psz_var, CFG_PREFIX "y"))
        p_sys->i_pos_y = newval.i_int;
    else if (!strcmp(psz_var, CFG_PREFIX "position"))
        p_sys->i_pos = newval.i_int;
    else if (!strcmp(psz_var, CFG_PREFIX "transparency"))
        p_BarGraph->i_alpha = VLC_CLIP(newval.i_int, 0, 255);
    else if (!strcmp(psz_var, CFG_PREFIX "i_values")) {
        shared_bargraph_data_t* p_shared_bargraph =  (shared_bargraph_data_t*)newval.p_address;
        if (!p_BarGraph->p_data && p_shared_bargraph)
        {
            shared_bargraph_data_unref(p_BarGraph->p_data);
            shared_bargraph_data_ref(p_shared_bargraph);
        }
        p_BarGraph->p_data = p_shared_bargraph;
    } else if (!strcmp(psz_var, CFG_PREFIX "alarm")) {
        p_BarGraph->alarm = newval.b_bool;
    } else if (!strcmp(psz_var, CFG_PREFIX "barWidth")) {
        p_BarGraph->barWidth = newval.i_int;
    } else if (!strcmp(psz_var, CFG_PREFIX "barHeight")) {
        p_BarGraph->barHeight = newval.i_int;
    }
    p_sys->b_spu_update = true;
    vlc_mutex_unlock(&p_sys->lock);

    return VLC_SUCCESS;
}


static char *DrawBargraph(BarGraph_t *p_BarGraph, int* pi_width, int* pi_height )
{
    shared_bargraph_data_t *p_values = p_BarGraph->p_data;
    vlc_mutex_lock( &p_values->mutex );

    const int i_height= p_BarGraph->barHeight;
    const int i_barWidth = p_BarGraph->barWidth;
    static const int i_sep = 5;
    static const int i_top = 10;
    const int i_bottom = i_height - 10;

    struct vlc_memstream svgtxt;
    if (vlc_memstream_open(&svgtxt) != 0)
        goto exit;

    int i_width = i_barWidth;
    for (int i_stream = 0; i_stream < p_values->i_streams; i_stream++ )
    {
        bargraph_data_t* p_data = p_values->p_streams[i_stream];
        i_width += p_data->i_nb_channels * (i_barWidth + i_sep) + i_barWidth;
    }

    if (pi_width)
        *pi_width = i_width;
    if (pi_height)
        *pi_height = i_height;

    vlc_memstream_printf(&svgtxt,
                         "<svg "
                         "viewBox=\"0 0 %i %i\" "
                         "width=\"%i\" height=\"%i\" "
                         "xmlns=\"http://www.w3.org/2000/svg\">",
                         i_width, i_height, i_width, i_height);

    float scale = i_bottom - i_top;

    int minus8  = scale - iec_scale(- 8) * scale;
    int minus18 = scale - iec_scale(-18) * scale;

    //draw scale
    int i_scale_x_pos = i_barWidth*0.8;
    vlc_memstream_printf(&svgtxt, \
        "<line x1=\"%i\" y1=\"%i\" x2=\"%i\" y2=\"%i\" style=\"stroke:#000000;stroke-width:2\" />",
        i_scale_x_pos , i_top, i_scale_x_pos, (int)(i_top + scale));
    vlc_memstream_printf(&svgtxt, \
        "<line x1=\"%i\" y1=\"%i\" x2=\"%i\" y2=\"%i\" style=\"stroke:#FFFFFF;stroke-width:2\" />",
        i_scale_x_pos + 2, i_top, i_scale_x_pos + 2, (int)(i_top + scale) );

    const char* text[] = {"10", "20", "30", "40", "50", "60"};
    for (int i = 0; i < 6; i++) {
        int level = scale - iec_scale(-(i+1) * 10) * scale;
        vlc_memstream_printf(&svgtxt,
            "<line x1=\"%i\" y1=\"%i\" x2=\"%i\" y2=\"%i\" style=\"stroke:#000000;stroke-width:2\" />",
            i_scale_x_pos, i_top + level -  1, i_barWidth-2, i_top + level - 1);
        vlc_memstream_printf(&svgtxt,
            "<line x1=\"%i\" y1=\"%i\" x2=\"%i\" y2=\"%i\" style=\"stroke:#FFFFFF;stroke-width:2\" />",
            i_scale_x_pos+2, i_top + level + 1, i_barWidth-2, i_top + level +1);
        vlc_memstream_printf(&svgtxt,
            "<text x=\"%i\" y=\"%i\" style=\"font-family: Arial; stroke: #FFFFFF; fill: #000000; stroke-width: 1; font-size: %ipx;\">%s</text>",
            0, i_top + level, (int)(3 * sqrt(i_barWidth)), text[i]);
    }

#define DRAW_LEVEL(db, lvlmin, lvlmax, COLOR_BG, COLOR_FG) \
            if (lvlmin <= db) { \
                vlc_memstream_printf(&svgtxt, \
                    "<rect x=\"%i\" y=\"%i\" width=\"%i\" height=\"%i\" style=\"fill: " COLOR_BG "\"/>", \
                    i_x, (int)(i_top + lvlmax), i_barWidth, (int)(lvlmin - lvlmax) ); \
            } else if ( db <= lvlmax ) { \
                vlc_memstream_printf(&svgtxt, \
                    "<rect x=\"%i\" y=\"%i\" width=\"%i\" height=\"%i\" style=\"fill: " COLOR_FG "\"/>", \
                    i_x, (int)(i_top + lvlmax), i_barWidth, (int)(lvlmin - lvlmax) ); \
            } else  { \
                vlc_memstream_printf(&svgtxt, \
                    "<rect x=\"%i\" y=\"%i\" width=\"%i\" height=\"%i\" style=\"fill: " COLOR_BG "\"/>", \
                    i_x, (int)(i_top + lvlmax), i_barWidth, (int)(db - lvlmax) ); \
                vlc_memstream_printf(&svgtxt, \
                    "<rect x=\"%i\" y=\"%i\" width=\"%i\" height=\"%i\" style=\"fill: " COLOR_FG "\"/>", \
                    i_x, (int)(i_top + db ), i_barWidth, (int)(lvlmin - db) ); \
            }


    int i_x = i_barWidth;
    for (int i_stream = 0; i_stream < p_values->i_streams; i_stream++ )
    {
        bargraph_data_t* p_data = p_values->p_streams[i_stream];
        int nbChannels = p_data->i_nb_channels;
        for (int i = 0; i < nbChannels; i++)
        {
            float db = log10(p_data->channels_peaks[i]) * 20;
            db = scale - VLC_CLIP(iec_scale(db)*scale, 0, scale);

            DRAW_LEVEL(((int)db), minus8, 0 , "#7D0000", "#FF0000");
            DRAW_LEVEL(((int)db), minus18 , minus8, "#808000", "#FFFF00");
            DRAW_LEVEL(((int)db), ((int)scale), minus18, "#008000", "#00FF00");
            vlc_memstream_printf(&svgtxt, \
                "<rect x=\"%i\" y=\"%i\" width=\"%i\" height=\"%i\" style=\"fill: #000000 \"/>", \
                    i_x, (int)i_bottom , i_barWidth, 10 ); \

            i_x += i_barWidth + i_sep;
        }
        vlc_memstream_printf(&svgtxt, \
            "<text x=\"%i\" y=\"%i\" style=\"text-anchor: middle; font-family: Arial; stroke: #FFFFFF; fill: #000000; stroke-width: 1; font-size: %ipx; writing-mode: tb;\">%s</text>",
            i_x + i_barWidth / 2 , (int)(scale / 2), (int)(5 * sqrt(i_barWidth)), p_data->psz_stream_name);
        i_x += i_barWidth;
    }
#undef DRAW_LEVEL
    vlc_memstream_puts(&svgtxt, "</svg>");

exit:
    vlc_mutex_unlock( &p_values->mutex );

    if (vlc_memstream_close(&svgtxt) != 0)
        return NULL;

    return svgtxt.ptr;
}

/**
 * Sub source
 */
static subpicture_t *FilterSub(filter_t *p_filter, mtime_t date)
{
    filter_sys_t *p_sys = p_filter->p_sys;
    BarGraph_t *p_BarGraph = &(p_sys->p_BarGraph);

    subpicture_t *p_spu;
    subpicture_region_t *p_region;
    video_format_t fmt;
    picture_t *p_pic;

    vlc_mutex_lock(&p_sys->lock);
    /* Basic test:  b_spu_update occurs on a dynamic change */
    if (!p_sys->b_spu_update || p_sys->p_BarGraph.p_data == NULL) {
        vlc_mutex_unlock(&p_sys->lock);
        return NULL;
    }

    Draw(p_BarGraph);
    p_pic = p_BarGraph->p_pic;

    /* Allocate the subpicture internal data. */
    p_spu = filter_NewSubpicture(p_filter);
    if (!p_spu)
        goto exit;

    p_spu->b_absolute = p_sys->b_absolute;
    p_spu->i_start = date;
    p_spu->i_stop = 0;
    p_spu->b_ephemer = true;

    /* Send an empty subpicture to clear the display when needed */
    if (!p_pic || !p_BarGraph->i_alpha)
        goto exit;

    /* Create new SPU region */
    memset(&fmt, 0, sizeof(video_format_t));
    fmt.i_chroma = VLC_CODEC_YUVA;
    fmt.i_sar_num = fmt.i_sar_den = 1;
    fmt.i_width = fmt.i_visible_width = p_pic->p[Y_PLANE].i_visible_pitch;
    fmt.i_height = fmt.i_visible_height = p_pic->p[Y_PLANE].i_visible_lines;
    fmt.i_x_offset = fmt.i_y_offset = 0;
    p_region = subpicture_region_New(&fmt);
    if (!p_region) {
        msg_Err(p_filter, "cannot allocate SPU region");
        subpicture_Delete(p_spu);
        p_spu = NULL;
        goto exit;
    }

    picture_Copy(p_region->p_picture, p_pic);

    /*  where to locate the bar graph: */
    if (p_sys->i_pos < 0) {   /*  set to an absolute xy */
        p_region->i_align = SUBPICTURE_ALIGN_RIGHT | SUBPICTURE_ALIGN_TOP;
        p_spu->b_absolute = true;
    } else {   /* set to one of the 9 relative locations */
        p_region->i_align = p_sys->i_pos;
        p_spu->b_absolute = false;
    }

    p_region->i_x = p_sys->i_pos_x;
    p_region->i_y = p_sys->i_pos_y;

    p_spu->p_region = p_region;

    fmt.i_chroma = VLC_CODEC_TEXT;

    const char* text[] = {"10", "20", "30", "40", "50", "60"};
    text_style_t* style = text_style_New();
    style->i_font_size = 3 * sqrt(p_BarGraph->barWidth);

    subpicture_region_t* p_current_region = p_region;
    for (int i = 0; i < 6; ++i)
    {
        int level = iec_scale(-(i+1) * 10) * p_BarGraph->barHeight + 20;
        subpicture_region_t* spu_txt = subpicture_region_New(&fmt);
        spu_txt->i_x = 0;
        spu_txt->i_y = fmt.i_height - level - 4;
        spu_txt->p_text = text_segment_New(text[i]);
        spu_txt->p_text->style = text_style_Duplicate(style);
        p_current_region->p_next = spu_txt;
        p_current_region = spu_txt;
    }

    vlc_mutex_lock( &p_BarGraph->p_data->mutex );
    int i_x = p_BarGraph->barWidth;
    for (int i_stream = 0; i_stream < p_BarGraph->p_data->i_streams; i_stream++ )
    {
        bargraph_data_t* p_stream =  p_BarGraph->p_data->p_streams[i_stream];
        const char* txt = p_stream->psz_stream_name;
        subpicture_region_t* spu_txt = subpicture_region_New(&fmt);
        spu_txt->i_x = i_x;
        spu_txt->i_y = p_BarGraph->barHeight + 20;
        spu_txt->p_text = text_segment_New(txt);
        spu_txt->p_text->style = text_style_Duplicate(style);

        p_current_region->p_next = spu_txt;
        p_current_region = spu_txt;
        i_x += ((p_BarGraph->barWidth + 5 ) * (p_stream->i_nb_channels + 1)) ;
    }
    vlc_mutex_unlock( &p_BarGraph->p_data->mutex );

    text_style_Delete(style);
    p_spu->i_alpha = p_BarGraph->i_alpha ;
exit:
    vlc_mutex_unlock(&p_sys->lock);

    return p_spu;
}

/**
 *  open function
 */
static int OpenSub(vlc_object_t *p_this)
{
    filter_t *p_filter = (filter_t *)p_this;
    filter_sys_t *p_sys;

    /* */
    p_filter->p_sys = p_sys = malloc(sizeof(*p_sys));
    if (!p_sys)
        return VLC_ENOMEM;

    /* */
    config_ChainParse(p_filter, CFG_PREFIX, ppsz_filter_options,
                       p_filter->p_cfg);

    /* create and initialize variables */
    p_sys->i_pos = var_CreateGetInteger(p_filter, CFG_PREFIX "position");
    p_sys->i_pos_x = var_CreateGetInteger(p_filter, CFG_PREFIX "x");
    p_sys->i_pos_y = var_CreateGetInteger(p_filter, CFG_PREFIX "y");
    BarGraph_t *p_BarGraph = &p_sys->p_BarGraph;
    p_BarGraph->p_pic = NULL;
    p_BarGraph->i_alpha = var_CreateGetInteger(p_filter, CFG_PREFIX "transparency");
    p_BarGraph->i_alpha = VLC_CLIP(p_BarGraph->i_alpha, 0, 255);
    p_BarGraph->p_data = NULL;
    p_BarGraph->alarm = false;

    p_BarGraph->barWidth = var_CreateGetInteger(p_filter, CFG_PREFIX "barWidth");
    p_BarGraph->barHeight = var_CreateGetInteger( p_filter, CFG_PREFIX "barHeight");

    vlc_mutex_init(&p_sys->lock);
    var_Create(p_filter->obj.libvlc, CFG_PREFIX "alarm", VLC_VAR_BOOL);
    var_Create(p_filter->obj.libvlc, CFG_PREFIX "i_values", VLC_VAR_ADDRESS);

    var_AddCallback(p_filter->obj.libvlc, CFG_PREFIX "alarm",
                    BarGraphCallback, p_sys);
    var_AddCallback(p_filter->obj.libvlc, CFG_PREFIX "i_values",
                    BarGraphCallback, p_sys);

    var_TriggerCallback(p_filter->obj.libvlc, CFG_PREFIX "alarm");
    var_TriggerCallback(p_filter->obj.libvlc, CFG_PREFIX "i_values");

    for (int i = 0; ppsz_filter_callbacks[i]; i++)
        var_AddCallback(p_filter, ppsz_filter_callbacks[i],
                         BarGraphCallback, p_sys);

    p_filter->pf_sub_source = FilterSub;

    return VLC_SUCCESS;
}

/**
 * Common close function
 */
static void Close(vlc_object_t *p_this)
{
    filter_t *p_filter = (filter_t *)p_this;
    filter_sys_t *p_sys = p_filter->p_sys;

    for (int i = 0; ppsz_filter_callbacks[i]; i++)
        var_DelCallback(p_filter, ppsz_filter_callbacks[i],
                         BarGraphCallback, p_sys);

    var_DelCallback(p_filter->obj.libvlc, CFG_PREFIX "i_values",
                    BarGraphCallback, p_sys);
    var_DelCallback(p_filter->obj.libvlc, CFG_PREFIX "alarm",
                    BarGraphCallback, p_sys);
    var_Destroy(p_filter->obj.libvlc, CFG_PREFIX "i_values");
    var_Destroy(p_filter->obj.libvlc, CFG_PREFIX "alarm");

    if (p_sys->p_BarGraph.p_data)
        shared_bargraph_data_unref(p_sys->p_BarGraph.p_data);

    vlc_mutex_destroy(&p_sys->lock);

    if (p_sys->p_BarGraph.p_pic)
        picture_Release(p_sys->p_BarGraph.p_pic);

    free(p_sys);
}
