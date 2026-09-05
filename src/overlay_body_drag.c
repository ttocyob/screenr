/*
 * overlay_body_drag.c
 *
 * Body-drag (click+drag inside the fill moves the whole rectangle).
 * Tracks the mouse delta in REAL PIXELS (no normalized confine math
 * needed anywhere in this model -- everything is already in the same
 * window-space pixel units throughout), adds it to fill's position at
 * drag-start, and calls overlay_fill_set_geometry() with the SAME
 * width/height -- clamping (never outside the window) is handled
 * entirely inside that one function; this file has no clamping logic
 * of its own at all.
 *
 * fill_obj (wired via overlay_wire_body_drag() below) is now a plain,
 * independent Evas rectangle created by overlay_fill_sync.c -- NOT an
 * Edje part -- see overlay.c's own header comment on ov->bg_obj for
 * the full history of why (rubberband.edc, which used to hold this
 * story, has been removed entirely). This
 * file's own logic is otherwise unaffected by that change: it only
 * ever calls overlay_fill_get_geometry()/overlay_fill_set_geometry(),
 * never touches fill_obj's Evas geometry directly, so the object type
 * moving underneath it doesn't matter to anything here.
 */

#include <Evas.h>
#include <Edje.h>
#include <stdio.h>

#include "overlay.h"

/* Drag state, private to this file -- same file-static pattern as
 * before (only one Overlay/rubberband exists in this app). */
static Eina_Bool g_dragging = EINA_FALSE;
static int g_drag_start_mouse_x = 0, g_drag_start_mouse_y = 0;
static int g_drag_start_fill_x = 0, g_drag_start_fill_y = 0;
static int g_drag_fill_w = 0, g_drag_fill_h = 0; /* constant for the whole drag */

static void
_on_fill_mouse_down(void *data EINA_UNUSED, Evas *e EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event_info)
{
   Evas_Event_Mouse_Down *ev = event_info;

   g_dragging = EINA_TRUE;
   g_drag_start_mouse_x = ev->canvas.x;
   g_drag_start_mouse_y = ev->canvas.y;

   int w, h;
   overlay_fill_get_geometry(&g_drag_start_fill_x, &g_drag_start_fill_y, &w, &h);
   g_drag_fill_w = w;
   g_drag_fill_h = h;
}

static void
_on_fill_mouse_move(void *data EINA_UNUSED, Evas *e EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event_info)
{
   if (!g_dragging) return;

   Evas_Event_Mouse_Move *ev = event_info;
   int dx = ev->cur.canvas.x - g_drag_start_mouse_x;
   int dy = ev->cur.canvas.y - g_drag_start_mouse_y;

   /* Same width/height throughout -- only position changes during a
    * body-drag. overlay_fill_set_geometry()'s own window-bounds clamp
    * naturally stops the rectangle at the window's edge, exactly like
    * any other geometry update -- no separate logic needed here for
    * that; it's just what the shared clamp already does. */
   overlay_fill_set_geometry(g_drag_start_fill_x + dx, g_drag_start_fill_y + dy,
                              g_drag_fill_w, g_drag_fill_h);
}

static void
_on_fill_mouse_up(void *data EINA_UNUSED, Evas *e EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event_info EINA_UNUSED)
{
   g_dragging = EINA_FALSE;
}

/*
 * Wires the three callbacks above onto fill_obj directly. Call once,
 * after overlay_wire_fill_sync() has created fill_obj (see overlay.c's
 * overlay_new(), which passes the object straight through rather than
 * this file resolving it itself -- there's no Edje part to resolve
 * anymore).
 */
void
overlay_wire_body_drag(Evas_Object *fill_obj)
{
   if (!fill_obj)
     {
        fprintf(stderr, "[overlay] fill_obj is NULL -- body-drag not wired\n");
        return;
     }

   evas_object_event_callback_add(fill_obj, EVAS_CALLBACK_MOUSE_DOWN, _on_fill_mouse_down, NULL);
   evas_object_event_callback_add(fill_obj, EVAS_CALLBACK_MOUSE_MOVE, _on_fill_mouse_move, NULL);
   evas_object_event_callback_add(fill_obj, EVAS_CALLBACK_MOUSE_UP, _on_fill_mouse_up, NULL);
}
