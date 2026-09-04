/*
 * capture_preview.c
 *
 * Owns everything that lives inside the main window's "preview_swallow"
 * Edje part -- Screen mode's static desktop screenshot/thumbnail -- plus
 * the real desktop resolution query used by both Screen and Selection
 * mode's recording geometry.
 *
 * Does NOT own any rubberband/fill/handle UI -- Selection mode's entire
 * drag-to-select interface lives in overlay.c, on its own separate
 * WM-managed window (see overlay.h's header comment for that
 * architecture). This file used to also load a second, independent
 * "screenr/rubberband" instance directly onto the MAIN window's own
 * canvas, as a leftover from an earlier in-window Selection design --
 * that was a real, confirmed bug (a stray, never-fully-hidden fragment
 * of Selection UI visible on the main window itself) and has been
 * removed entirely, not merely disabled.
 *
 * Design constraints this respects (see /areas/efl-ffmpeg-capture.md):
 *   - Edje owns layout/chrome; this file owns only the swallowed content.
 *   - The screenshot is a STILL, taken fresh on manual refresh or mode
 *     entry -- never a live feed.
 *   - The app's own window is never hidden to take the shot — it is fine,
 *     by design, for the app's own footprint to appear in the screenshot.
 */

#include <Eina.h>
#include <Evas.h>
#include <Ecore.h>
#include <Ecore_Evas.h>
#include <Ecore_X.h>
#include <Edje.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RUBBERBAND_MIN_W 100
#define RUBBERBAND_MIN_H 100
typedef struct _Capture_Preview
{
   Evas_Object *edje_obj;        /* the main window's edje object, for swallow */
   Ecore_X_Window x_win;         /* the app window's real X11 handle, resolved
                                     once at creation via
                                     ecore_evas_window_get(). No longer used
                                     for pointer grab/ungrab (that whole
                                     mechanism was specific to the old
                                     hand-rolled C mouse tracking, now
                                     removed) -- kept in case a future need
                                     for the raw window handle arises. */
   Evas_Object *shot_image;      /* the still screenshot, swallowed into Edje */

   int desktop_w, desktop_h;     /* real X11 root window size */
   int shot_disp_x, shot_disp_y; /* screenshot's on-screen position (canvas coords) */
   int shot_disp_w, shot_disp_h; /* screenshot's on-screen displayed size (scaled) */
} Capture_Preview;

static Capture_Preview *g_cp = NULL; /* single control window -> single instance is fine */

/* ---- forward decls ---- */

/*
 * Queries the real desktop resolution directly via X11 -- no screenshot,
 * no window hide/show, just the root window's size. This is now the
 * ONLY source of cp->desktop_w/desktop_h: previously that was a side
 * effect of _screenshot_capture()'s hide-flush-capture-restore sequence
 * (called from capture_preview_refresh(), called from the mode-enter
 * functions), which was correctly removed once the in-app preview was
 * dropped from main.edc -- but removing it also silently took the
 * desktop-size query down with it, breaking
 * capture_preview_get_desktop_size() for Screen-mode recordings. This
 * function restores that capability on its own, decoupled from any
 * screenshot/preview logic.
 */
static void
_query_desktop_size(Capture_Preview *cp)
{
   Ecore_X_Window root = ecore_x_window_root_first_get();
   ecore_x_window_size_get(root, &cp->desktop_w, &cp->desktop_h);
}

/* --------------------------------------------------------------------- */
/* Screenshot capture                                                     */
/* --------------------------------------------------------------------- */

/*
 * Captures a still frame of the X11 root window. Does NOT hide
 * screenr's own window first -- an earlier version did, via cp->x_win,
 * but this screenshot is never actually displayed anywhere (main.edc
 * has no "preview_swallow" part for it to be shown in -- see this
 * file's own header comment and capture_preview.h's comment on
 * capture_preview_refresh()), so hiding screenr's own window before
 * capturing had zero effect on anything anyone would ever see; it was
 * pointless motion, for a different reason than the identical pattern
 * removed from overlay.c's own _screenshot_capture() (which WAS a real
 * correctness bug, since that screenshot genuinely is shown to the
 * user -- see that file's own comment for the full story). This
 * function's only real remaining job is populating cp->desktop_w/
 * desktop_h and cp->shot_image (the latter still created and swallowed
 * into "preview_swallow" by capture_preview_refresh(), even though
 * nothing currently displays it -- see that function's own known-gap
 * comment).
 */
static Evas_Object *
_screenshot_capture(Capture_Preview *cp EINA_UNUSED, Evas *evas, int *out_w, int *out_h)
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
        fprintf(stderr, "[capture_preview] ecore_x_image_new failed\n");
        return NULL;
     }

   if (!ecore_x_image_get(xim, root, 0, 0, 0, 0, screen_w, screen_h))
     {
        fprintf(stderr, "[capture_preview] ecore_x_image_get failed\n");
        ecore_x_image_free(xim);
        return NULL;
     }

   int bpl = 0, rows = 0, bpp = 0;
   void *pixels = ecore_x_image_data_get(xim, &bpl, &rows, &bpp);
   if (!pixels)
     {
        fprintf(stderr, "[capture_preview] ecore_x_image_data_get failed\n");
        ecore_x_image_free(xim);
        return NULL;
     }

   Evas_Object *img = evas_object_image_add(evas);
   evas_object_image_size_set(img, screen_w, screen_h);
   evas_object_image_colorspace_set(img, EVAS_COLORSPACE_ARGB8888);
   evas_object_image_alpha_set(img, EINA_FALSE);
   evas_object_image_filled_set(img, EINA_TRUE);
   evas_object_image_smooth_scale_set(img, EINA_TRUE);
   evas_object_size_hint_aspect_set(img, EVAS_ASPECT_CONTROL_BOTH, screen_w, screen_h);

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

void
capture_preview_refresh(Capture_Preview *cp)
{
   if (!cp || !cp->edje_obj) return;

   Evas *evas = evas_object_evas_get(cp->edje_obj);

   if (cp->shot_image)
     {
        evas_object_del(cp->shot_image);
        cp->shot_image = NULL;
     }

   int w = 0, h = 0;
   Evas_Object *img = _screenshot_capture(cp, evas, &w, &h);
   if (!img) return;

   cp->shot_image = img;
   cp->desktop_w = w;
   cp->desktop_h = h;

   edje_object_part_swallow(cp->edje_obj, "preview_swallow", cp->shot_image);
   evas_object_show(cp->shot_image);

   edje_object_calc_force(cp->edje_obj);
   evas_object_geometry_get(cp->shot_image,
                             &cp->shot_disp_x, &cp->shot_disp_y,
                             &cp->shot_disp_w, &cp->shot_disp_h);
}

/* --------------------------------------------------------------------- */
/* NOTE: this file's own in-window rubberband mechanism (cp->rubberband,
 * _rubberband_set_default_positions(), _on_handle_drag(), the
 * RB_Handle_Id enum, cp->active_drag_handle, cp->selection_mode) has
 * been REMOVED ENTIRELY -- confirmed to be a genuine, active bug, not
 * merely unused code. cp->rubberband was a second, independent
 * "screenr/rubberband" Edje instance, loaded directly onto the MAIN
 * window's own canvas (see the old capture_preview_new()'s own
 * edje_object_add(evas_object_evas_get(edje_obj)) call, where edje_obj
 * was app->edje_obj) -- a leftover from a pre-overlay.c architecture
 * where Selection mode's rubberband lived inside the main window
 * itself, rather than in its own separate WM-managed window (see
 * overlay.c/overlay.h's own header comments on the current
 * architecture). This stray object was shown by
 * capture_preview_enter_selection_mode() and never hidden again except
 * by capture_preview_enter_screen_mode() -- exactly matching a real,
 * reported symptom: a small colored fragment (matching whatever the
 * CURRENT rubberband.edc's handle_tl/tr/bl/br parts are colored,
 * since this stray instance loads the same current compiled group)
 * visible at the main window's own top-left corner, appearing the
 * instant Selection mode was entered -- before the real Selection
 * window (owned by overlay.c) was ever shown, and persisting even
 * during recording, since nothing ever hid it again once shown. Two
 * independent, separately-created "screenr/rubberband" instances were
 * coexisting in the same process, one correctly on the Selection
 * window's own canvas (overlay.c's ov->edje_obj) and one incorrectly
 * on the main window's canvas (this file's old cp->rubberband) -- both
 * loading the SAME compiled group, which is exactly why coloring a
 * handle part in rubberband.edc changed both instances at once. This
 * file's own rubberband mechanism additionally targeted "handle_a"/
 * "handle_b" Edje dragable parts that no longer exist at all in the
 * current rubberband.edc (rewritten to plain-RECT handle_tl/tr/bl/br
 * with no dragable{} block -- see that file's own header comment) --
 * so even setting aside the leak, this code had already been silently
 * failing on every call for some time. */
/* --------------------------------------------------------------------- */

/* --------------------------------------------------------------------- */
/* Coordinate translation: displayed (scaled) -> real desktop pixels      */
/* --------------------------------------------------------------------- */

Eina_Bool
capture_preview_get_desktop_size(Capture_Preview *cp, int *out_w, int *out_h)
{
   if (!cp || !cp->desktop_w || !cp->desktop_h) return EINA_FALSE;
   *out_w = cp->desktop_w;
   *out_h = cp->desktop_h;
   return EINA_TRUE;
}

Ecore_X_Window
capture_preview_get_x_win(Capture_Preview *cp)
{
   if (!cp) return 0;
   return cp->x_win;
}

/* --------------------------------------------------------------------- */
/* Mode switching                                                         */
/* --------------------------------------------------------------------- */

/*
 * Both of these are now pure no-ops -- kept as real functions (rather
 * than removed and their call sites deleted from callbacks.c's
 * mode_set()) purely so that call site doesn't need to change. Their
 * old job was entirely about showing/hiding the stray cp->rubberband
 * object removed above; Selection mode's actual UI (the drag-to-select
 * window) is owned entirely by overlay.c now, opened via
 * overlay_show() from callbacks.c's _on_record_clicked() and closed
 * via overlay_hide() from _on_done_clicked() -- capture_preview.c has
 * no remaining role in Selection mode's visuals at all.
 */
void
capture_preview_enter_selection_mode(Capture_Preview *cp EINA_UNUSED)
{
}

void
capture_preview_enter_screen_mode(Capture_Preview *cp EINA_UNUSED)
{
}

/* --------------------------------------------------------------------- */
/* Lifecycle                                                              */
/* --------------------------------------------------------------------- */

Capture_Preview *
capture_preview_new(Evas_Object *edje_obj, const char *theme_file EINA_UNUSED)
{
   Capture_Preview *cp = calloc(1, sizeof(Capture_Preview));
   if (!cp) return NULL;

   cp->edje_obj = edje_obj;
   Evas *evas = evas_object_evas_get(edje_obj);

   Ecore_Evas *ee = ecore_evas_ecore_evas_get(evas);
   cp->x_win = (Ecore_X_Window)ecore_evas_window_get(ee);

   g_cp = cp;
   _query_desktop_size(cp);
   return cp;
}

void
capture_preview_free(Capture_Preview *cp)
{
   if (!cp) return;
   if (cp->shot_image) evas_object_del(cp->shot_image);
   if (g_cp == cp) g_cp = NULL;
   free(cp);
}
