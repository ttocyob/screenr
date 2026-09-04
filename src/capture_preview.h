#ifndef CAPTURE_PREVIEW_H
#define CAPTURE_PREVIEW_H

#include <Evas.h>
#include <Eina.h>
#include <Ecore_X.h>

typedef struct _Capture_Preview Capture_Preview;

/* Create/destroy. edje_obj is the main window's edje object -- used to
 * resolve screenr's own Evas/window context for the desktop-size query
 * and screenshot capture (see capture_preview_refresh()'s own comment
 * on why the screenshot itself is no longer displayed anywhere,
 * despite still being captured). theme_file is currently UNUSED by
 * this file -- it used to load a second, stray "screenr/rubberband"
 * instance directly onto the main window's own canvas (a real,
 * confirmed leak: a leftover from a pre-overlay.c in-window Selection
 * UI, fixed by removing it entirely -- see this function's own
 * definition in capture_preview.c). Kept in the signature only so
 * main.c's existing call site doesn't need to change; Selection mode's
 * real rubberband/fill UI is owned entirely by overlay.c now. */
Capture_Preview *capture_preview_new(Evas_Object *edje_obj, const char *theme_file);
void capture_preview_free(Capture_Preview *cp);

/* Mode transitions -- called from callbacks.c's mode_set(), once per
 * Screen/Selection toggle. Currently no-ops: kept as real functions
 * purely so mode_set()'s existing call sites don't need to change.
 * Selection mode's actual UI is owned entirely by overlay.c now (see
 * that file's own header comment) -- capture_preview.c has no visual
 * role in Selection mode at all. */
void capture_preview_enter_selection_mode(Capture_Preview *cp);
void capture_preview_enter_screen_mode(Capture_Preview *cp);

/* Forces an immediate recapture of the desktop screenshot. Called only
 * from callbacks.c's manual "Refresh" button handler.
 *
 * KNOWN DEAD UI, confirmed but not yet cleaned up: main.edc has no
 * "preview_swallow" part anymore (the in-window thumbnail preview this
 * function's own edje_object_part_swallow() call targets), so the
 * screenshot captured here is never actually displayed to the user --
 * the swallow call silently no-ops against a part that doesn't exist.
 * This function still matters regardless: it's the only place
 * cp->desktop_w/desktop_h get set (see capture_preview_get_desktop_size()),
 * which Screen-mode recording geometry depends on entirely. A future
 * pass should remove the dead swallow/display half of this function
 * while keeping the screenshot-capture/desktop-size-query half intact.
 *
 * Safe to call as often as needed: hide-flush-capture-restore in
 * _screenshot_capture() guarantees screenr's own window is never
 * included in the captured image, regardless of trigger or timing. */
void capture_preview_refresh(Capture_Preview *cp);

/* NOTE: capture_preview_get_record_geometry() has been REMOVED -- see
 * project notes. Selection mode's real geometry source is now
 * overlay_get_record_geometry(), declared in overlay.h. */

/* Screen mode's recording geometry source -- always the real desktop
 * resolution, queried fresh at capture_preview_new() time and again on
 * every manual Refresh (see capture_preview_refresh()). Returns
 * EINA_FALSE if no query has completed yet. */
Eina_Bool capture_preview_get_desktop_size(Capture_Preview *cp,
                                            int *out_w, int *out_h);

/* Exposes the app's real X11 window handle (Ecore_X_Window), resolved
 * once at capture_preview_new() time. KNOWN DEAD, not yet removed: its
 * one real caller was callbacks.c's _on_record_clicked(), passing this
 * into overlay_show()'s own x_win_to_hide parameter -- that parameter
 * was removed entirely (see overlay.c's own _screenshot_capture()
 * comment for the full story: hiding screenr's own main window before
 * a screenshot the user is about to make a selection from was a real,
 * confirmed correctness bug, not a legitimate technique, under the
 * current architecture). This function itself, and the cp->x_win field
 * it exposes, are left in place rather than removed, since that's a
 * separate cleanup decision from the bug fix that made them dead.
 * Returns 0 if cp is NULL or the handle was never resolved. */
Ecore_X_Window capture_preview_get_x_win(Capture_Preview *cp);

#endif /* CAPTURE_PREVIEW_H */
