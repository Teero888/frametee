#include "render_profile.h"

#include <engine/game_host.h>
#include <renderer/graphics_backend.h>
#include <stdio.h>
#include <string.h>
#include <user_interface/user_interface.h>

static const char *LAYER_NAMES[RENDER_LAYER_COUNT] = {
    [RENDER_LAYER_PREDICTION] = "Prediction lines",
    [RENDER_LAYER_SELECTION] = "Selected player highlight",
    [RENDER_LAYER_START_MARKERS] = "Start override markers",
    [RENDER_LAYER_CAMERA_PATH] = "Camera path and frames",
};

const char *render_layer_name(render_layer_t layer) {
  return layer >= 0 && layer < RENDER_LAYER_COUNT ? LAYER_NAMES[layer] : "";
}

bool render_setting_is_layer(const ft_setting_desc *desc) {
  return desc && desc->id && (desc->flags & FT_SETTING_RENDER) &&
         (desc->kind == FT_VALUE_BOOL || desc->kind == FT_VALUE_INT || desc->kind == FT_VALUE_FLOAT);
}

static game_host_t *host_of(ui_handler_t *ui) { return &ui->gfx_handler->game_host; }

bool render_profile_get(const render_profile_t *profile, const char *id, ft_value *out) {
  for (int i = 0; i < profile->setting_count; ++i)
    if (strcmp(profile->settings[i].id, id) == 0) {
      *out = profile->settings[i].value;
      return true;
    }
  return false;
}

void render_profile_set(render_profile_t *profile, const char *id, const ft_value *value) {
  for (int i = 0; i < profile->setting_count; ++i)
    if (strcmp(profile->settings[i].id, id) == 0) {
      profile->settings[i].value = *value;
      return;
    }
  if (profile->setting_count >= RENDER_PROFILE_MAX_SETTINGS || strlen(id) >= RENDER_SETTING_ID_MAX) return;
  render_setting_value_t *slot = &profile->settings[profile->setting_count++];
  snprintf(slot->id, sizeof(slot->id), "%s", id);
  slot->value = *value;
}

static bool values_equal(const ft_value *a, const ft_value *b) {
  if (a->kind != b->kind) return false;
  switch (a->kind) {
  case FT_VALUE_BOOL: return a->as.b == b->as.b;
  case FT_VALUE_INT: return a->as.i == b->as.i;
  case FT_VALUE_FLOAT: return a->as.f == b->as.f;
  default: return false;
  }
}

// The game's live values into the viewport profile. Only right while the
// viewport's values are the ones applied.
static void capture_live(ui_handler_t *ui) {
  game_host_t *host = host_of(ui);
  render_profile_t *profile = &ui->render.viewport;
  const unsigned count = gh_setting_count(host);
  for (unsigned i = 0; i < count; ++i) {
    const ft_setting_desc *desc = gh_setting_desc(host, i);
    ft_value value;
    if (render_setting_is_layer(desc) && gh_setting_get(host, i, &value)) render_profile_set(profile, desc->id, &value);
  }
  profile->valid = true;
}

void render_state_defaults(ui_handler_t *ui) {
  render_state_t *state = &ui->render;
  memset(state, 0, sizeof(*state));
  for (int layer = 0; layer < RENDER_LAYER_COUNT; ++layer) state->viewport.layers[layer] = true;
  state->applied = RENDER_TARGET_VIEWPORT;
}

void render_state_new_project(ui_handler_t *ui) {
  if (ui->render.applied != RENDER_TARGET_VIEWPORT && ui->gfx_handler) render_apply(ui, RENDER_TARGET_VIEWPORT);
  memset(&ui->render.video, 0, sizeof(ui->render.video));
}

render_profile_t *render_video_profile(ui_handler_t *ui) {
  render_state_t *state = &ui->render;
  if (!state->video.valid) {
    // A first video looks like the viewport, without the editor's overlays.
    state->video = state->viewport;
    for (int layer = 0; layer < RENDER_LAYER_COUNT; ++layer) state->video.layers[layer] = false;
    state->video.focus = RENDER_FOCUS_SELECTED;
    state->video.valid = true;
  }
  return &state->video;
}

static render_profile_t *profile_for(ui_handler_t *ui, render_target_t target) {
  return target == RENDER_TARGET_VIDEO ? render_video_profile(ui) : &ui->render.viewport;
}

bool render_setting_get(ui_handler_t *ui, render_target_t target, unsigned index, ft_value *out) {
  game_host_t *host = host_of(ui);
  const ft_setting_desc *desc = gh_setting_desc(host, index);
  if (!render_setting_is_layer(desc)) return false;
  if (render_profile_get(profile_for(ui, target), desc->id, out) && out->kind == desc->kind) return true;
  // Not stored yet: whatever the viewport shows.
  if (render_profile_get(&ui->render.viewport, desc->id, out) && out->kind == desc->kind) return true;
  return gh_setting_get(host, index, out);
}

void render_setting_set(ui_handler_t *ui, render_target_t target, unsigned index, const ft_value *value) {
  game_host_t *host = host_of(ui);
  const ft_setting_desc *desc = gh_setting_desc(host, index);
  if (!render_setting_is_layer(desc)) return;
  render_profile_set(profile_for(ui, target), desc->id, value);
  if (ui->render.applied == target) gh_setting_set(host, index, value);
}

void render_apply(ui_handler_t *ui, render_target_t target) {
  game_host_t *host = host_of(ui);
  // Leaving the viewport's values: keep them, however they were last changed.
  if (ui->render.applied == RENDER_TARGET_VIEWPORT && target != RENDER_TARGET_VIEWPORT) capture_live(ui);
  const unsigned count = gh_setting_count(host);
  for (unsigned i = 0; i < count; ++i) {
    const ft_setting_desc *desc = gh_setting_desc(host, i);
    ft_value wanted, live;
    if (!render_setting_is_layer(desc) || !render_setting_get(ui, target, i, &wanted)) continue;
    if (gh_setting_get(host, i, &live) && values_equal(&wanted, &live)) continue;
    gh_setting_set(host, i, &wanted);
  }
  ui->render.applied = target;
}

bool render_layer_get(ui_handler_t *ui, render_target_t target, render_layer_t layer) {
  if (layer < 0 || layer >= RENDER_LAYER_COUNT) return false;
  if (target == RENDER_TARGET_VIDEO) return layer != RENDER_LAYER_CAMERA_PATH && render_video_profile(ui)->layers[layer];
  // The timeline's prediction switch is the viewport's.
  if (layer == RENDER_LAYER_PREDICTION) return ui->timeline.prediction.enabled;
  return ui->render.viewport.layers[layer];
}

void render_layer_set(ui_handler_t *ui, render_target_t target, render_layer_t layer, bool enabled) {
  if (layer < 0 || layer >= RENDER_LAYER_COUNT) return;
  if (target == RENDER_TARGET_VIDEO) render_video_profile(ui)->layers[layer] = enabled;
  else if (layer == RENDER_LAYER_PREDICTION) ui->timeline.prediction.enabled = enabled;
  else ui->render.viewport.layers[layer] = enabled;
}

bool render_layer_enabled(ui_handler_t *ui, render_layer_t layer) {
  return render_layer_get(ui, ui->render.applied, layer);
}

bool render_group_visible(ui_handler_t *ui, int group_index) {
  timeline_state_t *ts = &ui->timeline;
  if (group_index < 0 || group_index >= ts->group_count) return false;
  const timeline_group_t *group = ts->groups[group_index];
  const bool video = ui->render.applied == RENDER_TARGET_VIDEO;
  // A group drawn fully transparent is not drawn at all.
  return video ? group->video_visible && group->video_opacity > 0.f : group->visible && group->opacity > 0.f;
}

float render_group_opacity(ui_handler_t *ui, int group_index) {
  timeline_state_t *ts = &ui->timeline;
  if (group_index < 0 || group_index >= ts->group_count) return 1.f;
  const timeline_group_t *group = ts->groups[group_index];
  const float opacity = ui->render.applied == RENDER_TARGET_VIDEO ? group->video_opacity : group->opacity;
  return opacity < 0.f ? 0.f : opacity > 1.f ? 1.f : opacity;
}

int render_focus_group(ui_handler_t *ui, bool *out_merged) {
  const timeline_state_t *ts = &ui->timeline;
  const int focus = ui->render.applied == RENDER_TARGET_VIDEO ? render_video_profile(ui)->focus : RENDER_FOCUS_SELECTED;
  if (out_merged) *out_merged = focus == RENDER_FOCUS_MERGED;
  // A group removed since it was chosen falls back to the editor's.
  return focus >= 0 && focus < ts->group_count ? focus : ts->active_group_index;
}
