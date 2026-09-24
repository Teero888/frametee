#ifndef FRAMETEE_CAMERA_WINDOW_H
#define FRAMETEE_CAMERA_WINDOW_H

#include "camera_timeline.h"

struct ui_handler_t;
struct gfx_handler_t;

// The Camera tab. It sits in the same dock node as the Timeline and, while it
// is the tab in use, owns the clock: its playhead is in seconds of output, and
// the game is shown at the remapped tick.
typedef struct camera_editor_t {
  double playhead; // seconds
  bool playing;
  double view_start; // seconds at the left edge of the lanes
  float pixels_per_second;

  bool drives_game;  // the Camera tab owns the clock this frame
  float game_intra;  // blend toward the drawn tick, for the fractional part
  bool look_through; // the viewport shows the camera
  bool graph_mode;   // the graph editor instead of the key lanes

  camera_channel_t selected_channel; // also the channel the graph editor shows
  int selected_key;

  // An undo step in progress, for edits that span several frames.
  bool edit_open;
  char edit_description[64];
  camera_timeline_t edit_before;
} camera_editor_t;

void camera_editor_reset(struct ui_handler_t *ui);
// Every frame, before the panels: playback and the game tick it drives.
void camera_editor_update(struct ui_handler_t *ui);
// Puts the game at the tick the playhead shows. Called again right before a
// frame is drawn, so a playhead moved by scrubbing and the world agree.
void camera_editor_sync_game(struct ui_handler_t *ui);
// Whether the camera's clock drives the game: the Camera tab, or the Render tab's preview.
bool camera_editor_owns_clock(const struct ui_handler_t *ui);
// Whether the camera's animation is what the viewport shows (Look through, or the Render tab's preview).
bool camera_editor_drives_view(const struct ui_handler_t *ui);
// Camera shortcuts while the Camera tab owns the clock. Returns true when the
// Camera tab owns the clock, so timeline transport shortcuts should be skipped.
bool camera_editor_process_keys(struct ui_handler_t *ui);
void camera_window_render(struct ui_handler_t *ui);
// The playhead field and play controls, for other tabs that play the camera.
void camera_editor_transport(struct ui_handler_t *ui);
// After the viewport's own camera update: looks through the camera when asked.
void camera_editor_viewport_update(struct gfx_handler_t *h);
void camera_editor_render_gizmos(struct gfx_handler_t *h);
// Key points and path handles over the viewport image, at `origin` on screen.
// Returns true when it took the mouse, so the viewport does not also pick.
bool camera_editor_viewport_overlay(struct ui_handler_t *ui, float origin_x, float origin_y, bool hovered);
// For export: the camera at a moment of camera time.
void camera_editor_apply_for_export(struct gfx_handler_t *h, double seconds);
// The span of camera time that is exported: the Camera tab's range, or all of
// the game timeline when none is set.
void camera_editor_export_range(struct ui_handler_t *ui, double *start, double *end);

#endif
