#include "recording_import.h"

#include "timeline/timeline_commands.h"
#include "timeline/timeline_model.h"
#include "timeline/timeline_recordings.h"
#include "timeline_events.h"
#include "undo_redo.h"
#include "user_interface.h"
#include <engine/game_host.h>
#include <frametee/icons.h>
#include <include_cimgui.h>
#include <logger/logger.h>
#include <nfd.h>
#include <renderer/graphics_backend.h>
#include <stdint.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/fs.h>
#include <user_interface/widgets/imcol.h>

static const char *LOG_SOURCE = "Recording Import";
static const char *POPUP_ID = "Import Recording##RecordingImport";

static struct {
  bool active;      // a dialog is up
  bool open_popup;  // open it on the next frame
  bool new_project; // start a project on the recording's level
  int recording_id;
  bool *selected; // per recording player
  int selected_count;
  bool switch_level; // put the project on the recording's level
  uint8_t *presence;  // per player, PRESENCE_BUCKETS of where in the recording it is
} imp;

bool recording_import_active(void) { return imp.active; }

bool recording_import_available(ui_handler_t *ui) {
  return ui && ui->gfx_handler && game_has_recordings(&ui->gfx_handler->game_host);
}

static const char *file_name(const char *path) {
  const char *name = path;
  for (const char *c = path; *c; ++c)
    if (*c == '/' || *c == '\\') name = c + 1;
  return name;
}

static bool read_file(const char *path, unsigned char **out, size_t *out_size) {
  FILE *file = fs_open(path, "rb");
  if (!file) return false;
  bool ok = fseek(file, 0, SEEK_END) == 0;
  const long length = ok ? ftell(file) : -1;
  ok = ok && length > 0 && fseek(file, 0, SEEK_SET) == 0;
  unsigned char *data = ok ? malloc((size_t)length) : NULL;
  ok = data && fread(data, (size_t)length, 1, file) == 1;
  fclose(file);
  if (!ok) {
    free(data);
    return false;
  }
  *out = data;
  *out_size = (size_t)length;
  return true;
}

static void close_dialog(ui_handler_t *ui, bool drop_recording) {
  // A recording nothing replays yet only held the dialog's attention.
  if (drop_recording) recordings_remove(&ui->timeline, imp.recording_id);
  free(imp.selected);
  free(imp.presence);
  memset(&imp, 0, sizeof(imp));
}

void recording_import_begin(ui_handler_t *ui, bool new_project) {
  if (!recording_import_available(ui) || imp.active || ui->timeline.recording) return;
  if (!new_project && !ui->gfx_handler->level) return; // nothing to import into yet
  const ft_game_constraints *constraints = &ui->gfx_handler->game_host.module->constraints;
  nfdu8char_t *path = NULL;
  nfdu8filteritem_t filters[] = {{constraints->recording_filter_name ? constraints->recording_filter_name : "Recording",
                                  constraints->recording_extension ? constraints->recording_extension : "*"}};
  nfdopendialogu8args_t args = {0};
  args.filterList = filters;
  args.filterCount = 1;
  if (NFD_OpenDialogU8_With(&path, &args) != NFD_OKAY || !path) return;
  recording_import_open(ui, path, new_project);
  NFD_FreePathU8(path);
}

void recording_import_open(ui_handler_t *ui, const char *path, bool new_project) {
  if (!recording_import_available(ui) || imp.active || ui->timeline.recording || !path) return;
  if (!new_project && !ui->gfx_handler->level) return;
  unsigned char *data = NULL;
  size_t size = 0;
  const bool read = read_file(path, &data, &size);
  char name[128];
  snprintf(name, sizeof(name), "%s", file_name(path));
  char *dot = strrchr(name, '.');
  if (dot && dot != name) *dot = '\0';
  if (!read) {
    log_error(LOG_SOURCE, "Could not read the recording.");
    return;
  }
  timeline_recording_t *recording = recordings_add(&ui->timeline, -1, name, data, size);
  free(data);
  if (!recording) return;
  imp.active = true;
  imp.open_popup = true;
  imp.new_project = new_project;
  imp.recording_id = recording->id;
}

// --- the import ------------------------------------------------------------------

// Switching the project's level and adding the recording's group are one step to undo: the level
// goes back first, so the timeline is restored onto the level it was made on.
typedef struct {
  undo_command_t base;
  undo_command_t *timeline_change;
  unsigned char *old_level, *new_level;
  size_t old_size, new_size;
  char old_name[128], new_name[128];
} ImportWithLevelCommand;

static void undo_import_with_level(void *command, void *ts) {
  ImportWithLevelCommand *c = command;
  timeline_state_t *timeline = ts;
  gfx_replace_level(timeline->ui->gfx_handler, c->old_level, c->old_size, c->old_name);
  if (c->timeline_change) c->timeline_change->undo(c->timeline_change, ts);
}

static void redo_import_with_level(void *command, void *ts) {
  ImportWithLevelCommand *c = command;
  timeline_state_t *timeline = ts;
  gfx_replace_level(timeline->ui->gfx_handler, c->new_level, c->new_size, c->new_name);
  if (c->timeline_change) c->timeline_change->redo(c->timeline_change, ts);
}

static void cleanup_import_with_level(void *command) {
  ImportWithLevelCommand *c = command;
  if (c->timeline_change && c->timeline_change->cleanup) c->timeline_change->cleanup(c->timeline_change);
  free(c->old_level);
  free(c->new_level);
  free(c);
}

static unsigned char *copy_level(game_host_t *host, const ft_level *level, size_t *out_size) {
  const size_t size = gh_level_serialize(host, level, NULL, 0);
  unsigned char *data = size ? malloc(size) : NULL;
  if (data && gh_level_serialize(host, level, data, size) != size) {
    free(data);
    data = NULL;
  }
  *out_size = data ? size : 0;
  return data;
}

typedef struct {
  timeline_state_t *ts;
  int group_index;
  int first_tick; // the recording tick shown at group tick 1
} event_import_t;

// A recording's event at recording tick m is shown by the group's world at tick m - first + 1:
// stepping from group tick t shows recording tick first + t.
static void import_event(void *user, const ft_timeline_event *event) {
  const event_import_t *target = user;
  ft_timeline_event placed = *event;
  placed.world_index = target->group_index;
  placed.tick = event->tick - target->first_tick + 1;
  timeline_event_t converted;
  if (placed.tick >= 0 && timeline_event_from_abi(&converted, &placed, target->group_index)) timeline_events_add(target->ts, converted);
}

// Adds a group with one replaying track per chosen player. Returns its index, or -1.
static int add_recording_group(ui_handler_t *ui, timeline_recording_t *recording, bool reuse_empty_first_group) {
  timeline_state_t *ts = &ui->timeline;
  game_host_t *host = &ui->gfx_handler->game_host;
  int group_index;
  if (reuse_empty_first_group && ts->group_count > 0 && model_group_track_count(ts, 0) == 0) {
    group_index = 0;
    snprintf(ts->groups[0]->name, sizeof(ts->groups[0]->name), "%s", recording->name);
  } else {
    if (!model_add_group(ts, recording->name)) return -1;
    group_index = ts->group_count - 1;
    // The recording starts where the playhead is.
    ts->groups[group_index]->start_offset = ts->current_tick > 0 ? ts->current_tick : 0;
  }
  model_set_active_group(ts, group_index);

  const int recording_ticks = recording->last_tick - recording->first_tick + 1;
  // Which world player each recorded player became, for the recording's own events.
  int32_t *world_players = malloc(sizeof(*world_players) * (size_t)(imp.selected_count > 0 ? imp.selected_count : 1));
  for (int i = 0; world_players && i < imp.selected_count; ++i) world_players[i] = -1;
  for (int i = 0; i < imp.selected_count; ++i) {
    if (!imp.selected[i]) continue;
    ft_recording_player player;
    if (!gh_recording_player(host, recording->handle, (unsigned)i, &player) || player.first_tick > player.last_tick) continue;
    player_track_t *track = model_add_new_track(ts, 1);
    if (!track) break;
    if (world_players) world_players[i] = model_group_local_track_index(ts, (int)(track - ts->player_tracks));
    snprintf(track->name, sizeof(track->name), "%s", player.name ? player.name : "Player");
    if (player.profile && player.profile_size <= sizeof(track->player_profile.data)) {
      memcpy(track->player_profile.data, player.profile, player.profile_size);
      track->player_profile.size = player.profile_size;
    }

    // The group's tick t shows the recording's first tick + t, so every player lines up with the
    // others and with the recording's own timing.
    input_snippet_t snippet = {0};
    snippet.id = ts->next_snippet_id++;
    snippet.kind = SNIPPET_PLAYBACK;
    snippet.recording_id = recording->id;
    snippet.recording_player = i;
    snippet.source_count = recording_ticks;
    snippet.source_offset = player.first_tick - recording->first_tick;
    snippet.input_count = player.last_tick - player.first_tick + 1;
    snippet.start_tick = snippet.source_offset;
    snippet.end_tick = snippet.start_tick + snippet.input_count;
    snippet.is_active = true;
    snippet.layer = 0;
    model_insert_snippet_into_track(track, &snippet);
  }
  if (world_players) {
    event_import_t target = {ts, group_index, recording->first_tick};
    gh_recording_events(host, recording->handle, world_players, (unsigned)imp.selected_count, import_event, &target);
    free(world_players);
  }
  model_recalc_group_physics(ts, group_index, 0);
  return group_index;
}

static void import_into_project(ui_handler_t *ui, timeline_recording_t *recording) {
  timeline_state_t *ts = &ui->timeline;
  game_host_t *host = &ui->gfx_handler->game_host;
  const ft_recording_info *info = &recording->info;

  unsigned char *old_level = NULL;
  size_t old_size = 0;
  char old_name[128];
  snprintf(old_name, sizeof(old_name), "%s", ui->loaded_level_name);
  if (imp.switch_level) {
    old_level = copy_level(host, ui->gfx_handler->level, &old_size);
    if (!old_level || !gfx_replace_level(ui->gfx_handler, info->level_data, info->level_size, info->level_name)) {
      free(old_level);
      log_error(LOG_SOURCE, "Could not switch the project to the recording's level.");
      return;
    }
  }

  timeline_data_snapshot_t *before = commands_capture_timeline_data(ts);
  const int group = before ? add_recording_group(ui, recording, false) : -1;
  undo_command_t *change = group >= 0 ? commands_create_timeline_data_change(ui, before, "Import Demo") : NULL;
  if (group < 0) commands_free_timeline_data_snapshot(before);
  if (!change) {
    if (imp.switch_level && old_level) gfx_replace_level(ui->gfx_handler, old_level, old_size, old_name);
    free(old_level);
    log_error(LOG_SOURCE, "Could not import '%s'.", recording->name);
    return;
  }

  if (imp.switch_level) {
    ImportWithLevelCommand *command = calloc(1, sizeof(*command));
    unsigned char *new_level = malloc(info->level_size);
    if (!command || !new_level) {
      // Without a way to undo the switch there is no way to undo the import either.
      free(command);
      free(new_level);
      free(old_level);
      undo_manager_register_command(&ui->undo_manager, change);
      ui_mark_unsaved(ui);
      return;
    }
    memcpy(new_level, info->level_data, info->level_size);
    snprintf(command->base.description, sizeof(command->base.description), "Import Demo (switch map)");
    command->base.undo = undo_import_with_level;
    command->base.redo = redo_import_with_level;
    command->base.cleanup = cleanup_import_with_level;
    command->timeline_change = change;
    command->old_level = old_level;
    command->old_size = old_size;
    command->new_level = new_level;
    command->new_size = info->level_size;
    snprintf(command->old_name, sizeof(command->old_name), "%s", old_name);
    snprintf(command->new_name, sizeof(command->new_name), "%s", info->level_name ? info->level_name : "map");
    undo_manager_register_command(&ui->undo_manager, &command->base);
  } else {
    undo_manager_register_command(&ui->undo_manager, change);
  }
  ui_mark_unsaved(ui);
  log_info(LOG_SOURCE, "Imported '%s' as group '%s'.", recording->name, ts->groups[group]->name);
}

static void import_as_new_project(ui_handler_t *ui, timeline_recording_t *recording) {
  // The recording is already open; it moves over to the new project instead of opening again.
  timeline_recording_t *kept = recordings_detach(&ui->timeline, recording->id);
  if (!kept) return;
  const ft_recording_info info = kept->info;
  on_level_load_memory(ui->gfx_handler, info.level_data, info.level_size);
  if (info.level_name) snprintf(ui->loaded_level_name, sizeof(ui->loaded_level_name), "%s", info.level_name);
  recordings_attach(&ui->timeline, kept);
  add_recording_group(ui, kept, true);
  ui_mark_unsaved(ui);
  log_info(LOG_SOURCE, "Started a project from '%s'.", kept->name);
}

// --- the dialog -----------------------------------------------------------------
//
// One fixed width; everything inside wraps to it. Spacing comes from a handful of constants so
// the header, the callout, the list and the footer line up on the same edges.

static const ImVec4 COLOR_OK = {0.40f, 0.80f, 0.50f, 1.f};
static const ImVec4 COLOR_WARNING = {1.00f, 0.72f, 0.28f, 1.f};
static const ImVec4 COLOR_ERROR = {1.00f, 0.45f, 0.40f, 1.f};
static const ImVec4 COLOR_INFO = {0.45f, 0.65f, 1.00f, 1.f};

#define DIALOG_WIDTH 540.f   // outer, before UI scale
#define DIALOG_PADDING 20.f  // around the content
#define SECTION_GAP 14.f     // between header, callout, list and footer
#define BUTTON_WIDTH 120.f

static float px(float value) { return value * gfx_get_ui_scale(); }

// The x of the content's right edge, in window coordinates, for right-aligning on the current line.
static float content_right(void) { return igGetCursorPosX() + igGetContentRegionAvail().x; }

static void gap(float height) { igDummy((ImVec2){0, px(height)}); }

static void format_duration(char *out, size_t size, int ticks, int tps) {
  const int seconds = tps > 0 ? ticks / tps : 0;
  snprintf(out, size, "%d:%02d", seconds / 60, seconds % 60);
}

// At least BUTTON_WIDTH wide, wider when the label needs it.
static float button_width(const char *label) {
  return fmaxf(px(BUTTON_WIDTH), igCalcTextSize(label, NULL, true, -1.f).x + igGetStyle()->FramePadding.x * 2.f);
}

static bool styled_button(const char *label, float width, ImVec4 base, ImVec4 hovered, ImVec4 active, bool enabled) {
  igBeginDisabled(!enabled);
  igPushStyleColor_Vec4(ImGuiCol_Button, base);
  igPushStyleColor_Vec4(ImGuiCol_ButtonHovered, hovered);
  igPushStyleColor_Vec4(ImGuiCol_ButtonActive, active);
  const bool pressed = igButton(label, (ImVec2){width, 0});
  igPopStyleColor(3);
  igEndDisabled();
  return pressed && enabled;
}

static bool primary_button(const char *label, float width, bool enabled) {
  return styled_button(label, width, (ImVec4){0.20f, 0.45f, 0.85f, 1.f}, (ImVec4){0.26f, 0.53f, 0.95f, 1.f}, (ImVec4){0.17f, 0.39f, 0.75f, 1.f},
                       enabled);
}

static bool secondary_button_small(const char *label) {
  igPushStyleColor_Vec4(ImGuiCol_Button, (ImVec4){0.22f, 0.23f, 0.26f, 1.f});
  igPushStyleColor_Vec4(ImGuiCol_ButtonHovered, (ImVec4){0.28f, 0.29f, 0.33f, 1.f});
  igPushStyleColor_Vec4(ImGuiCol_ButtonActive, (ImVec4){0.18f, 0.19f, 0.22f, 1.f});
  const bool pressed = igButton(label, (ImVec2){0, 0});
  igPopStyleColor(3);
  return pressed;
}

static bool secondary_button(const char *label) {
  return styled_button(label, button_width(label), (ImVec4){0.22f, 0.23f, 0.26f, 1.f}, (ImVec4){0.28f, 0.29f, 0.33f, 1.f}, (ImVec4){0.18f, 0.19f, 0.22f, 1.f},
                       true);
}

// Places the cursor so buttons of these widths end at the content's right edge.
static void begin_footer(const float *widths, int buttons) {
  const float spacing = igGetStyle()->ItemSpacing.x;
  float width = spacing * (float)(buttons - 1);
  for (int i = 0; i < buttons; ++i) width += widths[i];
  igSetCursorPosX(igGetCursorPosX() + fmaxf(0.f, igGetContentRegionAvail().x - width));
}

// The film icon in a tinted tile, the demo's name, and a line of details under it, all wrapped
// to the content width.
static void render_header(ui_handler_t *ui, const char *name, const char *details, ImVec4 accent) {
  const float tile = px(44.f), text_gap = px(12.f);
  ImDrawList *draw = igGetWindowDrawList();
  const ImVec2 at = igGetCursorScreenPos();
  const float left = igGetCursorPosX();
  ImDrawList_AddRectFilled(draw, at, (ImVec2){at.x + tile, at.y + tile}, igGetColorU32_Vec4((ImVec4){accent.x, accent.y, accent.z, 0.18f}),
                           px(8.f), 0);
  igPushFont(ui->font, 20.f);
  const ImVec2 icon = igCalcTextSize(ICON_FA_FILM, NULL, false, -1.f);
  ImDrawList_AddText_Vec2(draw, (ImVec2){at.x + (tile - icon.x) * 0.5f, at.y + (tile - icon.y) * 0.5f}, igGetColorU32_Vec4(accent), ICON_FA_FILM,
                          NULL);
  const float text_x = left + tile + text_gap;
  const float title_h = igGetTextLineHeight();
  igPopFont();
  const float details_h = details ? igGetTextLineHeight() : 0.f;
  // Centred on the tile when it fits on one line each; long names wrap below it.
  const float block_h = title_h + (details ? igGetStyle()->ItemSpacing.y * 0.5f + details_h : 0.f);
  igSetCursorPos((ImVec2){text_x, igGetCursorPosY() + fmaxf(0.f, (tile - block_h) * 0.5f)});
  igPushTextWrapPos(0.f); // the window's content edge
  igPushFont(ui->font, 20.f);
  igTextWrapped("%s", name);
  igPopFont();
  if (details) {
    igSetCursorPosX(text_x);
    igPushStyleColor_Vec4(ImGuiCol_Text, igGetStyle()->Colors[ImGuiCol_TextDisabled]);
    igTextWrapped("%s", details);
    igPopStyleColor(1);
  }
  igPopTextWrapPos();
  const float bottom = fmaxf(igGetCursorScreenPos().y, at.y + tile);
  igSetCursorScreenPos((ImVec2){at.x, bottom});
}

// A tinted box with an icon for the one thing the user should notice. Its text wraps inside.
static void begin_callout(const char *id, ImVec4 accent, const char *icon) {
  igPushStyleColor_Vec4(ImGuiCol_ChildBg, (ImVec4){accent.x, accent.y, accent.z, 0.10f});
  igPushStyleColor_Vec4(ImGuiCol_Border, (ImVec4){accent.x, accent.y, accent.z, 0.40f});
  igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2){px(12.f), px(10.f)});
  igPushStyleVar_Float(ImGuiStyleVar_ChildRounding, px(6.f));
  igBeginChild_Str(id, (ImVec2){0, 0}, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY,
                   ImGuiWindowFlags_NoScrollbar);
  igPopStyleVar(2);
  igPopStyleColor(2);
  igTextColored(accent, "%s", icon);
  igSameLine(0, px(10.f));
  igBeginGroup();
  igPushTextWrapPos(0.f); // the window's content edge
}

static void end_callout(void) {
  igPopTextWrapPos();
  igEndGroup();
  igEndChild();
}

#define PRESENCE_BUCKETS 96

// Per player, where in the demo it is: 0 absent, 1 present, 2 present but largely guessed. A bucket
// counts as guessed only when a good share of it is, or nearly every bar would be amber.
static void build_presence(game_host_t *host, timeline_recording_t *recording) {
  const int count = imp.selected_count;
  const int ticks = recording->last_tick - recording->first_tick + 1;
  imp.presence = calloc((size_t)(count > 0 ? count : 1), PRESENCE_BUCKETS);
  uint8_t *flags = ticks > 0 ? malloc((size_t)ticks) : NULL;
  if (!imp.presence || !flags) {
    free(flags);
    return;
  }
  for (int player = 0; player < count; ++player) {
    gh_recording_tick_flags(host, recording->handle, player, recording->first_tick, (unsigned)ticks, flags);
    int present[PRESENCE_BUCKETS] = {0}, guessed[PRESENCE_BUCKETS] = {0};
    for (int tick = 0; tick < ticks; ++tick) {
      if (!(flags[tick] & FT_RECORDING_TICK_PRESENT)) continue;
      const int bucket = (int)((long long)tick * PRESENCE_BUCKETS / ticks);
      present[bucket]++;
      if (flags[tick] & FT_RECORDING_TICK_APPROXIMATED) guessed[bucket]++;
    }
    uint8_t *row = &imp.presence[(size_t)player * PRESENCE_BUCKETS];
    for (int b = 0; b < PRESENCE_BUCKETS; ++b)
      row[b] = !present[b] ? 0 : guessed[b] * 4 >= present[b] ? 2 : 1;
  }
  free(flags);
}

static void draw_presence(int player, float width, float height, ImU32 color) {
  ImDrawList *draw = igGetWindowDrawList();
  const ImVec2 at = igGetCursorScreenPos();
  const ImVec2 end = {at.x + width, at.y + height};
  const float rounding = height * 0.5f;
  ImDrawList_AddRectFilled(draw, at, end, IM_COL32(255, 255, 255, 20), rounding, 0);
  if (imp.presence) {
    const uint8_t *row = &imp.presence[(size_t)player * PRESENCE_BUCKETS];
    for (int b = 0; b < PRESENCE_BUCKETS;) {
      if (!row[b]) {
        ++b;
        continue;
      }
      int e = b;
      while (e < PRESENCE_BUCKETS && row[e] == row[b]) ++e;
      const float x0 = at.x + width * (float)b / PRESENCE_BUCKETS, x1 = at.x + width * (float)e / PRESENCE_BUCKETS;
      ImDrawList_AddRectFilled(draw, (ImVec2){x0, at.y}, (ImVec2){fmaxf(x1, x0 + 1.f), end.y},
                               row[b] == 2 ? igGetColorU32_Vec4(COLOR_WARNING) : color, rounding, 0);
      b = e;
    }
  }
  igDummy((ImVec2){width, height});
}

static void render_players(ui_handler_t *ui, timeline_recording_t *recording) {
  game_host_t *host = &ui->gfx_handler->game_host;
  const int tps = game_ticks_per_second(host);
  const int count = (int)recording->info.player_count;
  if (!imp.selected) {
    imp.selected = calloc((size_t)(count > 0 ? count : 1), sizeof(*imp.selected));
    imp.selected_count = imp.selected ? count : 0;
    for (int i = 0; i < imp.selected_count; ++i) {
      ft_recording_player player;
      imp.selected[i] = gh_recording_player(host, recording->handle, (unsigned)i, &player) && player.suggested;
    }
    build_presence(host, recording);
  }

  // Heading: "Players  n of m" on the left, the toggle on the right, on one baseline.
  int chosen = 0;
  for (int i = 0; i < imp.selected_count; ++i) chosen += imp.selected[i];
  const char *toggle = chosen == imp.selected_count && chosen > 0 ? "Select none" : "Select all";
  igAlignTextToFramePadding();
  igTextUnformatted("Players", NULL);
  igSameLine(0, px(8.f));
  igAlignTextToFramePadding();
  igTextDisabled("%d of %d chosen", chosen, imp.selected_count);
  const float toggle_width = igCalcTextSize(toggle, NULL, false, -1.f).x + igGetStyle()->FramePadding.x * 2.f;
  igSameLine(0, 0);
  igSetCursorPosX(content_right() - toggle_width);
  if (secondary_button_small(toggle)) {
    const bool all = chosen != imp.selected_count;
    for (int i = 0; i < imp.selected_count; ++i) imp.selected[i] = all;
  }
  gap(6.f);

  // Rows carry no vertical cell padding, so the selectable fills the whole row; each cell centres
  // its content in the row itself.
  const float frame_h = igGetFrameHeight();
  const float row_h = frame_h + px(8.f);
  const float inset = (row_h - frame_h) * 0.5f;
  const int shown = count < 1 ? 1 : count > 7 ? 7 : count;
  // No player outlasts the demo, so its length is the widest time in the list.
  char longest[32];
  format_duration(longest, sizeof(longest), recording->last_tick - recording->first_tick + 1, tps);
  const float time_w = igCalcTextSize(longest, NULL, false, -1.f).x;
  const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_PadOuterX;
  igPushStyleVar_Vec2(ImGuiStyleVar_CellPadding, (ImVec2){px(6.f), 0.f});
  igPushStyleColor_Vec4(ImGuiCol_TableBorderStrong, (ImVec4){1.f, 1.f, 1.f, 0.08f});
  // The outer border takes a pixel above and below the rows.
  if (igBeginTable("##recording_players", 4, flags, (ImVec2){0, row_h * (float)shown + 2.f}, 0)) {
    igTableSetupColumn("##chosen", ImGuiTableColumnFlags_WidthFixed, frame_h, 0);
    igTableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch, 0, 0);
    igTableSetupColumn("In the demo", ImGuiTableColumnFlags_WidthFixed, px(140.f), 0);
    igTableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, time_w, 0);
    ImGuiListClipper *clipper = ImGuiListClipper_ImGuiListClipper();
    ImGuiListClipper_Begin(clipper, imp.selected_count, row_h);
    while (ImGuiListClipper_Step(clipper)) {
      for (int i = clipper->DisplayStart; i < clipper->DisplayEnd; ++i) {
        ft_recording_player player;
        if (!gh_recording_player(host, recording->handle, (unsigned)i, &player)) continue;
        igPushID_Int(i);
        igTableNextRow(0, row_h);

        // The whole row toggles through an invisible selectable underneath; the checkbox shows the state.
        igTableSetColumnIndex(0);
        const float row_y = igGetCursorPosY();
        igPushStyleColor_Vec4(ImGuiCol_HeaderHovered, (ImVec4){0});
        igPushStyleColor_Vec4(ImGuiCol_HeaderActive, (ImVec4){0});
        if (igSelectable_Bool("##row", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, (ImVec2){0, row_h}))
          imp.selected[i] = !imp.selected[i];
        igPopStyleColor(2);
        const float content_y = row_y + inset;
        igSetCursorPosY(content_y);
        igCheckbox("##chosen", &imp.selected[i]);

        igTableSetColumnIndex(1);
        igSetCursorPosY(content_y);
        const ImU32 color = player.has_color ? IM_COL32((player.color >> 16) & 255, (player.color >> 8) & 255, player.color & 255, 255)
                                             : IM_COL32(180, 180, 180, 255);
        {
          const ImVec2 at = igGetCursorScreenPos();
          const float r = px(4.5f);
          ImDrawList_AddCircleFilled(igGetWindowDrawList(), (ImVec2){at.x + r, at.y + frame_h * 0.5f}, r, color, 16);
          igDummy((ImVec2){r * 2.f, frame_h});
        }
        igSameLine(0, px(8.f));
        igAlignTextToFramePadding();
        if (imp.selected[i]) igTextUnformatted(player.name ? player.name : "Player", NULL);
        else igTextDisabled("%s", player.name ? player.name : "Player");
        if (player.suggested) {
          igSameLine(0, px(8.f));
          igAlignTextToFramePadding();
          igTextColored(COLOR_INFO, ICON_FA_VIDEO " recorder");
        }

        igTableSetColumnIndex(2);
        const float bar_h = px(6.f);
        igSetCursorPosY(row_y + (row_h - bar_h) * 0.5f);
        draw_presence(i, igGetContentRegionAvail().x, bar_h, imp.selected[i] ? (color & 0x00FFFFFFu) | 0xE6000000u : IM_COL32(150, 150, 150, 120));
        if (igIsItemHovered(0)) {
          char joins[32];
          format_duration(joins, sizeof(joins), player.first_tick - recording->first_tick, tps);
          igSetTooltip("First seen at %s. Amber: stretches the demo does not show exactly.", joins);
        }

        igTableSetColumnIndex(3);
        char text[32];
        format_duration(text, sizeof(text), player.last_tick - player.first_tick + 1, tps);
        igSetCursorPosY(content_y);
        igAlignTextToFramePadding();
        igSetCursorPosX(igGetCursorPosX() + fmaxf(0.f, igGetContentRegionAvail().x - igCalcTextSize(text, NULL, false, -1.f).x));
        igTextDisabled("%s", text);
        igPopID();
      }
    }
    ImGuiListClipper_End(clipper);
    ImGuiListClipper_destroy(clipper);
    igEndTable();
  }
  igPopStyleColor(1);
  igPopStyleVar(1);
}

static void render_level_notice(ui_handler_t *ui, timeline_recording_t *recording) {
  const ft_recording_info *info = &recording->info;
  const char *level = info->level_name ? info->level_name : "?";
  if (imp.new_project) {
    begin_callout("##level", COLOR_INFO, ICON_FA_CIRCLE_INFO);
    igTextWrapped("Starts a new project on '%s', the demo's own map.", level);
    end_callout();
    return;
  }
  if (gh_recording_level_matches(&ui->gfx_handler->game_host, recording->handle, ui->gfx_handler->level)) {
    begin_callout("##level", COLOR_OK, ICON_FA_CIRCLE_CHECK);
    igTextWrapped("Same map as this project. The demo becomes a new group starting at the playhead.");
    end_callout();
    return;
  }
  begin_callout("##level", COLOR_WARNING, ICON_FA_TRIANGLE_EXCLAMATION);
  igTextWrapped("Recorded on '%s', but this project uses '%s'. On another map its players move through walls.", level, ui->loaded_level_name);
  if (info->level_data) {
    gap(2.f);
    igCheckbox("Switch the project to the demo's map", &imp.switch_level);
    if (imp.switch_level) igTextDisabled("Every group keeps its tracks and runs on the new map.");
  }
  end_callout();
}

// Leaves the dialog. Without a level, the start screen is all there is behind it: it goes back to
// the game that is active, where the demo was picked from.
static void cancel_import(ui_handler_t *ui) {
  close_dialog(ui, true);
  if (!ui->gfx_handler->level) {
    game_host_t *host = &ui->gfx_handler->game_host;
    if (host->active >= 0 && game_host_browse(host, host->active)) game_host_browsed_set_variant(host, host->variant_id);
    ui->splash_stage = SPLASH_STAGE_START;
  }
  igCloseCurrentPopup();
}

static void render_loading(ui_handler_t *ui, timeline_recording_t *recording, float progress) {
  render_header(ui, recording->name, "Rebuilding every tick of the demo. The first time takes a while for long demos.", COLOR_INFO);
  gap(SECTION_GAP);
  // The bar and its percentage on one line, the percentage right-aligned.
  char percent[16];
  snprintf(percent, sizeof(percent), "%.0f%%", progress * 100.f);
  const float percent_w = igCalcTextSize("100%", NULL, false, -1.f).x;
  const float bar_w = igGetContentRegionAvail().x - percent_w - igGetStyle()->ItemSpacing.x;
  const float bar_h = px(6.f);
  const float line_h = igGetTextLineHeight();
  const ImVec2 at = igGetCursorScreenPos();
  ImDrawList *draw = igGetWindowDrawList();
  const float bar_y = at.y + (line_h - bar_h) * 0.5f;
  ImDrawList_AddRectFilled(draw, (ImVec2){at.x, bar_y}, (ImVec2){at.x + bar_w, bar_y + bar_h}, IM_COL32(255, 255, 255, 24), bar_h * 0.5f, 0);
  if (progress > 0.f)
    ImDrawList_AddRectFilled(draw, (ImVec2){at.x, bar_y}, (ImVec2){at.x + fmaxf(bar_h, bar_w * progress), bar_y + bar_h},
                             igGetColorU32_Vec4(COLOR_INFO), bar_h * 0.5f, 0);
  igDummy((ImVec2){bar_w, line_h});
  igSameLine(0, -1.f);
  igSetCursorPosX(content_right() - igCalcTextSize(percent, NULL, false, -1.f).x);
  igTextDisabled("%s", percent);
  gap(SECTION_GAP);
  begin_footer((float[]){button_width("Cancel")}, 1);
  if (secondary_button("Cancel")) cancel_import(ui);
}

static void render_failed(ui_handler_t *ui, timeline_recording_t *recording, const char *error) {
  render_header(ui, recording->name, NULL, COLOR_ERROR);
  gap(SECTION_GAP);
  begin_callout("##error", COLOR_ERROR, ICON_FA_TRIANGLE_EXCLAMATION);
  igTextWrapped("The demo could not be opened: %s", error[0] ? error : "unknown error");
  end_callout();
  gap(SECTION_GAP);
  begin_footer((float[]){button_width("Close")}, 1);
  if (secondary_button("Close")) cancel_import(ui);
}

static void render_ready(ui_handler_t *ui, timeline_recording_t *recording) {
  const ft_recording_info *info = &recording->info;
  char length[32], details[192];
  format_duration(length, sizeof(length), recording->last_tick - recording->first_tick + 1, game_ticks_per_second(&ui->gfx_handler->game_host));
  snprintf(details, sizeof(details), ICON_FA_MAP " %s    " ICON_FA_CLOCK " %s    " ICON_FA_USERS " %u", info->level_name ? info->level_name : "unknown map",
           length, info->player_count);
  render_header(ui, recording->name, details, COLOR_INFO);
  gap(SECTION_GAP);
  render_level_notice(ui, recording);
  gap(SECTION_GAP);
  render_players(ui, recording);
  gap(6.f);
  igTextDisabled("Demo players replay exactly as recorded.");
  gap(SECTION_GAP - 6.f);
  igSeparator();
  gap(SECTION_GAP);

  int chosen = 0;
  for (int i = 0; i < imp.selected_count; ++i) chosen += imp.selected[i];
  char import_label[48];
  snprintf(import_label, sizeof(import_label), chosen == 1 ? "Import 1 player" : "Import %d players", chosen);
  // Sized for every player at once, so the button keeps its width while players are picked.
  char widest[48];
  snprintf(widest, sizeof(widest), "Import %d players", imp.selected_count > 1 ? imp.selected_count : 2);
  const float import_w = fmaxf(button_width(widest), button_width(import_label));
  begin_footer((float[]){button_width("Cancel"), import_w}, 2);
  const bool cancel = secondary_button("Cancel");
  igSameLine(0, -1.f);
  const bool confirm = primary_button(import_label, import_w, chosen > 0);
  if (confirm) {
    if (imp.new_project) import_as_new_project(ui, recording);
    else import_into_project(ui, recording);
    close_dialog(ui, false);
    igCloseCurrentPopup();
  } else if (cancel || igIsKeyPressed_Bool(ImGuiKey_Escape, false)) {
    cancel_import(ui);
  }
}

void recording_import_render(ui_handler_t *ui) {
  if (!imp.active) return;
  timeline_state_t *ts = &ui->timeline;
  timeline_recording_t *recording = recordings_find(ts, imp.recording_id);
  if (!recording) {
    close_dialog(ui, false);
    return;
  }
  // Reopened should anything else close it: an import without its dialog leaves no way to finish or
  // cancel it, and the start screen stays away meanwhile.
  if (imp.open_popup || !igIsPopupOpen_Str(POPUP_ID, ImGuiPopupFlags_AnyPopupLevel)) {
    igOpenPopup_Str(POPUP_ID, ImGuiPopupFlags_None);
    imp.open_popup = false;
  }
  ImGuiViewport *viewport = igGetMainViewport();
  igSetNextWindowPos((ImVec2){viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.5f},
                     ImGuiCond_Always, (ImVec2){0.5f, 0.5f});
  // The width is fixed and the height follows the content, so nothing inside decides the width.
  igSetNextWindowSizeConstraints((ImVec2){px(DIALOG_WIDTH), 0}, (ImVec2){px(DIALOG_WIDTH), FLT_MAX}, NULL, NULL);
  igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2){px(DIALOG_PADDING), px(DIALOG_PADDING)});
  igPushStyleVar_Float(ImGuiStyleVar_WindowRounding, px(10.f));
  igPushStyleVar_Float(ImGuiStyleVar_FrameRounding, px(5.f));
  igPushStyleVar_Vec2(ImGuiStyleVar_ItemSpacing, (ImVec2){px(8.f), 0.f});
  igPushStyleVar_Vec2(ImGuiStyleVar_FramePadding, (ImVec2){px(10.f), px(5.f)});
  const bool open = igBeginPopupModal(POPUP_ID, NULL,
                                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar);
  if (!open) {
    igPopStyleVar(5);
    return;
  }

  float progress = 0.f;
  char error[256];
  const recording_status_t status = recordings_status(recording, &progress, error, sizeof(error));
  if (status == RECORDING_LOADING) render_loading(ui, recording, progress);
  else if (!recordings_wait(ts, recording)) render_failed(ui, recording, error);
  else render_ready(ui, recording);
  igPopStyleVar(5);
  igEndPopup();
}
