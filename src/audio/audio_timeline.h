#ifndef FRAMETEE_AUDIO_TIMELINE_H
#define FRAMETEE_AUDIO_TIMELINE_H

// Joins the sound to the editor: which groups are heard, and where the
// playhead is, whether the timeline or the Camera tab runs the clock.

#include <audio/audio.h>
#include <stdbool.h>

struct gfx_handler_t;

// The groups heard (the ones shown, at their volume; the active one first),
// each from its own worlds: how many, 0 when nothing is loaded.
#define AUDIO_TIMELINE_MAX_HEARD 64
int audio_timeline_listeners(struct gfx_handler_t *handler, audio_listener_t out[AUDIO_TIMELINE_MAX_HEARD]);
// A clock whose positions are seconds of the camera's time.
audio_clock_t audio_timeline_camera_clock(struct gfx_handler_t *handler);
// Once a frame, after the playhead moved.
void audio_timeline_update(struct gfx_handler_t *handler);

#endif
