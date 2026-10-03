#include "starting_state.h"

#include "timeline/timeline_commands.h"
#include "timeline/timeline_model.h"
#include "user_interface.h"
#include <frametee/icons.h>
#include <renderer/graphics_backend.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/include_cimgui.h>

// Edits are staged. The widgets below write into this draft and nothing reaches
// the track, the group's starting world or the run built from it until Apply,
// so a drag no longer resimulates the timeline on every frame it moves.
static starting_config_t g_draft;
// What the track held when the draft was seeded, so a config that moved on its
// own (an undo, a project load) can be told apart from an unapplied edit here.
static starting_config_t g_draft_baseline;
static int g_draft_track = -1;

// Armed by "Pick position", spent by the next click in the viewport.
static bool g_picking;
static int g_pick_track = -1;

static const ft_prop_desc *starting_prop(const ft_entity_class *player_class, uint32_t index) {
  const ft_prop_desc *prop = &player_class->props[index];
  if (!(prop->flags & FT_PROP_STARTING)) return NULL;
  if (prop->flags & FT_PROP_READ_ONLY_UI) return NULL;
  return prop;
}

static const starting_override_t *find_override_const(const starting_config_t *config, const char *prop_id) {
  for (int i = 0; i < config->override_count; ++i)
    if (strcmp(config->overrides[i].prop_id, prop_id) == 0) return &config->overrides[i];
  return NULL;
}

static starting_override_t *find_override(starting_config_t *config, const char *prop_id) {
  return (starting_override_t *)find_override_const(config, prop_id);
}

static starting_override_t *ensure_override(starting_config_t *config, const char *prop_id, ft_value_kind kind) {
  starting_override_t *existing = find_override(config, prop_id);
  if (existing) return existing;
  if (config->override_count >= MAX_STARTING_OVERRIDES) return NULL;

  starting_override_t *added = &config->overrides[config->override_count++];
  memset(added, 0, sizeof(*added));
  snprintf(added->prop_id, sizeof(added->prop_id), "%s", prop_id);
  added->value.kind = kind;
  if (kind == FT_VALUE_STRING) added->value.as.s = added->string_value;
  return added;
}

static void store_value(starting_override_t *override, const ft_value *value) {
  override->value = *value;
  if (value->kind == FT_VALUE_STRING) {
    snprintf(override->string_value, sizeof(override->string_value), "%s", value->as.s ? value->as.s : "");
    override->value.as.s = override->string_value;
  }
}

// Fills every override from what the player actually is at the tick on screen.
// This is how "start from here" works, and how enabling the override the first
// time gets sensible values instead of zeroes.
static bool seize_from_current(ui_handler_t *ui, int track_index, starting_config_t *target) {
  timeline_state_t *ts = &ui->timeline;
  game_host_t *host = &ui->gfx_handler->game_host;
  const ft_entity_class *player_class = gh_entity_class(host, FT_ENTITY_CLASS_PLAYER);
  if (!player_class) return false;

  const int group_index = model_track_group_index(ts, track_index);
  const int local_index = model_group_local_track_index(ts, track_index);
  if (group_index < 0 || local_index < 0) return false;
  const ft_world *world = model_group_world_at_tick(ts, group_index, ts->current_tick);
  if (!world) return false;

  bool any = false;
  for (uint32_t i = 0; i < player_class->prop_count; ++i) {
    const ft_prop_desc *prop = starting_prop(player_class, i);
    if (!prop || !prop->id) continue;
    ft_value value;
    if (!gh_entity_prop_get(host, world, FT_ENTITY_CLASS_PLAYER, local_index, i, &value)) continue;
    starting_override_t *override = ensure_override(target, prop->id, value.kind);
    if (!override) continue;
    store_value(override, &value);
    any = true;
  }
  return any;
}

// Registers whatever the edit turned out to be. The command owns copies of both
// states, so nothing here has to be freed.
static void commit_edit(ui_handler_t *ui, int track_index, const starting_config_t *before, const char *description) {
  undo_command_t *command = commands_create_starting_config_change(ui, track_index, before, description);
  if (command) undo_manager_register_command(&ui->undo_manager, command);
}

// A copy of a config points its string values at the config it was copied from,
// so every copy taken here is rebound to its own storage before it is used.
static void copy_config(starting_config_t *out, const starting_config_t *in) {
  *out = *in;
  model_rebind_starting_strings(out);
}

static bool override_equal(const starting_override_t *a, const starting_override_t *b) {
  if (strcmp(a->prop_id, b->prop_id) != 0) return false;
  if (a->value.kind != b->value.kind) return false;
  // The union holds a pointer for a string, and one that is only ever equal by
  // accident, so those compare through their own storage instead.
  if (a->value.kind == FT_VALUE_STRING) return strcmp(a->string_value, b->string_value) == 0;
  return memcmp(&a->value.as, &b->value.as, sizeof(a->value.as)) == 0;
}

static bool configs_equal(const starting_config_t *a, const starting_config_t *b) {
  if (a->enabled != b->enabled || a->override_count != b->override_count) return false;
  for (int i = 0; i < a->override_count; ++i)
    if (!override_equal(&a->overrides[i], &b->overrides[i])) return false;
  return true;
}

static void seed_draft(const starting_config_t *config, int track_index) {
  copy_config(&g_draft, config);
  copy_config(&g_draft_baseline, config);
  g_draft_track = track_index;
}

// The one property the editor knows the meaning of, and only because every game
// already reports a position under this id for the viewport to pick entities by.
static const ft_prop_desc *position_prop(const ft_entity_class *player_class) {
  for (uint32_t i = 0; i < player_class->prop_count; ++i) {
    const ft_prop_desc *prop = starting_prop(player_class, i);
    if (prop && prop->id && prop->kind == FT_VALUE_VEC2 && strcmp(prop->id, "position") == 0) return prop;
  }
  return NULL;
}

void starting_state_cancel_pick(void) {
  g_picking = false;
  g_pick_track = -1;
}

bool starting_state_is_picking(void) { return g_picking; }

bool starting_state_take_world_click(ui_handler_t *ui, float world_x, float world_y) {
  if (!g_picking) return false;
  const int track_index = g_pick_track;
  starting_state_cancel_pick();
  // The click is spent either way: it was aimed at placing a start, not at
  // whatever it would otherwise have selected.
  if (!ui || track_index < 0 || track_index != g_draft_track) return true;

  starting_override_t *override = ensure_override(&g_draft, "position", FT_VALUE_VEC2);
  if (!override) return true;
  override->value.kind = FT_VALUE_VEC2;
  override->value.as.v.x = world_x;
  override->value.as.v.y = world_y;
  return true;
}

// Just under the inspector's own highlight, so selecting an entity on top of a
// start still reads.
#define START_MARKER_Z 9.4f

// Which position the marker should show for a track: the panel's unapplied edit
// while it is the one being edited, so picking a spot can be seen before Apply,
// and the applied start otherwise. A draft whose baseline no longer matches the
// track was left behind by a config that moved on its own, and is ignored.
static const starting_override_t *marker_position(const starting_config_t *config, int track_index) {
  const starting_config_t *shown = config;
  if (track_index == g_draft_track && configs_equal(config, &g_draft_baseline)) shown = &g_draft;
  const starting_override_t *position = find_override_const(shown, "position");
  if (!position || position->value.kind != FT_VALUE_VEC2) return NULL;
  return position;
}

void starting_state_render_markers(ui_handler_t *ui, struct gfx_handler_t *gfx) {
  if (!ui || !gfx) return;
  // The override is a position on a plane; a game whose world is a volume gets
  // no marker rather than one an axis short.
  if (game_is_3d(&gfx->game_host)) return;

  timeline_state_t *ts = &ui->timeline;
  for (int track_index = 0; track_index < ts->player_track_count; ++track_index) {
    const starting_config_t *config = &ts->player_tracks[track_index].starting_config;
    if (!config->enabled) continue;
    const int group_index = model_track_group_index(ts, track_index);
    if (group_index < 0 || group_index >= ts->group_count || !ts->groups[group_index]->visible) continue;

    const starting_override_t *position = marker_position(config, track_index);
    if (!position) continue;

    const float x = position->value.as.v.x;
    const float y = position->value.as.v.y;
    const float *rgb = ts->groups[group_index]->color;
    const float alpha = track_index == ts->selected_player_track_index ? 0.95f : 0.6f;

    // A crosshair over a halo: the arms say exactly which point the start is,
    // and the halo keeps it findable over a busy level.
    renderer_submit_circle_filled(gfx, START_MARKER_Z, (vec2){x, y}, 0.45f, (vec4){rgb[0], rgb[1], rgb[2], alpha * 0.25f}, 16);
    renderer_submit_circle_filled(gfx, START_MARKER_Z, (vec2){x, y}, 0.13f, (vec4){rgb[0], rgb[1], rgb[2], alpha}, 12);
    renderer_submit_line(gfx, START_MARKER_Z, (vec2){x - 0.8f, y}, (vec2){x + 0.8f, y}, (vec4){rgb[0], rgb[1], rgb[2], alpha}, 0.07f);
    renderer_submit_line(gfx, START_MARKER_Z, (vec2){x, y - 0.8f}, (vec2){x, y + 0.8f}, (vec4){rgb[0], rgb[1], rgb[2], alpha}, 0.07f);
  }
}

static float drag_speed(const ft_prop_desc *prop) {
  const double span = prop->max_value - prop->min_value;
  if (span <= 0.0) return 0.25f;
  return (float)(span / 200.0) > 0.01f ? (float)(span / 200.0) : 0.01f;
}

// One control for one property, in whatever shape its kind calls for. Returns
// true when the value changed this frame.
static bool draw_prop_widget(const ft_prop_desc *prop, ft_value *value) {
  const bool bounded = prop->max_value > prop->min_value;
  const float minimum = bounded ? (float)prop->min_value : 0.f;
  const float maximum = bounded ? (float)prop->max_value : 0.f;
  const char *label = prop->display_name ? prop->display_name : prop->id;
  bool changed = false;

  char label_with_unit[96];
  if (prop->unit && *prop->unit) {
    snprintf(label_with_unit, sizeof(label_with_unit), "%s (%s)", label, prop->unit);
    label = label_with_unit;
  }

  switch (value->kind) {
  case FT_VALUE_BOOL:
    changed = igCheckbox(label, &value->as.b);
    break;
  case FT_VALUE_INT: {
    int as_int = (int)value->as.i;
    changed = igDragInt(label, &as_int, 1.f, bounded ? (int)minimum : 0, bounded ? (int)maximum : 0, "%d", 0);
    if (changed) value->as.i = as_int;
    break;
  }
  case FT_VALUE_FLOAT: {
    float as_float = (float)value->as.f;
    changed = igDragFloat(label, &as_float, drag_speed(prop), minimum, maximum, "%.4f", 0);
    if (changed) value->as.f = as_float;
    break;
  }
  case FT_VALUE_VEC2: {
    float axes[2] = {value->as.v.x, value->as.v.y};
    changed = igDragFloat2(label, axes, drag_speed(prop), minimum, maximum, "%.3f", 0);
    if (changed) {
      value->as.v.x = axes[0];
      value->as.v.y = axes[1];
    }
    break;
  }
  case FT_VALUE_VEC3: {
    float axes[3] = {value->as.v3.x, value->as.v3.y, value->as.v3.z};
    changed = igDragFloat3(label, axes, drag_speed(prop), minimum, maximum, "%.3f", 0);
    if (changed) {
      value->as.v3.x = axes[0];
      value->as.v3.y = axes[1];
      value->as.v3.z = axes[2];
    }
    break;
  }
  case FT_VALUE_STRING:
    break; // handled by the caller, which owns the storage
  default:
    break;
  }
  return changed;
}

// Nothing above has touched the run: the draft (the switch and the values) is
// written to the track here, in one step, which is also the one undo step the
// whole edit becomes.
static void draw_apply_row(ui_handler_t *ui, int track_index, int group_index, starting_config_t *config, float dpi) {
  timeline_state_t *ts = &ui->timeline;
  const bool dirty = !configs_equal(&g_draft, config);
  igSeparator();
  if (!dirty) igBeginDisabled(true);
  if (igButton(ICON_FA_CHECK " Apply", (ImVec2){-(110.f * dpi), 0.f})) {
    starting_config_t before_apply;
    copy_config(&before_apply, config);
    const bool switched = config->enabled != g_draft.enabled;
    copy_config(config, &g_draft);
    if (config->enabled) model_apply_starting_config(ts, track_index);
    else model_rebuild_group_start(ts, group_index);
    commit_edit(ui, track_index, &before_apply,
                !switched ? "Edit Starting State"
                : config->enabled ? "Enable Starting Override"
                                  : "Disable Starting Override");
    ui_mark_unsaved(ui);
    seed_draft(config, track_index);
  }
  igSameLine(0.f, 6.f * dpi);
  if (igButton(ICON_FA_ROTATE_LEFT " Revert", (ImVec2){-1.f, 0.f})) {
    seed_draft(config, track_index);
    starting_state_cancel_pick();
  }
  if (!dirty) igEndDisabled();
  if (dirty) igTextDisabled("Unapplied changes.");
}

// Which tracks "Apply changes to tracks" is ticked for, by track index. Sized to the
// track count it was opened with, and cleared whenever that count moves, since
// an index then no longer means the track it was ticked for.
static bool *g_targets;
static int g_target_count;

static bool reset_targets(int count) {
  bool *resized = realloc(g_targets, sizeof(bool) * (size_t)(count > 0 ? count : 1));
  if (!resized) return false;
  g_targets = resized;
  g_target_count = count;
  memset(g_targets, 0, sizeof(bool) * (size_t)count);
  return true;
}

// A new track in the same group that starts as the panel shows. The run it
// joins is rebuilt, so this is the whole timeline's undo step, like a clone.
static void add_track_with_draft(ui_handler_t *ui, int group_index) {
  timeline_state_t *ts = &ui->timeline;
  timeline_data_snapshot_t *before = commands_capture_timeline_data(ts);
  if (!before) return;

  const int old_active = ts->active_group_index;
  ts->active_group_index = group_index;
  player_track_t *added = model_add_new_track(ts, 1);
  ts->active_group_index = old_active;
  if (!added) {
    commands_free_timeline_data_snapshot(before);
    return;
  }
  copy_config(&added->starting_config, &g_draft);
  model_apply_starting_config(ts, (int)(added - ts->player_tracks));

  undo_command_t *command = commands_create_timeline_data_change(ui, before, "Add Track with Starting State");
  if (command) undo_manager_register_command(&ui->undo_manager, command);
  ui_mark_unsaved(ui);
}

static bool value_equal(const starting_override_t *override, const ft_value *value) {
  if (override->value.kind != value->kind) return false;
  switch (value->kind) {
  case FT_VALUE_BOOL:
    return override->value.as.b == value->as.b;
  case FT_VALUE_INT:
    return override->value.as.i == value->as.i;
  case FT_VALUE_FLOAT:
    return override->value.as.f == value->as.f;
  case FT_VALUE_VEC2:
    return override->value.as.v.x == value->as.v.x && override->value.as.v.y == value->as.v.y;
  case FT_VALUE_VEC3:
    return override->value.as.v3.x == value->as.v3.x && override->value.as.v3.y == value->as.v3.y &&
           override->value.as.v3.z == value->as.v3.z;
  case FT_VALUE_STRING:
    return strcmp(override->string_value, value->as.s ? value->as.s : "") == 0;
  default:
    return memcmp(&override->value.as, &value->as, sizeof(value->as)) == 0;
  }
}

// Which of the draft's overrides are edits: different from what the track has
// applied, or, for a property it has no override for yet, from what the track
// starts with anyway. Those are what "Apply changes to tracks" hands on, so a
// start that was only given a weapon hands on the weapon and not its position.
static int draft_changes(ui_handler_t *ui, int track_index, bool changed[MAX_STARTING_OVERRIDES]) {
  timeline_state_t *ts = &ui->timeline;
  game_host_t *host = &ui->gfx_handler->game_host;
  const starting_config_t *config = &ts->player_tracks[track_index].starting_config;
  const ft_world *start = model_group_world_at_tick(ts, model_track_group_index(ts, track_index), 0);
  const int local_index = model_group_local_track_index(ts, track_index);

  int count = 0;
  for (int i = 0; i < g_draft.override_count; ++i) {
    const starting_override_t *override = &g_draft.overrides[i];
    const starting_override_t *applied = config->enabled ? find_override_const(config, override->prop_id) : NULL;
    if (applied) {
      changed[i] = !override_equal(override, applied);
    } else {
      const int prop = model_find_player_prop(host, override->prop_id);
      ft_value current;
      changed[i] = prop < 0 || !start || local_index < 0 ||
                   !gh_entity_prop_get(host, start, FT_ENTITY_CLASS_PLAYER, local_index, (unsigned)prop, &current) ||
                   !value_equal(override, &current);
    }
    if (changed[i]) ++count;
  }
  return count;
}

// Applies the draft to the track being edited and writes only its edits onto
// every ticked one, as one undo step. Everything else a ticked track starts
// with is left as it was.
static void apply_changes_to_targets(ui_handler_t *ui, int track_index) {
  timeline_state_t *ts = &ui->timeline;
  game_host_t *host = &ui->gfx_handler->game_host;
  const ft_entity_class *player_class = gh_entity_class(host, FT_ENTITY_CLASS_PLAYER);
  bool changed[MAX_STARTING_OVERRIDES];
  draft_changes(ui, track_index, changed);

  int *tracks = malloc(sizeof(int) * (size_t)ts->player_track_count);
  starting_config_t *before = malloc(sizeof(starting_config_t) * (size_t)ts->player_track_count);
  int count = 0;
  if (tracks && before) {
    for (int i = 0; i < ts->player_track_count; ++i) {
      if (i != track_index && !(i < g_target_count && g_targets[i])) continue;
      starting_config_t *config = &ts->player_tracks[i].starting_config;
      tracks[count] = i;
      copy_config(&before[count++], config);
      if (i == track_index) {
        copy_config(config, &g_draft);
        continue;
      }
      // What a switched-off override still stores is not what the track starts
      // with, so it is not allowed to come back along with the edits.
      if (!config->enabled) config->override_count = 0;
      config->enabled = true;
      // Every other property is pinned to what the track starts with already.
      // That leaves a complete override, the same one the panel would build for
      // this track, so it does not then show up there as an edit still to apply.
      const int target_group = model_track_group_index(ts, i);
      const int target_local = model_group_local_track_index(ts, i);
      for (uint32_t p = 0; player_class && target_group >= 0 && target_local >= 0 && p < player_class->prop_count; ++p) {
        const ft_prop_desc *prop = starting_prop(player_class, p);
        ft_value current;
        if (!prop || !prop->id || find_override(config, prop->id)) continue;
        if (!gh_entity_prop_get(host, ts->groups[target_group]->initial_world, FT_ENTITY_CLASS_PLAYER, target_local, p, &current)) continue;
        starting_override_t *override = ensure_override(config, prop->id, current.kind);
        if (override) store_value(override, &current);
      }
      for (int o = 0; o < g_draft.override_count; ++o) {
        if (!changed[o]) continue;
        const starting_override_t *source = &g_draft.overrides[o];
        starting_override_t *override = ensure_override(config, source->prop_id, source->value.kind);
        if (override) store_value(override, &source->value);
      }
    }
    // rebuilt rather than written over, so nothing an old override set outlives it
    for (int group_index = 0; group_index < ts->group_count; ++group_index) {
      bool touched = false;
      for (int i = 0; i < count && !touched; ++i)
        touched = model_track_group_index(ts, tracks[i]) == group_index;
      if (touched) model_rebuild_group_start(ts, group_index);
    }
    undo_command_t *command = commands_create_starting_configs_change(ui, tracks, before, count, "Apply Starting Changes to Tracks");
    if (command) undo_manager_register_command(&ui->undo_manager, command);
    ui_mark_unsaved(ui);
    seed_draft(&ts->player_tracks[track_index].starting_config, track_index);
  }
  free(tracks);
  free(before);
}

// The two ways a start leaves the track it was made on: onto a new track, or
// onto tracks that already exist.
static void draw_share_row(ui_handler_t *ui, int track_index, int group_index, float dpi) {
  timeline_state_t *ts = &ui->timeline;
  game_host_t *host = &ui->gfx_handler->game_host;

  const bool can_add = !ts->recording && game_can_add_player(host, ts->player_track_count);
  if (!can_add) igBeginDisabled(true);
  const bool add = igButton(ICON_FA_PLUS " Add track with this start", (ImVec2){-1.f, 0.f});
  if (!can_add) igEndDisabled();

  bool changed[MAX_STARTING_OVERRIDES];
  const int change_count = draft_changes(ui, track_index, changed);
  const bool can_share = ts->player_track_count > 1 && change_count > 0;
  if (!can_share) igBeginDisabled(true);
  if (igButton(ICON_FA_COPY " Apply changes to tracks...", (ImVec2){-1.f, 0.f}) && reset_targets(ts->player_track_count))
    igOpenPopup_Str("StartingStateTargets", ImGuiPopupFlags_None);
  if (!can_share) igEndDisabled();

  bool apply = false;
  if (igBeginPopup("StartingStateTargets", ImGuiWindowFlags_AlwaysAutoResize)) {
    if (g_target_count != ts->player_track_count) reset_targets(ts->player_track_count);

    // what is about to be handed on, by the names the panel shows them under
    const ft_entity_class *player_class = gh_entity_class(host, FT_ENTITY_CLASS_PLAYER);
    igTextDisabled("Changes:");
    for (int o = 0; o < g_draft.override_count; ++o) {
      if (!changed[o]) continue;
      const int prop = model_find_player_prop(host, g_draft.overrides[o].prop_id);
      const char *name = player_class && prop >= 0 && player_class->props[prop].display_name ? player_class->props[prop].display_name
                                                                                              : g_draft.overrides[o].prop_id;
      igBulletText("%s", name);
    }
    igSeparator();

    if (igButton("All", (ImVec2){0.f, 0.f})) memset(g_targets, 1, sizeof(bool) * (size_t)g_target_count);
    igSameLine(0.f, 6.f * dpi);
    if (igButton("None", (ImVec2){0.f, 0.f})) memset(g_targets, 0, sizeof(bool) * (size_t)g_target_count);
    igSameLine(0.f, 6.f * dpi);
    if (igButton("This group", (ImVec2){0.f, 0.f}))
      for (int i = 0; i < g_target_count; ++i)
        g_targets[i] = model_track_group_index(ts, i) == group_index;

    int ticked = 0;
    const int rows = g_target_count - 1 < 12 ? g_target_count - 1 : 12;
    igBeginChild_Str("##targets", (ImVec2){260.f * dpi, (float)rows * igGetFrameHeightWithSpacing()}, false, ImGuiWindowFlags_None);
    for (int i = 0; i < g_target_count; ++i) {
      if (i == track_index) continue;
      const player_track_t *other = &ts->player_tracks[i];
      const int other_group = model_track_group_index(ts, i);
      char label[224];
      if (ts->group_count > 1 && other_group >= 0)
        snprintf(label, sizeof(label), "%s / %s", ts->groups[other_group]->name, other->name[0] ? other->name : "Track");
      else snprintf(label, sizeof(label), "%s", other->name[0] ? other->name : "Track");
      igPushID_Int(i);
      igCheckbox(label, &g_targets[i]);
      igPopID();
      if (g_targets[i]) ++ticked;
    }
    igEndChild();

    igSeparator();
    char apply_label[64];
    snprintf(apply_label, sizeof(apply_label), ICON_FA_CHECK " Apply to %d track%s", ticked, ticked == 1 ? "" : "s");
    if (ticked == 0 || change_count == 0) igBeginDisabled(true);
    if (igButton(apply_label, (ImVec2){-1.f, 0.f})) {
      apply = true;
      igCloseCurrentPopup();
    }
    if (ticked == 0 || change_count == 0) igEndDisabled();
    igEndPopup();
  }

  // Both move or rewrite the track array, so they run with nothing above still
  // holding a pointer into it.
  if (apply) apply_changes_to_targets(ui, track_index);
  else if (add) add_track_with_draft(ui, group_index);
}

bool starting_state_draw(ui_handler_t *ui, int track_index) {
  if (!ui) return false;

  timeline_state_t *ts = &ui->timeline;
  if (track_index < 0 || track_index >= ts->player_track_count) {
    igTextDisabled("No player track selected.");
    return false;
  }

  game_host_t *host = &ui->gfx_handler->game_host;
  const ft_entity_class *player_class = gh_entity_class(host, FT_ENTITY_CLASS_PLAYER);
  if (!player_class) {
    igTextDisabled("This game publishes no player properties.");
    return false;
  }

  uint32_t startable = 0;
  for (uint32_t i = 0; i < player_class->prop_count; ++i)
    if (starting_prop(player_class, i)) ++startable;
  if (startable == 0) {
    igTextDisabled("This game has nothing to override at the start.");
    return false;
  }

  player_track_t *track = &ts->player_tracks[track_index];
  starting_config_t *config = &track->starting_config;
  const int group_index = model_track_group_index(ts, track_index);

  igPushID_Int(track_index);

  // The draft follows the track it is drawn for, and re-seeds whenever the
  // track's own config moved underneath it, so the panel never shows an edit of
  // something that is no longer there.
  if (g_draft_track != track_index) starting_state_cancel_pick();
  if (g_draft_track != track_index || !configs_equal(config, &g_draft_baseline)) seed_draft(config, track_index);
  if (g_picking && igIsKeyPressed_Bool(ImGuiKey_Escape, false)) starting_state_cancel_pick();

  // The switch is staged like the values: it changes the draft, and Apply
  // (or Revert) decides, so ticking it never moves the start by itself.
  bool enabled = g_draft.enabled;
  if (igCheckbox("Override the start", &enabled)) {
    g_draft.enabled = enabled;
    // Turning it on with nothing stored yet takes the values the player has at
    // the tick on screen, which is nearly always what was meant by turning it
    // on while looking at that tick.
    if (enabled && g_draft.override_count == 0) seize_from_current(ui, track_index, &g_draft);
    // turned off again before it was ever applied: nothing left to apply
    if (!enabled && !config->enabled) seed_draft(config, track_index);
    starting_state_cancel_pick();
  }

  const float dpi = igGetFontSize() > 0.f ? igGetFontSize() / 19.f : 1.f;
  if (!g_draft.enabled) {
    igTextDisabled(config->enabled ? "Once applied, this player starts where the level puts it."
                                   : "This player starts where the level puts it.");
    starting_state_cancel_pick();
    if (config->enabled) draw_apply_row(ui, track_index, group_index, config, dpi);
    igPopID();
    return true;
  }

  const ft_prop_desc *place_prop = position_prop(player_class);
  const float take_width = place_prop ? -(160.f * dpi) : -1.f;
  if (igButton("Take from current tick", (ImVec2){take_width, 0.f})) seize_from_current(ui, track_index, &g_draft);

  if (place_prop) {
    igSameLine(0.f, 6.f * dpi);
    if (g_picking) {
      if (igButton(ICON_FA_XMARK " Cancel", (ImVec2){-1.f, 0.f})) starting_state_cancel_pick();
    } else {
      if (igButton(ICON_FA_CROSSHAIRS " Pick position", (ImVec2){-1.f, 0.f})) {
        g_picking = true;
        g_pick_track = track_index;
      }
    }
  }
  if (g_picking) igTextDisabled("Click in the viewport to place the start.");

  // Headings are the game's own grouping, and a game lists its properties in
  // whatever order suits its inspector rather than in heading order. So each
  // heading is drawn once, with everything under it, instead of every time the
  // list happens to come back around to it.
  for (uint32_t heading = 0; heading < player_class->prop_count; ++heading) {
    const ft_prop_desc *first = starting_prop(player_class, heading);
    if (!first || !first->id) continue;
    const char *group = first->group && *first->group ? first->group : NULL;

    bool heading_seen_before = false;
    for (uint32_t earlier = 0; earlier < heading && !heading_seen_before; ++earlier) {
      const ft_prop_desc *previous = starting_prop(player_class, earlier);
      if (!previous) continue;
      const char *previous_group = previous->group && *previous->group ? previous->group : NULL;
      heading_seen_before =
          (group == NULL && previous_group == NULL) || (group && previous_group && strcmp(group, previous_group) == 0);
    }
    if (heading_seen_before) continue;
    if (group) igSeparatorText(group);

    for (uint32_t i = heading; i < player_class->prop_count; ++i) {
      const ft_prop_desc *prop = starting_prop(player_class, i);
      if (!prop || !prop->id) continue;
      const char *prop_group = prop->group && *prop->group ? prop->group : NULL;
      if (!((group == NULL && prop_group == NULL) || (group && prop_group && strcmp(group, prop_group) == 0))) continue;

      starting_override_t *override = find_override(&g_draft, prop->id);
      if (!override) {
        // A property the game has published since this project was saved. Its
        // override starts as whatever the group already starts with, so merely
        // opening the panel cannot zero a value nobody touched.
        override = ensure_override(&g_draft, prop->id, prop->kind);
        if (!override) continue;
        const ft_world *start = model_group_world_at_tick(ts, group_index, 0);
        const int local_index = model_group_local_track_index(ts, track_index);
        ft_value current;
        if (start && local_index >= 0 && gh_entity_prop_get(host, start, FT_ENTITY_CLASS_PLAYER, local_index, i, &current))
          store_value(override, &current);
      }
      if (override->value.kind != prop->kind) {
        // The game changed a property's type since this project was saved.
        memset(&override->value, 0, sizeof(override->value));
        override->value.kind = prop->kind;
        if (prop->kind == FT_VALUE_STRING) override->value.as.s = override->string_value;
      }

      igPushID_Int((int)i);
      if (prop->kind == FT_VALUE_STRING) {
        if (igInputText(prop->display_name ? prop->display_name : prop->id, override->string_value, sizeof(override->string_value), 0,
                        NULL, NULL))
          override->value.as.s = override->string_value;
      } else {
        draw_prop_widget(prop, &override->value);
      }
      igPopID();
    }
  }

  draw_apply_row(ui, track_index, group_index, config, dpi);
  draw_share_row(ui, track_index, group_index, dpi);

  igPopID();
  return true;
}

void starting_state_render_window(ui_handler_t *ui) {
  game_host_t *host = &ui->gfx_handler->game_host;
  if (!game_host_ready(host)) return;
  // A game that declares FT_CAP_HOSTS_STARTING_STATE has already drawn the
  // editor somewhere it chose; opening a second one would be a duplicate.
  if (game_has_cap(host, FT_CAP_HOSTS_STARTING_STATE)) return;

  if (igBegin("Starting State", NULL, ImGuiWindowFlags_NoFocusOnAppearing)) {
    starting_state_draw(ui, ui->timeline.selected_player_track_index);
  }
  igEnd();
}
