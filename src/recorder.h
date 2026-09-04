#ifndef RECORDER_H
#define RECORDER_H

#include "app.h"

/* Play/Stop only -- pause was explicitly ruled out (see project notes:
 * raw FFmpeg has no clean pause like GStreamer, and the only motivating
 * case -- mic recording -- isn't in scope). */
void recorder_start(App *app);
void recorder_stop(App *app);

#endif /* RECORDER_H */
