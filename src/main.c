/*
 * main.c
 *
 * Entry point for screenr. Creates the main window, loads the
 * "screenr/main" edje group from screenr.edj, and wires up callbacks.c
 * (UI event handling) and capture_preview.c (screenshot/rubberband).
 *
 * Recording state and the Ecore_Exe FFmpeg spawn logic live in
 * recorder.c, not here.
 */

#include <Elementary.h>
#include <Ecore_Evas.h>
#include <Edje.h>
#include <stdio.h>

#include "app.h"
#include "callbacks.h"
#include "capture_preview.h"
#include "recorder.h"

#define WIN_WIDTH  95
#define WIN_HEIGHT 174 /* BASE values at UI scale 1.0 -- matches main.edc's
                           own min: 95 149, following the vertical "remote
                           control" layout redesign -- dimensions not
                           final, user wants even numbers eventually.
                           Still user-resizable; fixed-size remains an
                           open decision. Actual on-screen size is these
                           values times Elementary's current UI scale
                           factor (elm_config_scale_get()) -- see
                           elm_main()'s own initial evas_object_resize()
                           call and _config_changed_cb()'s matching one
                           for every later scale change. */

/* Resolved via SCREENR_DATADIR, a compile-time macro injected by
 * src/meson.build (-DSCREENR_DATADIR="{prefix}/{datadir}/screenr").
 * Used to be a bare relative filename ("screenr.edj"), which only
 * worked by coincidence when the app was always run directly from its
 * own source/build directory under the old Makefile -- confirmed
 * broken the moment the app was properly installed via Meson and run
 * from an arbitrary working directory (real error: "screenr.edj/
 * screenr/main: File Does Not Exist"). data/themes/default/meson.build
 * installs screenr.edj to exactly this same {datadir}/screenr location
 * (see that file's own install_dir), so this now resolves correctly
 * regardless of the caller's current working directory. */
#define EDJ_FILE   SCREENR_DATADIR "/screenr.edj"
#define EDJ_GROUP  "screenr/main"

/* First-run default mode, used until Eet config persistence exists to
 * restore the user's last-used mode instead. */
#define DEFAULT_MODE APP_MODE_SCREEN

static App g_app;

/*
 * Safety-net escalation: fires 5 seconds after _on_win_del() requested a
 * graceful ffmpeg stop, IF _on_ffmpeg_exit() hasn't already confirmed
 * the process died and cancelled this timer by then. Force-kills the
 * process (guaranteed termination, unlike the graceful 'q' over stdin
 * which depends on ffmpeg still being responsive) and exits regardless
 * -- a possibly-truncated output file from a forced kill is a strictly
 * better outcome than a recording running unnoticed forever after
 * screenr appears closed.
 */
static Eina_Bool
_force_quit_timeout(void *data)
{
   App *app = data;
   fprintf(stderr, "[main] ffmpeg did not exit gracefully within timeout -- force killing\n");
   if (app->ffmpeg_exe)
     {
        /* ecore_exe_kill(): confirmed real, Ecore_Common.h. Sends a
         * real termination signal to the child process. Last resort
         * only. */
        ecore_exe_kill(app->ffmpeg_exe);
     }
   app->quit_safety_timer = NULL;
   elm_exit();
   return ECORE_CALLBACK_CANCEL;
}

static void
_on_win_del(void *data EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event_info EINA_UNUSED)
{
   if (g_app.recording)
     {
        /* Don't exit immediately -- request a graceful stop and defer
         * real shutdown until _on_ffmpeg_exit() (recorder.c) confirms
         * the process has actually died. Prevents both a truncated/
         * corrupt file (killing ffmpeg mid-flush) and, far worse, a
         * silently orphaned recording running indefinitely after
         * screenr appears closed. */
        g_app.quit_after_stop = EINA_TRUE;
        recorder_stop(&g_app);

        /* Safety net: if the graceful path hasn't confirmed exit within
         * 5 seconds (hung/wedged ffmpeg), force-kill and exit anyway
         * rather than leave the app hung waiting forever. */
        g_app.quit_safety_timer = ecore_timer_add(5.0, _force_quit_timeout, &g_app);
        return;
     }
   elm_exit();
}

/*
 * Fires on ANY Elementary config change (scale is only one of many),
 * so de-dupes via last_scale the same way Prevue's own equivalent
 * callback does -- avoids redundant rescale work on unrelated config
 * changes. Clamp (1.0-2.0) matches Prevue's own defensive floor/
 * ceiling against a garbage or unset config value -- not a real design
 * constraint on what scale factors are conceptually valid, just a
 * guard against acting on nonsense.
 *
 * edje_scale_set() alone handles every Edje part already declared
 * scale:1 + offscale in main.edc (confirmed already scale-ready this
 * session) -- no per-part C-side work needed for those. What DOES need
 * explicit handling here: app->win's own base size (WIN_WIDTH/HEIGHT
 * are plain C constants, not Edje-managed), and everything in the
 * Selection window that's a plain, independently-created Evas object
 * rather than an Edje part -- Done's button/text and the 4 corner
 * handles (see overlay.c's own overlay_rescale(), which owns that
 * dispatch). SELECTION_WIN_MAX_W and the screenshot-fitting math in
 * overlay.c are deliberately NOT touched here -- confirmed via direct
 * discussion that ceiling is about physical screen-fitting (real
 * desktop pixels), unrelated to and independent of UI scale factor.
 */
static Eina_Bool
_config_changed_cb(void *data, int type EINA_UNUSED, void *event EINA_UNUSED)
{
   static double last_scale = 0.0;
   App *app = data;
   double scale = elm_config_scale_get();
   if (scale < 1.0 || scale > 2.0) scale = 1.0;
   if (scale == last_scale) return ECORE_CALLBACK_PASS_ON;
   last_scale = scale;

   edje_scale_set(scale);
   evas_object_resize(app->win, (int)(WIN_WIDTH * scale), (int)(WIN_HEIGHT * scale));

   if (app->overlay)
     overlay_rescale(app->overlay, scale);

   return ECORE_CALLBACK_PASS_ON;
}

EAPI_MAIN int
elm_main(int argc, char **argv)
{
   (void)argc;
   (void)argv;

   App *app = &g_app;

   /* Scale must be set BEFORE any Edje content is created below -- same
    * ordering Prevue's own main.c uses -- so the very first layout pass
    * (edje_object_file_set() -> elm_win_resize_object_add() ->
    * evas_object_show()) already accounts for it, rather than creating
    * everything at 1.0 and only correcting it on the first config-
    * change event (which may never fire at all if the user's scale
    * factor was already correct at launch -- Elementary doesn't emit a
    * synthetic "config changed" event just because a value differs
    * from some other default). Same clamp as _config_changed_cb()
    * above, for the same reason. */
   double scale = elm_config_scale_get();
   if (scale < 1.0 || scale > 2.0) scale = 1.0;
   edje_scale_set(scale);

   app->win = elm_win_util_standard_add("screenr", "screenr");
   elm_win_autodel_set(app->win, EINA_TRUE);
   evas_object_smart_callback_add(app->win, "delete,request", _on_win_del, NULL);
   ecore_event_handler_add(ELM_EVENT_CONFIG_ALL_CHANGED, _config_changed_cb, app);

   app->edje_obj = edje_object_add(evas_object_evas_get(app->win));
   if (!edje_object_file_set(app->edje_obj, EDJ_FILE, EDJ_GROUP))
     {
        int err = edje_object_load_error_get(app->edje_obj);
        fprintf(stderr, "[main] failed to load %s/%s: %s\n",
                EDJ_FILE, EDJ_GROUP, edje_load_error_str(err));
        return 1;
     }
   evas_object_size_hint_weight_set(app->edje_obj, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   elm_win_resize_object_add(app->win, app->edje_obj);
   evas_object_show(app->edje_obj);

   app->cp = capture_preview_new(app->edje_obj, EDJ_FILE);

   /* Selection mode's overlay window -- created once here, at startup,
    * not per mode switch. capture_preview_new() has already populated
    * the real desktop size by this point (via its own internal
    * _query_desktop_size() call), so it's available immediately.
    * overlay_new() returning NULL is non-fatal for the app as a whole
    * (matches the pattern used for a failed rubberband load elsewhere)
    * -- Selection mode just won't be functional if this fails, worth
    * a log so it's not silently broken. */
   int desktop_w = 0, desktop_h = 0;
   if (capture_preview_get_desktop_size(app->cp, &desktop_w, &desktop_h))
     {
        app->overlay = overlay_new(EDJ_FILE, desktop_w, desktop_h);
        if (!app->overlay)
          fprintf(stderr, "[main] overlay_new() failed -- Selection mode will not be functional\n");
        else
          overlay_rescale(app->overlay, scale);
     }
   else
     {
        fprintf(stderr, "[main] could not determine desktop size -- overlay not created\n");
        app->overlay = NULL;
     }

   app->recording = EINA_FALSE;
   app->mode_started = EINA_FALSE;
   app->elapsed_seconds = 0;
   app->tick_timer = NULL;
   app->ffmpeg_exe = NULL;
   /* Audio defaults OFF (cursor stays default-on) -- see project notes:
    * most UI/dev-work recordings don't want background audio, and off-
    * by-default avoids the accidental-audio-track footgun (a user who
    * DOES want audio makes one deliberate click; a user who didn't want
    * it never has to notice and disable it after the fact). */
   app->audio_enabled = EINA_FALSE;
   app->cursor_enabled = EINA_TRUE;
   app->quit_after_stop = EINA_FALSE;
   app->quit_safety_timer = NULL;

   app->sel_x = app->sel_y = app->sel_w = app->sel_h = 0;
   app->has_selection_geometry = EINA_FALSE;

   callbacks_connect(app);

   /* Real mode_set() call, not a bare struct assignment -- drives the
    * .edc's "mode,screen"/"mode,selection" signal so the Screen/
    * Selection buttons show the correct selected state on launch, and
    * sets the window title to the real desktop resolution in Screen
    * mode. Does NOT trigger any screenshot capture -- capture_preview's
    * mode-enter functions are pure no-ops now (see capture_preview.h's
    * own comment); the desktop-size query capture_preview_new() already
    * ran above is what makes the resolution available for this title-
    * setting to use immediately, not this call. */
   mode_set(app, DEFAULT_MODE);

   /* Same reasoning as mode_set() above: emit the real signal rather than
    * relying on the .edc's own default part state, so audio_icon/
    * mouse_icon are guaranteed to match app->audio_enabled/cursor_enabled
    * from the very first frame, not just whatever state happens to be
    * declared as "default" in the .edc. */
   edje_object_signal_emit(app->edje_obj, "app,audio,off", "app");
   edje_object_signal_emit(app->edje_obj, "app,cursor,on", "app");

   edje_object_part_text_set(app->edje_obj, "timer_text", "00:00");

   evas_object_resize(app->win, (int)(WIN_WIDTH * scale), (int)(WIN_HEIGHT * scale));
   evas_object_show(app->win);

   elm_run();

   overlay_free(app->overlay);
   capture_preview_free(app->cp);

   return 0;
}
ELM_MAIN()
