/*
 * recorder.c
 *
 * Owns recording state: the Play/Stop transition, the elapsed-time timer,
 * and the Ecore_Exe FFmpeg process lifecycle -- spawning ffmpeg with the
 * resolved geometry, sending 'q' over stdin to finalize on stop, and
 * reacting to ECORE_EXE_EVENT_DEL for process exit (whether requested or
 * unexpected).
 *
 * Fixed encode decisions already locked in (see project notes), all
 * baked into the command line as non-optional -- no UI exposes any of
 * this:
 *   - container/codec: .webm / libvpx
 *   - low-end-hardware profile: -framerate 15, -cpu-used 8, -b:v 1M,
 *     -fps_mode cfr -r 15 (deliberate, kept default -- the user's
 *     current machine drops frames badly at higher settings)
 *   - cursor visibility and audio are both real user toggles (see
 *     app->cursor_enabled / app->audio_enabled), not hardcoded
 *   - output path: ~/Videos/screenr/<timestamp>.webm, never overwrites
 */

#include <Edje.h>
#include <Ecore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <pwd.h>
#include <unistd.h>

#include "app.h"
#include "recorder.h"
#include "capture_preview.h"
#include "overlay.h"

/* ---- forward decls ---- */
static void _timer_text_update(App *app);
static Eina_Bool _build_output_path(char *out_path, size_t out_path_sz);
static Eina_Bool _ensure_output_dir(const char *dir_path);

/* --------------------------------------------------------------------- */
/* Timer                                                                  */
/* --------------------------------------------------------------------- */

static Eina_Bool
_on_tick(void *data)
{
   App *app = data;
   app->elapsed_seconds++;
   _timer_text_update(app);
   return ECORE_CALLBACK_RENEW;
}

static void
_timer_text_update(App *app)
{
   char buf[16];
   int m = app->elapsed_seconds / 60;
   int s = app->elapsed_seconds % 60;
   snprintf(buf, sizeof(buf), "%02d:%02d", m, s);
   edje_object_part_text_set(app->edje_obj, "timer_text", buf);
}

/* --------------------------------------------------------------------- */
/* Output path: ~/Videos/screenr/<timestamp>.webm                         */
/* --------------------------------------------------------------------- */

static Eina_Bool
_ensure_output_dir(const char *dir_path)
{
   if (mkdir(dir_path, 0755) != 0)
     {
        if (errno != EEXIST)
          {
             fprintf(stderr, "[recorder] failed to create %s: %s\n",
                     dir_path, strerror(errno));
             return EINA_FALSE;
          }
     }
   return EINA_TRUE;
}

static Eina_Bool
_build_output_path(char *out_path, size_t out_path_sz)
{
   const char *home = getenv("HOME");
   if (!home || !*home)
     {
        struct passwd *pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
     }
   if (!home || !*home)
     {
        fprintf(stderr, "[recorder] could not determine home directory\n");
        return EINA_FALSE;
     }

   char videos_dir[512];
   char app_dir[560];
   snprintf(videos_dir, sizeof(videos_dir), "%s/Videos", home);
   snprintf(app_dir, sizeof(app_dir), "%s/screenr", videos_dir);

   if (!_ensure_output_dir(videos_dir)) return EINA_FALSE;
   if (!_ensure_output_dir(app_dir)) return EINA_FALSE;

   time_t now = time(NULL);
   struct tm tm_now;
   localtime_r(&now, &tm_now);

   char stamp[32];
   strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &tm_now);

   snprintf(out_path, out_path_sz, "%s/%s.webm", app_dir, stamp);
   return EINA_TRUE;
}

/* --------------------------------------------------------------------- */
/* Ecore_Exe: ffmpeg process lifecycle                                    */
/* --------------------------------------------------------------------- */

static Ecore_Event_Handler *g_exe_del_handler = NULL;

/*
 * Fires when the ffmpeg child process actually terminates -- whether we
 * asked it to (via 'q' over stdin, in recorder_stop()) or it died on its
 * own. This is the single source of truth that recording has actually
 * ended; recorder_stop() only *requests* the stop.
 *
 * IMPORTANT ordering (a real bug caught during design, see project
 * notes): the recording/UI-settling block below runs UNCONDITIONALLY,
 * before the quit_after_stop check -- never behind an early return. It
 * must cover three cases identically: a normal user-initiated stop, an
 * unexpected crash/external kill while still "recording", AND a
 * quit-triggered stop from _on_win_del(). Skipping it via an early
 * return risked leaving app->recording stale/TRUE during the shutdown
 * window.
 */
static Eina_Bool
_on_ffmpeg_exit(void *data, int type EINA_UNUSED, void *event_info)
{
   App *app = data;
   Ecore_Exe_Event_Del *ev = event_info;

   if (ev->exe != app->ffmpeg_exe) return ECORE_CALLBACK_RENEW;

   printf("[recorder] ffmpeg exited (code %d)\n", ev->exit_code);
   app->ffmpeg_exe = NULL;

   /* Cancel the pending force-kill safety timer, if any -- the process
    * has now genuinely exited (gracefully or otherwise), so the timeout
    * escalation in main.c is no longer needed. */
   if (app->quit_safety_timer)
     {
        ecore_timer_del(app->quit_safety_timer);
        app->quit_safety_timer = NULL;
     }

   /* Settle recording/UI state unconditionally -- see the ordering note
    * above. This is a no-op if recorder_stop() already flipped
    * app->recording to FALSE (the normal Stop-button path), and is what
    * actually performs the settling for the crash/quit-triggered paths,
    * where nothing else would. */
   if (app->recording)
     {
        app->recording = EINA_FALSE;
        if (app->tick_timer)
          {
             ecore_timer_del(app->tick_timer);
             app->tick_timer = NULL;
          }
        app->elapsed_seconds = 0;
        _timer_text_update(app);
        edje_object_signal_emit(app->edje_obj, "app,state,idle", "app");
        /* NOTE: the old overlay_show_handle_window() call here is
         * REMOVED -- see NEXT_SESSION_PLAN.md/overlay.h. There is no
         * separate handle window anymore to show/hide around a
         * recording; under the Kooha-style flow, the Selection
         * rubberband-drawing window is already closed by the time
         * real recording is running at all (it closes on "Done",
         * before recording starts -- see recorder_start()'s own
         * comment below), so there is nothing left here to show
         * again once recording ends. */
     }

   if (app->quit_after_stop)
     {
        elm_exit();
        return ECORE_CALLBACK_RENEW;
     }

   return ECORE_CALLBACK_RENEW;
}

/* --------------------------------------------------------------------- */
/* Record state                                                           */
/* --------------------------------------------------------------------- */

void
recorder_start(App *app)
{
   if (app->recording) return;

   int x = 0, y = 0, w = 0, h = 0;
   Eina_Bool have_geom = EINA_FALSE;

   if (app->mode == APP_MODE_SELECTION)
     {
        /* Deliberately reads app->sel_x/y/w/h (the snapshot taken in
         * callbacks.c's _on_done_clicked() the moment Done was
         * pressed), NOT a live overlay_get_record_geometry() call.
         *
         * This used to read live from app->overlay here, which is the
         * root cause of a real, confirmed bug: overlay_show() resets
         * "fill" back to its centered 50%-of-window default every
         * time it runs (see overlay.c), so if the Selection window
         * still happens to be open, was ever re-shown, or is re-shown
         * by any future code path between the user's drag and this
         * point, this would silently record the centered default
         * instead of the user's actual chosen rectangle -- exactly
         * the symptom observed (dragging fill to a corner, pressing
         * Done, but ffmpeg recording the centered 960x540 default).
         * Reading the snapshot instead removes that failure mode
         * structurally: whatever was true the instant Done was
         * pressed is what gets recorded, full stop, regardless of
         * anything that happens to the overlay window afterward.
         *
         * has_selection_geometry catches the case where Selection mode
         * is active but Done was never actually pressed this session
         * (app just started in Selection mode, or the user backed out
         * of the Selection window via Escape/close instead of Done) --
         * refuse to start rather than silently using stale/zeroed
         * geometry. */
        have_geom = app->has_selection_geometry;
        if (have_geom)
          {
             x = app->sel_x;
             y = app->sel_y;
             w = app->sel_w;
             h = app->sel_h;
          }
        if (!have_geom)
          {
             fprintf(stderr, "[recorder] SELECTION mode but Done has not been pressed yet -- not starting\n");
             return;
          }
     }
   else /* APP_MODE_SCREEN */
     {
        have_geom = capture_preview_get_desktop_size(app->cp, &w, &h);
        x = 0;
        y = 0;
        if (!have_geom)
          {
             fprintf(stderr, "[recorder] SCREEN mode but desktop size not known yet -- not starting\n");
             return;
          }
     }

   char out_path[600];
   if (!_build_output_path(out_path, sizeof(out_path)))
     {
        fprintf(stderr, "[recorder] could not build output path -- not starting\n");
        return;
     }

   /*
    * -draw_mouse 0 must be placed BEFORE x11grab's own -i (it's an
    * input-level option for that specific input) -- placing it after -i
    * caused ffmpeg to misparse it as an option for the NEXT input
    * (pulse), which has no such option, and fail entirely. Confirmed
    * via real ffmpeg error output during testing.
    *
    * When audio is off, the -f pulse input is dropped ENTIRELY (not
    * just -c:a), so the audio device is never opened for nothing.
    */
   char cmd[1024];
   if (app->audio_enabled)
     {
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -f x11grab %s-framerate 15 -video_size %dx%d -i :0.0+%d,%d "
                 "-f pulse -i default "
                 "-c:v libvpx -deadline realtime -cpu-used 8 -b:v 1M "
                 "-fps_mode cfr -r 15 "
                 "-af aresample=async=1 -c:a libvorbis \"%s\"",
                 app->cursor_enabled ? "" : "-draw_mouse 0 ",
                 w, h, x, y,
                 out_path);
     }
   else
     {
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -f x11grab %s-framerate 15 -video_size %dx%d -i :0.0+%d,%d "
                 "-c:v libvpx -deadline realtime -cpu-used 8 -b:v 1M "
                 "-fps_mode cfr -r 15 \"%s\"",
                 app->cursor_enabled ? "" : "-draw_mouse 0 ",
                 w, h, x, y,
                 out_path);
     }

   printf("[recorder] spawning: %s\n", cmd);

   app->ffmpeg_exe = ecore_exe_pipe_run(cmd, ECORE_EXE_PIPE_WRITE, NULL);
   if (!app->ffmpeg_exe)
     {
        fprintf(stderr, "[recorder] failed to spawn ffmpeg\n");
        return;
     }

   if (!g_exe_del_handler)
     g_exe_del_handler = ecore_event_handler_add(ECORE_EXE_EVENT_DEL, _on_ffmpeg_exit, app);

   app->recording = EINA_TRUE;
   app->elapsed_seconds = 0;
   _timer_text_update(app);
   app->tick_timer = ecore_timer_add(1.0, _on_tick, app);

   edje_object_signal_emit(app->edje_obj, "app,state,recording", "app");

   /* NOTE: the old overlay_hide_handle_window() call here is REMOVED --
    * see NEXT_SESSION_PLAN.md/overlay.h. There is no separate handle
    * window anymore; under the finished Kooha-style flow, the
    * Selection rubberband-drawing window is closed (via "Done",
    * fully wired in callbacks.c's _on_done_clicked() -- see this
    * function's own comment above on app->sel_x/y/w/h) BEFORE real
    * recording ever starts, so by the time this line runs there's
    * nothing left on screen from Screenr that could show up in the
    * recording -- no hide call needed here at all, structurally, not
    * just as an optimization. This guarantee holds unconditionally:
    * recorder_start() only ever reaches this point in Selection mode
    * via app->has_selection_geometry being true, which is only ever
    * set by _on_done_clicked() after it has already hidden the
    * overlay window. */
}

void
recorder_stop(App *app)
{
   if (!app->recording) return;

   /* Graceful finalize: 'q' over stdin is ffmpeg's documented way to
    * stop cleanly and flush/finalize the container. The process's
    * actual death is still handled asynchronously in _on_ffmpeg_exit()
    * -- this only requests the stop. */
   if (app->ffmpeg_exe)
     ecore_exe_send(app->ffmpeg_exe, "q\n", 2);

   /* UI settles to idle immediately rather than waiting for the process
    * exit event -- pressing Stop should feel instant. This applies
    * identically whether called from the normal Stop button or from
    * main.c's _on_win_del() quit path; the only difference in the quit
    * path is that main.c separately defers elm_exit() via
    * quit_after_stop until _on_ffmpeg_exit() confirms the process has
    * actually died -- recorder_stop() itself behaves the same either
    * way. _on_ffmpeg_exit() firing later is a no-op against recording/
    * UI state at that point, since app->recording is already false. */
   app->recording = EINA_FALSE;

   if (app->tick_timer)
     {
        ecore_timer_del(app->tick_timer);
        app->tick_timer = NULL;
     }
   app->elapsed_seconds = 0;
   _timer_text_update(app);

   edje_object_signal_emit(app->edje_obj, "app,state,idle", "app");
   /* NOTE: the old overlay_show_handle_window() call here is REMOVED --
    * same reasoning as _on_ffmpeg_exit()'s copy of this comment above. */
}
