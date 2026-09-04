/*
 * overlay.c
 *
 * Implements the rebuilt Selection-mode window -- see overlay.h's
 * header comment for the full design rationale and NEXT_SESSION_PLAN.md
 * for the history this replaces.
 *
 * ov->win        -- ONE ordinary, DIALOG-typed (WM-managed,
 *                    ELM_WIN_DIALOG_BASIC -- NOT override-redirect,
 *                    NOT alpha) window, giving WM-level centering and
 *                    a close-only titlebar (no minimize/maximize) for
 *                    free -- see overlay_new()'s own comment on the
 *                    window-type choice. Sized 1:1 to the real desktop
 *                    when the desktop's width is at or under
 *                    SELECTION_WIN_MAX_W; scaled down proportionally
 *                    (preserving the desktop's own real aspect ratio)
 *                    when it's larger -- same model as Prevue's own
 *                    image-window sizing (PREVIEW_MAX_W/H): never
 *                    upscale, only ever cap and scale DOWN when the
 *                    real content exceeds a fixed ceiling. See this
 *                    file's own comment on SELECTION_WIN_MAX_W below
 *                    for why 1280 specifically.
 * ov->shot_image -- a static screenshot of the desktop, swallowed into
 *                    ov->win as a plain Evas image, filling the window
 *                    at whatever scale the window itself ended up at.
 * ov->edje_obj   -- the "screenr/rubberband" group, overlaid on top of
 *                    the screenshot at the window's own full size.
 * ov->scale      -- the ONE-TIME scale factor between the window's own
 *                    on-screen pixels and real desktop pixels, computed
 *                    once in overlay_new() and never touched again for
 *                    this window's life (same "compute once, never
 *                    live-track" principle that made the old
 *                    architecture's click-through mask safe -- see
 *                    NEXT_SESSION_PLAN.md). overlay_get_record_geometry()
 *                    divides by this to convert the rubberband's
 *                    on-screen position back to real desktop pixels.
 *
 * Nothing here is live. The screenshot is a picture; the window's size
 * never changes after creation; there is no XShape mask and no
 * tracking of anything. Standard widget/window behavior throughout.
 */

#include <Eina.h>
#include <Evas.h>
#include <Elementary.h>
#include <Edje.h>
#include <Ecore.h>
#include <Ecore_Evas.h>
#include <Ecore_X.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h> /* lround() -- see overlay_get_record_geometry()'s own
                     comment on why rounding, not truncating, the scale
                     conversion */
#include <string.h>

#include "overlay.h"

/*
 * Ceiling on the Selection window's own on-screen WIDTH (height is
 * always derived from this and the real desktop's own aspect ratio --
 * never a fixed WxH pair, since desktops vary in shape, not just size:
 * 16:9, 16:10, and ultrawide are all real and common -- see project
 * notes for the full reasoning session that landed here).
 *
 * 1280 specifically: reused verbatim from Prevue's own PREVIEW_MAX_W
 * (see /areas/prevue.md) -- a value already tuned once through real
 * use in a sibling EFL app on this exact desktop, rather than a fresh
 * guess. Confirmed via real testing against the user's own 1920x1080
 * monitor: a window requested at the FULL desktop resolution does not
 * actually fit (WM chrome -- titlebar/borders -- pushes the outer
 * frame past the screen's own edges), so the ceiling must sit
 * comfortably under the smallest common desktop width, not merely
 * under the literal largest one. 1280 clears that with real margin.
 *
 * Below this ceiling, the window is TRUE 1:1 -- no scaling applied at
 * all, matching Prevue's own refusal to ever upscale/needlessly
 * downscale content that already fits (the exact bug the two of you
 * hunted down together in Prevue's fit_zoom logic). A 1366x768 laptop
 * screen, for example, stays fully unscaled under this ceiling.
 *
 * UI SCALE: this is a BASE value at Elementary UI scale 1.0, scaled by
 * _apply_window_size() (e.g. 1280 -> 2560 at 200%, matching how the
 * user's own Prevue app scales its own equivalent ceiling). This was
 * tried, appeared to have zero observable effect, and was briefly
 * REVERTED -- that appearance was a red herring: the actual cause was
 * a separate, independent bug in overlay_fill_sync.c's own
 * _reposition_handles() (handles were repositioned but never actually
 * resized on a scale change, so the window's own resize had no visible
 * confirmation either way). With that bug fixed and confirmed via
 * direct user testing (screenshots at scale 1.5 and 2.0 showing
 * correctly-sized, correctly-centered handles), this scale-aware
 * behavior was reapplied. See _apply_window_size()'s own comment for
 * the full story.
 */
#define SELECTION_WIN_MAX_W 1280

struct _Overlay
{
   Evas_Object *win;         /* ordinary Elm_Win -- see SELECTION_WIN_MAX_W above */
   Evas_Object *shot_image;  /* static screenshot, filling the window at ov->scale */
   Evas_Object *edje_obj;    /* "screenr/rubberband" content, overlaid on top */
   int desktop_w, desktop_h; /* REAL desktop resolution (unscaled) */
   int win_w, win_h;         /* window's own on-screen size (== desktop size if
                                 desktop_w <= SELECTION_WIN_MAX_W, else scaled down) */
   double scale;             /* win_w / desktop_w -- computed once, see header comment.
                                 Always <= 1.0: this window only ever scales DOWN,
                                 matching Prevue's own never-upscale rule. */
};

/* File-static pointer to the one Overlay this app ever creates --
 * needed so overlay_on_fill_geometry_changed() (called from
 * overlay_fill_sync.c, a different file, with no Overlay* of its own
 * to pass around) can reach it. Set once, in overlay_new(). Same
 * single-instance reasoning already used throughout this codebase
 * (overlay_body_drag.c, overlay_fill_sync.c's own file-static state,
 * etc.) -- this app never creates more than one Overlay. */
static Overlay *g_the_overlay = NULL;

/*
 * Grabs a still frame of the X11 root window. Same technique as
 * capture_preview.c's _screenshot_capture() (ecore_x_image_get() pixel
 * readback, not a shelled-out `import`/`scrot`/ffmpeg call) -- see
 * that file's own comment for the full reasoning and the fallback
 * option if this proves fragile on some compositor.
 *
 * Does NOT hide screenr's own window before capturing -- an earlier
 * version of this function did (see this file's own git history for
 * the "x_win_to_hide" parameter this replaces), a pattern inherited
 * from a much older (1st) architecture where screenr's main window was
 * large and could plausibly land inside whatever region the user was
 * about to select. Under the CURRENT architecture, screenr's main
 * window is a small side panel, and hiding it created a real,
 * confirmed correctness bug: the user chooses their selection by
 * looking at their REAL desktop, where screenr's own window IS
 * visible -- a screenshot that then silently omits it no longer
 * matches what the user was actually looking at, and could lead to a
 * selection that doesn't match reality. It also produced a real,
 * visible flicker of screenr's own window on the user's screen every
 * time Record was pressed in Selection mode. Screenr appearing in this
 * screenshot (and therefore potentially in the final recording, if the
 * user's selection happens to include it) is correct, expected
 * behavior now, not something to avoid.
 *
 * Always returns the shot at its REAL, unscaled desktop resolution --
 * scaling to fit the (possibly smaller) window happens separately, via
 * the image object's own fill/size once swallowed (evas_object_image_
 * filled_set() + smooth_scale_set(), same as before), not by asking
 * the X server for anything other than the real pixels.
 */
static Evas_Object *
_screenshot_capture(Evas *evas, int *out_w, int *out_h)
{
   Ecore_X_Window root = ecore_x_window_root_first_get();
   int screen_w = 0, screen_h = 0;
   ecore_x_window_size_get(root, &screen_w, &screen_h);

   Ecore_X_Display *disp = ecore_x_display_get();
   Ecore_X_Screen *screen = ecore_x_default_screen_get();
   Ecore_X_Image *xim = ecore_x_image_new(screen_w, screen_h,
                                           ecore_x_default_visual_get(disp, screen),
                                           ecore_x_window_depth_get(root));
   if (!xim)
     {
        fprintf(stderr, "[overlay] ecore_x_image_new failed\n");
        return NULL;
     }

   if (!ecore_x_image_get(xim, root, 0, 0, 0, 0, screen_w, screen_h))
     {
        fprintf(stderr, "[overlay] ecore_x_image_get failed\n");
        ecore_x_image_free(xim);
        return NULL;
     }

   int bpl = 0, rows = 0, bpp = 0;
   void *pixels = ecore_x_image_data_get(xim, &bpl, &rows, &bpp);
   if (!pixels)
     {
        fprintf(stderr, "[overlay] ecore_x_image_data_get failed\n");
        ecore_x_image_free(xim);
        return NULL;
     }

   Evas_Object *img = evas_object_image_add(evas);
   evas_object_image_size_set(img, screen_w, screen_h);
   evas_object_image_colorspace_set(img, EVAS_COLORSPACE_ARGB8888);
   evas_object_image_alpha_set(img, EINA_FALSE);
   evas_object_image_filled_set(img, EINA_TRUE);
   evas_object_image_smooth_scale_set(img, EINA_TRUE);

   void *dst = evas_object_image_data_get(img, EINA_TRUE);
   if (dst)
     {
        memcpy(dst, pixels, (size_t)bpl * rows);
        evas_object_image_data_set(img, dst);
     }
   evas_object_image_data_update_add(img, 0, 0, screen_w, screen_h);

   ecore_x_image_free(xim);

   if (out_w) *out_w = screen_w;
   if (out_h) *out_h = screen_h;
   return img;
}

/*
 * Live dimensions callback -- registered by callbacks.c (via
 * overlay_set_dimensions_changed_callback() below) so this file can
 * report fill's current real-desktop-pixel size outward, without
 * needing to know anything about App or app->edje_obj itself (same
 * one-directional-hook pattern already used for overlay_done_button.c's
 * own click callback). NULL until registered; harmless to leave unset
 * (dimensions_text simply never updates from Selection mode, but
 * nothing crashes or misbehaves).
 */
static void (*g_dimensions_changed_cb)(int w, int h, void *data) = NULL;
static void *g_dimensions_changed_cb_data = NULL;

void
overlay_set_dimensions_changed_callback(void (*cb)(int w, int h, void *data), void *data)
{
   g_dimensions_changed_cb = cb;
   g_dimensions_changed_cb_data = data;
}

/*
 * Updates the window's titlebar with the rubberband's CURRENT real
 * desktop-pixel dimensions (W x H, not X/Y position) -- Prevue-style
 * "(WxH) filename" readout, reused as the same idea here. Reads
 * overlay_get_record_geometry() fresh every call rather than caching,
 * same reasoning as elsewhere in this codebase: a stale value should
 * never be left on screen. Static, file-local -- called from
 * overlay_on_fill_geometry_changed() below, which needs no external
 * data beyond the Overlay* itself. Also fires the dimensions-changed
 * callback above, so main.edc's dimensions_text part (on screenr's own
 * main window) stays in sync with the live drag too, not just this
 * window's own titlebar.
 */
static void
_update_titlebar_dimensions(Overlay *ov)
{
   int x, y, w, h;
   if (!overlay_get_record_geometry(ov, &x, &y, &w, &h)) return;

   char title[64];
   snprintf(title, sizeof(title), "screenr — %d x %d", w, h);
   elm_win_title_set(ov->win, title);

   /* CONFIRMED REGRESSION, fixed here: the dimensions-changed callback
    * (which overwrites main.edc's dimensions_text on screenr's own
    * MAIN window) must only fire when the Selection window is actually
    * the thing currently visible/relevant -- otherwise ANY touch of
    * fill's geometry (including overlay_rescale()'s own re-apply of
    * fill's CURRENT geometry on a UI scale change, see that function's
    * own comment) silently overwrites whatever dimensions_text is
    * correctly showing for Screen mode (the real desktop resolution)
    * with fill's own stale geometry -- confirmed via real testing:
    * changing Enlightenment's scale setting while running in Screen
    * mode replaced a correct "1920 x 1080" with a stale "100 x 100"
    * (fill's own MIN_FILL_REAL_PX floor, left over from wherever fill
    * was last positioned, unrelated to what Screen mode is actually
    * displaying). elm_win_title_set() above is left unconditional --
    * updating this window's own title is harmless even while hidden,
    * unlike overwriting a DIFFERENT window's UI. */
   if (evas_object_visible_get(ov->win) && g_dimensions_changed_cb)
     g_dimensions_changed_cb(w, h, g_dimensions_changed_cb_data);
}

/*
 * Called by overlay_fill_sync.c's overlay_fill_set_geometry() after
 * every geometry update -- see this function's own declaration in
 * overlay.h for the full reasoning (replaces the old model's Edje
 * "drag"/"drag,set" signal listening, which no longer applies since
 * fill has no Edje drag values to emit such a signal for at all).
 */
void
overlay_on_fill_geometry_changed(void)
{
   if (!g_the_overlay) return;
   _update_titlebar_dimensions(g_the_overlay);
}

/*
 * Computes and applies the Selection window's own real on-screen size
 * -- ov->scale, ov->win_w, ov->win_h, the actual evas_object_resize(),
 * and the min==max size-hint lock. Called once, from overlay_new()
 * only -- NOT reused on UI scale changes (see this function's own
 * REVERTED note below).
 *
 * REVERTED, this session: a UI-scale-aware version of this function
 * was tried (SELECTION_WIN_MAX_W scaled by Elementary's UI scale
 * factor, e.g. 1280 -> 2560 at 200%, matching how the user's own
 * Prevue app scales its own equivalent ceiling) but had ZERO
 * observable effect when tested -- handles rendered in the exact same
 * position as before the change. REAPPLIED, this session: the "zero
 * observable effect" was a red herring -- confirmed via direct user
 * testing (screenshots at scale 1.5 and 2.0) that the ACTUAL root
 * cause was a separate, independent bug in overlay_fill_sync.c's own
 * _reposition_handles(), which only ever called evas_object_move() on
 * the handles, never evas_object_resize() -- so a handle's own
 * on-screen SIZE stayed frozen at whatever it was at app startup
 * regardless of any later scale change, masking whether THIS
 * function's own window-resize logic was doing anything at all. With
 * that bug fixed (handles now correctly resize AND reposition on every
 * scale change), re-testing confirmed the window-resize logic below
 * was correct all along and is being restored as-is.
 */
static void
_apply_window_size(Overlay *ov, double ui_scale)
{
   int scaled_max_w = (int)(SELECTION_WIN_MAX_W * ui_scale);

   if (ov->desktop_w > scaled_max_w)
     {
        ov->scale = (double)scaled_max_w / (double)ov->desktop_w;
        ov->win_w = scaled_max_w;
        ov->win_h = (int)((double)ov->desktop_h * ov->scale);
     }
   else
     {
        ov->scale = 1.0;
        ov->win_w = ov->desktop_w;
        ov->win_h = ov->desktop_h;
     }

   evas_object_resize(ov->win, ov->win_w, ov->win_h);

   /* Locks the window to EXACTLY this size -- min and max set to the
    * SAME value, not just a floor. Re-applied here on every call (not
    * just once) so a scale change correctly updates the lock itself,
    * not just the window's current size -- otherwise the OLD min/max
    * from a previous scale factor would fight the WM against the new
    * size this function just set.
    *
    * CONFIRMED REAL BUG, fixed here: setting min then max
    * unconditionally (in that order, every time) produced a real Evas
    * ERR when the window SHRANK between calls (e.g. 1920 -> 1280):
    * "restricted max width hint is now smaller than restricted min
    * width hint! (1280 < 1920)" -- at the instant max_set(1280) ran,
    * the OLD min hint (1920, from the previous, larger call) was still
    * in effect, so the new max briefly violated min<=max before min
    * itself got updated. Fixed using the standard safe pattern for
    * updating a min/max pair that could move in either direction:
    * clear BOTH hints to 0 (no restriction) first, then set both to
    * their final values -- this guarantees min<=max holds at every
    * intermediate step, regardless of whether the window is growing or
    * shrinking relative to whatever it was before. */
   evas_object_size_hint_min_set(ov->win, 0, 0);
   evas_object_size_hint_max_set(ov->win, 0, 0);
   evas_object_size_hint_min_set(ov->win, ov->win_w, ov->win_h);
   evas_object_size_hint_max_set(ov->win, ov->win_w, ov->win_h);
}

Overlay *
overlay_new(const char *theme_file, int desktop_w, int desktop_h)
{
   Overlay *ov = calloc(1, sizeof(Overlay));
   if (!ov) return NULL;

   /* Ordinary, but DIALOG-typed window -- deliberately NOT
    * elm_win_override_set(), NOT elm_win_alpha_set() (same reasoning
    * as before: standard WM-managed window, nothing exotic). The
    * window TYPE, however, is ELM_WIN_DIALOG_BASIC rather than plain
    * ELM_WIN_BASIC -- confirmed via real testing plus corroborating
    * documentation (Tizen's Elementary API docs, which mirror the same
    * underlying EFL Elm_Win_Type enum) that DIALOG_BASIC gets special
    * handling from the window manager: centered automatically, and no
    * minimize/maximize affordance in the WM's own decorations (close
    * button only). A maximize button in particular would be a real
    * footgun here -- this window's whole sizing model (see
    * SELECTION_WIN_MAX_W above) assumes a fixed size for its entire
    * life; letting the WM offer to maximize it would silently break
    * that assumption via a path this file has no way to intercept.
    * This is also exactly the window type Prevue itself uses for its
    * own single window, for the same reasons. */
   ov->win = elm_win_add(NULL, "screenr-selection", ELM_WIN_DIALOG_BASIC);
   if (!ov->win)
     {
        free(ov);
        return NULL;
     }
   elm_win_title_set(ov->win, "screenr — drag to select a region");
   elm_win_autodel_set(ov->win, EINA_FALSE); /* we manage its lifetime explicitly */

   ov->desktop_w = desktop_w;
   ov->desktop_h = desktop_h;

   /* Real sizing computed by _apply_window_size() (see that function's
    * own comment for the full story) -- called here with 1.0 as a
    * safe initial default; overlay_rescale() (called immediately after
    * this function returns, see main.c's own elm_main()) corrects this
    * to the real UI scale factor before the window is ever shown. */
   _apply_window_size(ov, 1.0);

   /* Rubberband content -- loaded once here, shown/hidden together
    * with the window itself from here on. Fills the window at
    * ov->win_w/win_h -- i.e. 1:1 with the WINDOW's own pixels, which
    * may themselves be a scaled-down representation of the real
    * desktop (see ov->scale). overlay_get_record_geometry() is what
    * accounts for that scale when converting back to real desktop
    * pixels -- this object itself doesn't need to know or care about
    * the distinction. Now holds ONLY "bg" -- fill/handles/Done are all
    * plain, independent Evas objects now, none of them Edje parts at
    * all, see rubberband.edc's own header comment for the full
    * architectural change and why. */
   ov->edje_obj = edje_object_add(evas_object_evas_get(ov->win));
   if (!edje_object_file_set(ov->edje_obj, theme_file, "screenr/rubberband"))
     {
        int err = edje_object_load_error_get(ov->edje_obj);
        fprintf(stderr, "[overlay] failed to load screenr/rubberband from %s: %s\n",
                theme_file, edje_load_error_str(err));
     }
   evas_object_size_hint_weight_set(ov->edje_obj, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   elm_win_resize_object_add(ov->win, ov->edje_obj);
   evas_object_show(ov->edje_obj);

   /* Forces Edje to finish computing "bg"'s real on-screen geometry
    * RIGHT NOW, before overlay_wire_fill_sync()/overlay_done_button_
    * new() below both read it -- same defensive habit already needed
    * once before in this exact file (see overlay_show()'s own matching
    * call and comment: reading a part's geometry before its window has
    * ever been shown/laid out returns stale/zero values). The window
    * hasn't been shown yet at this point either, so this guard is
    * proactive here rather than reactive to an observed bug -- cheap
    * enough to apply on the strength of already having hit this exact
    * class of bug once. */
   edje_object_calc_force(ov->edje_obj);

   /* "bg" is the one remaining Edje part anything in this file reads
    * from (read-only, for the window's own bounds -- see overlay_fill_
    * sync.c's own header comment on why that's safe). Resolved once
    * here and passed to both overlay_wire_fill_sync() and overlay_
    * done_button_new() below, rather than either of them resolving it
    * independently. */
   Evas_Object *bg_obj = (Evas_Object *)edje_object_part_object_get(ov->edje_obj, "bg");
   Evas *evas = evas_object_evas_get(ov->win);

   /* Creates fill and the 4 corner handles as independent Evas
    * objects on EVAS_LAYER_FILL (see overlay_fill_sync.c's own header
    * comment for the full architectural change and why, and its own
    * definition of that layer constant) -- guaranteed to always
    * render above the screenshot (layer 0). Sets fill's initial
    * default geometry itself (middle 50% of the window), so no
    * separate initialization call is needed here. */
   overlay_wire_fill_sync(evas, bg_obj, ov->scale);

   Evas_Object *fill_obj = overlay_fill_sync_get_fill();
   overlay_wire_body_drag(fill_obj);
   overlay_wire_8handle_extras(overlay_fill_sync_get_handle_tl(),
                                overlay_fill_sync_get_handle_tr(),
                                overlay_fill_sync_get_handle_bl(),
                                overlay_fill_sync_get_handle_br());

   /* Creates the Done button as independent Evas objects too, on
    * EVAS_LAYER_FILL + 1 -- one layer above fill/handles, guaranteeing
    * it always renders and hit-tests on top of them regardless of
    * where the user has dragged the selection to (see overlay_done_
    * button.c's own header comment for the full rationale: this, not
    * fill/handles' own layer, is what actually fixes the "Done
    * unclickable when fill overlaps it" bug -- putting Done under the
    * SAME plain-Evas event-routing system as fill/handles, not merely
    * painting it on top of a different subsystem's object). Creation
    * and positioning are deliberately separate calls -- see overlay_
    * done_button_reposition()'s own comment for why: bg_obj has no
    * real geometry yet at this point in startup (the window hasn't
    * been shown/mapped), so this initial reposition call is mostly a
    * formality; the real, correct position is set on every subsequent
    * overlay_show() instead (see that function below). The real click
    * callback isn't registered here -- see overlay_done_button_set_
    * click_callback()'s own comment for why that happens later, from
    * callbacks.c. */
   overlay_done_button_new(evas);
   {
      int init_bx, init_by, init_bw, init_bh;
      evas_object_geometry_get(bg_obj, &init_bx, &init_by, &init_bw, &init_bh);
      (void)init_bx; (void)init_by;
      overlay_done_button_reposition(init_bw, init_bh);
   }

   /* Set BEFORE overlay_wire_fill_sync() above finishes -- that call
    * itself sets fill's initial default geometry, which triggers
    * overlay_on_fill_geometry_changed() -- but g_the_overlay isn't
    * assigned until this line, AFTER that call already ran. That
    * initial call's own overlay_on_fill_geometry_changed() is
    * therefore a harmless no-op (g_the_overlay still NULL at that
    * point) rather than a crash -- acceptable since the titlebar gets
    * set for real anyway on the next actual overlay_show() (see that
    * function's own _update_titlebar_dimensions() call). Assigning
    * here, right after, so every SUBSEQUENT geometry change (from
    * real user drags) correctly reaches the titlebar. */
   g_the_overlay = ov;

   /* Stays hidden until overlay_show() -- avoids a load-time delay on
    * first Selection-mode Record press, matches the pattern used
    * elsewhere in this app. */

   return ov;
}

void
overlay_show(Overlay *ov)
{
   if (!ov) return;

   /* Fresh screenshot every time this window is opened -- a stale shot
    * from a previous session has no guaranteed relationship to the
    * desktop's current state. evas_object_del() on a resize object
    * deletes it outright (not merely hides it) and Elementary drops its
    * own internal reference as part of that deletion -- there is no
    * separate elm_win_resize_object_del() needed before this, and no
    * risk of the old, now-dangling image staying registered against
    * the window after this call. */
   if (ov->shot_image)
     {
        evas_object_del(ov->shot_image);
        ov->shot_image = NULL;
     }

   /* Centered BEFORE the capture call below, not after -- closes a
    * separate, smaller gap between this window's own bare background
    * appearing and its final screenshot content being ready, by
    * front-loading everything that doesn't depend on the screenshot's
    * actual pixels (resize-object registration, elm_win_center()) so
    * showing the window becomes the very next real step after the
    * capture returns. NOTE: an earlier version of this comment
    * described this as fighting a flicker of screenr's OWN window,
    * caused by _screenshot_capture()'s old hide-flush-capture-restore
    * sequence around screenr's main window -- that hide/show has since
    * been removed entirely (see _screenshot_capture()'s own updated
    * header comment for why it was a real correctness bug, not just a
    * cosmetic one), so that particular flicker no longer exists at
    * all; this reordering remains worth keeping for the smaller,
    * separate gap described above. */
   elm_win_center(ov->win, EINA_TRUE, EINA_TRUE);

   Evas *evas = evas_object_evas_get(ov->win);
   int w = 0, h = 0;
   Evas_Object *img = _screenshot_capture(evas, &w, &h);
   if (!img)
     {
        fprintf(stderr, "[overlay] screenshot capture failed -- Selection window not shown\n");
        return;
     }
   ov->shot_image = img;

   /* Registered as a SECOND resize object on the same window (Elementary
    * supports multiple resize objects on one Elm_Win -- each is kept
    * sized to the window's own current geometry independently). The
    * screenshot itself was captured at REAL desktop resolution above
    * (w/h, from _screenshot_capture()) -- filled_set() + smooth_scale_set()
    * (set once, in _screenshot_capture()) means Evas scales it down to
    * whatever size this resize object actually ends up being, i.e.
    * ov->win_w/win_h, automatically -- no manual fill/letterbox math
    * needed, same technique already proven in capture_preview.c. */
   elm_win_resize_object_add(ov->win, ov->shot_image);
   evas_object_move(ov->shot_image, 0, 0);
   evas_object_show(ov->shot_image);
   evas_object_lower(ov->shot_image);

   /* fill and the 4 corner handles are independent Evas objects
    * (created once in overlay_new(), never touched by this function
    * otherwise), permanently on EVAS_LAYER_FILL (see overlay_fill_
    * sync.c's own definition) -- always render above the screenshot
    * (layer 0) with no per-call raising needed at all, regardless of
    * creation/recreation order. See rubberband.edc's own header
    * comment for why these are independent objects rather than Edje
    * parts in the first place.
    *
    * The Done button is ALSO an independent Evas object on its own
    * fixed layer (EVAS_LAYER_FILL + 1, see overlay_done_button.c's own
    * definition) -- but UNLIKE that layer assignment, its POSITION is
    * NOT a one-time thing set at overlay_new() time. CONFIRMED BUG,
    * this session: overlay_done_button_reposition() used to take
    * bg_obj directly and re-read its geometry itself -- a SECOND,
    * independent evas_object_geometry_get() call on the exact same
    * object this function had ALREADY read correctly, one line below
    * (ccw/cch). Confirmed via real testing/logging that the second
    * read could return (0,0,0,0) even though the first read, on the
    * same object, moments earlier in the same function, did not --
    * proven definitively by the fact that fill (which uses only the
    * FIRST read, ccw/cch, passed as plain int parameters) rendered at
    * the exact correct centered position on a genuine first open,
    * while Done (which re-read bg_obj a second time inside its own
    * function) did not. The precise Evas/Edje-internal reason two
    * adjacent reads of one object's geometry could disagree was not
    * fully traced -- but the fix does not depend on knowing that: not
    * reading a second time at all (passing ccw/cch straight through,
    * exactly like fill already does, below) removes the whole class
    * of risk regardless of its underlying cause. */

   /* Resets fill to a fresh default geometry each time this window is
    * (re)opened -- a stale rectangle from a previous session has no
    * guaranteed relationship to this new screenshot. Same default as
    * overlay_wire_fill_sync()'s own initial call: middle 50% of the
    * window on each axis.
    *
    * CONFIRMED REAL BUG, found via the user's own precise sequential
    * testing: this used to read ccw/cch (the window's own bounds) from
    * "bg" directly -- proven reliable for "window already shown once,
    * closed, reopened at the SAME scale" (an earlier round of testing
    * this session), but NOT reliable for a genuinely different
    * sequence: resizing an already-HIDDEN window via overlay_rescale()
    * -> _apply_window_size() (e.g. changing Enlightenment's scale
    * BEFORE ever pressing Record for the first time), then showing
    * that window for the very first time. In that specific case, bg's
    * own Edje-computed geometry could still reflect a stale, pre-
    * resize state even though ov->win_w/win_h (the values _apply_
    * window_size() already computed and stored on ov itself) were
    * correct -- the same underlying "a hidden window's resize doesn't
    * synchronously propagate back into Evas's own geometry model"
    * class of problem already diagnosed once before this session (the
    * original Done-position-at-startup bug). Fixed by using ov->win_w/
    * win_h directly (the authoritative, already-known values) for
    * fill's default-geometry reset and Done's reposition below,
    * instead of re-deriving both from bg's own geometry. bg is still
    * read for ccx/ccy (fill/Done are always positioned relative to the
    * window's own origin, which bg correctly represents regardless of
    * this particular staleness -- only its WIDTH/HEIGHT were ever the
    * unreliable part). */
   Evas_Object *bg_obj = (Evas_Object *)edje_object_part_object_get(ov->edje_obj, "bg");
   int ccx, ccy, ccw_unused, cch_unused;
   evas_object_geometry_get(bg_obj, &ccx, &ccy, &ccw_unused, &cch_unused);
   (void)ccw_unused; (void)cch_unused;
   overlay_fill_set_geometry(ov->win_w / 4, ov->win_h / 4, ov->win_w / 2, ov->win_h / 2);
   overlay_done_button_reposition(ov->win_w, ov->win_h);

   /* Shown immediately once the screenshot and rubberband content are
    * ready -- this is now the very next real step after
    * _screenshot_capture() returns (only cheap, local Evas/Edje calls
    * above it, no further waiting), closing the flicker gap described
    * above. Titlebar dimensions are read AFTER this, not before --
    * see that call's own comment further down for why (a separate,
    * previously-fixed timing bug: reading a part's geometry
    * before the window is shown returns stale/zero values). */
   evas_object_show(ov->win);
   evas_object_raise(ov->win);

   /* NOW read/report the titlebar dimensions -- AFTER the window is
    * genuinely shown. NOTE: a second edje_object_calc_force() call used
    * to sit here and was REMOVED -- confirmed via debug logging (see
    * project notes) that it was silently reverting every manually-
    * positioned RECT part back to its plain .edc-declared rel1/rel2
    * description, discarding our own evas_object_move()/resize() calls
    * from overlay_fill_set_geometry() entirely. It is not needed here:
    * fill's geometry (read by _update_titlebar_dimensions() below, via
    * overlay_fill_get_geometry()) is an object THIS FUNCTION just
    * positioned itself, moments ago -- there is no "Edje hasn't laid
    * it out yet" risk for reading that back, unlike "bg" earlier in
    * this same function (see that earlier, still-correct calc_force()
    * call, which reads from the .edc's OWN rel1/rel2 layout, not from
    * anything C has manually overridden). */
   _update_titlebar_dimensions(ov);
}

void
overlay_hide(Overlay *ov)
{
   if (!ov || !ov->win) return;

   evas_object_hide(ov->win);

   /* Force the X11 unmap through immediately rather than leaving it to
    * the next Ecore main-loop iteration -- evas_object_hide() alone
    * only schedules the underlying unmap request; it does not
    * guarantee the X server (or the WM/compositor sitting on top of
    * it) has actually finished making the window's pixels disappear
    * from the screen by the time this function returns. Callers of
    * overlay_hide() (see callbacks.c's _on_done_clicked()) routinely
    * spawn ffmpeg's x11grab on the very next line, with no other work
    * in between to naturally absorb that latency -- without a forced
    * flush here, there is a real window (no pun intended) where a
    * fragment of this window's own content (confirmed via real
    * testing: a stray corner handle, still visible at the screen's
    * origin) can persist on screen after Done is pressed, either
    * briefly captured by ffmpeg's first frame(s) or simply left
    * visible to the user's own eyes until the next unrelated repaint
    * happens to clear it. ecore_x_flush() forces every pending X
    * request (including this hide's own unmap) to be sent and
    * processed by the X server before this function returns -- the
    * same technique already proven and in active use elsewhere in
    * this exact file and in capture_preview.c, for the same
    * underlying class of "make sure this is REALLY gone before the
    * next thing happens" problem. */
   ecore_x_flush();
}

/*
 * Called once from main.c's own _config_changed_cb() whenever
 * Elementary's UI scale factor changes (and once, unconditionally,
 * right after overlay_new() returns -- see main.c's own elm_main() for
 * why: the very first launch at a non-1.0 scale factor needs this
 * applied immediately, not only on a LATER config-change event that
 * may never fire if the user's scale was already correct at startup).
 *
 * REAPPLIED, this session: an earlier attempt at resizing the
 * Selection window itself here was reverted after appearing to have
 * "zero observable effect" -- that was a red herring, caused by a
 * separate, independent bug in overlay_fill_sync.c's own
 * _reposition_handles() (handles were repositioned but never actually
 * RESIZED on a scale change, masking whether the window's own resize
 * was doing anything). With that bug now fixed and confirmed via
 * direct user testing (screenshots at scale 1.5 and 2.0 showing
 * correctly-sized, correctly-centered handles), this window-resizing
 * logic is being restored.
 *
 * This function now does three things, in this order (order matters):
 *
 *   1. Updates fill/Done's own base-pixel scale factors (overlay_fill_
 *      sync_set_ui_scale(), overlay_done_button_set_ui_scale()).
 *
 *   2. Resizes the Selection window ITSELF, via _apply_window_size().
 *      Must happen BEFORE step 3 below: fill's own clamp logic
 *      (overlay_fill_sync.c's _clamp_geometry()) reads the window's
 *      CURRENT bounds from "bg", so bg must already reflect the new
 *      window size before fill is touched, or the clamp would compute
 *      against stale bounds. The freshly-recomputed ov->scale is also
 *      pushed into overlay_fill_sync.c's own g_scale here (overlay_
 *      fill_sync_set_screenshot_scale()) -- that value is no longer
 *      constant for the window's whole life now that the window itself
 *      can resize, so MIN_FILL_REAL_PX's own real-pixel-to-window-space
 *      conversion needs the current ratio, not the one from app
 *      startup.
 *
 *   3. Proportionally rescales fill's own window-space geometry to
 *      match how much the window itself just grew or shrank -- NOT
 *      simply re-clamped at its old numeric coordinates. If the window
 *      just doubled in size, a selection that was centered and half
 *      the window's own size before must still be centered and half
 *      the window's own size after -- its window-space x/y/w/h values
 *      must all double too, not stay numerically unchanged (which
 *      would visually shrink the selection into a corner as the window
 *      grows around it, or the reverse on a shrink). The ratio is
 *      computed from the window's own OLD vs NEW win_w (captured
 *      before/after step 2), applied to fill's own last-known geometry
 *      (read before step 2 runs, so it's the OLD, pre-resize position/
 *      size being scaled forward). This also re-triggers _reposition_
 *      handles() (called internally by overlay_fill_set_geometry()),
 *      which is what actually resizes AND repositions the handles
 *      themselves at their newly-correct on-screen size.
 *
 * Done's own reposition (overlay_done_button_reposition()) doesn't
 * need this same proportional treatment -- it's always recomputed
 * fresh from the window's own CURRENT bounds (top-right anchored, see
 * that function's own comment), never carries forward a prior
 * position the way fill's user-chosen selection does.
 */
void
overlay_rescale(Overlay *ov, double scale)
{
   if (!ov) return;

   overlay_fill_sync_set_ui_scale(scale);
   overlay_done_button_set_ui_scale(scale);

   int old_win_w = ov->win_w;
   int old_win_h = ov->win_h;
   int fx, fy, fw, fh;
   overlay_fill_get_geometry(&fx, &fy, &fw, &fh);

   _apply_window_size(ov, scale);
   overlay_fill_sync_set_screenshot_scale(ov->scale);

   if (old_win_w > 0 && old_win_h > 0)
     {
        double rx = (double)ov->win_w / (double)old_win_w;
        double ry = (double)ov->win_h / (double)old_win_h;
        overlay_fill_set_geometry((int)(fx * rx), (int)(fy * ry),
                                   (int)(fw * rx), (int)(fh * ry));
     }
   else
     {
        /* old_win_w/h being 0 means this is the very first call (right
         * after overlay_new(), before _apply_window_size() has ever
         * run for real) -- no prior geometry to scale forward from, so
         * just re-apply fill's current geometry as-is. */
        overlay_fill_set_geometry(fx, fy, fw, fh);
     }

   if (ov->win)
     {
        /* CONFIRMED REAL BUG, fixed here: this used to re-read bg's
         * geometry a SECOND time via edje_object_part_object_get() +
         * evas_object_geometry_get(), immediately after _apply_
         * window_size() above had just resized ov->win -- Edje's own
         * recalculation of "bg" (sized relative to ov->edje_obj's
         * resize-object size, itself only updated as a CONSEQUENCE of
         * ov->win's resize) is not guaranteed to have settled
         * synchronously by the time a read like that runs, especially
         * for an already-mapped, already-visible window being resized
         * MID-SESSION (confirmed via direct user testing: scaling
         * BEFORE the Selection window ever opens works correctly every
         * time; scaling an ALREADY-OPEN window intermittently left
         * Done using a stale/wrong width, producing a vanished or
         * left-shifted button). Same underlying class of bug already
         * diagnosed and fixed once before in overlay_show() (see that
         * function's own history) -- fixed the same way: don't re-read
         * a second time at all. _apply_window_size() already computed
         * and knows the window's own intended new size (ov->win_w/
         * win_h) -- passing those directly is both simpler and
         * strictly more correct than asking Edje to confirm a value
         * this function already has. */
        overlay_done_button_reposition(ov->win_w, ov->win_h);
     }
}

void
overlay_free(Overlay *ov)
{
   if (!ov) return;
   /* fill, the 4 corner handles, and Done's own button/text objects
    * are all independent Evas objects (see rubberband.edc's own
    * header comment) -- NOT Edje parts, so deleting ov->win below
    * (which cleans up edje_obj + shot_image via Elementary's own
    * resize-object teardown) does not clean these up automatically.
    * Explicit deletion required, before ov->win itself is deleted
    * (though order doesn't strictly matter here since these objects
    * don't depend on ov->win's own lifetime to be valid to delete --
    * done first anyway, as the more defensive ordering). */
   overlay_fill_sync_free();
   overlay_done_button_free();
   if (ov->win) evas_object_del(ov->win); /* deletes edje_obj + shot_image too */
   free(ov);
}

Evas_Object *
overlay_get_win(Overlay *ov)
{
   if (!ov) return NULL;
   return ov->win;
}

/* Minimum selection size -- same values/reasoning as the old
 * architecture (x11grab + yuv420p requirement: even width/height,
 * reasonable minimum so a degenerate 1x1 selection can't be recorded).
 * Expressed in REAL DESKTOP pixels (post scale-factor conversion, see
 * overlay_get_record_geometry() below) -- ffmpeg's -video_size cares
 * about real pixels, not this window's own on-screen size. */
#define RUBBERBAND_MIN_W 100
#define RUBBERBAND_MIN_H 100

Eina_Bool
overlay_get_record_geometry(Overlay *ov, int *out_x, int *out_y, int *out_w, int *out_h)
{
   if (!ov || !ov->edje_obj) return EINA_FALSE;

   /* Under the new "fill owns its own real geometry" model (see
    * rubberband.edc's own header comment for the full architectural
    * rewrite this session), this function collapses to a simple unit
    * conversion -- fill's window-space geometry, read via
    * overlay_fill_get_geometry(), is ALREADY guaranteed correct
    * (never crossed, never below RUBBERBAND_MIN_W/H once converted
    * through scale, never outside the window) by overlay_fill_set_
    * geometry()'s own clamp, which runs on every single update from
    * every drag mechanism. There is no min/max-of-two-independent-
    * values resolution needed here anymore, and no risk of the old
    * per-value-truncation bug (see project notes for that earlier,
    * now-obsolete investigation) -- fill's x/y/w/h are already a
    * single, self-consistent rectangle by construction. */
   int win_x, win_y, win_w, win_h;
   overlay_fill_get_geometry(&win_x, &win_y, &win_w, &win_h);

   /* Convert window-space pixels to REAL DESKTOP pixels by dividing
    * out ov->scale -- the one-time factor computed in overlay_new().
    * lround() rather than a bare (int) cast: rounds to nearest rather
    * than always truncating toward zero (same reasoning as this
    * function's own earlier version, kept since it's still the
    * correct choice, just applied to far simpler inputs now). */
   int real_x = (int)lround((double)win_x / ov->scale);
   int real_y = (int)lround((double)win_y / ov->scale);
   int real_w = (int)lround((double)win_w / ov->scale);
   int real_h = (int)lround((double)win_h / ov->scale);

   if (real_w < RUBBERBAND_MIN_W) real_w = RUBBERBAND_MIN_W;
   if (real_h < RUBBERBAND_MIN_H) real_h = RUBBERBAND_MIN_H;
   if (real_w % 2 != 0) real_w -= 1;
   if (real_h % 2 != 0) real_h -= 1;

   /* Defensive final clamp to the real desktop's own bounds -- kept
    * from the previous version rather than assumed unnecessary: the
    * scale-division/rounding above, and the even-number snap just
    * above, can each still shift a value by a sub-pixel amount that
    * matters exactly at a screen edge (see project notes for the
    * original real ffmpeg failure this same clamp was written to
    * fix). Lower-stakes now than before (fill's own window-space
    * geometry is already guaranteed in-bounds), but the REAL-pixel
    * conversion happens after that guarantee, so a fresh, cheap check
    * here costs nothing and removes any doubt. */
   if (real_x < 0) real_x = 0;
   if (real_y < 0) real_y = 0;
   if (real_x + real_w > ov->desktop_w) real_x = ov->desktop_w - real_w;
   if (real_y + real_h > ov->desktop_h) real_y = ov->desktop_h - real_h;
   if (real_x < 0) real_x = 0;
   if (real_y < 0) real_y = 0;

   *out_x = real_x;
   *out_y = real_y;
   *out_w = real_w;
   *out_h = real_h;
   return EINA_TRUE;
}
