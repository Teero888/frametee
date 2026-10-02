#ifndef FRAMETEE_AUDIO_TIMELINE_H
#define FRAMETEE_AUDIO_TIMELINE_H

// Joins the sound to the editor: which group is heard, and where its playhead
// is, whether the timeline or the Camera tab runs the clock.

#include <audio/audio.h>
#include <stdbool.h>

struct gfx_handler_t;

// The active group, heard from its own worlds. False when nothing is loaded.
bool audio_timeline_listener(struct gfx_handler_t *handler, audio_listener_t *out);
// A clock whose positions are seconds of the camera's time.
audio_clock_t audio_timeline_camera_clock(struct gfx_handler_t *handler);
// Once a frame, after the playhead moved.
void audio_timeline_update(struct gfx_handler_t *handler);

#endif
