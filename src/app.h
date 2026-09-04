#ifndef APP_H
#define APP_H

#include <Elementary.h>
#include <Ecore.h>

#include "capture_preview.h"
#include "overlay.h"

typedef enum {
   APP_MODE_SCREEN,
   APP_MODE_SELECTION
} App_Mode;

typedef struct _App
{
   Evas_Object *win;
   Evas_Object *edje_obj;
   Capture_Preview *cp;

   /* Selection mode's live desktop overlay window (see project notes,
    * "MAJOR ARCHITECTURE"). Created once at startup, shown/hidden as
    * Selection mode is entered/left -- not created/destroyed per mode
    * switch, matching the same "create once, show/hide" pattern used
    * for other long-lived resources in this app. */
   Overlay *overlay;

   App_Mode mode;
   Eina_Bool mode_started; /* tracks whether mode_set() has been called at
                               least once -- see callbacks.c's mode_set()
                               for why this must be separate from
                               app->mode itself (APP_MODE_SCREEN's enum
                               value 0 is indistinguishable from
                               "genuinely set" otherwise). */
   Eina_Bool recording;

   Ecore_Timer *tick_timer;
   int elapsed_seconds;

   /* Owned by recorder.c: the running ffmpeg child process, if any. NULL
    * when idle. */
   Ecore_Exe *ffmpeg_exe;

   /* User-controllable recording toggles. Both default per project
    * decision: audio_enabled starts FALSE (avoids an accidental
    * background-audio track), cursor_enabled starts TRUE (pointer
    * position is usually meaningful in a UI/dev screen recording).
    * recorder_start() reads these when building the ffmpeg command;
    * callbacks.c flips them on click and drives the .edc's
    * audio_icon/mouse_icon visual state via edje_object_signal_emit(). */
   Eina_Bool audio_enabled;
   Eina_Bool cursor_enabled;

   /* Safety-critical shutdown handling (see project notes): if the user
    * closes the window while a recording is in progress, we must not
    * let the ffmpeg child process become orphaned and keep recording
    * invisibly forever. quit_after_stop marks that _on_win_del() has
    * requested a graceful stop and is waiting for _on_ffmpeg_exit() to
    * confirm the process has genuinely died before actually calling
    * elm_exit(). quit_safety_timer is a bounded (5s) escalation timer:
    * if the graceful stop hasn't been confirmed by the time it fires,
    * _force_quit_timeout() force-kills the process via ecore_exe_kill()
    * rather than leaving the app hung waiting indefinitely. */
   Eina_Bool quit_after_stop;
   Ecore_Timer *quit_safety_timer;

   /* Selection mode's chosen recording geometry, in REAL DESKTOP
    * pixels -- snapshotted ONCE, at the moment the user presses Done
    * on the Selection window (see callbacks.c's _on_done_clicked()),
    * by reading overlay_get_record_geometry() while the window is
    * still open and "fill" still holds the user's own chosen
    * rectangle. This is deliberately NOT read live from the overlay
    * at recorder_start() time: overlay_show() resets "fill" back to
    * its centered 50%-of-window default every time it runs (see
    * overlay.c), so any code path that re-shows or recreates the
    * overlay between Done and the real ffmpeg spawn would silently
    * clobber the user's actual selection back to the centered
    * default -- which is exactly the bug this field exists to avoid.
    * has_selection_geometry distinguishes "Done was pressed, a real
    * selection exists" from "Selection mode was entered but Done was
    * never pressed this session" (e.g. app just started, or the user
    * backed out via Escape/window-close instead) -- recorder_start()
    * must refuse to start in Selection mode when this is EINA_FALSE
    * rather than silently falling back to whatever the overlay
    * happens to currently contain. */
   int   sel_x, sel_y, sel_w, sel_h;
   Eina_Bool has_selection_geometry;
} App;

#endif /* APP_H */
