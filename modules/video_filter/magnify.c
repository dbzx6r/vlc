/*****************************************************************************
 * magnify.c : Magnify/Zoom interactive effect
 *****************************************************************************
 * Copyright (C) 2005-2009 VLC authors and VideoLAN
 * $Id$
 *
 * Authors: Antoine Cellerier <dionoea -at- videolan -dot- org>
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
#include <math.h>
#include <assert.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_image.h>
#include <vlc_filter.h>
#include <vlc_mouse.h>
#include <vlc_picture.h>
#include "filter_picture.h"

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int  Create    ( vlc_object_t * );
static void Destroy   ( vlc_object_t * );

vlc_module_begin ()
    set_description( N_("Magnify/Zoom interactive video filter") )
    set_shortname( N_( "Magnify" ))
    set_capability( "video filter", 0 )
    set_category( CAT_VIDEO )
    set_subcategory( SUBCAT_VIDEO_VFILTER )

    set_callbacks( Create, Destroy )
vlc_module_end ()


/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static picture_t *Filter( filter_t *, picture_t * );
static int Mouse( filter_t *, vlc_mouse_t *, const vlc_mouse_t *, const vlc_mouse_t * );

/* */
static void DrawZoomStatus( plane_t *, video_orientation_t,
                            int scale, int top, bool b_visible );
static void DrawRectangle( uint8_t *, int i_pitch, int i_width, int i_height,
                           int x, int y, int i_w, int i_h );

/* */
struct filter_sys_t
{
    image_handler_t *p_image;

    int64_t i_hide_timeout;

    int i_zoom; /* zoom level in percent */
    int i_x, i_y; /* top left corner coordinates in original image */

    bool b_visible; /* is "interface" visible ? */

    int64_t i_last_activity;
};

#define VIS_ZOOM 4
#define ZOOM_FACTOR 8

/* VLC orientation bits describe source-axis reflections followed by a transpose.
 * Work in visual coordinates for the controls, and stored coordinates for the crop. */
static bool ReverseX(video_orientation_t o)
{
    return o == ORIENT_HFLIPPED || o == ORIENT_ROTATED_180 ||
           o == ORIENT_ROTATED_270 || o == ORIENT_ANTI_TRANSPOSED;
}

static bool ReverseY(video_orientation_t o)
{
    return o == ORIENT_VFLIPPED || o == ORIENT_ROTATED_180 ||
           o == ORIENT_ROTATED_90 || o == ORIENT_ANTI_TRANSPOSED;
}

static void Plot(plane_t *p, video_orientation_t o, int x, int y, uint8_t value)
{
    if (ORIENT_IS_SWAP(o)) { int tmp = x; x = y; y = tmp; }
    if (x < 0 || x >= p->i_visible_pitch || y < 0 || y >= p->i_visible_lines)
        return;
    if (ReverseX(o)) x = p->i_visible_pitch - 1 - x;
    if (ReverseY(o)) y = p->i_visible_lines - 1 - y;
    p->p_pixels[y * (ptrdiff_t)p->i_pitch + x] = value;
}

static int PreviewSize(int size)
{
    return (size / VIS_ZOOM) & ~1;
}

/* Keep stock proportions for a 720-line picture. For larger pictures
 * scale with the upright height, just like the quarter-frame preview. */
#define UI_BASE 720
static int ControlScale(int width, int height)
{
    return __MAX(UI_BASE, __MIN(height, (int64_t)width * UI_BASE / 80));
}

static int CellEdge(int n, int scale)
{
    return ((int64_t)n * scale + UI_BASE - 1) / UI_BASE;
}

static void PlotCell(plane_t *plane, video_orientation_t orient, int scale,
                     int top, int x, int y, uint8_t value)
{
    int right = CellEdge(x + 1, scale), bottom = CellEdge(y + 1, scale);
    for (int py = CellEdge(y, scale); py < bottom; ++py)
        for (int px = CellEdge(x, scale); px < right; ++px)
            Plot(plane, orient, px, top + py, value);
}

static int GaugeWidth(int row)
{
    return __MAX(1, (80 - row) * (80 - row) / 160);
}

static int GaugeZoom(int row)
{
    return __MAX(ZOOM_FACTOR, (73 - row) * ZOOM_FACTOR / 10);
}

/*****************************************************************************
 * Create:
 *****************************************************************************/
static int Create( vlc_object_t *p_this )
{
    filter_t *p_filter = (filter_t *)p_this;
    filter_sys_t *p_sys;

    /* */
    switch( p_filter->fmt_in.i_codec )
    {
    CASE_PLANAR_YUV
    case VLC_CODEC_GREY:
        break;
    default:
        msg_Err( p_filter, "Unsupported chroma %4.4s", (char *)&p_filter->fmt_in.i_codec );
        return VLC_EGENERIC;
    }
    if( !es_format_IsSimilar( &p_filter->fmt_in, &p_filter->fmt_out ) )
    {
        msg_Err( p_filter, "Input and output format does not match" );
        return VLC_EGENERIC;
    }

    /* Allocate structure */
    p_filter->p_sys = p_sys = malloc( sizeof( *p_sys ) );
    if( !p_filter->p_sys )
        return VLC_ENOMEM;

    p_sys->p_image = image_HandlerCreate( p_filter );
    if( !p_sys->p_image )
    {
        free( p_sys );
        return VLC_EGENERIC;
    }

    p_sys->i_x = 0;
    p_sys->i_y = 0;
    p_sys->i_zoom = 2*ZOOM_FACTOR;
    p_sys->b_visible = true;
    p_sys->i_last_activity = mdate();
    p_sys->i_hide_timeout = 1000 * var_InheritInteger( p_filter, "mouse-hide-timeout" );

    /* */
    p_filter->pf_video_filter = Filter;
    p_filter->pf_video_mouse = Mouse;
    return VLC_SUCCESS;
}

/*****************************************************************************
 * Destroy:
 *****************************************************************************/
static void Destroy( vlc_object_t *p_this )
{
    filter_t *p_filter = (filter_t *)p_this;
    filter_sys_t *p_sys = p_filter->p_sys;

    image_HandlerDelete( p_sys->p_image );

    free( p_sys );
}

/*****************************************************************************
 * Render: displays previously rendered output
 *****************************************************************************/
static picture_t *Filter( filter_t *p_filter, picture_t *p_pic )
{
    filter_sys_t *p_sys = p_filter->p_sys;
    picture_t *p_outpic;

    int v_w, v_h;
    picture_t *p_converted;
    plane_t *p_oyp;

    p_outpic = filter_NewPicture( p_filter );
    if( !p_outpic )
    {
        picture_Release( p_pic );
        return NULL;
    }

    /* */
    const bool b_visible = p_sys->b_visible;
    const int o_x = p_sys->i_x;
    const int o_y = p_sys->i_y;
    const int o_zoom = p_sys->i_zoom;

    /* background magnified image */
    if( o_zoom != ZOOM_FACTOR )
    {
        video_format_t fmt_in;
        video_format_t fmt_out;
        plane_t orig_planes[PICTURE_PLANE_MAX];
        memcpy(orig_planes, p_pic->p, sizeof orig_planes);

        for( int i_plane = 0; i_plane < p_pic->i_planes; i_plane++ )
        {
            const int o_yp = o_y * p_outpic->p[i_plane].i_visible_lines / p_outpic->p[Y_PLANE].i_visible_lines;
            const int o_xp = o_x * p_outpic->p[i_plane].i_visible_pitch / p_outpic->p[Y_PLANE].i_visible_pitch;

            p_pic->p[i_plane].p_pixels += o_yp * p_pic->p[i_plane].i_pitch + o_xp;
        }

        /* */
        fmt_in = p_filter->fmt_in.video;
        fmt_in.i_width  = fmt_in.i_visible_width  = (fmt_in.i_visible_width  * ZOOM_FACTOR / o_zoom) & ~1;
        fmt_in.i_height = fmt_in.i_visible_height = (fmt_in.i_visible_height * ZOOM_FACTOR / o_zoom) & ~1;

        /* */
        fmt_out = p_filter->fmt_out.video;
        p_converted = image_Convert( p_sys->p_image, p_pic, &fmt_in, &fmt_out );
        if (unlikely(!p_converted))
        {
            picture_Release( p_outpic );
            return NULL;
        }
        memcpy(p_pic->p, orig_planes, sizeof orig_planes);

        picture_CopyPixels( p_outpic, p_converted );

        picture_Release( p_converted );
    }
    else
    {
        picture_CopyPixels( p_outpic, p_pic );
    }

    /* */
    const video_orientation_t orient = p_filter->fmt_out.video.orientation;
    p_oyp = &p_outpic->p[Y_PLANE];
    if( b_visible )
    {
        video_format_t fmt_out;

        /* image visualization */
        fmt_out = p_filter->fmt_out.video;
        fmt_out.i_width  = fmt_out.i_visible_width  = PreviewSize(fmt_out.i_visible_width);
        fmt_out.i_height = fmt_out.i_visible_height = PreviewSize(fmt_out.i_visible_height);
        p_converted = image_Convert( p_sys->p_image, p_pic,
                                     &p_pic->format, &fmt_out );
        if (unlikely(!p_converted))
        {
            picture_Release( p_outpic );
            return NULL;
        }

        /* Move the thumbnail, without rotating its pixels a second time. */
        for (int i = 0; i < p_converted->i_planes; ++i) {
            plane_t dst = p_outpic->p[i];
            const plane_t *src = &p_converted->p[i];
            dst.i_visible_pitch = __MIN(dst.i_visible_pitch, src->i_visible_pitch);
            dst.i_visible_lines = __MIN(dst.i_visible_lines, src->i_visible_lines);
            if (ReverseX(orient))
                dst.p_pixels += p_outpic->p[i].i_visible_pitch - dst.i_visible_pitch;
            if (ReverseY(orient))
                dst.p_pixels += (ptrdiff_t)dst.i_pitch *
                                (p_outpic->p[i].i_visible_lines - dst.i_visible_lines);
            for (int row = 0; row < dst.i_visible_lines; ++row)
                memcpy(dst.p_pixels + (ptrdiff_t)row * dst.i_pitch,
                       src->p_pixels + (ptrdiff_t)row * src->i_pitch, dst.i_visible_pitch);
        }

        picture_Release( p_converted );

        /* white rectangle on visualization */
        v_w = __MIN( fmt_out.i_visible_width  * ZOOM_FACTOR / o_zoom, fmt_out.i_visible_width - 1 );
        v_h = __MIN( fmt_out.i_visible_height * ZOOM_FACTOR / o_zoom, fmt_out.i_visible_height - 1 );

        DrawRectangle( p_oyp->p_pixels, p_oyp->i_pitch,
                       p_oyp->i_visible_pitch, p_oyp->i_visible_lines,
                       (ReverseX(orient) ? p_oyp->i_visible_pitch - (int)fmt_out.i_visible_width : 0) + o_x/VIS_ZOOM,
                       (ReverseY(orient) ? p_oyp->i_visible_lines - (int)fmt_out.i_visible_height : 0) + o_y/VIS_ZOOM,
                       v_w, v_h );

        /* */
        v_h = (ORIENT_IS_SWAP(orient) ? fmt_out.i_visible_width : fmt_out.i_visible_height);
    }
    else
    {
        v_h = 0;
    }

    const int w = p_filter->fmt_out.video.i_visible_width;
    const int h = p_filter->fmt_out.video.i_visible_height;
    const int scale = ORIENT_IS_SWAP(orient) ? ControlScale(h, w) : ControlScale(w, h);

    if( b_visible || p_sys->i_last_activity + p_sys->i_hide_timeout > mdate() )
        DrawZoomStatus( p_oyp, orient,
                        scale, v_h, b_visible );

    if( b_visible )
    {
        /* Paint a contrasting track so the gauge remains visible on bright video. */
        for (int row = 0; row <= 80; ++row) {
            int width = GaugeWidth(row);
            for (int x = 0; x <= width; ++x)
                PlotCell(p_oyp, orient, scale, v_h, x, 9 + row,
                     x == 0 || x == width || row == 0 ? 128 :
                     (GaugeZoom(row) <= o_zoom ? 235 : 16));
        }
    }

    return CopyInfoAndRelease( p_outpic, p_pic );
}

static void DrawZoomStatus( plane_t *plane, video_orientation_t orient,
                            int scale, int top, bool b_visible )
{
    static const char *p_hide =
        "X   X X      XXXX   XXXXX  XXX   XXX  XX XX   X   X XXXXX XXXX  XXXXXL"
        "X   X X     X          X  X   X X   X X X X   X   X   X   X   X X    L"
        " X X  X     X         X   X   X X   X X   X   XXXXX   X   X   X XXXX L"
        " X X  X     X        X    X   X X   X X   X   X   X   X   X   X X    L"
        "  X   XXXXX  XXXX   XXXXX  XXX   XXX  X   X   X   X XXXXX XXXX  XXXXXL";
    static const char *p_show =
        "X   X X      XXXX   XXXXX  XXX   XXX  XX XX    XXXX X   X  XXX  X   XL"
        "X   X X     X          X  X   X X   X X X X   X     X   X X   X X   XL"
        " X X  X     X         X   X   X X   X X   X    XXX  XXXXX X   X X X XL"
        " X X  X     X        X    X   X X   X X   X       X X   X X   X X X XL"
        "  X   XXXXX  XXXX   XXXXX  XXX   XXX  X   X   XXXX  X   X  XXX   X X L";
    const char *p_draw = b_visible ? p_hide : p_show;

    for( int i = 0, x = 1, y = 1; p_draw[i] != '\0'; i++ )
    {
        if( p_draw[i] == 'X' )
        {
            PlotCell(plane, orient, scale, top, x, y, 235);
            x++;
        }
        else if( p_draw[i] == ' ' )
        {
            x++;
        }
        else if( p_draw[i] == 'L' )
        {
            x = 1;
            y++;
        }
    }
}
static void DrawRectangle( uint8_t *pb_dst, int i_pitch, int i_width, int i_height,
                           int x, int y, int i_w, int i_h )
{
    if( i_w <= 0 || i_h <= 0 || x < 0 || y < 0 ||
        x + i_w > i_width || y + i_h > i_height )
        return;

    /* top line */
    memset( &pb_dst[y * i_pitch + x], 0xff, i_w );

    /* left and right */
    for( int dy = 1; dy < i_h-1; dy++ )
    {
        pb_dst[(y+dy) * i_pitch + x +     0] = 0xff;
        pb_dst[(y+dy) * i_pitch + x + i_w-1] = 0xff;
    }

    /* bottom line */
    memset( &pb_dst[(y+i_h-1) * i_pitch + x], 0xff, i_w );
}

static int Mouse( filter_t *p_filter, vlc_mouse_t *p_mouse, const vlc_mouse_t *p_old, const vlc_mouse_t *p_new )
{
    filter_sys_t *p_sys = p_filter->p_sys;
    const video_format_t *p_fmt = &p_filter->fmt_in.video;

    /* */
    const bool b_click = vlc_mouse_HasPressed( p_old, p_new, MOUSE_BUTTON_LEFT );
    const bool b_pressed = vlc_mouse_IsLeftPressed( p_new );

    bool b_grab = false;

    const video_orientation_t orient = p_fmt->orientation;
    const int w = p_fmt->i_visible_width, h = p_fmt->i_visible_height;
    const int sx = p_new->i_x - (int)p_fmt->i_x_offset;
    const int sy = p_new->i_y - (int)p_fmt->i_y_offset;
    const int rx = ReverseX(orient) ? w - 1 - sx : sx;
    const int ry = ReverseY(orient) ? h - 1 - sy : sy;
    const int mx = ORIENT_IS_SWAP(orient) ? ry : rx;
    const int my = ORIENT_IS_SWAP(orient) ? rx : ry;

    const int scale = ORIENT_IS_SWAP(orient) ? ControlScale(h, w) : ControlScale(w, h);
    const int top = p_sys->b_visible ? PreviewSize(ORIENT_IS_SWAP(orient) ? w : h) : 0;
    /* Inverse of CellEdge. Points above or left of the controls stay outside. */
    const int ux = mx < 0 ? -1 : (int64_t)mx * UI_BASE / scale;
    const int uy = my < top ? -1 : (int64_t)(my - top) * UI_BASE / scale;

    /* Find the mouse position */
    if( p_sys->b_visible )
    {
        const int i_visu_width  = PreviewSize(ORIENT_IS_SWAP(orient) ? h : w);
        const int i_visu_height = PreviewSize(ORIENT_IS_SWAP(orient) ? w : h);

        if( mx >= 0 && mx < i_visu_width &&
            my >= 0 && my < i_visu_height )
        {
            /* Visualization */
            if( b_pressed )
            {
                const int v_w = p_fmt->i_visible_width  * ZOOM_FACTOR / p_sys->i_zoom;
                const int v_h = p_fmt->i_visible_height * ZOOM_FACTOR / p_sys->i_zoom;

                p_sys->i_x = VLC_CLIP( (sx - (ReverseX(orient) ? w - PreviewSize(w) : 0)) * VIS_ZOOM - v_w/2, 0,
                                           (int)p_fmt->i_visible_width  - v_w);
                p_sys->i_y = VLC_CLIP( (sy - (ReverseY(orient) ? h - PreviewSize(h) : 0)) * VIS_ZOOM - v_h/2, 0,
                                           (int)p_fmt->i_visible_height - v_h);

                b_grab = true;
            }
        }
        else if( ux >= 0 && ux < 80 && uy >= 0 && uy < 9 )
        {
            /* Hide text */
            if( b_click )
            {
                p_sys->b_visible = false;
                b_grab = true;
            }
        }
        else if( uy >= 9 && uy <= 89 && ux >= 0 && ux <= 40 )
        {
            /* Zoom gauge */
            if( b_pressed )
            {
                p_sys->i_zoom = GaugeZoom(uy - 9);

                const int v_w = p_fmt->i_visible_width  * ZOOM_FACTOR / p_sys->i_zoom;
                const int v_h = p_fmt->i_visible_height * ZOOM_FACTOR / p_sys->i_zoom;
                p_sys->i_x = VLC_CLIP( p_sys->i_x, 0, (int)p_fmt->i_visible_width  - v_w );
                p_sys->i_y = VLC_CLIP( p_sys->i_y, 0, (int)p_fmt->i_visible_height - v_h );

                b_grab = true;
            }
        }
    }
    else
    {
        if( ux >= 0 && ux < 80 && uy >= 0 && uy <= 10 )
        {
            /* Show text */
            if( b_click )
            {
                p_sys->b_visible = true;
                b_grab = true;
            }
        }
    }

    if( vlc_mouse_HasMoved( p_old, p_new ) )
        p_sys->i_last_activity = mdate();

    if( b_grab )
        return VLC_EGENERIC;

    /* */
    *p_mouse = *p_new;
    p_mouse->i_x = p_sys->i_x + p_new->i_x * ZOOM_FACTOR / p_sys->i_zoom;
    p_mouse->i_y = p_sys->i_y + p_new->i_y * ZOOM_FACTOR / p_sys->i_zoom;
    return VLC_SUCCESS;
}

