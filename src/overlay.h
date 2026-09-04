#ifndef OVERLAY_H
#define OVERLAY_H

#include <Elementary.h>
#include <Eina.h>
#include <Ecore_X.h>

/* Evas layer fill and the 4 corner handles render on (see
 * overlay_fill_sync.c's own evas_object_layer_set() calls) -- one
 * above Evas's default layer (0). Layers control render order at the
 * canvas level, entirely independent of smart-object parentage, which
 * is why layers are used here instead of evas_object_stack_above()/
 * below(): those two functions require both objects to share the same
 * "smart parent" status, and ov->edje_obj/ov->shot_image both have an
 * implicit smart parent from Elementary's elm_win_resize_object_add()
 * (which wraps resize objects in an internal Evas.Box) that plain
 * evas_object_rectangle_add() objects (fill/handles) never get --
 * confirmed via a real Evas ERR during testing ("has no parent but
 * above has smart parent") when stack_above() was tried against them.
 *
 * The Done button (overlay_done_button.c) is set to EVAS_LAYER_FILL +
 * 1 directly on its own two Evas objects, guaranteeing it always
 * renders above fill/handles. IMPORTANT: this layer difference alone
 * is NOT what makes Done reliably clickable -- an earlier version of
 * this fix tried raising ov->edje_obj itself (which then held Done as
 * an Edje part, done_outline) to a higher layer, and while that fixed
 * PAINT order, Done still failed to receive clicks whenever fill
 * visually overlapped it, confirmed via real testing. The actual fix
 * was moving Done out of Edje's object tree entirely into a plain Evas
 * object (see rubberband.edc's own header comment for the full two-
 * part rationale) -- putting every interactive object in this window
 * under ONE single, consistent event-routing system is what makes
 * clicks resolve correctly; the layer difference here is what then
 * makes the PAINT order match that same intent. Shared here (rather
 * than defined privately in overlay_fill_sync.c) since that file and
 * overlay_done_button.c both need the same value. */
#define EVAS_LAYER_FILL 1

/*
 * SELECTION-MODE REBUILD (Kooha-style static-screenshot model -- see
 * NEXT_SESSION_PLAN.md for the full rationale). Replaces the earlier
 * two-window live-overlay architecture entirely:
 *
 *   OLD: a full-desktop click-through override-redirect window plus a
 *   second small always-solid override-redirect window tracking the
 *   rubberband's live bounding box, with the real desktop visible and
 *   interactive underneath the whole time. Source of a long chain of
 *   real bugs (XShape mask corruption, live-tracking feedback loops --
 *   see project notes) precisely because the rubberband coexisted with
 *   a live, interactive desktop under it.
 *
 *   NEW: ONE ordinary (NOT override-redirect, NOT alpha), normal
 *   WM-managed window. When opened, it shows a STATIC screenshot of the
 *   full desktop -- a plain picture, nothing underneath is live -- with
 *   the "screenr/rubberband" Edje group overlaid on top at 1:1 scale.
 *   The user drags the rubberband on top of that picture using
 *   perfectly ordinary widget behavior. There is no XShape mask, no
 *   click-through requirement, and no live window-tracking: this
 *   window's own size is fixed (the desktop's size) for its entire
 *   life. Real recording only starts once this window has been closed
 *   entirely -- see project notes for the full Kooha-style flow.
 *
 * CURRENT TRIGGER DESIGN, for callbacks.c: unlike the old architecture
 * (which showed/hid the overlay at Selection-mode ENTRY), this window
 * has nothing to show until the user actually presses Record --
 * there is no live desktop-covering state for "I am in Selection mode
 * but haven't pressed Record yet". callbacks.c's mode_set() does
 * nothing overlay-related in its Selection branch beyond mode
 * bookkeeping (app->mode, the .edc signal). The real trigger points
 * are:
 *   - overlay_show() is called from _on_record_clicked()'s Selection
 *     branch, the moment Record is pressed while in Selection mode.
 *   - overlay_hide() is called from _on_done_clicked(), the moment
 *     Done is pressed -- which also snapshots the chosen geometry
 *     into app->sel_x/y/w/h and starts recording immediately (see
 *     that function's own header comment in callbacks.c).
 * Both are the ONE call site each in the whole app; no other code
 * path shows or hides this window.
 */
typedef struct _Overlay Overlay;

/* Creates the window and loads the rubberband Edje group into it, but
 * does NOT show it and does NOT take a screenshot yet -- call
 * overlay_show() to do both of those together, at the point Selection
 * mode's Record flow actually needs this window to appear.
 * theme_file is the compiled .edj containing "screenr/rubberband".
 * desktop_w/desktop_h size the window to the full real desktop
 * resolution (1:1 display, per current design decision -- see project
 * notes' OPEN QUESTIONS, now resolved). Returns NULL on failure --
 * non-fatal for the app as a whole, Selection mode just won't be
 * functional. */
Overlay *overlay_new(const char *theme_file, int desktop_w, int desktop_h);

/* Takes a fresh screenshot, swallows it into the window, and shows the
 * window. This is the real entry point into "Selection mode's
 * rubberband-drawing step" -- call it when the user presses Record
 * while in Selection mode (not on mode entry itself; see
 * NEXT_SESSION_PLAN.md's Kooha-style flow, step 2). Does NOT hide
 * screenr's own main window before capturing -- an earlier version did
 * (a pattern inherited from an older architecture where screenr's main
 * window was large enough to plausibly overlap the region being
 * selected), but this was a real, confirmed correctness bug under the
 * current architecture: the user chooses their selection by looking at
 * their REAL desktop, where screenr's own (now small) window IS
 * visible, so a screenshot that silently omitted it no longer matched
 * what the user was actually looking at -- see overlay.c's own
 * _screenshot_capture() for the full reasoning. Screenr appearing in
 * this screenshot, and potentially in the final recording if the
 * user's own selection happens to include it, is correct and expected
 * now. */
void overlay_show(Overlay *ov);

/* Hides the window. Does NOT destroy it -- overlay_show() can be
 * called again later without recreating anything (each Selection-mode
 * recording opens a fresh window via Record, closes it via Done -- see
 * this file's own header comment). Called from two places: Done
 * (_on_done_clicked(), the normal path -- geometry is snapshotted
 * first, then this hides the window before recording starts) and the
 * Selection window's own Escape/titlebar-close handlers (a cancel
 * path -- see callbacks.c's _on_key_down()/_on_overlay_win_del(),
 * which fall back to Screen mode without ever calling
 * recorder_start()). */
void overlay_hide(Overlay *ov);

/* Destroys the window and frees the handle. Call once, at app
 * shutdown. */
void overlay_free(Overlay *ov);

/* Called from main.c's own scale-change handling (see that file's own
 * _config_changed_cb() and its call right after overlay_new() at
 * startup) whenever Elementary's UI scale factor changes. Dispatches
 * the new scale to fill/handles and Done's own scale-aware sizing
 * (overlay_fill_sync.c, overlay_done_button.c) and immediately
 * re-applies both -- see overlay.c's own definition for the full
 * reasoning, including why SELECTION_WIN_MAX_W/ov->scale are
 * deliberately untouched. */
void overlay_rescale(Overlay *ov, double scale);

/* Exposes the window's own Evas_Object (its Elm_Win) -- needed for
 * wiring an Escape-key handler, same reasoning as the old
 * architecture's overlay_get_win(). Returns NULL if ov is NULL. */
Evas_Object *overlay_get_win(Overlay *ov);

/* Done button -- overlay_done_button.c. NOT an Edje part (see
 * rubberband.edc's own header comment for the full two-part
 * architectural rationale: putting Done under the same plain-Evas
 * event-routing system as fill/handles is what actually fixes the
 * "Done unclickable when fill overlaps it" bug -- layer-based paint
 * ordering alone could not, confirmed via real testing).
 *
 * overlay_done_button_new() creates the button's two Evas objects
 * (rectangle + text) but does NOT position them -- called once from
 * overlay_new(), which has no real window geometry to position
 * against yet (the Selection window hasn't been shown/mapped at that
 * point in startup). overlay_done_button_reposition() gives the
 * button its real on-screen position, given the window's own current
 * bounds (bw x bh, window-space pixels) -- must be called both once
 * right after overlay_done_button_new() AND every time overlay_show()
 * runs (exactly mirroring how overlay_fill_set_geometry()'s own
 * centered-default reset already works there). Takes bw/bh as plain
 * int parameters rather than an Evas_Object* to re-read geometry from
 * -- confirmed via real testing that a SECOND, independent geometry
 * read of the same "bg" object overlay_show() had already read
 * correctly, moments earlier, could return stale/zero values even
 * though the first read did not; passing the already-known-good
 * values straight through removes that whole class of risk (see
 * overlay_done_button.c's own header comment for the full story).
 * overlay_done_button_set_click_callback() registers the real click
 * callback separately, called once from callbacks.c's
 * callbacks_connect(), which runs later in main.c's own init sequence
 * and does have an App* by then. */
void overlay_done_button_new(Evas *evas);
void overlay_done_button_reposition(int bw, int bh);
void overlay_done_button_set_click_callback(void (*click_cb)(void *data), void *click_cb_data);
void overlay_done_button_free(void);

/* Sets Elementary's UI scale factor for Done's own button/text sizing
 * -- same reasoning as overlay_fill_sync_set_ui_scale()'s own comment.
 * Called once from overlay.c's own overlay_rescale(). Defined in
 * overlay_done_button.c. */
void overlay_done_button_set_ui_scale(double scale);

/* Wiring helpers -- see overlay_body_drag.c, overlay_fill_sync.c,
 * overlay_handle_hit_ext.c. Called internally from overlay_new().
 * Declared here since those files define these.
 *
 * fill and the 4 corner handles are plain, independent Evas objects
 * now (created by overlay_wire_fill_sync(), NOT Edje parts) -- see
 * rubberband.edc's own header comment for the full architectural
 * change and why. overlay_wire_body_drag()/overlay_wire_8handle_
 * extras() below take these objects directly rather than resolving
 * them from an Edje object, since there is no longer an Edje object
 * to resolve them from. */
void overlay_wire_body_drag(Evas_Object *fill_obj);

/* overlay_fill_sync.c's own wiring -- creates fill and the 4 corner
 * handles as independent Evas objects on the given evas, reads bg_obj
 * (the rubberband edje object's own "bg" part, resolved once by
 * overlay.c) for the window's own bounds, and takes the Selection
 * window's one-time scale factor (window-space px per real-desktop
 * px, see overlay.c's own ov->scale) to convert the real-pixel minimum
 * size into window-space -- see that file's own header comment for
 * the full derivation. Sets fill to its initial default geometry. */
void overlay_wire_fill_sync(Evas *evas, Evas_Object *bg_obj, double scale);

/* Wires the 4 corner handles, given the objects directly (see overlay_
 * fill_sync_get_handle_*() below for how overlay.c obtains them after
 * overlay_wire_fill_sync() has created them). Call once from
 * overlay_new(), AFTER overlay_wire_fill_sync() has already run (fill
 * needs a real initial geometry for each handle's own first mouse-down
 * to read). */
void overlay_wire_8handle_extras(Evas_Object *handle_tl, Evas_Object *handle_tr,
                                  Evas_Object *handle_bl, Evas_Object *handle_br);

/* Sets Elementary's UI scale factor (e.g. 2.0 at 200% HiDPI), used to
 * scale the base pixel size of the 4 corner handles (HANDLE_PX) --
 * NOT the same value as ov->scale (overlay.c)/the file-static g_scale
 * (overlay_fill_sync.c) -- see overlay_fill_sync_set_screenshot_
 * scale()'s own comment just below for that separate, related setter,
 * and overlay_fill_sync.c's own g_scale_ui comment for the full
 * distinction between the two. Called once from overlay.c's own
 * overlay_rescale() -- see that function's own comment for the full
 * dispatch chain from main.c's config-changed handler. Defined in
 * overlay_fill_sync.c. */
void overlay_fill_sync_set_ui_scale(double scale);

/* Sets g_scale (the screenshot-fit ratio, mirrors overlay.c's own
 * ov->scale) after a UI-scale-driven Selection window resize -- needed
 * because g_scale is no longer constant for the window's whole life
 * now that the window itself can resize (see overlay.c's own
 * _apply_window_size()). Called once from overlay.c's own
 * overlay_rescale(), right after _apply_window_size() has recomputed
 * ov->scale. Defined in overlay_fill_sync.c. */
void overlay_fill_sync_set_screenshot_scale(double scale);

/* Accessors for the independent fill/handle objects overlay_wire_
 * fill_sync() creates -- needed by overlay.c to wire body-drag/corner-
 * drag (passing them into the two functions above) and to raise them
 * above the screenshot on every overlay_show() (see overlay_fill_
 * raise_above() below). Only valid after overlay_wire_fill_sync() has
 * run; return NULL before that. Defined in overlay_fill_sync.c. */
Evas_Object *overlay_fill_sync_get_fill(void);
Evas_Object *overlay_fill_sync_get_handle_tl(void);
Evas_Object *overlay_fill_sync_get_handle_tr(void);
Evas_Object *overlay_fill_sync_get_handle_bl(void);
Evas_Object *overlay_fill_sync_get_handle_br(void);

/* Raises fill and all 4 handles above reference_obj (typically the
 * Selection window's screenshot, ov->shot_image) -- call from
 * overlay_show() every time the window is (re)shown, since the
 * screenshot object is deleted and recreated fresh on every open,
 * which would otherwise leave fill/handles stacked below a newly-
 * created screenshot object (Evas stacks newly-added objects on top
 * by default). Defined in overlay_fill_sync.c. */
void overlay_fill_raise_above(Evas_Object *reference_obj);

/* Deletes fill and all 4 handles -- call once from overlay_free() at
 * app shutdown. These are independent Evas objects, not Edje parts, so
 * they are NOT automatically cleaned up by edje_object_del() or the
 * Selection window's own teardown; this explicit deletion is required.
 * Defined in overlay_fill_sync.c. */
void overlay_fill_sync_free(void);

/* THE real geometry accessors, now owned by overlay_fill_sync.c --
 * fill IS the selection's real geometry under the new model, in
 * window-space pixels (the Selection window's own on-screen pixels).
 * overlay_fill_set_geometry() clamps (minimum size in REAL desktop
 * pixels, converted via scale; never outside the window) and applies;
 * overlay_fill_get_geometry() reads fill's current real Evas geometry.
 * Every drag mechanism (overlay_body_drag.c, overlay_handle_hit_ext.c)
 * calls these instead of touching fill's Evas object directly, and
 * neither of them has any clamping logic of its own -- it all lives in
 * overlay_fill_sync.c, in one place. */
void overlay_fill_set_geometry(int x, int y, int w, int h);
void overlay_fill_get_geometry(int *out_x, int *out_y, int *out_w, int *out_h);

/* Reads fill's current window-space geometry (via overlay_fill_get_
 * geometry() above) and converts it to real desktop pixel coordinates
 * via the Selection window's own one-time scale factor -- this is now
 * a simple, direct conversion (window-space pixels / scale), with no
 * handle-value indirection at all, since fill's own geometry already
 * IS the real state under the new model. Returns EINA_FALSE if ov or
 * its edje object is NULL. Defined in overlay.c. */
Eina_Bool overlay_get_record_geometry(Overlay *ov, int *out_x, int *out_y, int *out_w, int *out_h);

/* Hook called by overlay_fill_sync.c's overlay_fill_set_geometry(),
 * after every single geometry update from every drag mechanism (body-
 * drag, corner handles) -- this is what replaces the old model's Edje
 * "drag"/"drag,set" signal listening for driving the live titlebar
 * dimensions readout, now that fill has no Edje drag values to emit
 * such a signal for at all. Defined in overlay.c (which owns the
 * window/titlebar), declared here so overlay_fill_sync.c (which owns
 * fill's geometry) can call it without needing to know anything about
 * windows or titlebars itself -- a small, one-directional bridge
 * between the two files' otherwise-separate responsibilities. No-op
 * if no Overlay currently exists yet (e.g. called during
 * overlay_new()'s own initial overlay_wire_fill_sync() call, before
 * overlay_new() has returned a valid Overlay* to anything). */
void overlay_on_fill_geometry_changed(void);

/* Registers a callback that fires every time fill's live dimensions
 * change (see overlay.c's own _update_titlebar_dimensions(), called
 * from overlay_on_fill_geometry_changed() above) -- called once from
 * callbacks.c's callbacks_connect(), which is what actually knows
 * about App/app->edje_obj and writes the reported w/h into main.edc's
 * dimensions_text part. Same one-directional-hook pattern already
 * established for overlay_done_button.c's own click callback: this
 * file (overlay.c) never needs to know about App or app->edje_obj at
 * all. Safe to leave unregistered -- dimensions_text simply never
 * updates from Selection mode in that case, nothing else is affected. */
void overlay_set_dimensions_changed_callback(void (*cb)(int w, int h, void *data), void *data);

#endif /* OVERLAY_H */
