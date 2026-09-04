#ifndef CALLBACKS_H
#define CALLBACKS_H

#include "app.h"

/* Connects every UI event handler this app has: button clicks on the
 * main window (mode toggle, Record, Refresh, audio/cursor toggles),
 * the Done button on the Selection window's own edje object, Escape-
 * key handling on both windows, and the Selection window's own
 * titlebar close button. Call once, after app->edje_obj and app->win
 * both exist (and after app->overlay has been created, if Selection
 * mode's Done/Escape/close wiring is to be connected too -- guarded
 * internally if app->overlay is NULL). */
void callbacks_connect(App *app);

/* Exposed separately from callbacks_connect() because main.c needs to set
 * the initial mode at startup (and, later, Eet config restore will call
 * this directly too, bypassing the click path entirely). */
void mode_set(App *app, App_Mode mode);

#endif /* CALLBACKS_H */
