/*
 * overlay_fill_sync.c
 *
 * REWRITTEN this session -- see rubberband.edc's own header comment
 * for the full architectural change this is part of. fill and the 4
 * corner handles are no longer Edje parts at all: this file creates
 * them as genuinely independent plain Evas rectangles
 * (evas_object_rectangle_add()), owns their geometry entirely in C,
 * and Edje never has any knowledge of or authority over them. This is
 * the fix for a confirmed, reproducible bug: the previous architecture
 * positioned these via edje_object_part_object_get() + raw
 * evas_object_move()/resize() calls, which is EFL's own documented
 * anti-pattern (Edje recalculates a group's parts from their
 * .edc-declared "default" state on ANY recalculation of the group, for
 * ANY reason, silently discarding whatever C last set) -- confirmed by
 * real logging that fill was reverting to its old .edc default
 * (25%/25%-75%/75%) with no C function anywhere responsible for the
 * write. Taking these objects out of Edje's object tree entirely
 * closes that whole bug class structurally.
 *
 * This file remains the single owner of fill's real geometry (x/y/w/h
 * in the Selection window's own on-screen pixels), and the one place
 * both of this app's real constraints on that geometry are enforced:
 *
 *   1. Minimum size -- fill's real-desktop-pixel size (after
 *      converting through the window's own scale factor) can never go
 *      below MIN_FILL_REAL_PX x MIN_FILL_REAL_PX (100x100), matching
 *      ffmpeg's own practical recording floor exactly. Enforced in
 *      REAL desktop pixels, not window-space ones -- real pixels is
 *      the one framing that can't produce a surprise depending on
 *      which monitor/scale factor is in play.
 *
 *   2. Window bounds -- fill's edges can never go outside the
 *      Selection window's own bounds (0,0)-(window_w,window_h), read
 *      from "bg" (the one remaining Edje part this file still reads
 *      from -- see overlay_fill_set_geometry()'s own comment on why
 *      that's safe).
 *
 * Every caller that wants to move or resize the selection (body-drag,
 * corner-handle drag) computes whatever new x/y/w/h it wants and calls
 * overlay_fill_set_geometry() below -- this file does the clamping,
 * applies it to the independent fill/handle Evas objects, and NOTHING
 * ELSE in this codebase needs its own copy of either constraint or any
 * direct access to these objects at all.
 */

#include <Evas.h>
#include <Edje.h>
#include <stdio.h>

#include "overlay.h"

/* Real-desktop-pixel minimum -- see this file's own header comment for
 * why real pixels, not window-space ones. Matches overlay.c's own
 * RUBBERBAND_MIN_W/RUBBERBAND_MIN_H exactly (both are ffmpeg's
 * practical recording floor) -- restated here since this file has no
 * direct access to overlay.c's own #define; if that constant ever
 * changes, this one needs to change with it (a real seam, flagged
 * rather than hidden). */
#define MIN_FILL_REAL_PX 100

/* Handle visual size (window-space pixels), base value at UI scale
 * 1.0. Matches the old .edc declaration's min/max 10x10 -- now
 * expressed here in C instead, since these are no longer Edje parts
 * with their own .edc description to declare a size in. Actual
 * on-screen size is HANDLE_PX * g_scale_ui (see that variable's own
 * comment below), computed at point of use, not baked into a second
 * constant. HANDLE_HALF_PX is KEPT but no longer used for any live
 * math -- see _reposition_handles()'s own body comment for a real,
 * confirmed bug this exact constant was involved in (computing a
 * handle's centering offset via a separately-scaled HANDLE_HALF_PX
 * could disagree with the handle's own actual scaled size at non-1.0
 * scale factors); left in place only as the base "half of 10"
 * reference value for that comment's own history, not because
 * anything still reads it. */
#define HANDLE_PX 10
#define HANDLE_HALF_PX 5

/* Resolved via SCREENR_DATADIR, same reasoning as overlay_done_
 * button.c's own DONE_BTN_IMAGE_PATH -- see that file's comment for
 * the full explanation (Meson-injected compile-time macro,
 * -DSCREENR_DATADIR="{prefix}/{datadir}/screenr" from src/meson.build).
 * handle.png is installed to {datadir}/screenr/images/ by data/themes/
 * default/meson.build's own install_data() rule, alongside Done's own
 * rec_outline.png -- the only two images loaded this way, everything
 * else being already embedded in screenr.edj by edje_cc. */
#define HANDLE_IMAGE_PATH SCREENR_DATADIR "/images/handle.png"

/* Evas layer fill and the 4 handles render on -- see overlay.h's own
 * definition of EVAS_LAYER_FILL for why layers (not stack_above/below)
 * are used here, and for why this constant lives in the shared header
 * rather than privately in this file (overlay.c needs it too, to set
 * ov->edje_obj's own layer above this one). */

/* File-static handles to the independent Evas objects this file owns,
 * plus the window-bounds source and scale factor -- all set once by
 * overlay_wire_fill_sync(), read/written by every geometry update
 * after. Same single-instance pattern as before (only one
 * Overlay/rubberband exists in this app). */
static Evas_Object *g_bg_obj = NULL; /* Edje part -- window bounds source, read-only */
static Evas_Object *g_fill_obj = NULL;
static Evas_Object *g_handle_tl = NULL;
static Evas_Object *g_handle_tr = NULL;
static Evas_Object *g_handle_bl = NULL;
static Evas_Object *g_handle_br = NULL;
static double g_scale = 1.0; /* window-space px per real-desktop px, e.g. 0.667.
                                 NOT constant for the window's whole life -- see
                                 overlay_fill_sync_set_screenshot_scale() below. */

/*
 * Updates g_scale (the screenshot-fit ratio -- window-space px per
 * real-desktop px, mirrors overlay.c's own ov->scale) after a UI-scale
 * -driven window resize. Needed because overlay.c's own
 * _apply_window_size() recomputes ov->scale every time Elementary's UI
 * scale factor changes -- without pushing that updated value back into
 * this file's own g_scale, MIN_FILL_REAL_PX's own real-pixel-to-
 * window-space conversion (see _clamp_geometry() below) would silently
 * keep using a stale ratio from whatever scale factor was active at
 * app startup, forever. Called once from overlay.c's own
 * overlay_rescale(), right after it calls _apply_window_size() and
 * reads the freshly-updated ov->scale back out.
 */
void
overlay_fill_sync_set_screenshot_scale(double scale)
{
   g_scale = scale;
}

/* Elementary's UI scale factor (elm_config_scale_get(), e.g. 2.0 at
 * 200% HiDPI) -- DELIBERATELY a separate variable from g_scale above,
 * not a reuse of it. The two mean completely different things: g_scale
 * is the Selection window's own screenshot-fit ratio (real desktop px
 * vs this window's own on-screen px, see overlay.c's own ov->scale) --
 * it DOES change with UI scale now (indirectly, via the Selection
 * window's own size changing -- see overlay.c's own
 * _apply_window_size()), it's just never SET directly from g_scale_ui
 * -- the two remain conceptually distinct values (one is a window-size
 * ratio, the other a UI-chrome multiplier), updated through separate
 * setters, for separate reasons. g_scale_ui affects ONLY the base
 * pixel size of things like HANDLE_PX -- plain C constants for UI
 * chrome this file owns, analogous to Procvue's own ELM_SCALE_SIZE()
 * calls on its own manually-created Evas objects. Defaults to 1.0;
 * updated via overlay_fill_sync_set_ui_scale(), called from overlay.c's
 * own overlay_rescale() (see that function's own comment for the full
 * dispatch chain from main.c's config-changed handler). */
static double g_scale_ui = 1.0;

void
overlay_fill_sync_set_ui_scale(double scale)
{
   g_scale_ui = scale;
}

/*
 * Repositions the 4 corner handles to sit exactly at (x,y), (x+w,y),
 * (x,y+h), (x+w,y+h) -- fill's own 4 corners, centered via half the
 * handle's own actual (scale-aware) on-screen size, computed fresh
 * inside this function -- see this function's own body comment for
 * why HANDLE_HALF_PX itself is no longer used for this. Called every
 * time fill's geometry changes, so the handles never visibly lag
 * behind.
 */
static void
_reposition_handles(int x, int y, int w, int h)
{
   /* CONFIRMED REAL BUG, fixed here: this function used to only ever
    * call evas_object_move() on the handles, never evas_object_resize()
    * -- so a handle's own actual on-screen SIZE stayed frozen at
    * whatever it was computed to be ONCE, at app startup (inside
    * overlay_wire_fill_sync()'s own creation loop), even after
    * g_scale_ui itself correctly updated on a later scale change. The
    * POSITION math below was recalculating correctly against an
    * ASSUMED new size, while the actual rendered object silently
    * stayed the old size -- confirmed via direct user testing/
    * measurement and diagnosis (comparing handle.png's own real 32x32
    * canvas against its rendered on-screen footprint at different
    * scale factors). Fixed by resizing here too, every time this
    * function runs -- not just once at creation -- using the exact
    * same size expression the creation loop uses, so position and
    * size can never drift apart again. half is derived directly from
    * THIS freshly-computed size (not a separately-scaled
    * HANDLE_HALF_PX), for the same reason already established
    * elsewhere this session: two independent (int) truncations of
    * "half the handle" can disagree at some scale factors even when
    * they happen to agree at others. */
   int handle_px = (int)(HANDLE_PX * g_scale_ui);
   int half = handle_px / 2;

   if (g_handle_tl)
     {
        evas_object_resize(g_handle_tl, handle_px, handle_px);
        evas_object_move(g_handle_tl, x - half, y - half);
     }
   if (g_handle_tr)
     {
        evas_object_resize(g_handle_tr, handle_px, handle_px);
        evas_object_move(g_handle_tr, x + w - half, y - half);
     }
   if (g_handle_bl)
     {
        evas_object_resize(g_handle_bl, handle_px, handle_px);
        evas_object_move(g_handle_bl, x - half, y + h - half);
     }
   if (g_handle_br)
     {
        evas_object_resize(g_handle_br, handle_px, handle_px);
        evas_object_move(g_handle_br, x + w - half, y + h - half);
     }
}

/*
 * THE clamp -- the one place both real constraints (minimum size,
 * window bounds) are enforced, on a plain x/y/w/h rectangle. Straight
 * interval clamping: no crossing, no swapping, no "which one is being
 * dragged" bookkeeping needed at all, because a single rectangle with
 * a real width/height cannot invert itself the way two independent
 * points could.
 */
static void
_clamp_geometry(int *x, int *y, int *w, int *h, int win_w, int win_h)
{
   /* Real-pixel minimum converted to window-space via the one-time
    * scale factor -- see this file's own header comment on why real
    * pixels is the chosen frame. g_scale is win_px / real_px, so
    * window-space minimum = real minimum * scale. */
   int min_w_px = (int)(MIN_FILL_REAL_PX * g_scale);
   int min_h_px = (int)(MIN_FILL_REAL_PX * g_scale);
   if (min_w_px < 1) min_w_px = 1; /* degenerate guard, shouldn't happen */
   if (min_h_px < 1) min_h_px = 1;

   /* Size floor first -- applied before position clamping, since a
    * too-small size could otherwise get clamped into an inconsistent
    * position (e.g. width floored AFTER x/x+w were already forced to
    * fit the window, producing a width that doesn't match what was
    * just enforced). */
   if (*w < min_w_px) *w = min_w_px;
   if (*h < min_h_px) *h = min_h_px;

   /* Never larger than the window itself -- a real edge case (a very
    * small Selection window combined with the 100px real-pixel floor
    * scaled up) worth guarding rather than assuming can't happen. */
   if (*w > win_w) *w = win_w;
   if (*h > win_h) *h = win_h;

   /* Position bounds -- x/y never negative, x+w/y+h never past the
    * window's own edge. */
   if (*x < 0) *x = 0;
   if (*y < 0) *y = 0;
   if (*x + *w > win_w) *x = win_w - *w;
   if (*y + *h > win_h) *y = win_h - *h;
   /* Re-clamp position to >= 0 once more -- if width/height (after the
    * size floor above) is itself larger than the window, the right or
    * bottom-edge clamp just above could push x/y negative. Not
    * expected in practice (the window is always far larger than the
    * 100-real-pixel floor once scaled), but cheap to guard. */
   if (*x < 0) *x = 0;
   if (*y < 0) *y = 0;
}

/*
 * THE single entry point every drag mechanism calls with whatever new
 * geometry it wants -- clamps it, applies it to the independent fill
 * object, and repositions all 4 independent handle objects to match.
 * This is the ONLY function in this whole codebase that writes to
 * fill's actual Evas geometry.
 */
void
overlay_fill_set_geometry(int x, int y, int w, int h)
{
   if (!g_fill_obj || !g_bg_obj) return;

   /* Window bounds read from "bg" -- the one Edje part this file still
    * touches, and only ever for a read-only geometry query, never a
    * write. Safe: bg is a plain, fully-transparent, full-window rect
    * that nothing in this codebase ever moves or resizes, so it can
    * never be subject to the "C wrote it, Edje reverted it" problem
    * that affected fill/border/handles under the old architecture --
    * there is no C-side write to bg for Edje to ever revert. */
   int cx, cy, cw, ch;
   evas_object_geometry_get(g_bg_obj, &cx, &cy, &cw, &ch);
   (void)cx; (void)cy; /* bg is always at the window's own origin */

   _clamp_geometry(&x, &y, &w, &h, cw, ch);

   evas_object_move(g_fill_obj, x, y);
   evas_object_resize(g_fill_obj, w, h);

   _reposition_handles(x, y, w, h);

   /* Notify overlay.c so it can update the window's titlebar with the
    * new dimensions -- see overlay.h's declaration of
    * overlay_on_fill_geometry_changed() for the full reasoning.
    * Called last, after fill's own geometry and all 4 handles are
    * already fully updated, so whatever overlay.c reads back via
    * overlay_get_record_geometry() (which itself calls
    * overlay_fill_get_geometry()) sees the final, settled state. */
   overlay_on_fill_geometry_changed();
}

/*
 * Reads fill's CURRENT real Evas geometry -- the single source of
 * truth callers (overlay_body_drag.c, overlay_handle_hit_ext.c) read
 * from before computing a new geometry to pass to
 * overlay_fill_set_geometry() above. Now a completely trivial read of
 * an independent Evas object's own geometry -- no Edje involved at
 * all, so nothing can silently revert it between this read and the
 * caller's own use of the value.
 */
void
overlay_fill_get_geometry(int *out_x, int *out_y, int *out_w, int *out_h)
{
   if (!g_fill_obj) { *out_x = *out_y = *out_w = *out_h = 0; return; }
   evas_object_geometry_get(g_fill_obj, out_x, out_y, out_w, out_h);
}

/*
 * Call once from overlay_new(), after the rubberband's Edje content
 * has loaded and been shown/laid out (so "bg" has real geometry to
 * read). evas is the same Evas the rubberband edje object itself
 * lives on (see overlay.c's own evas_object_evas_get(ov->win) call) --
 * fill and the 4 handles are created directly on this canvas,
 * independent of and outside the edje object's own part tree. bg_obj
 * is the rubberband's "bg" part, resolved once here by overlay.c and
 * passed in, since this file no longer resolves any Edje parts of its
 * own beyond this one read-only geometry reference. scale is the
 * Selection window's own one-time scale factor (window-space px per
 * real-desktop px -- see overlay.c's own SELECTION_WIN_MAX_W/
 * ov->scale), needed here to convert MIN_FILL_REAL_PX into a
 * window-space minimum. Sets fill to its initial default geometry
 * (middle 50% of the window on each axis) and positions the 4 handles
 * to match.
 */
void
overlay_wire_fill_sync(Evas *evas, Evas_Object *bg_obj, double scale)
{
   g_bg_obj = bg_obj;
   g_scale = scale;

   g_fill_obj = evas_object_rectangle_add(evas);

#define PREMUL(c, a) (((c) * (a)) / 255)

   int r = 100, g = 140, b = 200;
   int a = 32;

   evas_object_color_set(g_fill_obj, PREMUL(r, a), PREMUL(g, a), PREMUL(b, a), a);
   evas_object_layer_set(g_fill_obj, EVAS_LAYER_FILL);
   evas_object_show(g_fill_obj);

   /* The 4 corner handles: images/handle.png  */
   g_handle_tl = evas_object_image_add(evas);
   g_handle_tr = evas_object_image_add(evas);
   g_handle_bl = evas_object_image_add(evas);
   g_handle_br = evas_object_image_add(evas);

   Evas_Object *handles[4] = { g_handle_tl, g_handle_tr, g_handle_bl, g_handle_br };
   int i;
   for (i = 0; i < 4; i++)
     {
        evas_object_image_file_set(handles[i], HANDLE_IMAGE_PATH, NULL);
        Evas_Load_Error err = evas_object_image_load_error_get(handles[i]);
        if (err != EVAS_LOAD_ERROR_NONE)
          fprintf(stderr, "[overlay_fill_sync] failed to load %s: %s\n",
                  HANDLE_IMAGE_PATH, evas_load_error_str(err));
        evas_object_image_filled_set(handles[i], EINA_TRUE);
        evas_object_layer_set(handles[i], EVAS_LAYER_FILL);
        evas_object_resize(handles[i], (int)(HANDLE_PX * g_scale_ui), (int)(HANDLE_PX * g_scale_ui));
        evas_object_show(handles[i]);
     }

   /* Reads "bg" for the window's own bounds -- see
    * overlay_fill_set_geometry()'s own comment for why this is safe
    * (bg is never written to by C, so nothing can have reverted its
    * geometry by the time this reads it). */
   int cx, cy, cw, ch;
   evas_object_geometry_get(g_bg_obj, &cx, &cy, &cw, &ch);
   (void)cx; (void)cy;

   /* Default: middle 50% of the window on each axis, matching the
    * original design's 0.25/0.75 normalized default -- expressed
    * directly as window-space pixels. */
   int default_x = cw / 4;
   int default_y = ch / 4;
   int default_w = cw / 2;
   int default_h = ch / 2;

   overlay_fill_set_geometry(default_x, default_y, default_w, default_h);
}

/*
 * Historically raised fill/handles above a given reference object via
 * evas_object_stack_above() -- REMOVED, confirmed broken via real
 * testing: that call silently failed with an Evas ERR ("has no parent
 * but above has smart parent") whenever the reference object had a
 * smart parent (true for both ov->edje_obj and ov->shot_image, both
 * wrapped by Elementary's elm_win_resize_object_add()) while
 * fill/handles, being plain evas_object_rectangle_add() objects,
 * never do. Layer-based ordering (see EVAS_LAYER_FILL above, applied
 * once at object-creation time in overlay_wire_fill_sync()) makes this
 * function's original job unnecessary: fill/handles are permanently on
 * a higher layer than the screenshot, so they always render above it
 * with no per-call stacking needed at all. Kept as a documented no-op
 * (rather than removed, which would require touching overlay.c/
 * overlay.h's call sites for no behavioral gain) so those call sites
 * don't need to change.
 */
void
overlay_fill_raise_above(Evas_Object *reference_obj EINA_UNUSED)
{
}

/*
 * Accessors for the independent objects this file owns -- needed by
 * overlay.c to pass them into overlay_wire_body_drag()/overlay_wire_
 * 8handle_extras() (overlay_body_drag.c, overlay_handle_hit_ext.c),
 * and by overlay_fill_raise_above() below for stacking. Only valid
 * after overlay_wire_fill_sync() has run.
 */
Evas_Object *overlay_fill_sync_get_fill(void) { return g_fill_obj; }
Evas_Object *overlay_fill_sync_get_handle_tl(void) { return g_handle_tl; }
Evas_Object *overlay_fill_sync_get_handle_tr(void) { return g_handle_tr; }
Evas_Object *overlay_fill_sync_get_handle_bl(void) { return g_handle_bl; }
Evas_Object *overlay_fill_sync_get_handle_br(void) { return g_handle_br; }

/*
 * Deletes fill and all 4 handles -- called once from overlay_free() at
 * app shutdown. These are independent Evas objects (not Edje parts),
 * so they are NOT automatically cleaned up by edje_object_del() or the
 * Selection window's own teardown -- this explicit deletion is
 * required, unlike the old architecture where Edje owned their
 * lifetime along with everything else in the group.
 */
void
overlay_fill_sync_free(void)
{
   if (g_fill_obj) { evas_object_del(g_fill_obj); g_fill_obj = NULL; }
   if (g_handle_tl) { evas_object_del(g_handle_tl); g_handle_tl = NULL; }
   if (g_handle_tr) { evas_object_del(g_handle_tr); g_handle_tr = NULL; }
   if (g_handle_bl) { evas_object_del(g_handle_bl); g_handle_bl = NULL; }
   if (g_handle_br) { evas_object_del(g_handle_br); g_handle_br = NULL; }
   g_bg_obj = NULL;
}
