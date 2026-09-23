#ifndef FRAMETEE_RENDER_WINDOW_H
#define FRAMETEE_RENDER_WINDOW_H

#include <stdbool.h>

struct ui_handler_t;

// The Render tab: what is drawn in the viewport and in video, the output
// settings, and the render itself. While it is the tab in use the viewport
// previews the video.
void render_window_render(struct ui_handler_t *ui);

// A background render outlives the editor. Quitting while one runs asks
// first; this is the question, drawn every frame.
void render_quit_prompt(struct ui_handler_t *ui);
// Whether the window may close now. False opens the question instead.
bool render_allow_quit(struct ui_handler_t *ui);

// Fits the viewport to the video's shape while previewing. Takes the space the
// viewport has and returns the part of it the picture uses.
void render_fit_viewport(struct ui_handler_t *ui, float *x, float *y, float *width, float *height);

#endif
