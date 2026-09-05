/*
 * overlay_done_button.c
 *
 * Owns the Selection window's "Done" button -- now a plain,
 * independent Evas object (an image, images/rec_outline.png, plus a
 * text label), NOT an Edje part. See rubberband.edc's own header comment for the full
 * two-part architectural rationale; the short version: once fill/the
 * corner handles became plain Evas objects (living outside Edje's
 * object tree), they and done_outline (still an Edje part at the
 * time) belonged to two different event-routing subsystems -- plain
 * Evas hit-testing for fill, Edje's own internal part dispatch for
 * done_outline. Confirmed via real testing that no amount of paint-
 * order fixing (evas_object_layer_set(), tried first) could make
 * done_outline reliably receive its click when fill visually
 * overlapped it: layers control paint order, not which subsystem gets
 * first refusal on an input event. The only structural fix is putting
 * every interactive object in this window under ONE single, consistent
 * hit-testing system -- this file is that fix applied to Done.
 *
 * VISUAL: uses images/rec_outline.png directly (evas_object_image_add(),
 * resolved via SCREENR_DATADIR -- see DONE_BTN_IMAGE_PATH's own
 * comment below) for the pill-shaped outline, replacing an earlier
 * flat-rectangle placeholder. No 9-patch border scaling is applied
 * (evas_object_image_border_set()) -- matches this exact same image's
 * only other usage in this codebase (main.edc's "timer_outline" part,
 * which also uses it as a plain stretched image with no image.border
 * declared), so this stays visually consistent with precedent already
 * established elsewhere rather than inventing new border values with
 * no way to verify them against the actual image content.
 *
 * POSITIONING HISTORY -- two distinct bugs, found in sequence:
 *
 *   BUG 1: the button was originally positioned ONCE, inside
 *   overlay_done_button_new() itself, which runs from overlay_new()
 *   at APP STARTUP -- before the Selection window has ever been
 *   shown/mapped at all. bg's own geometry at that point is genuinely
 *   (0,0) 0x0 (window not yet realized by the WM), so the button was
 *   placed at a large NEGATIVE x coordinate -- not invisible in the
 *   strict sense, just permanently off-screen. Fixed by splitting
 *   creation (overlay_done_button_new()) from positioning (overlay_
 *   done_button_reposition(), NEW), with overlay.c calling the latter
 *   again every time overlay_show() runs, mirroring how fill's own
 *   centered-default reset already works.
 *
 *   BUG 2, found after BUG 1's fix still didn't work on a genuine
 *   first open: overlay_done_button_reposition() itself used to take
 *   bg_obj directly and read its geometry itself -- a SECOND,
 *   independent evas_object_geometry_get() call on the exact same "bg"
 *   object overlay.c's own overlay_show() had ALREADY read correctly,
 *   one line earlier, on the same call stack. Confirmed via real
 *   testing/logging that this second read could return (0,0,0,0) even
 *   though the first one, on the same object, moments earlier, did
 *   not -- proven definitively by fill (which only ever uses the FIRST
 *   read) rendering at the exact correct centered position on a
 *   genuine first open, while Done (re-reading a second time) did not.
 *   The precise Evas/Edje-internal reason two adjacent reads of one
 *   object's geometry could disagree was not fully traced. Fixed by
 *   not reading a second time at all: overlay_done_button_reposition()
 *   now takes plain int bw/bh, with overlay.c passing through the
 *   exact same ccw/cch it already read for fill's own reset.
 */

#include <Evas.h>
#include <stdio.h>

#include "overlay.h"

/* Same fixed size/position as the old done_outline/spc_done pairing:
 * 48x24, anchored to the window's own top-right corner, 12px down
 * from the top and 13px in from the right -- matching spc_done's own
 * rel1/rel2 offsets (0 12 / -13 -1) against "bg". Expressed here
 * directly in C pixel arithmetic against bg's own geometry, since
 * there is no more Edje spacer part to derive this from. */
#define DONE_BTN_W 40
#define DONE_BTN_H 19
#define DONE_BTN_RIGHT_MARGIN 13
#define DONE_BTN_TOP_MARGIN 12

/* Resolved via SCREENR_DATADIR, a compile-time macro injected by
 * src/meson.build (-DSCREENR_DATADIR="{prefix}/{datadir}/screenr") --
 * this project moved from a Makefile to Meson specifically to solve
 * this: evas_object_image_file_set() needs a REAL path on disk (unlike
 * Edje-managed images, which get compiled/embedded into screenr.edj by
 * edje_cc and never need a standalone install location at all -- see
 * data/themes/default/meson.build's own install_data() rule, which
 * installs ONLY the handful of images loaded this way, images/
 * rec_outline.png and images/handle.png, to {datadir}/screenr/images/
 * -- not the whole images/ directory, since everything else is already
 * embedded in the .edj and would be pure duplication to also install
 * as loose files). Adjacent string literals are concatenated by the C
 * preprocessor at compile time, so this needs no runtime string
 * building at all. */
#define DONE_BTN_IMAGE_PATH SCREENR_DATADIR "/images/done_outline.png"

static Evas_Object *g_done_btn = NULL;
static Evas_Object *g_done_text = NULL;
static void (*g_done_click_cb)(void *data) = NULL;
static void *g_done_click_cb_data = NULL;

/* Elementary's UI scale factor (elm_config_scale_get(), e.g. 2.0 at
 * 200% HiDPI) -- scales DONE_BTN_W/H/margins and the "Done" text's own
 * font size. Same distinct-from-any-screenshot-fit-ratio reasoning as
 * overlay_fill_sync.c's own g_scale_ui (see that file's comment for
 * the full explanation) -- there is no equivalent conflation risk in
 * THIS file specifically (it has no g_scale of its own), but the name
 * and defaulting-to-1.0 convention are kept consistent across both
 * files regardless. Defaults to 1.0; updated via overlay_done_button_
 * set_ui_scale(), called from overlay.c's own overlay_rescale(). */
static double g_scale_ui = 1.0;

void
overlay_done_button_set_ui_scale(double scale)
{
   g_scale_ui = scale;
}

static void
_on_done_mouse_up(void *data EINA_UNUSED, Evas *e EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event_info EINA_UNUSED)
{
   fprintf(stderr, "[overlay_done_button] Done clicked\n");
   if (g_done_click_cb) g_done_click_cb(g_done_click_cb_data);
}

/*
 * Creates the Done button as two plain Evas objects (an image and a
 * text label on top of it) on the given canvas. Does
 * NOT position them with any real geometry -- see overlay_done_
 * button_reposition() below, which is what actually gives them a real
 * on-screen position, and which must be called separately (both once
 * here, right after creation, AND every time the Selection window is
 * actually shown -- see this file's own header comment for why the
 * latter is required). Call this function once, from overlay_new().
 *
 * Does NOT take a click callback -- see overlay_done_button_set_
 * click_callback() below for why that's registered separately: this
 * function runs from overlay_new(), which has no App* to give a
 * callback to fire against; callbacks_connect() (callbacks.c), which
 * does have that, runs afterward in main.c's own init sequence and
 * registers the real callback once app exists.
 */
void
overlay_done_button_new(Evas *evas)
{
   g_done_btn = evas_object_image_add(evas);
   evas_object_image_file_set(g_done_btn, DONE_BTN_IMAGE_PATH, NULL);
   Evas_Load_Error err = evas_object_image_load_error_get(g_done_btn);
   if (err != EVAS_LOAD_ERROR_NONE)
     fprintf(stderr, "[overlay_done_button] failed to load %s: %s\n",
             DONE_BTN_IMAGE_PATH, evas_load_error_str(err));
   evas_object_image_filled_set(g_done_btn, EINA_TRUE);

#define PREMUL(c, a) (((c) * (a)) / 255)

   int r = 255, g = 59, b = 48;
   int a = 224;

   evas_object_color_set(g_done_btn, PREMUL(r, a), PREMUL(g, a), PREMUL(b, a), a);
   evas_object_layer_set(g_done_btn, EVAS_LAYER_FILL + 1);
   evas_object_show(g_done_btn);

   /* mouse,up rather than mouse,down: matches the old Edje program's
    * own trigger ("mouse,clicked,1", which Edje itself only emits on
    * a matched down+up pair inside the same part) -- mouse,up is the
    * closest plain-Evas equivalent to "a real click completed here",
    * avoiding a false trigger if the user mouses down on the button
    * but drags off before releasing. */
   evas_object_event_callback_add(g_done_btn, EVAS_CALLBACK_MOUSE_UP, _on_done_mouse_up, NULL);

   g_done_text = evas_object_text_add(evas);
   evas_object_color_set(g_done_text, 255, 255, 255, 255);
   evas_object_text_style_set(g_done_text, EVAS_TEXT_STYLE_PLAIN);
   evas_object_text_font_set(g_done_text, "Sans:style=Bold", 11);
   evas_object_text_text_set(g_done_text, "Done");
   evas_object_layer_set(g_done_text, EVAS_LAYER_FILL + 1);
   evas_object_pass_events_set(g_done_text, EINA_TRUE); /* clicks go to g_done_btn underneath, not swallowed here */
   evas_object_show(g_done_text);
}

/*
 * Gives the Done button its real on-screen position, given the
 * Selection window's own current bounds (bw x bh, window-space pixels
 * -- same "bg" part's geometry overlay.c's own overlay_show() already
 * reads for fill's own reset, passed straight through here rather than
 * re-read a second time inside this function).
 *
 * CONFIRMED REAL BUG, this session: an earlier version of this
 * function took bg_obj directly and called evas_object_geometry_get()
 * on it itself -- a SECOND, independent read of the same object whose
 * geometry overlay.c's own overlay_show() had ALREADY read correctly,
 * moments earlier, on the very same call stack (proven by fill
 * rendering at the exact correct centered position on a genuine first
 * open, using that same first read). That second read returned
 * (0,0,0,0) even though the first one, on the same object, at
 * essentially the same instant, did not -- confirmed via real
 * testing/logging. Rather than chase the exact underlying Evas/Edje
 * reason two adjacent reads of one object could disagree, the fix is
 * to not read a second time at all: bw/bh are passed in directly,
 * already known good, exactly mirroring how overlay_fill_set_geometry()
 * already receives its own geometry as plain int parameters rather
 * than deriving it internally.
 *
 * Call this:
 *   1. Once from overlay_new(), right after overlay_done_button_new()
 *      -- harmless even if bw/bh are still 0 at that point (window not
 *      shown/mapped yet); the button will simply be positioned at a
 *      nonsensical (but harmless, since the window isn't shown yet
 *      anyway) location until step 2 below corrects it.
 *   2. Every time overlay_show() runs, using the SAME ccw/cch it just
 *      read for fill's own reset -- this is the call that actually
 *      matters.
 */
void
overlay_done_button_reposition(int bw, int bh EINA_UNUSED)
{
   /* bh (window height) is genuinely unused here, by design -- Done is
    * anchored to the window's TOP-right corner: btn_x depends on bw
    * (needed for the right-margin offset), but btn_y is a fixed
    * DONE_BTN_TOP_MARGIN from the top, never dependent on the window's
    * height at all. Kept in the signature anyway (rather than dropped
    * entirely) so this function's own parameters stay symmetric with
    * what overlay.c actually has on hand and passes in -- the same
    * ccw/cch pair fill's own reset already uses (see overlay.c's own
    * overlay_show()) -- even though only one of the two is needed
    * here. */
   if (!g_done_btn || !g_done_text) return;

   int scaled_w = (int)(DONE_BTN_W * g_scale_ui);
   int scaled_h = (int)(DONE_BTN_H * g_scale_ui);
   int scaled_right_margin = (int)(DONE_BTN_RIGHT_MARGIN * g_scale_ui);
   int scaled_top_margin = (int)(DONE_BTN_TOP_MARGIN * g_scale_ui);

   int btn_x = bw - scaled_right_margin - scaled_w;
   int btn_y = scaled_top_margin;

   evas_object_move(g_done_btn, btn_x, btn_y);
   evas_object_resize(g_done_btn, scaled_w, scaled_h);

   /* Font size scaled the same way -- re-set here (not just once at
    * creation time) since it directly affects the natural-size read
    * (tw/th) used for centering just below, and needs to track scale
    * changes the same way the button's own size does. evas_object_
    * text_font_set() is safe to call repeatedly with the same or a
    * different size -- it's a plain property setter, not a one-time
    * initialization call. */
   evas_object_text_font_set(g_done_text, "Sans:style=Bold", (int)(10 * g_scale_ui));

   /* Centered within the button -- evas_object_text objects size
    * themselves to their own text content, so position is computed
    * from that natural size rather than a fixed offset. Reads the
    * text object's own geometry right after the font_set() call above
    * -- confirmed via real testing this round that plain Evas text
    * objects DO report their real natural size synchronously once
    * text/font are set, no separate render pass required (unlike the
    * bg_obj/Edje-part geometry problem this same file's header comment
    * documents). Uses scaled_w/scaled_h (the button's own just-resized
    * dimensions), not the raw DONE_BTN_W/H constants, so centering
    * stays correct at any scale factor. */
   Evas_Coord tw = 0, th = 0;
   evas_object_geometry_get(g_done_text, NULL, NULL, &tw, &th);
   evas_object_move(g_done_text,
                     btn_x + (scaled_w - tw) / 2,
                     btn_y + (scaled_h - th) / 2);
}

/*
 * Registers the real click callback -- called once from callbacks.c's
 * callbacks_connect(), after both the Done button (created in
 * overlay_new(), which runs earlier in main.c's init sequence) and
 * App itself exist. Safe to call even if overlay_done_button_new()
 * failed to create the button objects (e.g. overlay creation failed
 * entirely at startup) -- _on_done_mouse_up() itself is never wired to
 * fire if g_done_btn was never created, so an unset callback here is
 * simply never invoked.
 */
void
overlay_done_button_set_click_callback(void (*click_cb)(void *data), void *click_cb_data)
{
   g_done_click_cb = click_cb;
   g_done_click_cb_data = click_cb_data;
}

/*
 * Deletes the Done button's two objects -- called once from
 * overlay_free() at app shutdown, same reasoning as overlay_fill_
 * sync_free(): these are independent Evas objects, not Edje parts, so
 * nothing else cleans them up automatically.
 */
void
overlay_done_button_free(void)
{
   if (g_done_btn) { evas_object_del(g_done_btn); g_done_btn = NULL; }
   if (g_done_text) { evas_object_del(g_done_text); g_done_text = NULL; }
   g_done_click_cb = NULL;
   g_done_click_cb_data = NULL;
}
