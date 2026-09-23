#include "camera_window.h"

#include "camera_rig.h"
#include <GLFW/glfw3.h>
#include <frametee/icons.h>
#include <limits.h>
#include <math.h>
#include <renderer/graphics_backend.h>
#include <renderer/renderer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/include_cimgui.h>
#include <system/input.h>
#include <user_interface/keybinds.h>
#include <user_interface/timeline/timeline_model.h>
#include <user_interface/timeline/timeline_renderer.h>
#include <user_interface/undo_redo.h>
#include <user_interface/user_interface.h>
#include <user_interface/widgets/imcol.h>

static const char *INTERP_NAMES[CAMERA_CHANNEL_COUNT][CAMERA_INTERP_COUNT] = {
    [CAMERA_CHANNEL_TIME] = {"Smooth", "Linear (constant speed)", "Hold (jump to next)"},
    [CAMERA_CHANNEL_POSE] = {"Smooth", "Linear (constant speed)", "Cut (hold)"},
    [CAMERA_CHANNEL_FOLLOW] = {"Smooth", "Linear", "Hold"},
    [CAMERA_CHANNEL_AIM] = {"Smooth", "Linear", "Hold"},
};
static const char *HANDLE_NAMES[CAMERA_HANDLE_COUNT] = {"Auto", "Aligned", "Free (corner)"};
static const char *CHANNEL_NAMES[CAMERA_CHANNEL_COUNT] = {"Game time", "Camera", "Follow", "Aim"};

static const float DEGREES_PER_RADIAN = 57.29577951308232f;
static const double MIN_PIXELS_PER_SECOND = 4.0, MAX_PIXELS_PER_SECOND = 4000.0;

static const ImU32 PLAYHEAD_COLOR = IM_COL32(255, 86, 86, 255);
static const ImU32 KEY_COLOR = IM_COL32(226, 232, 240, 255);
static const ImU32 SELECTED_COLOR = IM_COL32(255, 206, 84, 255);
static const ImU32 HANDLE_COLOR = IM_COL32(120, 200, 255, 255);
static const ImU32 RANGE_COLOR = IM_COL32(120, 190, 255, 255);

// Clock helpers

static double ticks_per_second(ui_handler_t *ui) { return camera_rig_ticks_per_second(ui->gfx_handler); }

// The editing grid is the export frame rate: keys and the playhead land on
// frames, so what is keyed is what is rendered.
static double frame_rate(const ui_handler_t *ui) {
  const video_export_options_t *o = &ui->video_options;
  return o->fps_num > 0 && o->fps_den > 0 ? (double)o->fps_num / (double)o->fps_den : 60.0;
}

static double snap_to_frame(const ui_handler_t *ui, double seconds) {
  const double fps = frame_rate(ui);
  return round(seconds * fps) / fps;
}

static double half_frame(const ui_handler_t *ui) { return 0.5 / frame_rate(ui); }

void camera_editor_export_range(ui_handler_t *ui, double *start, double *end) {
  const camera_timeline_t *camera = &ui->camera_timeline;
  if (camera->range_set) {
    *start = camera->range_start;
    *end = camera->range_end;
    return;
  }
  // The whole game timeline, wherever the remap puts its last tick, and every key.
  const double tps = ticks_per_second(ui);
  double last = camera_last_key_time(camera), game_end;
  if (camera_seconds_for_game_tick(camera, (double)model_get_max_timeline_tick(&ui->timeline), tps, 0.0, &game_end))
    last = fmax(last, game_end);
  *start = 0.0;
  *end = last;
}

// Undo

typedef struct camera_edit_command_t {
  undo_command_t base;
  camera_timeline_t before, after;
} camera_edit_command_t;

static void clamp_selection(ui_handler_t *ui) {
  camera_editor_t *ed = &ui->camera_editor;
  if (ed->selected_key >= camera_channel_count(&ui->camera_timeline, ed->selected_channel)) ed->selected_key = -1;
}

static void command_apply(void *ts, const camera_timeline_t *state) {
  ui_handler_t *ui = ((timeline_state_t *)ts)->ui;
  ui->camera_timeline = *state;
  clamp_selection(ui);
  ui_mark_unsaved(ui);
}

static void command_undo(void *command, void *ts) { command_apply(ts, &((camera_edit_command_t *)command)->before); }
static void command_redo(void *command, void *ts) { command_apply(ts, &((camera_edit_command_t *)command)->after); }
static void command_cleanup(void *command) { free(command); }

// Opens an undo step, unless one is already open. Returns whether it opened
// one, so the caller that did is the one to close it.
static bool edit_begin(ui_handler_t *ui, const char *description) {
  camera_editor_t *ed = &ui->camera_editor;
  if (ed->edit_open) return false;
  ed->edit_open = true;
  ed->edit_before = ui->camera_timeline;
  snprintf(ed->edit_description, sizeof(ed->edit_description), "%s", description);
  return true;
}

static void edit_end(ui_handler_t *ui) {
  camera_editor_t *ed = &ui->camera_editor;
  if (!ed->edit_open) return;
  ed->edit_open = false;
  if (memcmp(&ed->edit_before, &ui->camera_timeline, sizeof(ui->camera_timeline)) == 0) return;
  camera_edit_command_t *command = calloc(1, sizeof(*command));
  if (!command) return;
  snprintf(command->base.description, sizeof(command->base.description), "%s", ed->edit_description);
  command->base.undo = command_undo;
  command->base.redo = command_redo;
  command->base.cleanup = command_cleanup;
  command->before = ed->edit_before;
  command->after = ui->camera_timeline;
  undo_manager_register_command(&ui->undo_manager, &command->base);
  ui_mark_unsaved(ui);
}

// For a widget that edits over several frames: the step opens when it is
// grabbed and closes when it is let go.
static void track_item(ui_handler_t *ui, const char *description) {
  if (igIsItemActivated()) edit_begin(ui, description);
  if (igIsItemDeactivated()) edit_end(ui);
}

// Actions

static void select_key(ui_handler_t *ui, camera_channel_t channel, int key) {
  ui->camera_editor.selected_channel = channel;
  ui->camera_editor.selected_key = key;
}

static void set_playhead(ui_handler_t *ui, double seconds) { ui->camera_editor.playhead = fmax(0.0, seconds); }

static void key_current_view(ui_handler_t *ui) {
  camera_timeline_t *camera = &ui->camera_timeline;
  const double playhead = ui->camera_editor.playhead;
  camera_pose_t pose;
  if (!camera_rig_capture(ui->gfx_handler, playhead, &pose)) return;
  const bool opened = edit_begin(ui, "Key camera view");
  int index = camera_find_key(camera, CAMERA_CHANNEL_POSE, playhead, half_frame(ui));
  if (index >= 0) camera->pose_keys[index].pose = pose;
  else index = camera_insert_pose_key(camera, snap_to_frame(ui, playhead), &pose, CAMERA_INTERP_SMOOTH);
  if (opened) edit_end(ui);
  if (index >= 0) select_key(ui, CAMERA_CHANNEL_POSE, index);
}

static void delete_key(ui_handler_t *ui, camera_channel_t channel, int index) {
  if (index < 0) return;
  const bool opened = edit_begin(ui, "Delete camera key");
  camera_delete_key(&ui->camera_timeline, channel, index);
  if (opened) edit_end(ui);
  clamp_selection(ui);
}

static void easy_ease(ui_handler_t *ui, camera_channel_t channel, int index) {
  if (index < 0 || index >= camera_channel_count(&ui->camera_timeline, channel)) return;
  const bool opened = edit_begin(ui, "Easy ease");
  camera_easy_ease(&ui->camera_timeline, channel, index);
  if (opened) edit_end(ui);
}

static void toggle_play(ui_handler_t *ui) {
  camera_editor_t *ed = &ui->camera_editor;
  double start, end;
  camera_editor_export_range(ui, &start, &end);
  ed->playing = !ed->playing;
  if (ed->playing && end > start && ed->playhead >= end - half_frame(ui)) set_playhead(ui, start);
}

static void step_frames(ui_handler_t *ui, int frames) {
  ui->camera_editor.playing = false;
  set_playhead(ui, snap_to_frame(ui, ui->camera_editor.playhead) + frames / frame_rate(ui));
}

static void jump_to_key(ui_handler_t *ui, int direction) {
  const camera_timeline_t *camera = &ui->camera_timeline;
  const double playhead = ui->camera_editor.playhead, epsilon = half_frame(ui);
  double best = direction > 0 ? INFINITY : -INFINITY;
  for (int channel = 0; channel < CAMERA_CHANNEL_COUNT; ++channel)
    for (int i = 0; i < camera_channel_count(camera, (camera_channel_t)channel); ++i) {
      const double time = camera_channel_key_time(camera, (camera_channel_t)channel, i);
      if (direction > 0 && time > playhead + epsilon) best = fmin(best, time);
      if (direction < 0 && time < playhead - epsilon) best = fmax(best, time);
    }
  if (isfinite(best)) {
    ui->camera_editor.playing = false;
    set_playhead(ui, best);
  }
}

static void set_look_through(ui_handler_t *ui, bool look_through) {
  camera_editor_t *ed = &ui->camera_editor;
  if (ed->look_through && !look_through && ed->drives_game) {
    // Leaving the camera view starts the free camera where the shot is, so
    // the next key is a move from here.
    camera_pose_t pose;
    if (camera_rig_evaluate(ui->gfx_handler, ed->playhead, &pose)) camera_rig_seed_free_view(ui->gfx_handler, &pose);
  }
  ed->look_through = look_through;
}

static void set_range(ui_handler_t *ui, double start, double end) {
  camera_timeline_t *camera = &ui->camera_timeline;
  camera->range_set = true;
  camera->range_start = fmax(0.0, fmin(start, end));
  camera->range_end = fmax(camera->range_start, end);
}

// The camera's clock runs while the Camera tab is in use, or while the
// Render tab previews the video through it.
static bool owns_clock(const ui_handler_t *ui) { return ui->camera_editor.drives_game || ui->render.preview; }
bool camera_editor_owns_clock(const ui_handler_t *ui) { return owns_clock(ui); }
static bool looking_through(const ui_handler_t *ui) { return ui->camera_editor.look_through || ui->render.preview; }

// Frame loop

void camera_editor_reset(ui_handler_t *ui) {
  camera_editor_t *ed = &ui->camera_editor;
  memset(ed, 0, sizeof(*ed));
  ed->pixels_per_second = 120.f;
  ed->look_through = true;
  ed->selected_key = -1;
  ed->selected_channel = CAMERA_CHANNEL_POSE;
  camera_rig_forget();
}

// The level's 2D view size, which the timing needs to weigh zooms against pans.
static void sync_view_scale(gfx_handler_t *h) {
  const float scale = camera_rig_view_scale(h);
  if (h->user_interface.camera_timeline.view_scale != scale) h->user_interface.camera_timeline.view_scale = scale;
}

void camera_editor_update(ui_handler_t *ui) {
  camera_editor_t *ed = &ui->camera_editor;
  timeline_state_t *ts = &ui->timeline;
  if (!ui->gfx_handler->level || ts->recording) ed->drives_game = false;
  if (!owns_clock(ui)) {
    ed->playing = false;
    return;
  }
  sync_view_scale(ui->gfx_handler);
  if (ed->playing) set_playhead(ui, ed->playhead + igGetIO_Nil()->DeltaTime);
  // The path drawn in the level needs the characters' positions all along it,
  // and so does a chase, for which way they are heading.
  if (!looking_through(ui) || camera_rig_needs_heading(ui->gfx_handler)) camera_rig_prepare_paths(ui->gfx_handler);

  camera_editor_sync_game(ui);
}

void camera_editor_sync_game(ui_handler_t *ui) {
  camera_editor_t *ed = &ui->camera_editor;
  timeline_state_t *ts = &ui->timeline;
  if (!owns_clock(ui) || !ui->gfx_handler->level || ts->recording) return;
  // The game is the footage: show it at the remapped tick. The drawn tick is
  // the one at or after it, blended from the previous one by the fraction.
  double tick = camera_game_tick(&ui->camera_timeline, ed->playhead, ticks_per_second(ui));
  tick = fmin(fmax(tick, (double)model_get_min_global_tick(ts)), (double)(INT_MAX / 2));
  const double whole = ceil(tick);
  ts->current_tick = (int)whole;
  ed->game_intra = (float)(1.0 - (whole - tick));
  ts->is_playing = false;
}

bool camera_editor_process_keys(ui_handler_t *ui) {
  camera_editor_t *ed = &ui->camera_editor;
  keybind_manager_t *kb = &ui->keybinds;
  if (!owns_clock(ui)) return false;
  if (keybinds_is_action_pressed(kb, ACTION_PLAY_PAUSE, false)) toggle_play(ui);
  if (keybinds_is_action_pressed(kb, ACTION_PREV_FRAME, true)) step_frames(ui, -1);
  if (keybinds_is_action_pressed(kb, ACTION_NEXT_FRAME, true)) step_frames(ui, 1);
  if (keybinds_is_action_pressed(kb, ACTION_CAMERA_PREV_KEY, true)) jump_to_key(ui, -1);
  if (keybinds_is_action_pressed(kb, ACTION_CAMERA_NEXT_KEY, true)) jump_to_key(ui, 1);
  if (keybinds_is_action_pressed(kb, ACTION_CAMERA_INSERT_KEY, false)) key_current_view(ui);
  if (keybinds_is_action_pressed(kb, ACTION_CAMERA_DELETE_KEY, false))
    delete_key(ui, CAMERA_CHANNEL_POSE,
               camera_find_key(&ui->camera_timeline, CAMERA_CHANNEL_POSE, ed->playhead, half_frame(ui)));
  if (keybinds_is_action_pressed(kb, ACTION_CAMERA_LOOK_THROUGH, false)) set_look_through(ui, !ed->look_through);
  if (keybinds_is_action_pressed(kb, ACTION_CAMERA_EASY_EASE, false)) easy_ease(ui, ed->selected_channel, ed->selected_key);
  if (keybinds_is_action_pressed(kb, ACTION_DELETE_SNIPPET, false)) delete_key(ui, ed->selected_channel, ed->selected_key);
  return true;
}

void camera_editor_viewport_update(gfx_handler_t *h) {
  camera_editor_t *ed = &h->user_interface.camera_editor;
  if (!owns_clock(&h->user_interface) || !h->level || !looking_through(&h->user_interface)) return;
  camera_pose_t pose;
  if (camera_rig_evaluate(h, ed->playhead, &pose)) camera_rig_apply(h, &pose);
}

void camera_editor_render_gizmos(gfx_handler_t *h) {
  camera_editor_t *ed = &h->user_interface.camera_editor;
  // Looking through the camera, its own outline would only frame the view.
  if (!owns_clock(&h->user_interface) || !render_layer_enabled(&h->user_interface, RENDER_LAYER_CAMERA_PATH) ||
      looking_through(&h->user_interface))
    return;
  camera_rig_render_gizmos(h, ed->playhead, ed->selected_channel == CAMERA_CHANNEL_POSE ? ed->selected_key : -1);
}

void camera_editor_apply_for_export(gfx_handler_t *h, double seconds) {
  sync_view_scale(h);
  // Exporting has no frame to keep smooth: walk whatever the chase needs at once.
  if (camera_rig_needs_heading(h)) camera_rig_prepare_paths_now(h);
  camera_pose_t pose;
  if (camera_rig_evaluate(h, seconds, &pose)) camera_rig_apply(h, &pose);
}

// Viewport overlay: key points and path handles, drawn over the image so they
// stay crisp and easy to grab. Dragging moves them on the plane facing the
// view through where they started.

typedef enum grab_kind_t { GRAB_NONE, GRAB_EYE, GRAB_TARGET, GRAB_HANDLE } grab_kind_t;

typedef struct viewport_grab_t {
  grab_kind_t kind;
  int key;
  int point, side; // for handles
  float plane[3];  // where the grabbed thing was shown
  float raw[3];    // the keyed point it moves
  float factor;    // how far the keyed point moves per unit it is seen to move
} viewport_grab_t;

static viewport_grab_t g_viewport_grab;
static int add_key(ui_handler_t *ui, camera_channel_t channel, double time, float value, bool value_given);

static bool project_point(gfx_handler_t *h, const float world[3], ImVec2 origin, ImVec2 *out) {
  if (game_is_3d(&h->game_host)) {
    mat4 view_proj;
    renderer_camera3_view_proj(h, view_proj);
    vec4 clip, point = {world[0], world[1], world[2], 1.f};
    glm_mat4_mulv(view_proj, point, clip);
    if (clip[3] <= 1e-4f) return false;
    out->x = origin.x + (clip[0] / clip[3] + 1.f) * 0.5f * h->viewport[0];
    out->y = origin.y + (clip[1] / clip[3] + 1.f) * 0.5f * h->viewport[1];
    return true;
  }
  float sx, sy;
  world_to_screen(h, world[0], world[1], &sx, &sy);
  out->x = origin.x + sx;
  out->y = origin.y + sy;
  return true;
}

static bool unproject_point(gfx_handler_t *h, ImVec2 mouse, ImVec2 origin, const float plane[3], float out[3]) {
  const float sx = mouse.x - origin.x, sy = mouse.y - origin.y;
  if (!game_is_3d(&h->game_host)) {
    screen_to_world(h, sx, sy, &out[0], &out[1]);
    out[2] = plane[2];
    return true;
  }
  vec3 ray_origin, direction, normal;
  if (!screen_ray3(h, sx, sy, ray_origin, direction)) return false;
  renderer_camera3_forward(h, normal);
  const float facing = glm_vec3_dot(direction, normal);
  if (fabsf(facing) < 1e-6f) return false;
  vec3 to_plane = {plane[0] - ray_origin[0], plane[1] - ray_origin[1], plane[2] - ray_origin[2]};
  const float distance = glm_vec3_dot(to_plane, normal) / facing;
  for (int i = 0; i < 3; ++i) out[i] = ray_origin[i] + direction[i] * distance;
  return true;
}

// How far a keyed point moves on screen per unit it is moved in the key, with
// that moment's Follow and Aim in the way: Follow drags the pivot toward the
// character and Aim the look-at point. Zero where they pin it completely.
static float key_point_response(ui_handler_t *ui, int key, grab_kind_t kind) {
  const camera_timeline_t *camera = &ui->camera_timeline;
  const double time = camera->pose_keys[key].time;
  const bool is_3d = game_is_3d(&ui->gfx_handler->game_host);
  if (is_3d && kind == GRAB_EYE) return 1.f;
  const float follow = camera_eval_follow(camera, time), aim = is_3d ? camera_eval_aim(camera, time) : 0.f;
  return (1.f - follow) * (1.f - aim);
}

static void distance_to_segment(ImVec2 p, ImVec2 a, ImVec2 b, float *distance, float *along) {
  const float dx = b.x - a.x, dy = b.y - a.y, length = dx * dx + dy * dy;
  float t = length > 1e-6f ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / length : 0.f;
  t = fminf(1.f, fmaxf(0.f, t));
  *along = t;
  *distance = hypotf(p.x - (a.x + dx * t), p.y - (a.y + dy * t));
}

bool camera_editor_viewport_overlay(ui_handler_t *ui, float origin_x, float origin_y, bool hovered) {
  camera_editor_t *ed = &ui->camera_editor;
  camera_timeline_t *camera = &ui->camera_timeline;
  gfx_handler_t *h = ui->gfx_handler;
  if (!ed->drives_game || looking_through(ui) || !h->level || camera->pose_count == 0 ||
      !render_layer_enabled(ui, RENDER_LAYER_CAMERA_PATH)) {
    if (g_viewport_grab.kind != GRAB_NONE) edit_end(ui);
    g_viewport_grab.kind = GRAB_NONE;
    return false;
  }
  const bool is_3d = game_is_3d(&h->game_host);
  const float scale = gfx_get_ui_scale();
  const ImVec2 origin = {origin_x, origin_y};
  ImGuiIO *io = igGetIO_Nil();
  ImDrawList *draw = igGetWindowDrawList();
  const int selected = ed->selected_channel == CAMERA_CHANNEL_POSE ? ed->selected_key : -1;
  const float grab_radius = 9.f * scale;

  viewport_grab_t hover = {GRAB_NONE, -1, 0, 0, {0}, {0}, 1.f};
  float hover_distance = grab_radius;
  // Key points where the camera really is at each key: the eye in 3D, the
  // view center in 2D, and the look-at point of the selected key in 3D.
  for (int i = 0; i < camera->pose_count; ++i) {
    camera_pose_t shown;
    if (!camera_rig_key_pose(h, i, &shown)) continue;
    const camera_pose_t *keyed = &camera->pose_keys[i].pose;
    const float *point = is_3d ? shown.eye : shown.target;
    ImVec2 at;
    if (!project_point(h, point, origin, &at)) continue;
    const bool is_selected = i == selected;
    if (is_3d && is_selected) {
      ImVec2 target;
      if (project_point(h, shown.target, origin, &target)) {
        ImDrawList_AddLine(draw, at, target, IM_COL32(255, 206, 84, 120), 1.f * scale);
        ImDrawList_AddQuadFilled(draw, (ImVec2){target.x, target.y - 6.f * scale}, (ImVec2){target.x + 6.f * scale, target.y},
                                 (ImVec2){target.x, target.y + 6.f * scale}, (ImVec2){target.x - 6.f * scale, target.y},
                                 SELECTED_COLOR);
        const float d = hypotf(io->MousePos.x - target.x, io->MousePos.y - target.y);
        if (d < hover_distance) {
          hover_distance = d;
          hover = (viewport_grab_t){GRAB_TARGET, i, 1, 0, {0}, {0}, key_point_response(ui, i, GRAB_TARGET)};
          memcpy(hover.plane, shown.target, sizeof(hover.plane));
          memcpy(hover.raw, keyed->target, sizeof(hover.raw));
        }
      }
    }
    ImDrawList_AddCircleFilled(draw, at, (is_selected ? 7.f : 5.5f) * scale, is_selected ? SELECTED_COLOR : KEY_COLOR, 16);
    ImDrawList_AddCircle(draw, at, (is_selected ? 7.f : 5.5f) * scale, IM_COL32(0, 0, 0, 160), 16, 1.f);
    const float d = hypotf(io->MousePos.x - at.x, io->MousePos.y - at.y);
    if (d < hover_distance) {
      hover_distance = d;
      hover = (viewport_grab_t){GRAB_EYE, i, 0, 0, {0}, {0}, key_point_response(ui, i, GRAB_EYE)};
      memcpy(hover.plane, point, sizeof(hover.plane));
      memcpy(hover.raw, is_3d ? keyed->eye : keyed->target, sizeof(hover.raw));
    }
  }

  // The selected key's handles, which bend the path on either side of it.
  // They hang off the key where it is seen, keeping their keyed shape.
  camera_pose_t selected_shown;
  if (selected >= 0 && camera_rig_key_pose(h, selected, &selected_shown)) {
    const camera_spatial_t spatial = camera_pose_key_spatial(camera, selected);
    for (int point = is_3d ? 0 : 1; point < 2; ++point)
      for (int side = -1; side <= 1; side += 2) {
        if (side < 0 && (selected == 0 || camera->pose_keys[selected - 1].interp == CAMERA_INTERP_HOLD)) continue;
        if (side > 0 && (selected + 1 >= camera->pose_count || camera->pose_keys[selected].interp == CAMERA_INTERP_HOLD))
          continue;
        const float *base = point ? selected_shown.target : selected_shown.eye;
        const float *offset = point ? (side < 0 ? spatial.target_in : spatial.target_out)
                                    : (side < 0 ? spatial.eye_in : spatial.eye_out);
        const float tip[3] = {base[0] + offset[0], base[1] + offset[1], base[2] + offset[2]};
        ImVec2 from, to;
        if (!project_point(h, base, origin, &from) || !project_point(h, tip, origin, &to)) continue;
        const ImU32 color = point == 0 || !is_3d ? HANDLE_COLOR : IM_COL32(120, 200, 255, 140);
        ImDrawList_AddLine(draw, from, to, color, 1.5f * scale);
        ImDrawList_AddCircleFilled(draw, to, 4.5f * scale, color, 12);
        const float d = hypotf(io->MousePos.x - to.x, io->MousePos.y - to.y);
        if (d < hover_distance) {
          hover_distance = d;
          hover = (viewport_grab_t){GRAB_HANDLE, selected, point, side, {0}, {0}, 1.f};
          memcpy(hover.plane, base, sizeof(hover.plane));
        }
      }
  }

  // Nothing grabbable under the pointer: maybe the path itself, to key.
  double path_time = -1.0;
  ImVec2 path_at = {0, 0};
  if (hovered && hover.kind == GRAB_NONE && g_viewport_grab.kind == GRAB_NONE) {
    int count;
    const camera_path_sample_t *samples = camera_rig_path_samples(h, &count);
    float best = 6.f * scale;
    ImVec2 previous = {0, 0};
    bool have_previous = false;
    for (int i = 0; i < count; ++i) {
      ImVec2 at;
      const bool visible = project_point(h, samples[i].point, origin, &at);
      if (visible && have_previous && !samples[i].shot_start) {
        float distance, along;
        distance_to_segment(io->MousePos, previous, at, &distance, &along);
        if (distance < best) {
          best = distance;
          path_time = samples[i - 1].time + (samples[i].time - samples[i - 1].time) * along;
          path_at = (ImVec2){previous.x + (at.x - previous.x) * along, previous.y + (at.y - previous.y) * along};
        }
      }
      previous = at;
      have_previous = visible;
    }
  }

  bool took_mouse = g_viewport_grab.kind != GRAB_NONE;
  if (hovered && g_viewport_grab.kind == GRAB_NONE && hover.kind != GRAB_NONE) {
    const bool pinned = hover.kind != GRAB_HANDLE && hover.factor < 0.05f;
    igSetTooltip(hover.kind == GRAB_HANDLE ? "Drag to bend the path. Alt: move this side only"
                 : pinned                  ? "Follow and Aim hold this point on the character here"
                                           : "Click to select, drag to move the key");
    if (igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) {
      select_key(ui, CAMERA_CHANNEL_POSE, hover.key);
      took_mouse = true;
      if (!pinned) {
        g_viewport_grab = hover;
        edit_begin(ui, hover.kind == GRAB_HANDLE ? "Bend camera path" : "Move camera key");
        if (hover.kind == GRAB_HANDLE && input_alt_down()) camera_set_spatial_mode(camera, hover.key, CAMERA_HANDLE_FREE);
      }
    }
  } else if (path_time >= 0.0) {
    const double time = input_shift_down() ? path_time : snap_to_frame(ui, path_time);
    ImDrawList_AddCircleFilled(draw, path_at, 5.f * scale, IM_COL32(255, 255, 255, 200), 12);
    igSetTooltip("Click to add a camera key at %.2fs", time);
    if (igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) {
      add_key(ui, CAMERA_CHANNEL_POSE, time, 0.f, false);
      took_mouse = true;
    }
  }

  if (g_viewport_grab.kind != GRAB_NONE) {
    if (!igIsMouseDown_Nil(ImGuiMouseButton_Left)) {
      edit_end(ui);
      g_viewport_grab.kind = GRAB_NONE;
    } else if (g_viewport_grab.key < camera->pose_count) {
      float at[3];
      if (unproject_point(h, io->MousePos, origin, g_viewport_grab.plane, at)) {
        if (g_viewport_grab.kind == GRAB_HANDLE) {
          const float offset[3] = {at[0] - g_viewport_grab.plane[0], at[1] - g_viewport_grab.plane[1],
                                   at[2] - g_viewport_grab.plane[2]};
          camera_set_spatial_handle(camera, g_viewport_grab.key, g_viewport_grab.point, g_viewport_grab.side, offset);
          // In 2D the eye follows the view center's path.
          if (!is_3d) camera_set_spatial_handle(camera, g_viewport_grab.key, 0, g_viewport_grab.side, offset);
        } else {
          // Move the keyed point by what it takes to move the seen one under the pointer.
          float moved[3];
          for (int i = 0; i < 3; ++i)
            moved[i] = g_viewport_grab.raw[i] + (at[i] - g_viewport_grab.plane[i]) / g_viewport_grab.factor;
          camera_pose_t *pose = &camera->pose_keys[g_viewport_grab.key].pose;
          if (!is_3d) {
            pose->target[0] = pose->eye[0] = moved[0];
            pose->target[1] = pose->eye[1] = moved[1];
          } else {
            memcpy(g_viewport_grab.kind == GRAB_EYE ? pose->eye : pose->target, moved, sizeof(moved));
          }
        }
      }
    }
  }
  return took_mouse;
}

// Window geometry

typedef struct lane_t {
  camera_channel_t channel;
  float top, bottom;
} lane_t;

typedef struct layout_t {
  float label_x, x0, x1; // the label column's left edge; the lanes or graph, horizontally
  float ruler_top, ruler_split, ruler_bottom;
  lane_t lanes[CAMERA_CHANNEL_COUNT];
  int lane_count;
  float bottom;
  // The graph editor's area and value range.
  float graph_top, graph_bottom;
  double graph_low, graph_high;
  const double *distances;
} layout_t;

static float x_of(const camera_editor_t *ed, const layout_t *l, double seconds) {
  return l->x0 + (float)((seconds - ed->view_start) * ed->pixels_per_second);
}

static double time_of(const camera_editor_t *ed, const layout_t *l, float x) {
  return ed->view_start + (x - l->x0) / ed->pixels_per_second;
}

static float graph_pad(void) { return 10.f * gfx_get_ui_scale(); }

static float graph_y(const layout_t *l, double value) {
  const float pad = graph_pad();
  return l->graph_bottom - pad -
         (float)((value - l->graph_low) / (l->graph_high - l->graph_low)) * (l->graph_bottom - l->graph_top - 2.f * pad);
}

static double graph_value(const layout_t *l, float y) {
  const float pad = graph_pad();
  return l->graph_low + (l->graph_bottom - pad - y) / (l->graph_bottom - l->graph_top - 2.f * pad) * (l->graph_high - l->graph_low);
}

static float value_y(const lane_t *lane, float value) {
  const float pad = 6.f * gfx_get_ui_scale();
  return lane->bottom - pad - value * (lane->bottom - lane->top - 2.f * pad);
}

static float value_at(const lane_t *lane, float y) {
  const float pad = 6.f * gfx_get_ui_scale();
  return fminf(1.f, fmaxf(0.f, (lane->bottom - pad - y) / (lane->bottom - lane->top - 2.f * pad)));
}

static const lane_t *lane_for(const layout_t *l, camera_channel_t channel) {
  for (int i = 0; i < l->lane_count; ++i)
    if (l->lanes[i].channel == channel) return &l->lanes[i];
  return NULL;
}

static const lane_t *lane_at(const layout_t *l, float y) {
  for (int i = 0; i < l->lane_count; ++i)
    if (y >= l->lanes[i].top && y < l->lanes[i].bottom) return &l->lanes[i];
  return NULL;
}

// Where a key is drawn in the lanes, for drawing and hit testing alike.
static ImVec2 lane_key_position(ui_handler_t *ui, const layout_t *l, const lane_t *lane, int index) {
  const camera_timeline_t *camera = &ui->camera_timeline;
  const double time = camera_channel_key_time(camera, lane->channel, index);
  float y = 0.5f * (lane->top + lane->bottom);
  if (lane->channel == CAMERA_CHANNEL_FOLLOW) y = value_y(lane, camera->follow_keys[index].value);
  if (lane->channel == CAMERA_CHANNEL_AIM) y = value_y(lane, camera->aim_keys[index].value);
  return (ImVec2){x_of(&ui->camera_editor, l, time), y};
}

static ImVec2 graph_key_position(ui_handler_t *ui, const layout_t *l, int index) {
  const camera_channel_t channel = ui->camera_editor.selected_channel;
  const double time = camera_channel_key_time(&ui->camera_timeline, channel, index);
  const double value = camera_curve_key_value(&ui->camera_timeline, channel, index, l->distances);
  return (ImVec2){x_of(&ui->camera_editor, l, time), graph_y(l, value)};
}

// Where a key's ease handle sits in the graph, if it has one on that side: a
// smooth segment has to run that way.
static bool graph_handle(ui_handler_t *ui, const layout_t *l, int index, int side, double *time, double *value) {
  camera_timeline_t *camera = &ui->camera_timeline;
  const camera_channel_t channel = ui->camera_editor.selected_channel;
  const int count = camera_channel_count(camera, channel);
  if (index < 0 || index >= count || (side < 0 && index == 0) || (side > 0 && index + 1 >= count)) return false;
  if (*camera_channel_key_interp(camera, channel, side < 0 ? index - 1 : index) != CAMERA_INTERP_SMOOTH) return false;
  const camera_ease_t ease = camera_curve_key_ease(camera, channel, index, ticks_per_second(ui));
  const double key_time = camera_channel_key_time(camera, channel, index);
  const double span = fabs(camera_channel_key_time(camera, channel, index + side) - key_time);
  const double reach = (side < 0 ? ease.in_influence : ease.out_influence) * span;
  const double slope = side < 0 ? ease.in_slope : ease.out_slope;
  *time = key_time + side * reach;
  *value = camera_curve_key_value(camera, channel, index, l->distances) + side * slope * reach;
  return true;
}

// Drawing

static void draw_key(ImDrawList *draw, ImVec2 at, int interp, int handle_mode, bool selected, float scale) {
  const float r = (selected ? 7.5f : 6.f) * scale;
  const ImU32 color = selected ? SELECTED_COLOR : KEY_COLOR;
  const ImU32 outline = IM_COL32(0, 0, 0, 170);
  if (interp == CAMERA_INTERP_HOLD) {
    // A hold: a square with the step it makes drawn out of its right side.
    const float s = r * 0.8f;
    ImDrawList_AddRectFilled(draw, (ImVec2){at.x - s, at.y - s}, (ImVec2){at.x + s, at.y + s}, color, 0.f, 0);
    ImDrawList_AddLine(draw, (ImVec2){at.x + s, at.y - s}, (ImVec2){at.x + s + 5.f * scale, at.y - s}, color, 2.f * scale);
  } else if (interp == CAMERA_INTERP_LINEAR) {
    ImDrawList_AddTriangleFilled(draw, (ImVec2){at.x - r, at.y + r * 0.8f}, (ImVec2){at.x + r, at.y + r * 0.8f},
                                 (ImVec2){at.x, at.y - r}, color);
  } else if (handle_mode != CAMERA_HANDLE_AUTO) {
    // Shaped by hand: an hourglass, as After Effects draws a bezier key.
    ImDrawList_AddTriangleFilled(draw, (ImVec2){at.x - r, at.y - r}, (ImVec2){at.x, at.y}, (ImVec2){at.x - r, at.y + r}, color);
    ImDrawList_AddTriangleFilled(draw, (ImVec2){at.x + r, at.y - r}, (ImVec2){at.x + r, at.y + r}, (ImVec2){at.x, at.y}, color);
  } else {
    const ImVec2 points[4] = {{at.x, at.y - r}, {at.x + r, at.y}, {at.x, at.y + r}, {at.x - r, at.y}};
    ImDrawList_AddConvexPolyFilled(draw, points, 4, color);
    ImDrawList_AddPolyline(draw, points, 4, outline, ImDrawFlags_Closed, 1.f);
  }
}

static double nice_step(const double *steps, int count, double pixels_per_unit, double min_pixels) {
  for (int i = 0; i < count; ++i)
    if (steps[i] * pixels_per_unit >= min_pixels) return steps[i];
  return steps[count - 1];
}

static double nice_value_step(double span, int wanted) {
  const double raw = span / wanted;
  const double magnitude = pow(10.0, floor(log10(raw)));
  const double fraction = raw / magnitude;
  return (fraction < 1.5 ? 1.0 : fraction < 3.5 ? 2.0 : fraction < 7.5 ? 5.0 : 10.0) * magnitude;
}

static float text_width(const char *text) { return igCalcTextSize(text, NULL, false, 0.f).x; }

static void draw_seconds_ruler(ui_handler_t *ui, ImDrawList *draw, const layout_t *l) {
  const camera_editor_t *ed = &ui->camera_editor;
  const float scale = gfx_get_ui_scale();
  const double fps = frame_rate(ui);
  const float top = l->ruler_top, bottom = l->ruler_split;
  const ImU32 minor = igGetColorU32_Col(ImGuiCol_TextDisabled, 0.35f);
  const ImU32 major = igGetColorU32_Col(ImGuiCol_TextDisabled, 0.9f);
  const ImU32 text = igGetColorU32_Col(ImGuiCol_Text, 1.f);
  ImDrawList_AddRectFilled(draw, (ImVec2){l->x0, top}, (ImVec2){l->x1, bottom}, igGetColorU32_Col(ImGuiCol_FrameBg, 0.9f), 0.f, 0);

  const double t0 = fmax(0.0, time_of(ed, l, l->x0)), t1 = time_of(ed, l, l->x1);
  if (ed->pixels_per_second / fps >= 6.0)
    for (double f = ceil(t0 * fps); f <= t1 * fps; f += 1.0) {
      const float x = x_of(ed, l, f / fps);
      ImDrawList_AddLine(draw, (ImVec2){x, bottom - 4.f * scale}, (ImVec2){x, bottom}, minor, 1.f);
    }
  // Labels at least a label and a gap apart.
  const double steps[] = {1.0 / fps, 2.0 / fps, 5.0 / fps, 10.0 / fps, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800};
  const double step = nice_step(steps, (int)(sizeof(steps) / sizeof(steps[0])), ed->pixels_per_second,
                                text_width("00:00") + 28.f * scale);
  for (double t = floor(t0 / step) * step; t <= t1 + step; t += step) {
    if (t < -1e-9) continue;
    const float x = x_of(ed, l, t);
    if (x < l->x0 - 1.f || x > l->x1) continue;
    const double whole = floor(t + 1e-6);
    const int frame = (int)llround((t - whole) * fps);
    char label[32];
    if (frame != 0 && frame < (int)llround(fps)) snprintf(label, sizeof(label), "+%df", frame);
    else if (whole >= 60.0) snprintf(label, sizeof(label), "%d:%02d", (int)whole / 60, (int)whole % 60);
    else snprintf(label, sizeof(label), "%ds", (int)whole);
    ImDrawList_AddLine(draw, (ImVec2){x, bottom - 8.f * scale}, (ImVec2){x, bottom}, major, 1.f);
    ImDrawList_AddText_Vec2(draw, (ImVec2){x + 4.f * scale, top + 0.5f * (bottom - top - igGetTextLineHeight()) - 2.f * scale},
                            frame ? major : text, label, NULL);
  }
}

// Game ticks under the camera clock. The ruler is warped by the remap: it
// stretches in slow motion, stops in a freeze and runs backwards in reverse.
static void draw_game_ruler(ui_handler_t *ui, ImDrawList *draw, const layout_t *l) {
  const camera_editor_t *ed = &ui->camera_editor;
  const camera_timeline_t *camera = &ui->camera_timeline;
  const float scale = gfx_get_ui_scale();
  const double tps = ticks_per_second(ui);
  const float top = l->ruler_split, bottom = l->ruler_bottom;
  ImDrawList_AddRectFilled(draw, (ImVec2){l->x0, top}, (ImVec2){l->x1, bottom}, igGetColorU32_Col(ImGuiCol_FrameBg, 0.55f), 0.f, 0);
  const ImU32 tick_color = igGetColorU32_Col(ImGuiCol_TextDisabled, 0.8f);
  const ImU32 text = igGetColorU32_Col(ImGuiCol_TextDisabled, 1.f);

  const double steps[] = {1, 2, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000, 25000, 50000, 100000};
  const double step = nice_step(steps, (int)(sizeof(steps) / sizeof(steps[0])), ed->pixels_per_second / tps,
                                text_width("00000") + 20.f * scale);
  const float sample = 2.f;
  float free_from = -1e9f; // labels start past the last one
  double previous = camera_game_tick(camera, time_of(ed, l, l->x0), tps);
  for (float x = l->x0 + sample; x <= l->x1; x += sample) {
    const double tick = camera_game_tick(camera, time_of(ed, l, x), tps);
    const double a = floor(previous / step), b = floor(tick / step);
    if (a != b) {
      // A mark where the remap crosses a multiple of the step, either way.
      const double crossed = (b > a ? b : a) * step;
      ImDrawList_AddLine(draw, (ImVec2){x, bottom - 6.f * scale}, (ImVec2){x, bottom}, tick_color, 1.f);
      char label[32];
      snprintf(label, sizeof(label), "%.0f", crossed);
      if (x >= free_from && crossed >= 0.0) {
        ImDrawList_AddText_Vec2(draw, (ImVec2){x + 3.f * scale, top + 0.5f * (bottom - top - igGetTextLineHeight())}, text,
                                label, NULL);
        free_from = x + text_width(label) + 12.f * scale;
      }
    }
    previous = tick;
  }
}

// The export range: bright between its brackets, shaded outside.
static void draw_range(ui_handler_t *ui, ImDrawList *draw, const layout_t *l, bool shade) {
  const camera_editor_t *ed = &ui->camera_editor;
  const float scale = gfx_get_ui_scale();
  double start, end;
  camera_editor_export_range(ui, &start, &end);
  const float left = x_of(ed, l, start), right = x_of(ed, l, end);
  if (shade) {
    const ImU32 outside = IM_COL32(0, 0, 0, 80);
    if (left > l->x0) ImDrawList_AddRectFilled(draw, (ImVec2){l->x0, l->ruler_bottom}, (ImVec2){left, l->bottom}, outside, 0.f, 0);
    if (right < l->x1)
      ImDrawList_AddRectFilled(draw, (ImVec2){fmaxf(right, l->x0), l->ruler_bottom}, (ImVec2){l->x1, l->bottom}, outside, 0.f, 0);
    return;
  }
  const float bar = l->ruler_top + 1.5f * scale;
  ImDrawList_AddLine(draw, (ImVec2){left, bar}, (ImVec2){right, bar}, RANGE_COLOR, 3.f * scale);
  for (int side = 0; side < 2; ++side) {
    const float x = side ? right : left, inward = side ? -5.f * scale : 5.f * scale;
    ImDrawList_AddLine(draw, (ImVec2){x, l->ruler_top}, (ImVec2){x, l->ruler_split}, RANGE_COLOR, 2.f * scale);
    ImDrawList_AddLine(draw, (ImVec2){x, l->ruler_split - 1.f}, (ImVec2){x + inward, l->ruler_split - 1.f}, RANGE_COLOR, 2.f * scale);
  }
}

// How fast the game runs at `seconds`, in multiples of real time.
static double game_speed(ui_handler_t *ui, double seconds, double dt) {
  const double tps = ticks_per_second(ui);
  const camera_timeline_t *camera = &ui->camera_timeline;
  return (camera_game_tick(camera, seconds + dt, tps) - camera_game_tick(camera, seconds - dt, tps)) / (2.0 * dt * tps);
}

// Game speed as a colour that changes continuously with it: blue when the
// game stands still, the lane's green at 1x, orange speeding up (twice as fast
// is half way, four times is all the way) and red running backwards.
typedef struct rgb_t {
  float r, g, b;
} rgb_t;

static rgb_t mix(rgb_t a, rgb_t b, float t) {
  t = fminf(1.f, fmaxf(0.f, t));
  return (rgb_t){a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

static ImU32 speed_color(double speed) {
  static const rgb_t frozen = {74, 124, 214}, normal = {66, 104, 80}, fast = {222, 150, 62}, reverse = {206, 82, 82};
  rgb_t c;
  if (speed < 0.0) c = mix(frozen, reverse, (float)-speed);
  else if (speed <= 1.0) c = mix(frozen, normal, (float)speed);
  else c = mix(normal, fast, (float)(log2(speed) * 0.5));
  return IM_COL32((int)c.r, (int)c.g, (int)c.b, 255);
}

// A hold key jumps the game: at that instant it has no speed to show.
static bool is_jump(double speed) { return !isfinite(speed) || fabs(speed) > 100.0; }

static void draw_time_lane(ui_handler_t *ui, ImDrawList *draw, const layout_t *l, const lane_t *lane) {
  const camera_editor_t *ed = &ui->camera_editor;
  const camera_timeline_t *camera = &ui->camera_timeline;
  const float scale = gfx_get_ui_scale();
  const float step = 2.f;
  const float top = lane->top + 8.f * scale, bottom = lane->bottom - 8.f * scale;
  const double dt = 0.5 / ed->pixels_per_second;
  // Each sample's colour at its own x, blended to the next across the gap.
  const float first = fmaxf(l->x0, x_of(ed, l, 0.0));
  ImU32 previous = speed_color(game_speed(ui, time_of(ed, l, first), dt));
  for (float x = first; x < l->x1; x += step) {
    const float next_x = fminf(x + step, l->x1);
    const double speed = game_speed(ui, time_of(ed, l, next_x), dt);
    if (is_jump(speed)) {
      ImDrawList_AddRectFilled(draw, (ImVec2){x, top}, (ImVec2){next_x, bottom}, previous, 0.f, 0);
      ImDrawList_AddLine(draw, (ImVec2){next_x, top}, (ImVec2){next_x, bottom}, IM_COL32(240, 240, 240, 255), 2.f * scale);
      continue;
    }
    const ImU32 color = speed_color(speed);
    ImDrawList_AddRectFilledMultiColor(draw, (ImVec2){x, top}, (ImVec2){next_x, bottom}, previous, color, color, previous);
    previous = color;
  }
  for (int i = 0; i < camera->time_count; ++i)
    draw_key(draw, lane_key_position(ui, l, lane, i), camera->time_keys[i].interp, camera->time_keys[i].ease.mode,
             ed->selected_channel == CAMERA_CHANNEL_TIME && ed->selected_key == i, scale);
}

static void draw_pose_lane(ui_handler_t *ui, ImDrawList *draw, const layout_t *l, const lane_t *lane) {
  const camera_editor_t *ed = &ui->camera_editor;
  const camera_timeline_t *camera = &ui->camera_timeline;
  const float scale = gfx_get_ui_scale();
  const float middle = 0.5f * (lane->top + lane->bottom);
  // A bar for each stretch the camera moves through, broken at every cut.
  for (int i = 0; i + 1 < camera->pose_count; ++i) {
    const camera_pose_key_t *a = &camera->pose_keys[i], *b = &camera->pose_keys[i + 1];
    const float xa = x_of(ed, l, a->time), xb = x_of(ed, l, b->time);
    if (a->interp == CAMERA_INTERP_HOLD) {
      ImDrawList_AddLine(draw, (ImVec2){xa, middle}, (ImVec2){xb, middle}, igGetColorU32_Col(ImGuiCol_TextDisabled, 0.4f), 1.f);
      ImDrawList_AddLine(draw, (ImVec2){xb, lane->top + 4.f * scale}, (ImVec2){xb, lane->bottom - 4.f * scale},
                         IM_COL32(255, 255, 255, 140), 1.f);
    } else {
      ImDrawList_AddRectFilled(draw, (ImVec2){xa, middle - 4.f * scale}, (ImVec2){xb, middle + 4.f * scale},
                               IM_COL32(64, 128, 196, 200), 3.f * scale, 0);
    }
  }
  for (int i = 0; i < camera->pose_count; ++i)
    draw_key(draw, lane_key_position(ui, l, lane, i), camera->pose_keys[i].interp, camera->pose_keys[i].ease.mode,
             ed->selected_channel == CAMERA_CHANNEL_POSE && ed->selected_key == i, scale);
  if (camera->pose_count == 0) {
    char hint[160];
    keybind_entry_t *bind = keybinds_get_binding_for_action(&ui->keybinds, ACTION_CAMERA_INSERT_KEY, 0);
    snprintf(hint, sizeof(hint), "No camera keys yet, so the game camera is used. Press %s or Key view to key the viewport.",
             bind ? keybind_get_combo_string(&bind->combo) : "Key view");
    ImDrawList_AddText_Vec2(draw, (ImVec2){l->x0 + 10.f * scale, middle - igGetTextLineHeight() * 0.5f},
                            igGetColorU32_Col(ImGuiCol_TextDisabled, 1.f), hint, NULL);
  }
}

static void draw_influence_lane(ui_handler_t *ui, ImDrawList *draw, const layout_t *l, const lane_t *lane) {
  const camera_editor_t *ed = &ui->camera_editor;
  const camera_timeline_t *camera = &ui->camera_timeline;
  const float scale = gfx_get_ui_scale();
  const bool follow = lane->channel == CAMERA_CHANNEL_FOLLOW;
  const ImU32 line = follow ? IM_COL32(120, 220, 150, 255) : IM_COL32(230, 160, 240, 255);
  const ImU32 fill = follow ? IM_COL32(120, 220, 150, 50) : IM_COL32(230, 160, 240, 50);
  const float step = 3.f;
  float previous_y = 0.f;
  for (float x = l->x0; x <= l->x1; x += step) {
    const double t = time_of(ed, l, x);
    const float value = follow ? camera_eval_follow(camera, t) : camera_eval_aim(camera, t);
    const float y = value_y(lane, value);
    if (value > 0.f)
      ImDrawList_AddRectFilled(draw, (ImVec2){x, y}, (ImVec2){fminf(x + step, l->x1), value_y(lane, 0.f)}, fill, 0.f, 0);
    if (x > l->x0) ImDrawList_AddLine(draw, (ImVec2){x - step, previous_y}, (ImVec2){x, y}, line, 1.5f * scale);
    previous_y = y;
  }
  const camera_influence_key_t *keys = follow ? camera->follow_keys : camera->aim_keys;
  for (int i = 0; i < camera_channel_count(camera, lane->channel); ++i)
    draw_key(draw, lane_key_position(ui, l, lane, i), keys[i].interp, keys[i].ease.mode,
             ed->selected_channel == lane->channel && ed->selected_key == i, scale);
}

static void lane_value_text(ui_handler_t *ui, camera_channel_t channel, char *out, size_t size) {
  const camera_timeline_t *camera = &ui->camera_timeline;
  const double playhead = ui->camera_editor.playhead;
  switch (channel) {
  case CAMERA_CHANNEL_TIME: snprintf(out, size, "%.2fx", game_speed(ui, playhead, 0.5 / frame_rate(ui))); break;
  case CAMERA_CHANNEL_POSE: snprintf(out, size, "%d keys", camera->pose_count); break;
  case CAMERA_CHANNEL_FOLLOW: snprintf(out, size, "%.0f%%", camera_eval_follow(camera, playhead) * 100.f); break;
  case CAMERA_CHANNEL_AIM: snprintf(out, size, "%.0f%%", camera_eval_aim(camera, playhead) * 100.f); break;
  default: out[0] = '\0'; break;
  }
}

// The name on top and the value at the playhead under it.
static void draw_lane_label(ui_handler_t *ui, ImDrawList *draw, const layout_t *l, const lane_t *lane, bool active) {
  const float scale = gfx_get_ui_scale();
  const float line = igGetTextLineHeight();
  char value[32];
  lane_value_text(ui, lane->channel, value, sizeof(value));
  if (active)
    ImDrawList_AddRectFilled(draw, (ImVec2){l->label_x, lane->top}, (ImVec2){l->label_x + 3.f * scale, lane->bottom},
                             SELECTED_COLOR, 0.f, 0);
  const float top = 0.5f * (lane->top + lane->bottom) - line - 1.f * scale;
  ImDrawList_AddText_Vec2(draw, (ImVec2){l->label_x + 10.f * scale, top}, igGetColorU32_Col(ImGuiCol_Text, 1.f),
                          CHANNEL_NAMES[lane->channel], NULL);
  ImDrawList_AddText_Vec2(draw, (ImVec2){l->label_x + 10.f * scale, top + line + 2.f * scale},
                          igGetColorU32_Col(ImGuiCol_TextDisabled, 1.f), value, NULL);
}

// Graph editor

static void graph_fit(ui_handler_t *ui, layout_t *l) {
  const camera_editor_t *ed = &ui->camera_editor;
  const camera_timeline_t *camera = &ui->camera_timeline;
  const camera_channel_t channel = ed->selected_channel;
  const double tps = ticks_per_second(ui);
  if (channel == CAMERA_CHANNEL_FOLLOW || channel == CAMERA_CHANNEL_AIM) {
    l->graph_low = -0.05;
    l->graph_high = 1.05;
    return;
  }
  double low = INFINITY, high = -INFINITY;
  for (int i = 0; i <= 96; ++i) {
    const double value = camera_curve_value(camera, channel, time_of(ed, l, l->x0 + (l->x1 - l->x0) * i / 96.f), tps, l->distances);
    low = fmin(low, value);
    high = fmax(high, value);
  }
  for (int side = -1; side <= 1; side += 2) {
    double time, value;
    if (graph_handle(ui, l, ed->selected_key, side, &time, &value)) {
      low = fmin(low, value);
      high = fmax(high, value);
    }
  }
  if (!isfinite(low) || !isfinite(high)) {
    low = 0.0;
    high = 1.0;
  }
  const double margin = fmax(1e-3, (high - low) * 0.08);
  l->graph_low = low - margin;
  l->graph_high = high + margin;
}

static void draw_graph(ui_handler_t *ui, ImDrawList *draw, const layout_t *l) {
  const camera_editor_t *ed = &ui->camera_editor;
  const camera_timeline_t *camera = &ui->camera_timeline;
  const camera_channel_t channel = ed->selected_channel;
  const float scale = gfx_get_ui_scale();
  const double tps = ticks_per_second(ui);

  // Horizontal grid lines with their values.
  const double grid = nice_value_step(l->graph_high - l->graph_low, 4);
  for (double v = ceil(l->graph_low / grid) * grid; v <= l->graph_high; v += grid) {
    const float y = graph_y(l, v);
    ImDrawList_AddLine(draw, (ImVec2){l->x0, y}, (ImVec2){l->x1, y}, igGetColorU32_Col(ImGuiCol_TextDisabled, 0.18f), 1.f);
    char label[32];
    if (channel == CAMERA_CHANNEL_FOLLOW || channel == CAMERA_CHANNEL_AIM) snprintf(label, sizeof(label), "%.0f%%", v * 100.0);
    else snprintf(label, sizeof(label), "%g", fabs(v) < grid * 1e-6 ? 0.0 : v);
    ImDrawList_AddText_Vec2(draw, (ImVec2){l->x0 + 4.f * scale, y - igGetTextLineHeight() - 1.f},
                            igGetColorU32_Col(ImGuiCol_TextDisabled, 0.8f), label, NULL);
  }

  // Game time runs at 1x along this line.
  if (channel == CAMERA_CHANNEL_TIME)
    ImDrawList_AddLine(draw, (ImVec2){l->x0, graph_y(l, time_of(ed, l, l->x0) * tps)},
                       (ImVec2){l->x1, graph_y(l, time_of(ed, l, l->x1) * tps)}, igGetColorU32_Col(ImGuiCol_TextDisabled, 0.35f), 1.f);

  // The curve, and for the camera its speed along the path underneath, as
  // After Effects' speed graph shows it.
  enum { MAX_POINTS = 1400 };
  static ImVec2 points[MAX_POINTS];
  static float speeds[MAX_POINTS];
  int count = 0;
  const float step = fmaxf(2.f, (l->x1 - l->x0) / (MAX_POINTS - 100));
  for (float x = l->x0; x <= l->x1 && count < MAX_POINTS; x += step)
    points[count++] = (ImVec2){x, graph_y(l, camera_curve_value(camera, channel, time_of(ed, l, x), tps, l->distances))};
  if (channel == CAMERA_CHANNEL_POSE && camera->pose_count > 1 && count > 2) {
    float top_speed = 1e-6f;
    const double dt = step / ed->pixels_per_second;
    speeds[0] = 0.f;
    for (int i = 1; i < count; ++i) {
      const double a = camera_curve_value(camera, channel, time_of(ed, l, points[i - 1].x), tps, l->distances);
      const double b = camera_curve_value(camera, channel, time_of(ed, l, points[i].x), tps, l->distances);
      speeds[i] = (float)fabs((b - a) / dt);
      top_speed = fmaxf(top_speed, speeds[i]);
    }
    const float base = l->graph_bottom - 4.f * scale, height = (l->graph_bottom - l->graph_top) * 0.3f;
    for (int i = 2; i < count; ++i)
      ImDrawList_AddLine(draw, (ImVec2){points[i - 1].x, base - speeds[i - 1] / top_speed * height},
                         (ImVec2){points[i].x, base - speeds[i] / top_speed * height}, IM_COL32(255, 170, 90, 150), 1.5f * scale);
    ImDrawList_AddText_Vec2(draw, (ImVec2){l->x1 - text_width("speed") - 6.f * scale, base - height - igGetTextLineHeight()},
                            IM_COL32(255, 170, 90, 200), "speed", NULL);
  }
  ImDrawList_AddPolyline(draw, points, count, IM_COL32(110, 200, 255, 255), 0, 2.f * scale);

  // The selected key's handles.
  const int selected = ed->selected_key;
  if (selected >= 0 && selected < camera_channel_count(camera, channel)) {
    const ImVec2 at = graph_key_position(ui, l, selected);
    for (int side = -1; side <= 1; side += 2) {
      double time, value;
      if (!graph_handle(ui, l, selected, side, &time, &value)) continue;
      const ImVec2 tip = {x_of(ed, l, time), graph_y(l, value)};
      ImDrawList_AddLine(draw, at, tip, HANDLE_COLOR, 1.5f * scale);
      ImDrawList_AddCircleFilled(draw, tip, 5.f * scale, HANDLE_COLOR, 12);
    }
  }
  for (int i = 0; i < camera_channel_count(camera, channel); ++i)
    draw_key(draw, graph_key_position(ui, l, i), *camera_channel_key_interp((camera_timeline_t *)camera, channel, i),
             camera_channel_key_ease((camera_timeline_t *)camera, channel, i)->mode, i == selected, scale);
  if (camera_channel_count(camera, channel) == 0)
    ImDrawList_AddText_Vec2(draw, (ImVec2){l->x0 + 10.f * scale, l->graph_top + 10.f * scale},
                            igGetColorU32_Col(ImGuiCol_TextDisabled, 1.f), "No keys on this channel. Double-click to add one.", NULL);
}

// Interaction

typedef enum drag_mode_t {
  DRAG_NONE,
  DRAG_SCRUB,
  DRAG_KEY,
  DRAG_HANDLE,
  DRAG_RANGE_START,
  DRAG_RANGE_END,
} drag_mode_t;

typedef struct drag_state_t {
  drag_mode_t mode;
  camera_channel_t channel;
  int key, side;
  double grab_offset; // key time minus mouse time when grabbed
  double graph_low, graph_high;
} drag_state_t;

static drag_state_t g_drag;
static camera_channel_t g_menu_channel;
static int g_menu_key = -1;
static double g_menu_time;
static float g_menu_value;

static double snap_key_time(ui_handler_t *ui, const layout_t *l, double time) {
  if (input_shift_down()) return time;
  const camera_editor_t *ed = &ui->camera_editor;
  if (fabsf(x_of(ed, l, time) - x_of(ed, l, ed->playhead)) < 6.f * gfx_get_ui_scale()) return ed->playhead;
  return snap_to_frame(ui, time);
}

// New keys go onto the existing curve, so adding one changes nothing until
// it is moved. The first camera key takes the viewport.
static int add_key(ui_handler_t *ui, camera_channel_t channel, double time, float value, bool value_given) {
  camera_timeline_t *camera = &ui->camera_timeline;
  const double tps = ticks_per_second(ui);
  const bool opened = edit_begin(ui, "Add camera key");
  int index = -1;
  switch (channel) {
  case CAMERA_CHANNEL_TIME:
    index = camera_insert_time_key(camera, time, camera_game_tick(camera, time, tps), CAMERA_INTERP_LINEAR);
    break;
  case CAMERA_CHANNEL_POSE: {
    camera_pose_t pose;
    if (camera->pose_count) pose = camera_eval_pose(camera, time);
    else if (!camera_rig_capture(ui->gfx_handler, time, &pose)) break;
    index = camera_insert_pose_key(camera, time, &pose, CAMERA_INTERP_SMOOTH);
    break;
  }
  case CAMERA_CHANNEL_FOLLOW:
  case CAMERA_CHANNEL_AIM:
    if (!value_given) value = (float)camera_curve_value(camera, channel, time, tps, NULL);
    index = camera_insert_influence_key(camera, channel, time, value, CAMERA_INTERP_SMOOTH);
    break;
  default: break;
  }
  if (opened) edit_end(ui);
  if (index >= 0) select_key(ui, channel, index);
  return index;
}

static const char *binding_text(ui_handler_t *ui, action_t action) {
  keybind_entry_t *bind = keybinds_get_binding_for_action(&ui->keybinds, action, 0);
  return bind ? keybind_get_combo_string(&bind->combo) : "unbound";
}

static void key_menu(ui_handler_t *ui) {
  camera_timeline_t *camera = &ui->camera_timeline;
  const double tps = ticks_per_second(ui);
  const camera_channel_t channel = g_menu_channel;
  const int key = g_menu_key;
  int *interp = camera_channel_key_interp(camera, channel, key);
  igTextDisabled("Into the next key");
  for (int i = 0; i < CAMERA_INTERP_COUNT; ++i)
    if (igMenuItem_Bool(INTERP_NAMES[channel][i], NULL, *interp == i, true) && *interp != i) {
      const bool opened = edit_begin(ui, "Change camera interpolation");
      *interp = i;
      if (opened) edit_end(ui);
    }
  igTextDisabled("Ease handles");
  const int mode = camera_channel_key_ease(camera, channel, key)->mode;
  for (int i = 0; i < CAMERA_HANDLE_COUNT; ++i)
    if (igMenuItem_Bool(HANDLE_NAMES[i], NULL, mode == i, true) && mode != i) {
      const bool opened = edit_begin(ui, "Change camera handles");
      camera_set_ease_mode(camera, channel, key, i, tps);
      if (opened) edit_end(ui);
    }
  if (igMenuItem_Bool("Easy ease", binding_text(ui, ACTION_CAMERA_EASY_EASE), false, true)) easy_ease(ui, channel, key);
  igSeparator();
  if (igMenuItem_Bool("Move playhead here", NULL, false, true)) set_playhead(ui, camera_channel_key_time(camera, channel, key));
  if (channel == CAMERA_CHANNEL_POSE && igMenuItem_Bool("Set from view", NULL, false, true)) {
    camera_pose_t pose;
    if (camera_rig_capture(ui->gfx_handler, camera->pose_keys[key].time, &pose)) {
      const bool opened = edit_begin(ui, "Set camera key from view");
      camera->pose_keys[key].pose = pose;
      if (opened) edit_end(ui);
    }
  }
  if (igMenuItem_Bool("Delete key", NULL, false, true)) delete_key(ui, channel, key);
}

static void empty_menu(ui_handler_t *ui) {
  char label[64];
  snprintf(label, sizeof(label), "Add %s key here", CHANNEL_NAMES[g_menu_channel]);
  if (igMenuItem_Bool(label, NULL, false, true)) add_key(ui, g_menu_channel, g_menu_time, g_menu_value, true);
  if (g_menu_channel == CAMERA_CHANNEL_POSE) {
    if (igMenuItem_Bool("Key the viewport here", NULL, false, true)) {
      set_playhead(ui, g_menu_time);
      key_current_view(ui);
    }
    if (igMenuItem_Bool("Even out speed", NULL, false, ui->camera_timeline.pose_count > 2)) {
      const bool opened = edit_begin(ui, "Even out camera speed");
      camera_even_speed(&ui->camera_timeline);
      if (opened) edit_end(ui);
    }
    if (igIsItemHovered(0)) igSetTooltip("Retime the keys inside each shot so the camera moves at one speed");
  }
}

static void ruler_menu(ui_handler_t *ui) {
  double start, end;
  camera_editor_export_range(ui, &start, &end);
  const double playhead = ui->camera_editor.playhead;
  if (igMenuItem_Bool("Start export range at playhead", NULL, false, playhead < end)) {
    const bool opened = edit_begin(ui, "Change export range");
    set_range(ui, playhead, end);
    if (opened) edit_end(ui);
  }
  if (igMenuItem_Bool("End export range at playhead", NULL, false, playhead > start)) {
    const bool opened = edit_begin(ui, "Change export range");
    set_range(ui, start, playhead);
    if (opened) edit_end(ui);
  }
  if (igMenuItem_Bool("Export the whole timeline", NULL, !ui->camera_timeline.range_set, true)) {
    const bool opened = edit_begin(ui, "Change export range");
    ui->camera_timeline.range_set = false;
    if (opened) edit_end(ui);
  }
}

// What is under the pointer in the lanes or the graph.
typedef struct hit_t {
  camera_channel_t channel;
  int key;
  int side; // a graph handle, when nonzero
} hit_t;

static hit_t hit_test(ui_handler_t *ui, const layout_t *l, ImVec2 mouse, const lane_t *lane) {
  camera_editor_t *ed = &ui->camera_editor;
  const float radius = 9.f * gfx_get_ui_scale();
  hit_t hit = {ed->selected_channel, -1, 0};
  float best = radius;
  if (ed->graph_mode) {
    for (int side = -1; side <= 1; side += 2) {
      double time, value;
      if (!graph_handle(ui, l, ed->selected_key, side, &time, &value)) continue;
      const float d = hypotf(mouse.x - x_of(ed, l, time), mouse.y - graph_y(l, value));
      if (d <= best) {
        best = d;
        hit = (hit_t){ed->selected_channel, ed->selected_key, side};
      }
    }
    if (hit.side) return hit;
    for (int i = 0; i < camera_channel_count(&ui->camera_timeline, ed->selected_channel); ++i) {
      const ImVec2 at = graph_key_position(ui, l, i);
      const float d = hypotf(mouse.x - at.x, mouse.y - at.y);
      if (d <= best) {
        best = d;
        hit.key = i;
      }
    }
    return hit;
  }
  if (!lane) return hit;
  hit.channel = lane->channel;
  for (int i = 0; i < camera_channel_count(&ui->camera_timeline, lane->channel); ++i) {
    const ImVec2 at = lane_key_position(ui, l, lane, i);
    const float d = hypotf(mouse.x - at.x, mouse.y - at.y);
    if (d <= best) {
      best = d;
      hit.key = i;
    }
  }
  return hit;
}

static void drag_key(ui_handler_t *ui, const layout_t *l, ImVec2 mouse, double mouse_time) {
  camera_editor_t *ed = &ui->camera_editor;
  camera_timeline_t *camera = &ui->camera_timeline;
  const int index = camera_move_key(camera, g_drag.channel, g_drag.key, snap_key_time(ui, l, mouse_time + g_drag.grab_offset));
  if (index < 0) return;
  g_drag.key = index;
  select_key(ui, g_drag.channel, index);
  if (ed->graph_mode) {
    // The graph's scale is held still while a key is dragged up and down it.
    const double value = graph_value(l, mouse.y);
    if (g_drag.channel == CAMERA_CHANNEL_TIME)
      camera->time_keys[index].game_tick = input_shift_down() ? value : round(value);
    else if (g_drag.channel == CAMERA_CHANNEL_FOLLOW) camera->follow_keys[index].value = (float)fmin(1.0, fmax(0.0, value));
    else if (g_drag.channel == CAMERA_CHANNEL_AIM) camera->aim_keys[index].value = (float)fmin(1.0, fmax(0.0, value));
    return;
  }
  const lane_t *lane = lane_for(l, g_drag.channel);
  if (lane && g_drag.channel == CAMERA_CHANNEL_FOLLOW) camera->follow_keys[index].value = value_at(lane, mouse.y);
  if (lane && g_drag.channel == CAMERA_CHANNEL_AIM) camera->aim_keys[index].value = value_at(lane, mouse.y);
}

// A handle keeps to its own side of the key and reaches at most the whole
// neighbouring segment.
static void drag_handle(ui_handler_t *ui, const layout_t *l, ImVec2 mouse) {
  camera_editor_t *ed = &ui->camera_editor;
  camera_timeline_t *camera = &ui->camera_timeline;
  const int key = g_drag.key, side = g_drag.side;
  const int count = camera_channel_count(camera, g_drag.channel);
  if (key < 0 || key >= count || key + side < 0 || key + side >= count) return;
  const double key_time = camera_channel_key_time(camera, g_drag.channel, key);
  const double span = fabs(camera_channel_key_time(camera, g_drag.channel, key + side) - key_time);
  const double reach = fmax(span * 0.01, fmin(span, side * (time_of(ed, l, mouse.x) - key_time)));
  const double key_value = camera_curve_key_value(camera, g_drag.channel, key, l->distances);
  const double slope = (graph_value(l, mouse.y) - key_value) * side / reach;
  camera_set_ease_handle(camera, g_drag.channel, key, side, (float)slope, (float)(reach / span), ticks_per_second(ui));
}

// How far left the view may scroll: a little before zero, or before a key an
// older project left at a negative time, so it can still be reached.
static double view_minimum(const camera_timeline_t *camera) {
  double earliest = 0.0;
  for (int channel = 0; channel < CAMERA_CHANNEL_COUNT; ++channel)
    if (camera_channel_count(camera, (camera_channel_t)channel))
      earliest = fmin(earliest, camera_channel_key_time(camera, (camera_channel_t)channel, 0));
  return earliest - 0.25;
}

// The scrollbar under the lanes: it spans every key and the export range,
// with room to add more past them.
static void render_scrollbar(ui_handler_t *ui, const layout_t *l, float top, float height) {
  camera_editor_t *ed = &ui->camera_editor;
  double start, end;
  camera_editor_export_range(ui, &start, &end);
  const double visible = (l->x1 - l->x0) / ed->pixels_per_second;
  const double minimum = view_minimum(&ui->camera_timeline);
  const double content_end = fmax(fmax(end, camera_last_key_time(&ui->camera_timeline)) + visible * 0.5, ed->view_start + visible);
  static double last_view_start = NAN;
  if (ed->view_start != last_view_start)
    igSetNextWindowScroll((ImVec2){(float)((ed->view_start - minimum) * ed->pixels_per_second), 0.f});
  igSetCursorScreenPos((ImVec2){l->x0, top});
  igBeginChild_Str("##camera_scroll", (ImVec2){l->x1 - l->x0, height}, false,
                   ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  igDummy((ImVec2){(float)((content_end - minimum) * ed->pixels_per_second), 1.f});
  if (igIsWindowHovered(0) || igIsWindowFocused(0)) ed->view_start = minimum + igGetScrollX() / ed->pixels_per_second;
  last_view_start = ed->view_start;
  igEndChild();
}

static void handle_input(ui_handler_t *ui, layout_t *l) {
  camera_editor_t *ed = &ui->camera_editor;
  ImGuiIO *io = igGetIO_Nil();
  const float scale = gfx_get_ui_scale();
  igSetCursorScreenPos((ImVec2){l->x0, l->ruler_top});
  igInvisibleButton("##camera_lanes", (ImVec2){fmaxf(1.f, l->x1 - l->x0), fmaxf(1.f, l->bottom - l->ruler_top)},
                    ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
  const bool hovered = igIsItemHovered(0);
  const ImVec2 mouse = io->MousePos;
  const double mouse_time = time_of(ed, l, mouse.x);
  const bool in_ruler = mouse.y < l->ruler_bottom;
  const lane_t *lane = !in_ruler && !ed->graph_mode ? lane_at(l, mouse.y) : NULL;

  // Ctrl+wheel zooms around the pointer, like the timeline; the wheel alone pans.
  if (hovered) {
    const float wheel = (float)input_scroll_y();
    if (wheel != 0.f && input_ctrl_down()) {
      ed->pixels_per_second = (float)fmin(MAX_PIXELS_PER_SECOND,
                                          fmax(MIN_PIXELS_PER_SECOND, ed->pixels_per_second * powf(1.15f, wheel)));
      ed->view_start = mouse_time - (mouse.x - l->x0) / ed->pixels_per_second;
    } else if (wheel != 0.f || io->MouseWheelH != 0.f) {
      ed->view_start -= (wheel + io->MouseWheelH) * 60.f * scale / ed->pixels_per_second;
    }
  }
  if (igIsItemActive() && igIsMouseDragging(ImGuiMouseButton_Middle, 0.f))
    ed->view_start -= io->MouseDelta.x / ed->pixels_per_second;
  ed->view_start = fmax(view_minimum(&ui->camera_timeline), ed->view_start);

  double range_start, range_end;
  camera_editor_export_range(ui, &range_start, &range_end);
  const float near_range = 6.f * scale;
  const bool on_range_start = in_ruler && mouse.y < l->ruler_split && fabsf(mouse.x - x_of(ed, l, range_start)) < near_range;
  const bool on_range_end = in_ruler && mouse.y < l->ruler_split && fabsf(mouse.x - x_of(ed, l, range_end)) < near_range;

  if (hovered && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) {
    const hit_t hit = in_ruler ? (hit_t){ed->selected_channel, -1, 0} : hit_test(ui, l, mouse, lane);
    g_drag = (drag_state_t){DRAG_NONE, hit.channel, hit.key, hit.side, 0.0, l->graph_low, l->graph_high};
    if (on_range_start || on_range_end) {
      g_drag.mode = on_range_end ? DRAG_RANGE_END : DRAG_RANGE_START;
      edit_begin(ui, "Change export range");
    } else if (hit.side) {
      g_drag.mode = DRAG_HANDLE;
      edit_begin(ui, "Shape camera ease");
      if (input_alt_down()) camera_set_ease_mode(&ui->camera_timeline, hit.channel, hit.key, CAMERA_HANDLE_FREE, ticks_per_second(ui));
    } else if (hit.key >= 0) {
      select_key(ui, hit.channel, hit.key);
      g_drag.mode = DRAG_KEY;
      g_drag.grab_offset = camera_channel_key_time(&ui->camera_timeline, hit.channel, hit.key) - mouse_time;
      edit_begin(ui, "Move camera key");
    } else if (!in_ruler && (lane || ed->graph_mode) && igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left)) {
      const camera_channel_t channel = ed->graph_mode ? ed->selected_channel : lane->channel;
      add_key(ui, channel, snap_key_time(ui, l, mouse_time), lane ? value_at(lane, mouse.y) : 0.f,
              lane && (channel == CAMERA_CHANNEL_FOLLOW || channel == CAMERA_CHANNEL_AIM));
    } else {
      if (!in_ruler) ed->selected_key = -1;
      ed->playing = false;
      g_drag.mode = DRAG_SCRUB;
    }
  }
  if (hovered && g_drag.mode == DRAG_NONE && (on_range_start || on_range_end))
    igSetTooltip("Drag to set where the export %s", on_range_end ? "ends" : "starts");

  if (igIsItemActive()) {
    const double snapped = input_shift_down() ? mouse_time : snap_to_frame(ui, mouse_time);
    switch (g_drag.mode) {
    case DRAG_SCRUB: set_playhead(ui, snapped); break;
    case DRAG_KEY:
      if (igIsMouseDragging(ImGuiMouseButton_Left, 2.f)) drag_key(ui, l, mouse, mouse_time);
      break;
    case DRAG_HANDLE:
      if (igIsMouseDragging(ImGuiMouseButton_Left, 1.f)) drag_handle(ui, l, mouse);
      break;
    case DRAG_RANGE_START: set_range(ui, fmin(snapped, range_end), range_end); break;
    case DRAG_RANGE_END: set_range(ui, range_start, fmax(snapped, range_start)); break;
    case DRAG_NONE: break;
    }
  }
  if (!igIsMouseDown_Nil(ImGuiMouseButton_Left) && g_drag.mode != DRAG_NONE) {
    if (g_drag.mode != DRAG_SCRUB) edit_end(ui);
    g_drag.mode = DRAG_NONE;
  }

  if (hovered && igIsMouseClicked_Bool(ImGuiMouseButton_Right, false)) {
    if (in_ruler) {
      igOpenPopup_Str("##camera_ruler_menu", 0);
    } else if (lane || ed->graph_mode) {
      const hit_t hit = hit_test(ui, l, mouse, lane);
      g_menu_channel = ed->graph_mode ? ed->selected_channel : lane->channel;
      g_menu_key = hit.key;
      g_menu_time = snap_key_time(ui, l, mouse_time);
      g_menu_value = lane ? value_at(lane, mouse.y) : 0.f;
      if (hit.key >= 0) select_key(ui, g_menu_channel, hit.key);
      igOpenPopup_Str("##camera_lane_menu", 0);
    }
  }
  if (igBeginPopup("##camera_lane_menu", 0)) {
    if (g_menu_key >= 0 && g_menu_key < camera_channel_count(&ui->camera_timeline, g_menu_channel)) key_menu(ui);
    else empty_menu(ui);
    igEndPopup();
  }
  if (igBeginPopup("##camera_ruler_menu", 0)) {
    ruler_menu(ui);
    igEndPopup();
  }

  // Hovering the game time tells what the game is doing there.
  if (hovered && g_drag.mode == DRAG_NONE && mouse_time >= 0.0 && !in_ruler &&
      ((lane && lane->channel == CAMERA_CHANNEL_TIME) || (ed->graph_mode && ed->selected_channel == CAMERA_CHANNEL_TIME)))
    igSetTooltip("%.2fs: game tick %.2f at %.2fx", mouse_time, camera_game_tick(&ui->camera_timeline, mouse_time, ticks_per_second(ui)),
                 game_speed(ui, mouse_time, 0.5 / ed->pixels_per_second));
}

// Inspector

static bool combo_edit(ui_handler_t *ui, const char *label, int *value, const char *const *names, int count,
                       const char *description) {
  int chosen = *value;
  if (!igCombo_Str_arr(label, &chosen, names, count, -1) || chosen == *value) return false;
  const bool opened = edit_begin(ui, description);
  *value = chosen;
  if (opened) edit_end(ui);
  return true;
}

// How fast a key's value changes, in the channel's own terms.
static float speed_display(camera_channel_t channel, float slope, double tps) {
  if (channel == CAMERA_CHANNEL_TIME) return (float)(slope / tps);
  if (channel == CAMERA_CHANNEL_FOLLOW || channel == CAMERA_CHANNEL_AIM) return slope * 100.f;
  return slope;
}

static float speed_stored(camera_channel_t channel, float shown, double tps) {
  if (channel == CAMERA_CHANNEL_TIME) return (float)(shown * tps);
  if (channel == CAMERA_CHANNEL_FOLLOW || channel == CAMERA_CHANNEL_AIM) return shown / 100.f;
  return shown;
}

static void inspect_timing(ui_handler_t *ui, camera_channel_t channel, int index) {
  camera_timeline_t *camera = &ui->camera_timeline;
  const double tps = ticks_per_second(ui);
  igSeparatorText("Timing");
  combo_edit(ui, "Into next", camera_channel_key_interp(camera, channel, index), INTERP_NAMES[channel], CAMERA_INTERP_COUNT,
             "Change camera interpolation");
  int mode = camera_channel_key_ease(camera, channel, index)->mode;
  if (igCombo_Str_arr("Ease handles", &mode, HANDLE_NAMES, CAMERA_HANDLE_COUNT, -1)) {
    const bool opened = edit_begin(ui, "Change camera handles");
    camera_set_ease_mode(camera, channel, index, mode, tps);
    if (opened) edit_end(ui);
  }
  const char *unit = channel == CAMERA_CHANNEL_TIME ? "%.2fx" : channel == CAMERA_CHANNEL_POSE ? "%.2f u/s" : "%.0f%%/s";
  const camera_ease_t ease = camera_curve_key_ease(camera, channel, index, tps);
  float speeds[2] = {speed_display(channel, ease.in_slope, tps), speed_display(channel, ease.out_slope, tps)};
  float influences[2] = {ease.in_influence * 100.f, ease.out_influence * 100.f};
  const char *speed_labels[2] = {"Speed in", "Speed out"}, *reach_labels[2] = {"Influence in", "Influence out"};
  for (int s = 0; s < 2; ++s) {
    const int side = s ? 1 : -1;
    if (igDragFloat(speed_labels[s], &speeds[s], 0.01f, 0.f, 0.f, unit, 0))
      camera_set_ease_handle(camera, channel, index, side, speed_stored(channel, speeds[s], tps), influences[s] / 100.f, tps);
    track_item(ui, "Shape camera ease");
    if (igSliderFloat(reach_labels[s], &influences[s], 1.f, 100.f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp))
      camera_set_ease_handle(camera, channel, index, side, speed_stored(channel, speeds[s], tps), influences[s] / 100.f, tps);
    track_item(ui, "Shape camera ease");
  }
  if (ease.mode == CAMERA_HANDLE_AUTO) igTextDisabled("Auto: shaped by the neighbouring keys.");
  char label[64];
  snprintf(label, sizeof(label), "Easy ease (%s)", binding_text(ui, ACTION_CAMERA_EASY_EASE));
  if (igButton(label, (ImVec2){0, 0})) easy_ease(ui, channel, index);
  if (igIsItemHovered(0)) igSetTooltip("Arrive and leave at rest");
}

static void inspect_pose(ui_handler_t *ui, int index) {
  camera_timeline_t *camera = &ui->camera_timeline;
  camera_pose_key_t *key = &camera->pose_keys[index];
  const bool is_3d = game_is_3d(&ui->gfx_handler->game_host);
  camera_pose_t *pose = &key->pose;
  igSeparatorText("View");
  if (is_3d) {
    igDragFloat3("Position", pose->eye, 0.05f, 0.f, 0.f, "%.2f", 0);
    track_item(ui, "Edit camera position");
    igDragFloat3("Look at", pose->target, 0.05f, 0.f, 0.f, "%.2f", 0);
    track_item(ui, "Edit camera target");
    float roll = pose->roll * DEGREES_PER_RADIAN;
    if (igDragFloat("Roll", &roll, 0.25f, -180.f, 180.f, "%.1f deg", 0)) pose->roll = roll / DEGREES_PER_RADIAN;
    track_item(ui, "Edit camera roll");
    float fov = pose->fov_y * DEGREES_PER_RADIAN;
    if (igDragFloat("Field of view", &fov, 0.2f, 5.f, 170.f, "%.1f deg", ImGuiSliderFlags_AlwaysClamp))
      pose->fov_y = fov / DEGREES_PER_RADIAN;
    track_item(ui, "Edit camera field of view");
  } else {
    if (igDragFloat2("Center", pose->target, 0.05f, 0.f, 0.f, "%.2f", 0)) memcpy(pose->eye, pose->target, sizeof(float) * 2);
    track_item(ui, "Edit camera center");
    igDragFloat("Zoom", &pose->zoom, 0.01f, 0.1f, 1000.f, "%.3f", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
    track_item(ui, "Edit camera zoom");
  }
  if (igButton("Set from view", (ImVec2){0, 0})) {
    camera_pose_t captured;
    if (camera_rig_capture(ui->gfx_handler, key->time, &captured)) {
      const bool opened = edit_begin(ui, "Set camera key from view");
      camera->pose_keys[index].pose = captured;
      if (opened) edit_end(ui);
    }
  }
  if (igIsItemHovered(0)) igSetTooltip("Replace this key with what the viewport shows");

  igSeparatorText("Path");
  int mode = key->spatial.mode;
  if (igCombo_Str_arr("Path handles", &mode, HANDLE_NAMES, CAMERA_HANDLE_COUNT, -1)) {
    const bool opened = edit_begin(ui, "Change path handles");
    camera_set_spatial_mode(camera, index, mode);
    if (opened) edit_end(ui);
  }
  if (igButton("Straight lines", (ImVec2){0, 0})) {
    const bool opened = edit_begin(ui, "Straighten camera path");
    camera_set_spatial_mode(camera, index, CAMERA_HANDLE_FREE);
    memset(key->spatial.eye_in, 0, sizeof(key->spatial.eye_in));
    memset(key->spatial.eye_out, 0, sizeof(key->spatial.eye_out));
    memset(key->spatial.target_in, 0, sizeof(key->spatial.target_in));
    memset(key->spatial.target_out, 0, sizeof(key->spatial.target_out));
    if (opened) edit_end(ui);
  }
  if (igIsItemHovered(0)) igSetTooltip("A sharp corner: the path runs straight into and out of this key");
  igTextDisabled(ui->camera_editor.look_through ? "Turn off Look through to drag path handles in the viewport."
                                                : "Drag the blue handles in the viewport; Alt moves one side.");
}

static bool is_subject(const camera_timeline_t *camera, int track) {
  for (int i = 0; i < camera->subject_count; ++i)
    if (camera->subject_tracks[i] == track) return true;
  return false;
}

static void inspect_target(ui_handler_t *ui) {
  camera_timeline_t *camera = &ui->camera_timeline;
  timeline_state_t *ts = &ui->timeline;
  const bool is_3d = game_is_3d(&ui->gfx_handler->game_host);
  char preview[96];
  if (camera->subject_count == 0) snprintf(preview, sizeof(preview), "None");
  else if (camera->subject_count == 1 && camera->subject_tracks[0] < ts->player_track_count)
    snprintf(preview, sizeof(preview), "%s", ts->player_tracks[camera->subject_tracks[0]].name);
  else snprintf(preview, sizeof(preview), "%d characters", camera->subject_count);
  if (igBeginCombo("Characters", preview, 0)) {
    for (int i = 0; i < ts->player_track_count; ++i) {
      igPushID_Int(i);
      bool tracked = is_subject(camera, i);
      const bool full = !tracked && camera->subject_count >= CAMERA_MAX_SUBJECTS;
      if (full) igBeginDisabled(true);
      if (igCheckbox(ts->player_tracks[i].name, &tracked)) {
        const bool opened = edit_begin(ui, "Change camera characters");
        if (tracked) {
          camera->subject_tracks[camera->subject_count++] = i;
        } else {
          int kept = 0;
          for (int s = 0; s < camera->subject_count; ++s)
            if (camera->subject_tracks[s] != i) camera->subject_tracks[kept++] = camera->subject_tracks[s];
          camera->subject_count = kept;
        }
        if (opened) edit_end(ui);
      }
      if (full) igEndDisabled();
      igPopID();
    }
    igEndCombo();
  }
  if (igIsItemHovered(0)) igSetTooltip("Follow and Aim track the middle of the box around every character ticked here");
  if (is_3d) igDragFloat3("Offset", camera->subject_offset, 0.02f, 0.f, 0.f, "%.2f", 0);
  else igDragFloat2("Offset", camera->subject_offset, 0.02f, 0.f, 0.f, "%.2f", 0);
  track_item(ui, "Edit target offset");
  if (igIsItemHovered(0)) igSetTooltip("Added to the characters' middle, e.g. to aim at the head");
  if (is_3d) {
    static const char *styles[CAMERA_FOLLOW_COUNT] = {"Fixed offset", "Behind", "Game camera"};
    combo_edit(ui, "Follow style", &camera->follow_style, styles, CAMERA_FOLLOW_COUNT, "Change follow style");
    if (igIsItemHovered(0))
      igSetTooltip("Fixed offset: the keyed offset stays put in the world, circling nothing.\n"
                   "Behind: the keyed offset turns with the way the characters head, like a chase camera.\n"
                   "Game camera: follows the way the game's own camera does, e.g. TMNF's race camera.");
  }
  if (!is_3d) {
    bool fit = camera->fit_subjects;
    if (igCheckbox("Keep all in frame", &fit)) {
      const bool opened = edit_begin(ui, "Change camera framing");
      camera->fit_subjects = fit;
      if (opened) edit_end(ui);
    }
    if (igIsItemHovered(0)) igSetTooltip("While following, zoom out as far as needed to keep every character in view");
    if (camera->fit_subjects) {
      float fill = camera->fit_fill * 100.f;
      if (igSliderFloat("Fill", &fill, 20.f, 95.f, "%.0f%% of view", ImGuiSliderFlags_AlwaysClamp)) camera->fit_fill = fill / 100.f;
      track_item(ui, "Change camera framing");
    }
  }
}

static void inspect_key(ui_handler_t *ui) {
  camera_editor_t *ed = &ui->camera_editor;
  camera_timeline_t *camera = &ui->camera_timeline;
  const camera_channel_t channel = ed->selected_channel;
  const int index = ed->selected_key;
  char title[64];
  snprintf(title, sizeof(title), "%s key %d of %d", CHANNEL_NAMES[channel], index + 1, camera_channel_count(camera, channel));
  igSeparatorText(title);

  double time = camera_channel_key_time(camera, channel, index);
  const double zero = 0.0, most = 1e6;
  if (igDragScalar("Time", ImGuiDataType_Double, &time, 0.01f, &zero, &most, "%.3f s", ImGuiSliderFlags_AlwaysClamp)) {
    const int moved = camera_move_key(camera, channel, index, time);
    if (moved >= 0) select_key(ui, channel, moved);
  }
  track_item(ui, "Retime camera key");
  if (ed->selected_key < 0) return;
  const int key = ed->selected_key;

  switch (channel) {
  case CAMERA_CHANNEL_TIME:
    igDragScalar("Game tick", ImGuiDataType_Double, &camera->time_keys[key].game_tick, 0.25f, NULL, NULL, "%.2f", 0);
    track_item(ui, "Edit game time");
    break;
  case CAMERA_CHANNEL_POSE: inspect_pose(ui, key); break;
  case CAMERA_CHANNEL_FOLLOW:
  case CAMERA_CHANNEL_AIM: {
    camera_influence_key_t *influence = channel == CAMERA_CHANNEL_FOLLOW ? &camera->follow_keys[key] : &camera->aim_keys[key];
    float percent = influence->value * 100.f;
    if (igSliderFloat("Influence", &percent, 0.f, 100.f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) influence->value = percent / 100.f;
    track_item(ui, "Edit camera influence");
    igSeparatorText(channel == CAMERA_CHANNEL_FOLLOW ? "Follows" : "Aims at");
    inspect_target(ui);
    break;
  }
  default: break;
  }
  inspect_timing(ui, channel, key);
  igSpacing();
  if (igButton("Move playhead here", (ImVec2){0, 0})) set_playhead(ui, camera_channel_key_time(camera, channel, key));
  igSameLine(0.f, 8.f * gfx_get_ui_scale());
  if (igButton(ICON_FA_TRASH " Delete key", (ImVec2){0, 0})) delete_key(ui, channel, key);
}

static void inspect_camera(ui_handler_t *ui) {
  camera_timeline_t *camera = &ui->camera_timeline;
  igSeparatorText("Export range");
  double start, end;
  camera_editor_export_range(ui, &start, &end);
  const double zero = 0.0, most = 1e6;
  if (igDragScalar("Start", ImGuiDataType_Double, &start, 0.01f, &zero, &end, "%.2f s", ImGuiSliderFlags_AlwaysClamp))
    set_range(ui, start, end);
  track_item(ui, "Change export range");
  if (igDragScalar("End", ImGuiDataType_Double, &end, 0.01f, &start, &most, "%.2f s", ImGuiSliderFlags_AlwaysClamp))
    set_range(ui, start, end);
  track_item(ui, "Change export range");
  if (camera->range_set) {
    if (igButton("Export the whole timeline", (ImVec2){0, 0})) {
      const bool opened = edit_begin(ui, "Change export range");
      camera->range_set = false;
      if (opened) edit_end(ui);
    }
  } else {
    igTextDisabled("Covers the whole game timeline.");
  }
  igTextDisabled("Render > Export MP4 renders this span.");

  igSeparatorText("Follow and Aim target");
  inspect_target(ui);

  igSeparatorText("Shortcuts");
  igTextDisabled("%s: key the viewport at the playhead", binding_text(ui, ACTION_CAMERA_INSERT_KEY));
  igTextDisabled("%s: look through the camera", binding_text(ui, ACTION_CAMERA_LOOK_THROUGH));
  igTextDisabled("%s / %s: previous / next key", binding_text(ui, ACTION_CAMERA_PREV_KEY), binding_text(ui, ACTION_CAMERA_NEXT_KEY));
  igTextDisabled("%s: easy ease the selected key", binding_text(ui, ACTION_CAMERA_EASY_EASE));
  igTextDisabled("Double-click a lane to add a key.");
}

static void render_inspector(ui_handler_t *ui, float width, float height) {
  const float scale = gfx_get_ui_scale();
  igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2){10.f * scale, 8.f * scale});
  igBeginChild_Str("##camera_inspector", (ImVec2){width, height}, true, 0);
  igPushItemWidth(-110.f * scale);
  if (ui->camera_editor.selected_key >= 0) inspect_key(ui);
  else inspect_camera(ui);
  igPopItemWidth();
  igEndChild();
  igPopStyleVar(1);
}

// Controls

static bool toggle_button(const char *label, bool active, const char *tooltip) {
  if (active) igPushStyleColor_Vec4(ImGuiCol_Button, igGetStyle()->Colors[ImGuiCol_ButtonActive]);
  const bool pressed = igButton(label, (ImVec2){0, 0});
  if (active) igPopStyleColor(1);
  if (igIsItemHovered(0)) igSetTooltip("%s", tooltip);
  return pressed;
}

void camera_editor_transport(ui_handler_t *ui) {
  camera_editor_t *ed = &ui->camera_editor;
  const float scale = gfx_get_ui_scale();
  const double zero = 0.0, most = 1e6;
  igPushItemWidth(90.f * scale);
  igDragScalar("##camera_time", ImGuiDataType_Double, &ed->playhead, 0.01f, &zero, &most, "%.2f s", ImGuiSliderFlags_AlwaysClamp);
  igPopItemWidth();

  double start, end;
  camera_editor_export_range(ui, &start, &end);
  switch (renderer_draw_transport(ui, ed->playing)) {
  case TRANSPORT_START: ed->playing = false; set_playhead(ui, start); break;
  case TRANSPORT_BACK: step_frames(ui, -1); break;
  case TRANSPORT_PLAY: toggle_play(ui); break;
  case TRANSPORT_FORWARD: step_frames(ui, 1); break;
  case TRANSPORT_END: ed->playing = false; set_playhead(ui, end); break;
  case TRANSPORT_NONE: break;
  }
}

static void render_controls(ui_handler_t *ui) {
  camera_editor_t *ed = &ui->camera_editor;
  const float scale = gfx_get_ui_scale();
  camera_editor_transport(ui);

  igSameLine(0, 16.f * scale);
  char label[96], tip[192];
  snprintf(label, sizeof(label), "%s Look through", ed->look_through ? ICON_FA_EYE : ICON_FA_EYE_SLASH);
  snprintf(tip, sizeof(tip), "Look through the camera (%s)\nOff: a free view with the path and its handles in the level",
           binding_text(ui, ACTION_CAMERA_LOOK_THROUGH));
  if (toggle_button(label, ed->look_through, tip)) set_look_through(ui, !ed->look_through);
  igSameLine(0, 8.f * scale);
  snprintf(label, sizeof(label), "%s Key view (%s)", ICON_FA_KEY, binding_text(ui, ACTION_CAMERA_INSERT_KEY));
  if (toggle_button(label, false, "Key what the viewport shows at the playhead, or update the key there")) key_current_view(ui);

  igSameLine(0, 16.f * scale);
  if (toggle_button("Keys", !ed->graph_mode, "Key lanes for every channel")) ed->graph_mode = false;
  igSameLine(0, 2.f * scale);
  snprintf(label, sizeof(label), "%s Graph", ICON_FA_CHART_LINE);
  if (toggle_button(label, ed->graph_mode, "Curve editor for one channel, with ease handles")) ed->graph_mode = true;

  igSameLine(0, 16.f * scale);
  igSetNextItemWidth(90.f * scale);
  igDragFloat("##camera_zoom", &ed->pixels_per_second, 1.f, (float)MIN_PIXELS_PER_SECOND, (float)MAX_PIXELS_PER_SECOND,
              "%.0f px/s", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
  if (igIsItemHovered(0)) igSetTooltip("Zoom (Ctrl+wheel over the lanes)");

  igSameLine(0, 16.f * scale);
  igTextDisabled("Game tick %.2f", camera_game_tick(&ui->camera_timeline, ed->playhead, ticks_per_second(ui)));
}

// Who owns the clock: the Camera tab while it is the one in view, or, when
// both windows are showing, whichever was used last.
static void update_clock_owner(ui_handler_t *ui, bool visible, bool focused) {
  camera_editor_t *ed = &ui->camera_editor;
  const bool before = ed->drives_game;
  if (!visible || ui->timeline.recording || !ui->gfx_handler->level) ed->drives_game = false;
  else if (!ui->timeline_window_visible || focused) ed->drives_game = true;
  else if (ui->timeline_window_focused) ed->drives_game = false;
  if (ed->drives_game && !before) ui->timeline.is_playing = false;
  if (!ed->drives_game && before) ed->playing = false;
}

// Window

void camera_window_render(ui_handler_t *ui) {
  camera_editor_t *ed = &ui->camera_editor;
  igSetNextWindowClass(&((ImGuiWindowClass){.DockingAllowUnclassed = false}));
  igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2){8, 8});
  const bool visible = igBegin("Camera", NULL, ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar);
  igPopStyleVar(1);
  update_clock_owner(ui, visible, visible && igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows));
  if (!visible) {
    igEnd();
    return;
  }
  if (!ui->gfx_handler->level) {
    igTextDisabled("Open a level to edit its camera.");
    igEnd();
    return;
  }
  clamp_selection(ui);
  render_controls(ui);
  igSpacing();

  const float scale = gfx_get_ui_scale();
  const float line = igGetTextLineHeight();
  const bool is_3d = game_is_3d(&ui->gfx_handler->game_host);
  if (!is_3d && ed->selected_channel == CAMERA_CHANNEL_AIM) ed->selected_channel = CAMERA_CHANNEL_POSE;
  const ImVec2 origin = igGetCursorScreenPos();
  const ImVec2 avail = igGetContentRegionAvail();
  const float inspector_width = fminf(380.f * scale, fmaxf(260.f * scale, avail.x * 0.3f));

  // The label column fits its widest name or value.
  float label_width = text_width("Game tick");
  for (int c = 0; c < CAMERA_CHANNEL_COUNT; ++c) {
    char value[32];
    lane_value_text(ui, (camera_channel_t)c, value, sizeof(value));
    label_width = fmaxf(label_width, fmaxf(text_width(CHANNEL_NAMES[c]), text_width(value)));
  }
  label_width += 30.f * scale;

  layout_t l = {0};
  l.label_x = origin.x;
  l.x0 = origin.x + label_width;
  l.x1 = fmaxf(l.x0 + 80.f * scale, origin.x + avail.x - inspector_width - 10.f * scale);
  l.ruler_top = origin.y;
  l.ruler_split = l.ruler_top + line + 12.f * scale;
  l.ruler_bottom = l.ruler_split + line + 8.f * scale;

  const float lane_height = fmaxf(2.f * line + 20.f * scale, 46.f * scale);
  const camera_channel_t order[] = {CAMERA_CHANNEL_TIME, CAMERA_CHANNEL_POSE, CAMERA_CHANNEL_FOLLOW, CAMERA_CHANNEL_AIM};
  float y = l.ruler_bottom;
  for (int i = 0; i < CAMERA_CHANNEL_COUNT; ++i) {
    if (order[i] == CAMERA_CHANNEL_AIM && !is_3d) continue;
    l.lanes[l.lane_count++] = (lane_t){order[i], y, y + lane_height};
    y += lane_height;
  }
  // The scrollbar sits under the lanes, or at the bottom when they fill the panel.
  const float scrollbar = igGetStyle()->ScrollbarSize;
  const float area_bottom = origin.y + avail.y - scrollbar - 4.f * scale;
  l.bottom = ed->graph_mode ? area_bottom : fminf(y, area_bottom);

  static double distances[CAMERA_MAX_POSE_KEYS];
  camera_pose_distances(&ui->camera_timeline, distances);
  l.distances = distances;
  l.graph_top = l.ruler_bottom;
  l.graph_bottom = l.bottom;
  // A drag holds the graph's scale still, so the curve does not slide away under it.
  if (g_drag.mode == DRAG_KEY || g_drag.mode == DRAG_HANDLE) {
    l.graph_low = g_drag.graph_low;
    l.graph_high = g_drag.graph_high;
  } else {
    graph_fit(ui, &l);
  }

  handle_input(ui, &l);

  // In the graph editor the label column lists the channels to pick from.
  if (ed->graph_mode)
    for (int i = 0; i < l.lane_count; ++i) {
      igSetCursorScreenPos((ImVec2){l.label_x, l.lanes[i].top});
      igPushID_Int(i);
      if (igInvisibleButton("##channel", (ImVec2){label_width - 4.f * scale, lane_height}, 0) &&
          ed->selected_channel != l.lanes[i].channel) {
        ed->selected_channel = l.lanes[i].channel;
        ed->selected_key = -1;
      }
      if (igIsItemHovered(0)) igSetTooltip("Show %s in the graph", CHANNEL_NAMES[l.lanes[i].channel]);
      igPopID();
    }

  ImDrawList *draw = igGetWindowDrawList();
  const float bottom = l.bottom;
  ImDrawList_PushClipRect(draw, (ImVec2){l.label_x, l.ruler_top}, (ImVec2){l.x1, bottom}, true);
  for (int i = 0; i < l.lane_count; ++i) {
    const lane_t *lane = &l.lanes[i];
    ImDrawList_AddRectFilled(draw, (ImVec2){l.label_x, lane->top}, (ImVec2){l.x0, lane->bottom},
                             igGetColorU32_Col(ImGuiCol_FrameBg, 0.8f), 0.f, 0);
    const float right = ed->graph_mode ? l.x0 : l.x1;
    if (!ed->graph_mode)
      ImDrawList_AddRectFilled(draw, (ImVec2){l.x0, lane->top}, (ImVec2){l.x1, lane->bottom},
                               igGetColorU32_Col(ImGuiCol_FrameBg, i % 2 ? 0.22f : 0.38f), 0.f, 0);
    ImDrawList_AddLine(draw, (ImVec2){l.label_x, lane->bottom}, (ImVec2){right, lane->bottom},
                       igGetColorU32_Col(ImGuiCol_Border, 0.5f), 1.f);
  }
  if (ed->graph_mode)
    ImDrawList_AddRectFilled(draw, (ImVec2){l.x0, l.graph_top}, (ImVec2){l.x1, l.graph_bottom},
                             igGetColorU32_Col(ImGuiCol_FrameBg, 0.3f), 0.f, 0);

  ImDrawList_PushClipRect(draw, (ImVec2){l.x0, l.ruler_top}, (ImVec2){l.x1, bottom}, true);
  draw_range(ui, draw, &l, true);
  if (ed->graph_mode) {
    draw_graph(ui, draw, &l);
  } else {
    for (int i = 0; i < l.lane_count; ++i) {
      const lane_t *lane = &l.lanes[i];
      if (lane->channel == CAMERA_CHANNEL_TIME) draw_time_lane(ui, draw, &l, lane);
      else if (lane->channel == CAMERA_CHANNEL_POSE) draw_pose_lane(ui, draw, &l, lane);
      else draw_influence_lane(ui, draw, &l, lane);
    }
  }
  draw_seconds_ruler(ui, draw, &l);
  draw_game_ruler(ui, draw, &l);
  draw_range(ui, draw, &l, false);
  const float playhead_x = x_of(ed, &l, ed->playhead);
  ImDrawList_AddLine(draw, (ImVec2){playhead_x, l.ruler_top}, (ImVec2){playhead_x, l.bottom}, PLAYHEAD_COLOR, 2.f * scale);
  ImDrawList_AddTriangleFilled(draw, (ImVec2){playhead_x - 7.f * scale, l.ruler_top}, (ImVec2){playhead_x + 7.f * scale, l.ruler_top},
                               (ImVec2){playhead_x, l.ruler_top + 9.f * scale}, PLAYHEAD_COLOR);
  ImDrawList_PopClipRect(draw);

  const ImU32 muted = igGetColorU32_Col(ImGuiCol_TextDisabled, 1.f);
  ImDrawList_AddText_Vec2(draw, (ImVec2){l.label_x + 10.f * scale, 0.5f * (l.ruler_top + l.ruler_split - line)}, muted, "Seconds", NULL);
  ImDrawList_AddText_Vec2(draw, (ImVec2){l.label_x + 10.f * scale, 0.5f * (l.ruler_split + l.ruler_bottom - line)}, muted,
                          "Game tick", NULL);
  for (int i = 0; i < l.lane_count; ++i)
    draw_lane_label(ui, draw, &l, &l.lanes[i], ed->graph_mode && l.lanes[i].channel == ed->selected_channel);
  ImDrawList_PopClipRect(draw);

  render_scrollbar(ui, &l, l.bottom + 3.f * scale, scrollbar);
  igSetCursorScreenPos((ImVec2){l.x1 + 10.f * scale, origin.y});
  render_inspector(ui, origin.x + avail.x - (l.x1 + 10.f * scale), avail.y);
  igEnd();
}
