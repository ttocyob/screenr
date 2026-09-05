/*
 * overlay_handle_hit_ext.c
 *
 * Wires all 4 corner handles (handle_tl, handle_tr, handle_bl,
 * handle_br). These are now plain, independent Evas objects created
 * by overlay_fill_sync.c -- NOT Edje parts -- see overlay.c's own
 * header comment on ov->bg_obj for the full architectural change
 * (rubberband.edc, which used to hold this story, has been removed
 * entirely). overlay_wire_8handle_extras() below now takes the 4 objects
 * directly rather than resolving them from an Edje object; this file's
 * own drag logic is otherwise unaffected -- it never touches the
 * handle objects' Evas geometry directly either way, only through
 * overlay_fill_get_geometry()/overlay_fill_set_geometry().
 *
 * DESIGN, per corner: at mouse-down, snapshot fill's CURRENT geometry
 * and compute the OPPOSITE corner's real pixel position -- that point
 * stays FIXED for the whole drag. On every mouse-move, compute the
 * dragged corner's new real pixel position (start position + mouse
 * delta), then derive a new fill rect as the rectangle spanning the
 * fixed opposite corner and the dragged corner's new position, taking
 * min/max on each axis. This is the whole mechanism -- no separate
 * "has the user dragged past the opposite corner" case to detect: a
 * rectangle described by literally any two corner points, resolved via
 * min/max, is ALWAYS well-defined, even when the "dragged" corner ends
 * up numerically left of/above the "fixed" one.
 *
 * overlay_fill_set_geometry() (overlay_fill_sync.c) is what actually
 * applies and clamps (minimum size, window bounds) whatever rectangle
 * this file computes -- this file has NO clamping logic of its own,
 * matching overlay_body_drag.c's own design.
 */

#include <Evas.h>
#include <Edje.h>
#include <stdio.h>

#include "overlay.h"

typedef enum { CORNER_TL, CORNER_TR, CORNER_BL, CORNER_BR } _Corner;

typedef struct
{
   _Corner corner;
   Eina_Bool dragging;
   int drag_start_mouse_x, drag_start_mouse_y;
   int drag_start_corner_x, drag_start_corner_y; /* THIS corner's own real position at mouse-down */
   int fixed_x, fixed_y; /* the OPPOSITE corner's position -- constant for the whole drag */
} _Corner_Hit_Ctx;

/* One context per corner -- exactly 4. */
static _Corner_Hit_Ctx g_ctxs[4];

static void
_on_corner_mouse_down(void *data, Evas *e EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event_info)
{
   _Corner_Hit_Ctx *ctx = data;
   Evas_Event_Mouse_Down *ev = event_info;

   ctx->dragging = EINA_TRUE;
   ctx->drag_start_mouse_x = ev->canvas.x;
   ctx->drag_start_mouse_y = ev->canvas.y;

   int fx, fy, fw, fh;
   overlay_fill_get_geometry(&fx, &fy, &fw, &fh);

   switch (ctx->corner)
     {
      case CORNER_TL:
         ctx->drag_start_corner_x = fx;
         ctx->drag_start_corner_y = fy;
         ctx->fixed_x = fx + fw; /* opposite = bottom-right */
         ctx->fixed_y = fy + fh;
         break;
      case CORNER_TR:
         ctx->drag_start_corner_x = fx + fw;
         ctx->drag_start_corner_y = fy;
         ctx->fixed_x = fx; /* opposite = bottom-left */
         ctx->fixed_y = fy + fh;
         break;
      case CORNER_BL:
         ctx->drag_start_corner_x = fx;
         ctx->drag_start_corner_y = fy + fh;
         ctx->fixed_x = fx + fw; /* opposite = top-right */
         ctx->fixed_y = fy;
         break;
      case CORNER_BR:
         ctx->drag_start_corner_x = fx + fw;
         ctx->drag_start_corner_y = fy + fh;
         ctx->fixed_x = fx; /* opposite = top-left */
         ctx->fixed_y = fy;
         break;
     }
}

static void
_on_corner_mouse_move(void *data, Evas *e EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event_info)
{
   _Corner_Hit_Ctx *ctx = data;
   if (!ctx->dragging) return;

   Evas_Event_Mouse_Move *ev = event_info;
   int dx = ev->cur.canvas.x - ctx->drag_start_mouse_x;
   int dy = ev->cur.canvas.y - ctx->drag_start_mouse_y;

   int new_corner_x = ctx->drag_start_corner_x + dx;
   int new_corner_y = ctx->drag_start_corner_y + dy;

   /* Rectangle spanning the fixed opposite corner and this corner's
    * new position -- min/max resolves it regardless of which corner
    * ends up numerically where. See this file's own header comment:
    * this is the whole mechanism, no separate crossing case exists. */
   int x = (new_corner_x < ctx->fixed_x) ? new_corner_x : ctx->fixed_x;
   int y = (new_corner_y < ctx->fixed_y) ? new_corner_y : ctx->fixed_y;
   int right  = (new_corner_x > ctx->fixed_x) ? new_corner_x : ctx->fixed_x;
   int bottom = (new_corner_y > ctx->fixed_y) ? new_corner_y : ctx->fixed_y;

   overlay_fill_set_geometry(x, y, right - x, bottom - y);
}

static void
_on_corner_mouse_up(void *data, Evas *e EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event_info EINA_UNUSED)
{
   _Corner_Hit_Ctx *ctx = data;
   ctx->dragging = EINA_FALSE;
}

static void
_wire_corner(Evas_Object *handle_obj, _Corner corner, _Corner_Hit_Ctx *ctx)
{
   if (!handle_obj)
     {
        fprintf(stderr, "[overlay] handle object is NULL -- corner handle not wired\n");
        return;
     }

   ctx->corner = corner;
   ctx->dragging = EINA_FALSE;

   evas_object_event_callback_add(handle_obj, EVAS_CALLBACK_MOUSE_DOWN, _on_corner_mouse_down, ctx);
   evas_object_event_callback_add(handle_obj, EVAS_CALLBACK_MOUSE_MOVE, _on_corner_mouse_move, ctx);
   evas_object_event_callback_add(handle_obj, EVAS_CALLBACK_MOUSE_UP, _on_corner_mouse_up, ctx);
}

/*
 * Wires all 4 corner handles, given the objects directly. Call once
 * from overlay_new(), after overlay_wire_fill_sync() has already run
 * (so the handle objects exist and fill has a real initial geometry
 * to read at each handle's own first mouse-down).
 */
void
overlay_wire_8handle_extras(Evas_Object *handle_tl, Evas_Object *handle_tr,
                             Evas_Object *handle_bl, Evas_Object *handle_br)
{
   _wire_corner(handle_tl, CORNER_TL, &g_ctxs[0]);
   _wire_corner(handle_tr, CORNER_TR, &g_ctxs[1]);
   _wire_corner(handle_bl, CORNER_BL, &g_ctxs[2]);
   _wire_corner(handle_br, CORNER_BR, &g_ctxs[3]);
}
