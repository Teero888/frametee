#include "starting_state.h"

#include "timeline/timeline_commands.h"
#include "timeline/timeline_model.h"
#include "user_interface.h"
#include <renderer/graphics_backend.h>
#include <stdio.h>
#include <string.h>
#include <system/include_cimgui.h>

// One edit spans many frames while a drag is held, so the state to undo back to
// is captured when the widget is grabbed and registered when it is let go.
static starting_config_t g_edit_before;
static int g_edit_track = -1;
static char g_edit_description[64];

static const ft_prop_desc *starting_prop(const ft_entity_class *player_class, uint32_t index) {
  const ft_prop_desc *prop = &player_class->props[index];
  if (!(prop->flags & FT_PROP_STARTING)) return NULL;
  if (prop->flags & FT_PROP_READ_ONLY_UI) return NULL;
  return prop;
}

static starting_override_t *find_override(starting_config_t *config, const char *prop_id) {
  for (int i = 0; i < config->override_count; ++i)
    if (strcmp(config->overrides[i].prop_id, prop_id) == 0) return &config->overrides[i];
  return NULL;
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
static bool seize_from_current(ui_handler_t *ui, int track_index) {
  timeline_state_t *ts = &ui->timeline;
  game_host_t *host = &ui->gfx_handler->game_host;
  const ft_entity_class *player_class = gh_entity_class(host, FT_ENTITY_CLASS_PLAYER);
  if (!player_class) return false;

  const int group_index = model_track_group_index(ts, track_index);
  const int local_index = model_group_local_track_index(ts, track_index);
  if (group_index < 0 || local_index < 0) return false;
  const ft_world *world = model_group_world_at_tick(ts, group_index, ts->current_tick);
  if (!world) return false;

  starting_config_t *config = &ts->player_tracks[track_index].starting_config;
  bool any = false;
  for (uint32_t i = 0; i < player_class->prop_count; ++i) {
    const ft_prop_desc *prop = starting_prop(player_class, i);
    if (!prop || !prop->id) continue;
    ft_value value;
    if (!gh_entity_prop_get(host, world, FT_ENTITY_CLASS_PLAYER, local_index, i, &value)) continue;
    starting_override_t *override = ensure_override(config, prop->id, value.kind);
    if (!override) continue;
    store_value(override, &value);
    any = true;
  }
  return any;
}

// Remembers what to undo back to, from a copy taken before this frame's widgets
// ran: a checkbox has already flipped by the time it says it changed, so asking
// the track for its state at that point would record the new one as the old.
static void begin_edit(int track_index, const starting_config_t *before_frame, const char *description) {
  if (g_edit_track == track_index) return; // already inside an edit, e.g. a held drag
  g_edit_before = *before_frame;
  model_rebind_starting_strings(&g_edit_before);
  g_edit_track = track_index;
  snprintf(g_edit_description, sizeof(g_edit_description), "%s", description);
}

// Registers whatever the edit turned out to be. The command owns copies of both
// states, so nothing here has to be freed.
static void commit_edit(ui_handler_t *ui, int track_index, const starting_config_t *before, const char *description) {
  undo_command_t *command = commands_create_starting_config_change(ui, track_index, before, description);
  if (command) undo_manager_register_command(&ui->undo_manager, command);
}

static void end_edit(ui_handler_t *ui) {
  if (g_edit_track < 0) return;
  commit_edit(ui, g_edit_track, &g_edit_before, g_edit_description);
  g_edit_track = -1;
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
  case FT_VALUE_BOOL: changed = igCheckbox(label, &value->as.b); break;
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
  case FT_VALUE_STRING: break; // handled by the caller, which owns the storage
  default: break;
  }
  return changed;
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

  if (igCheckbox("Override the start", &config->enabled)) {
    // The checkbox has already flipped, so what to undo back to is this config
    // with the flag the other way round.
    starting_config_t before_toggle = *config;
    before_toggle.enabled = !config->enabled;
    model_rebind_starting_strings(&before_toggle);
    // Turning it on with nothing stored yet takes the values the player has at
    // the tick on screen, which is nearly always what was meant by turning it
    // on while looking at that tick.
    if (config->enabled && config->override_count == 0) seize_from_current(ui, track_index);
    if (config->enabled) model_apply_starting_config(ts, track_index);
    else model_rebuild_group_start(ts, group_index);
    commit_edit(ui, track_index, &before_toggle, config->enabled ? "Enable Starting Override" : "Disable Starting Override");
    ui_mark_unsaved(ui);
  }
  if (igIsItemHovered(0))
    igSetTooltip("Start this player from the values below instead of wherever the level puts it.");

  if (!config->enabled) {
    igTextDisabled("This player starts where the level puts it.");
    igPopID();
    return true;
  }

  if (igButton("Take from current tick", (ImVec2){-1.f, 0.f})) {
    const starting_config_t before_take = *config;
    if (seize_from_current(ui, track_index)) {
      model_apply_starting_config(ts, track_index);
      commit_edit(ui, track_index, &before_take, "Take Starting State From Tick");
      ui_mark_unsaved(ui);
    }
  }
  if (igIsItemHovered(0)) igSetTooltip("Copies everything below out of the player as it is right now.");

  // Headings are the game's own grouping, and a game lists its properties in
  // whatever order suits its inspector rather than in heading order. So each
  // heading is drawn once, with everything under it, instead of every time the
  // list happens to come back around to it.
  bool changed = false;
  // The state every widget below is about to edit. One copy a frame, so an edit
  // that lands this frame has something truthful to undo back to.
  const starting_config_t before_frame = *config;
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

      starting_override_t *override = find_override(config, prop->id);
      if (!override) {
        // A property the game has published since this project was saved. Its
        // override starts as whatever the group already starts with, so merely
        // opening the panel cannot zero a value nobody touched.
        override = ensure_override(config, prop->id, prop->kind);
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
      bool prop_changed = false;
      if (prop->kind == FT_VALUE_STRING) {
        if (igInputText(prop->display_name ? prop->display_name : prop->id, override->string_value, sizeof(override->string_value), 0,
                        NULL, NULL)) {
          override->value.as.s = override->string_value;
          prop_changed = true;
        }
      } else {
        prop_changed = draw_prop_widget(prop, &override->value);
      }
      if (prop_changed) {
        changed = true;
        begin_edit(track_index, &before_frame, "Edit Starting State");
      }
      // A drag stays active across frames and becomes one undo step when it is
      // let go; a checkbox says both in the same frame and becomes one on its
      // own.
      if (igIsItemDeactivatedAfterEdit()) end_edit(ui);
      igPopID();
    }
  }

  // An edit whose widget went away mid-drag (the panel closed, the selection
  // moved) would otherwise stay open and swallow the next one into its undo
  // step. Nothing is being touched any more, so it is finished.
  if (g_edit_track >= 0 && !igIsAnyItemActive()) end_edit(ui);

  if (changed) {
    // Every edit lands in the group's starting world straight away, so the
    // timeline below shows the run this start actually produces. Applying is
    // enough here: an override that exists is written over the same field every
    // time. Rebuilding from the level is only for one that stops existing.
    model_apply_starting_config(ts, track_index);
    ui_mark_unsaved(ui);
  }

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
