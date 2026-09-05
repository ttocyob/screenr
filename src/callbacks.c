/*
 * callbacks.c
 *
 * Owns the "UI event -> app state" layer: the Screen/Selection mode
 * toggle, all button clicks on the main window (Record, Refresh,
 * audio/cursor toggles), the Selection window's own Done button, and
 * Escape-key/titlebar-close handling for the Selection window. Record
 * and Done both forward to recorder.c for the actual start/stop logic
 * -- this file owns triggering it, not the ffmpeg process itself.
 */

#include <Edje.h>
#include <Ecore_X.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "callbacks.h"
#include "recorder.h"

/* --------------------------------------------------------------------- */
/* dimensions_text (main.edc) -- shows the active mode's real capture    */
/* resolution: static desktop size in Screen mode, live fill size in     */
/* Selection mode (updated continuously while dragging, via the          */
/* callback wired below).                                                */
/* --------------------------------------------------------------------- */

static void
_dimensions_text_set(App *app, int w, int h)
{
   char buf[32];
   snprintf(buf, sizeof(buf), "%d x %d", w, h);
   edje_object_part_text_set(app->edje_obj, "dimensions_text", buf);
}

/* Callback wrapper matching overlay_set_dimensions_changed_callback()'s
 * own (int w, int h, void *data) signature -- registered once in
 * callbacks_connect() below. Fires on every live fill geometry change
 * while dragging in Selection mode (see overlay.c's own _update_
 * titlebar_dimensions(), which is what actually calls this). */
static void
_on_overlay_dimensions_changed(int w, int h, void *data)
{
   App *app = data;
   _dimensions_text_set(app, w, h);
}

/* --------------------------------------------------------------------- */
/* Mode toggle (Screen / Selection)                                       */
/* --------------------------------------------------------------------- */

void
mode_set(App *app, App_Mode mode)
{
   /* Recording lock: once recording has started, mode is fixed for the
    * duration -- switching capture geometry mid-recording is out of
    * scope and was never discussed as a feature. Guard here so a stray
    * click during recording doesn't do anything surprising. */
   if (app->recording) return;

   /* Same-mode guard: A->A is a no-op, A->B recaptures. Combined with
    * the hide-flush-capture-restore sequence in capture_preview.c (see
    * project notes), this is now sufficient on its own to prevent the
    * recursive-thumbnail bug -- the earlier version of this guard was
    * already correct; what caused the regression was a SEPARATE
    * mechanism (focus-in auto-recapture), which has been dropped
    * entirely rather than patched further. mode_started distinguishes
    * "genuinely already set" from app->mode's zero-value default
    * (APP_MODE_SCREEN = enum 0), so main.c's essential startup call
    * still fires. */
   if (app->mode_started && app->mode == mode) return;
   app->mode_started = EINA_TRUE;

   app->mode = mode;

   if (mode == APP_MODE_SCREEN)
     {
        capture_preview_enter_screen_mode(app->cp);
        edje_object_signal_emit(app->edje_obj, "mode,screen", "app");

        /* Closes the Selection window if it happens to be open when
         * switching to Screen mode -- e.g. the user pressed Record in
         * Selection mode (opening the window, see _on_record_clicked()),
         * then clicked the Screen icon instead of finishing with Done.
         * Without this, the window would linger open while the app's
         * own mode has already switched away from Selection, a
         * confusing, inconsistent state. */
        if (app->overlay) overlay_hide(app->overlay);

        /* Static desktop dimensions, shown once on entering the mode --
         * not live/draggable like Selection mode, but still useful as
         * an at-a-glance confirmation of the actual capture resolution.
         * Written to both the window title AND main.edc's own
         * dimensions_text part (the "X x Y" readout next to the mode
         * toggle) -- falls back to the plain title / a cleared
         * dimensions_text if desktop size isn't known yet (shouldn't
         * normally happen, since capture_preview_new() already queried
         * it directly at app startup, before mode_set() is ever called
         * for the first time -- see main.c -- but kept defensive to
         * match the pattern used everywhere else this accessor is
         * called). */
        int sw, sh;
        if (capture_preview_get_desktop_size(app->cp, &sw, &sh))
          {
             char title[128];
             snprintf(title, sizeof(title), "screenr — %d x %d", sw, sh);
             elm_win_title_set(app->win, title);
             _dimensions_text_set(app, sw, sh);
          }
        else
          {
             elm_win_title_set(app->win, "screenr");
             edje_object_part_text_set(app->edje_obj, "dimensions_text", "");
          }
     }
   else
     {
        capture_preview_enter_selection_mode(app->cp);
        edje_object_signal_emit(app->edje_obj, "mode,selection", "app");

        /* Populates dimensions_text with fill's CURRENT real geometry
         * on Selection mode entry -- not blanked. fill's default
         * (whatever the middle-50%-of-the-window default currently
         * works out to in real desktop pixels, e.g. 960x540) is not a
         * stale or meaningless placeholder: it's the REAL geometry
         * that would actually be recorded if the user pressed Record
         * then Done without ever dragging, so showing it immediately
         * is more accurate than leaving the field blank, not less --
         * confirmed correct per direct testing/feedback. Uses the same
         * overlay_get_record_geometry() accessor _on_done_clicked()
         * already relies on elsewhere in this file. Also covers a real
         * gap the dimensions-changed callback alone can't: that
         * callback isn't registered until callbacks_connect() runs,
         * which happens AFTER overlay_new() already set fill's initial
         * default and fired the callback once (see main.c's own init
         * order) -- so relying on the callback alone would leave
         * dimensions_text unset on a fresh app start until the user's
         * first drag. If app->overlay is NULL (creation failed at
         * startup) or no geometry is available yet, clears the field
         * instead of showing garbage. */
        {
           int rx, ry, rw, rh;
           if (app->overlay && overlay_get_record_geometry(app->overlay, &rx, &ry, &rw, &rh))
             _dimensions_text_set(app, rw, rh);
           else
             edje_object_part_text_set(app->edje_obj, "dimensions_text", "");
        }

        /* Selection mode's overlay window is NOT shown here anymore.
         * Selection mode entry only tracks which mode is active; the
         * actual drag-to-select window only ever opens via
         * _on_record_clicked()'s Selection branch, which is the ONE
         * trigger for overlay_show() in this codebase now. A second
         * call site here (there used to be one, restored in an
         * earlier session under the mistaken belief it was needed for
         * "the user's actual working workflow") meant a user could
         * drag a selection in the window this line opened, then press
         * Record -- which calls overlay_show() again and silently
         * resets fill back to its centered default (see overlay.c's
         * overlay_show()), discarding the drag entirely before Done
         * ever got a chance to snapshot it. That was the real cause of
         * Done reporting a centered 960x540 selection regardless of
         * what was actually dragged. There is now exactly one place in
         * the whole app that shows this window, so there is no longer
         * any path where two separate overlay_show() calls can race
         * against a single drag. */
     }
}

/* NOTE: _on_x_window_configure() has been REMOVED (see project notes).
 * It existed to keep an XShape input hole tracking screenr's own
 * window position as it moved. Under the two-window architecture
 * (see overlay.h), screenr's own window is never inside either
 * overlay window's rectangle at all -- it's an ordinary, separately-
 * stacked WM window -- so no hole, and no position tracking for one,
 * is needed anymore. The struct fields this fed (app->last_sx/sy/sw/sh,
 * app->has_geometry) have since been removed from app.h entirely as
 * part of a broader dead-code cleanup pass -- this NOTE is kept only
 * as a pointer to why _on_x_window_configure() itself doesn't exist,
 * for anyone who goes looking for it. */

static void
_on_screen_clicked(void *data, Evas_Object *obj EINA_UNUSED,
                    const char *emission EINA_UNUSED, const char *source EINA_UNUSED)
{
   fprintf(stderr, "[BTN] screen_icon clicked -> requesting APP_MODE_SCREEN\n");
   mode_set((App *)data, APP_MODE_SCREEN);
}

static void
_on_selection_clicked(void *data, Evas_Object *obj EINA_UNUSED,
                       const char *emission EINA_UNUSED, const char *source EINA_UNUSED)
{
   fprintf(stderr, "[BTN] selection_icon clicked -> requesting APP_MODE_SELECTION\n");
   mode_set((App *)data, APP_MODE_SELECTION);
}

/* --------------------------------------------------------------------- */
/* Record button -- stops if recording; otherwise starts recording       */
/* directly in Screen mode, or opens the Selection window in Selection   */
/* mode (see _on_record_clicked()'s own comment below for why)           */
/* --------------------------------------------------------------------- */

static void
_on_record_clicked(void *data, Evas_Object *obj EINA_UNUSED,
                    const char *emission EINA_UNUSED, const char *source EINA_UNUSED)
{
   App *app = data;

   if (app->recording)
     {
        fprintf(stderr, "[BTN] btn_record clicked -> stopping recording\n");
        recorder_stop(app);
        return;
     }

   if (app->mode == APP_MODE_SELECTION)
     {
        /* Selection mode's Record press opens the drag-to-select
         * window rather than starting recording directly -- recording
         * itself now only ever starts from _on_done_clicked(), once
         * the user has actually drawn a selection and app->sel_* has
         * been snapshotted (see that function's own header comment).
         * Calling recorder_start() here instead would just fail its
         * own has_selection_geometry guard on first use of the
         * session, or -- worse -- silently reuse a STALE selection
         * from a previous recording if one was already taken earlier
         * this session, since app->has_selection_geometry stays TRUE
         * once set. Always re-opening the window here means every
         * Selection-mode recording requires a fresh, deliberate Done
         * press, with no stale-geometry path at all. */
        fprintf(stderr, "[BTN] btn_record clicked -> opening Selection window\n");
        if (app->overlay)
          overlay_show(app->overlay);
        return;
     }

   fprintf(stderr, "[BTN] btn_record clicked -> starting recording\n");
   recorder_start(app);
}

/* --------------------------------------------------------------------- */
/* Manual refresh -- new architecture: no more automatic focus-in         */
/* recapture, so this is the user's explicit way to update a stale        */
/* thumbnail after rearranging other windows on the desktop.              */
/* --------------------------------------------------------------------- */

static void
_on_refresh_clicked(void *data, Evas_Object *obj EINA_UNUSED,
                     const char *emission EINA_UNUSED, const char *source EINA_UNUSED)
{
   App *app = data;
   fprintf(stderr, "[BTN] btn_refresh clicked\n");
   /* Locked during recording, same reasoning as every other capture-
    * changing action in this file -- refreshing the thumbnail mid-
    * recording isn't meaningful and shouldn't do anything surprising. */
   if (app->recording)
     {
        fprintf(stderr, "[BTN] btn_refresh ignored -- recording in progress\n");
        return;
     }
   capture_preview_refresh(app->cp);
}

/* --------------------------------------------------------------------- */
/* Audio / cursor toggles                                                 */
/* --------------------------------------------------------------------- */

static void
_on_audio_toggled(void *data, Evas_Object *obj EINA_UNUSED,
                   const char *emission EINA_UNUSED, const char *source EINA_UNUSED)
{
   App *app = data;
   fprintf(stderr, "[BTN] audio_icon clicked (currently %s)\n",
           app->audio_enabled ? "ON" : "OFF");
   /* Locked once recording starts, same reasoning as mode_set()'s lock --
    * changing what's being captured mid-recording isn't a supported
    * feature. */
   if (app->recording)
     {
        fprintf(stderr, "[BTN] audio_icon ignored -- recording in progress\n");
        return;
     }

   app->audio_enabled = !app->audio_enabled;
   fprintf(stderr, "[BTN] audio_icon -> now %s\n", app->audio_enabled ? "ON" : "OFF");
   edje_object_signal_emit(app->edje_obj,
                            app->audio_enabled ? "app,audio,on" : "app,audio,off",
                            "app");
}

static void
_on_cursor_toggled(void *data, Evas_Object *obj EINA_UNUSED,
                    const char *emission EINA_UNUSED, const char *source EINA_UNUSED)
{
   App *app = data;
   fprintf(stderr, "[BTN] mouse_icon clicked (currently %s)\n",
           app->cursor_enabled ? "ON" : "OFF");
   if (app->recording)
     {
        fprintf(stderr, "[BTN] mouse_icon ignored -- recording in progress\n");
        return;
     }

   app->cursor_enabled = !app->cursor_enabled;
   fprintf(stderr, "[BTN] mouse_icon -> now %s\n", app->cursor_enabled ? "ON" : "OFF");
   edje_object_signal_emit(app->edje_obj,
                            app->cursor_enabled ? "app,cursor,on" : "app,cursor,off",
                            "app");
}

/* --------------------------------------------------------------------- */
/* Tab (mode toggle, screenr's own window only) and Escape (safety-      */
/* critical: backs out of Selection mode, wired to both windows)         */
/* --------------------------------------------------------------------- */

/*
 * ESCAPE: without this, a user with the Selection rubberband-drawing
 * window open has no way to back out short of killing the process
 * externally. Wired to BOTH screenr's own window and the Selection
 * window (see callbacks_connect() below) since it's not certain which
 * one reliably holds keyboard focus while that window is showing --
 * two independent chances of catching the key press rather than
 * betting on one.
 *
 * Under the new architecture this is lower-stakes than it was under
 * the old two-window design (that overlay was fully click-through and
 * full-desktop, genuinely blocking all other interaction if Escape
 * didn't work -- a real lockout occurred during testing before this
 * existed). The new Selection window is an ordinary WM-managed window,
 * so the WM's own close/alt-tab affordances are a real fallback even
 * if this somehow failed -- but keeping this wired costs nothing and
 * remains the intended primary escape hatch.
 *
 * TAB: deliberately scoped to app->win ONLY -- unlike Escape, Tab
 * toggling modes while the Selection window itself has focus would be
 * confusing: that window's whole reason for existing is a single,
 * focused drag-to-select task, and Tab is also a completely ordinary,
 * expected way to move focus BETWEEN that window's own widgets (Done,
 * the fill/handle objects) in any standard desktop environment --
 * hijacking it there for mode-switching would fight that expectation
 * for no real benefit, since Escape/the window's own close button
 * already cover "I want out of Selection mode" from there. Only
 * toggles app->mode + the .edc's own mode signal, via mode_set() --
 * exactly like clicking the Screen/Selection icons directly; does NOT
 * itself open/close the Selection window (Record remains the sole
 * trigger for that, per this codebase's own established design -- see
 * overlay.h's own header comment).
 */
static void
_on_key_down(void *data, Evas *e EINA_UNUSED, Evas_Object *obj, void *event_info)
{
   App *app = data;
   Evas_Event_Key_Down *ev = event_info;

   if (!ev->keyname) return;

   if (strcmp(ev->keyname, "Tab") == 0)
     {
        if (obj != app->win) return;

        App_Mode next = (app->mode == APP_MODE_SCREEN) ? APP_MODE_SELECTION : APP_MODE_SCREEN;
        fprintf(stderr, "[BTN] Tab pressed -> switching to %s\n",
                next == APP_MODE_SELECTION ? "SELECTION" : "SCREEN");
        mode_set(app, next);
        return;
     }

   if (strcmp(ev->keyname, "Escape") != 0) return;

   fprintf(stderr, "[BTN] Escape key pressed (mode=%s)\n",
           app->mode == APP_MODE_SELECTION ? "SELECTION" : "SCREEN");

   /* Falls back to Screen mode entirely, not just closing the window --
    * confirmed via real testing that the expected behavior is "Escape
    * returns to Fullscreen", matching the old architecture's Escape
    * handler. mode_set()'s Screen branch already hides the Selection
    * window as one of its own steps (see mode_set() above), so calling
    * mode_set() here accomplishes both "close the window" and "leave
    * Selection mode" with the one call, rather than these being two
    * separately-tracked behaviors that could drift out of sync with
    * each other. mode_set()'s own same-mode guard makes this a safe
    * no-op if Escape is pressed while already in Screen mode (window
    * not open
    * at all -- e.g. focus landed on screenr's own main window). */
   mode_set(app, APP_MODE_SCREEN);
}

/*
 * Fires when the user clicks the Selection window's own Close (X)
 * button, or otherwise triggers a WM-level close on it (Alt+F4, etc.)
 * -- "delete,request" is the same smart callback main.c wires on
 * screenr's own main window for the same purpose. Before this existed,
 * that button was a dead click: overlay_new() deliberately sets
 * elm_win_autodel_set(EINA_FALSE) on this window (see overlay.c) so
 * its lifetime is managed explicitly rather than auto-deleted out from
 * under us, but nothing was listening for the delete request at all,
 * so clicking Close visibly did nothing. Same target behavior as
 * Escape (see _on_key_down() above) -- confirmed via real testing:
 * closing this window, by whatever means, is expected to fall back to
 * Screen mode, not just hide the window while leaving app->mode at
 * APP_MODE_SELECTION.
 */
static void
_on_overlay_win_del(void *data, Evas_Object *obj EINA_UNUSED, void *event_info EINA_UNUSED)
{
   App *app = data;
   fprintf(stderr, "[BTN] Selection window's own Close button clicked\n");
   mode_set(app, APP_MODE_SCREEN);
}

/*
 * Done button -- snapshots the user's chosen selection rectangle into
 * app->sel_x/y/w/h, hides the Selection window, and starts recording
 * immediately. This is now the ONE trigger for starting a Selection-
 * mode recording -- there is no separate "press Done, then press
 * Record" second step; folding recorder_start() in here removes that
 * extra click/mouse-travel entirely, per explicit request.
 *
 * Order is deliberate and matters:
 *
 *   1. Read geometry FIRST, while the overlay window is still open and
 *      "fill" still holds the user's own chosen rectangle. This is the
 *      one and only place Selection-mode geometry is ever read from
 *      the live overlay -- recorder_start() itself reads app->sel_*
 *      instead of calling overlay_get_record_geometry() (see app.h's
 *      own comment on why: overlay_show() resets "fill" back to its
 *      centered default every time it runs, so reading live geometry
 *      anywhere other than this one earliest-possible moment risks
 *      silently recording the centered default instead of the user's
 *      real selection -- exactly the bug this snapshot exists to
 *      avoid).
 *
 *   2. Hide the window SECOND, before recorder_start() runs. Selection-
 *      mode recording's geometry is expressed in real desktop pixels
 *      (see overlay_get_record_geometry()), and x11grab captures
 *      whatever is actually on screen at that location -- if the
 *      Selection window itself were still visible and happened to
 *      overlap the chosen region, it would appear in the recording.
 *      Hiding first, then spawning ffmpeg, guarantees the window is
 *      already gone from the screen before the very first frame is
 *      grabbed.
 *
 *   3. Call recorder_start() LAST, only once both of the above are
 *      done -- it reads app->sel_* (already populated in step 1) and
 *      app->mode (already APP_MODE_SELECTION, set whenever Selection
 *      mode was entered; unchanged by this handler).
 */
static void
_on_done_clicked(void *data)
{
   App *app = data;
   fprintf(stderr, "[BTN] Done clicked\n");

   if (!app->overlay) return;

   int x, y, w, h;
   if (overlay_get_record_geometry(app->overlay, &x, &y, &w, &h))
     {
        app->sel_x = x;
        app->sel_y = y;
        app->sel_w = w;
        app->sel_h = h;
        app->has_selection_geometry = EINA_TRUE;
        fprintf(stderr, "[BTN] done -- snapshotted selection geometry %dx%d+%d+%d\n",
                w, h, x, y);
     }
   else
     {
        fprintf(stderr, "[BTN] done -- overlay_get_record_geometry() failed, no snapshot taken\n");
     }

   overlay_hide(app->overlay);

   /* Auto-starts recording -- see this function's own header comment.
    * recorder_start() is itself a no-op (returns immediately) if
    * app->recording is already true, if app->has_selection_geometry
    * ended up false above (overlay_get_record_geometry() failed), or
    * if output-path setup fails -- all pre-existing guards in
    * recorder.c, none of which needed to change for this. */
   recorder_start(app);
}

/* --------------------------------------------------------------------- */
/* Wiring                                                                  */
/* --------------------------------------------------------------------- */

void
callbacks_connect(App *app)
{
   /*
    * NOTE: these signal names ("btn,screen,clicked" etc.) must be emitted
    * by main.edc's own mouse,clicked,1 programs (source "app") -- see
    * that file's screen_icon_click / selection_icon_click /
    * audio_icon_click / mouse_icon_click / record_btn_click programs.
    * Each of those requires a matching mouse,down,* program on the
    * same part or the click never fires (found and fixed during the
    * first build/test session -- see project notes).
    *
    * KNOWN GAP, confirmed but not yet fixed: main.edc has no emitter
    * for "btn,refresh,clicked" at all -- no part or program anywhere
    * produces that signal. The C side
    * below is fully wired and _on_refresh_clicked() works correctly if
    * ever triggered, but nothing in the current UI can trigger it; the
    * Refresh button this was built for isn't present in main.edc's
    * current vertical layout. Needs either restoring a Refresh
    * affordance in main.edc, or removing this dead wiring + its C
    * handler, as a deliberate decision rather than silently leaving a
    * half-wired feature in place.
    */
   edje_object_signal_callback_add(app->edje_obj, "btn,screen,clicked", "app",
                                    _on_screen_clicked, app);
   edje_object_signal_callback_add(app->edje_obj, "btn,selection,clicked", "app",
                                    _on_selection_clicked, app);
   edje_object_signal_callback_add(app->edje_obj, "btn,record,clicked", "app",
                                    _on_record_clicked, app);
   edje_object_signal_callback_add(app->edje_obj, "btn,refresh,clicked", "app",
                                    _on_refresh_clicked, app);
   edje_object_signal_callback_add(app->edje_obj, "btn,audio,toggled", "app",
                                    _on_audio_toggled, app);
   edje_object_signal_callback_add(app->edje_obj, "btn,cursor,toggled", "app",
                                    _on_cursor_toggled, app);

   /* Done button -- no longer an Edje part at all (see overlay_done_
    * button.c's own header comment for why: it's now a plain Evas
    * object, created inside overlay_new() itself since it needs bg's
    * real geometry at creation time, same as fill/handles). This call
    * only registers the click callback; the button's own Evas objects
    * already exist by the time callbacks_connect() runs. Guarded the
    * same way as the overlay's other conditional wiring below
    * (app->overlay may be NULL if overlay creation failed at
    * startup). */
   if (app->overlay)
     overlay_done_button_set_click_callback(_on_done_clicked, app);

   /* Selection mode's live dimensions readout -- the Selection
    * window's own titlebar is still updated internally by overlay.c's
    * own overlay_fill_sync.c chain (see overlay_on_fill_geometry_
    * changed()), but main.edc's dimensions_text part (on screenr's own
    * main window, next to the mode toggle) needs App/app->edje_obj to
    * write into, which overlay.c deliberately doesn't know about --
    * this registers the one-directional callback that bridges that
    * gap (see overlay.h's own overlay_set_dimensions_changed_callback()
    * comment for the full reasoning). Guarded the same way as the
    * overlay's other conditional wiring here (app->overlay may be NULL
    * if overlay creation failed at startup). */
   if (app->overlay)
     overlay_set_dimensions_changed_callback(_on_overlay_dimensions_changed, app);

   /*
    * Safety-critical: Escape closes the Selection window if open. Wired
    * to BOTH screenr's own window and the Selection window itself (see
    * that handler's own comment for why). app->overlay may be NULL if
    * overlay creation failed at startup; guarded accordingly.
    */
   evas_object_event_callback_add(app->win, EVAS_CALLBACK_KEY_DOWN, _on_key_down, app);
   if (app->overlay)
     {
        Evas_Object *overlay_win = overlay_get_win(app->overlay);
        if (overlay_win)
          {
             evas_object_event_callback_add(overlay_win, EVAS_CALLBACK_KEY_DOWN, _on_key_down, app);

             /* The Selection window's own Close (X) button -- see
              * _on_overlay_win_del()'s own comment above for why this
              * was previously a dead click. Same smart callback name
              * ("delete,request") main.c uses for screenr's own main
              * window. */
             evas_object_smart_callback_add(overlay_win, "delete,request",
                                             _on_overlay_win_del, app);
          }
     }

   /* NOTE: the old ECORE_X_EVENT_WINDOW_CONFIGURE / _on_x_window_
    * configure() wiring has been REMOVED here (see project notes and
    * the NOTE comment near mode_set() above) -- nothing tracks
    * screenr's own window position for masking purposes anymore under
    * the two-window architecture. */
}
