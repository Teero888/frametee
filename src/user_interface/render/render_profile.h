#ifndef FRAMETEE_RENDER_PROFILE_H
#define FRAMETEE_RENDER_PROFILE_H

#include <frametee/game_abi.h>
#include <stdbool.h>

struct ui_handler_t;

// What a frame shows, twice over: once for the editor's viewport and once for
// rendered video. A profile holds a value for every game setting flagged
// FT_SETTING_RENDER, by id, and the editor's own overlays. The game only ever
// holds one set of values; the editor applies whichever profile is being drawn
// and switches only when that changes, because some settings are costly to set.

enum { RENDER_PROFILE_MAX_SETTINGS = 96, RENDER_SETTING_ID_MAX = 64 };

// The editor's own drawing, which a game knows nothing about.
typedef enum render_layer_t {
  RENDER_LAYER_PREDICTION = 0, // prediction lines
  RENDER_LAYER_SELECTION,      // the selected player's highlight
  RENDER_LAYER_START_MARKERS,  // markers for overridden starts
  RENDER_LAYER_CAMERA_PATH,    // the camera's path and frames; never in video
  RENDER_LAYER_COUNT
} render_layer_t;

typedef struct render_setting_value_t {
  char id[RENDER_SETTING_ID_MAX];
  ft_value value;
} render_setting_value_t;

// Which group a frame is about: whose HUD, chat and pickups it shows. A group index, or one of these.
enum { RENDER_FOCUS_SELECTED = -1, // the group selected in the editor
       RENDER_FOCUS_MERGED = -2 }; // the selected group, with every group's chat together

typedef struct render_profile_t {
  bool valid;
  int setting_count;
  render_setting_value_t settings[RENDER_PROFILE_MAX_SETTINGS];
  bool layers[RENDER_LAYER_COUNT];
  int focus; // RENDER_FOCUS_* or a group index; only the video's is used
} render_profile_t;

typedef enum render_target_t { RENDER_TARGET_VIEWPORT = 0, RENDER_TARGET_VIDEO } render_target_t;

typedef struct render_state_t {
  render_profile_t viewport; // saved in the user's config
  render_profile_t video;    // saved in the project
  render_target_t applied;   // which one the game holds now
  bool preview;              // the Render tab shows the video in the viewport
} render_state_t;

const char *render_layer_name(render_layer_t layer);
bool render_setting_is_layer(const ft_setting_desc *desc);

// Every overlay on in the viewport, no video yet. Before the config loads.
void render_state_defaults(struct ui_handler_t *ui);
// A new project has no video look yet; the viewport's values go back on the game.
void render_state_new_project(struct ui_handler_t *ui);
// The video profile, made from the viewport one with the editor's overlays off
// when a project has none yet.
render_profile_t *render_video_profile(struct ui_handler_t *ui);

bool render_profile_get(const render_profile_t *profile, const char *id, ft_value *out);
void render_profile_set(render_profile_t *profile, const char *id, const ft_value *value);

// One setting's value for a target: stored, and live on the game if that
// target is applied.
bool render_setting_get(struct ui_handler_t *ui, render_target_t target, unsigned index, ft_value *out);
void render_setting_set(struct ui_handler_t *ui, render_target_t target, unsigned index, const ft_value *value);

// Makes the game hold a target's values, setting only those that differ.
void render_apply(struct ui_handler_t *ui, render_target_t target);

// Whether an overlay is drawn in what is being rendered now.
bool render_layer_enabled(struct ui_handler_t *ui, render_layer_t layer);
bool render_layer_get(struct ui_handler_t *ui, render_target_t target, render_layer_t layer);
void render_layer_set(struct ui_handler_t *ui, render_target_t target, render_layer_t layer, bool enabled);
// Whether a timeline group is drawn in what is being rendered now, and how opaque.
bool render_group_visible(struct ui_handler_t *ui, int group_index);
float render_group_opacity(struct ui_handler_t *ui, int group_index);
// The group what is being rendered now is about, and whether every group's chat shows together.
// The viewport follows the editor's selection; video has its own choice.
int render_focus_group(struct ui_handler_t *ui, bool *out_merged);

#endif
