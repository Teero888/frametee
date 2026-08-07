#include "timeline_interaction.h"
#include "cglm/util.h"
#include "renderer/graphics_backend.h"
#include "timeline_commands.h"
#include "timeline_model.h"
#include "timeline_renderer.h"
#include "user_interface/timeline/timeline_types.h"
#include <GLFW/glfw3.h>
#include <system/input.h>
#include <ddnet_physics/gamecore.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <user_interface/user_interface.h>
#include <user_interface/widgets/imcol.h>

#define SNAP_THRESHOLD_PX 5.0f
#define DRAG_THRESHOLD_PX 5.0f
#define TRIM_HANDLE_PX 7.0f

// Forward Declarations for Static Interaction Helpers
static void handle_pan_and_zoom(timeline_state_t *ts, ImRect timeline_bb);
static void handle_snippet_drag_and_drop(timeline_state_t *ts, ImRect timeline_bb);
static void handle_trim_handles(timeline_state_t *ts, input_snippet_t *snippet, ImRect timeline_bb, ImRect rect);
static void handle_selection_box(timeline_state_t *ts, ImRect timeline_bb);
static void select_snippets_in_rect(timeline_state_t *ts, ImRect rect, ImRect timeline_bb);
static int calculate_snapped_tick(const timeline_state_t *ts, int desired_start_tick, int duration, int exclude_id);
static void interaction_start_recording_on_track(timeline_state_t *ts, int track_index);

// TODO: Make this faster. Performance is kind of horrible since we're doing this for every frame and every dummy for the prediction rendering.
// Or we should probably just keep track of prediction worlds instead of recalculating everything every frame.
void interaction_apply_dummy_inputs(ui_handler_t *ui) {
  timeline_state_t *ts = &ui->timeline;
  if (!ts->recording || ts->selected_player_track_index == -1) return;

  SWorldCore world = wc_empty();
  model_get_world_state_at_tick(ts, ts->current_tick, &world, false);

  if (ts->selected_player_track_index >= world.m_NumCharacters) {
    wc_free(&world);
    return;
  }

  SCharacterCore *recording_char = &world.m_pCharacters[ts->selected_player_track_index];
  mvec2 recording_pos = recording_char->m_Pos;

  SPlayerInput source_input = ts->player_tracks[ts->selected_player_track_index].current_input;

  bool dummy_left = keybinds_is_action_down(&ts->ui->keybinds, ACTION_DUMMY_LEFT);
  bool dummy_right = keybinds_is_action_down(&ts->ui->keybinds, ACTION_DUMMY_RIGHT);
  bool dummy_jump = keybinds_is_action_down(&ts->ui->keybinds, ACTION_DUMMY_JUMP);
  bool dummy_fire = keybinds_is_action_down(&ts->ui->keybinds, ACTION_DUMMY_FIRE);
  bool dummy_hook = keybinds_is_action_down(&ts->ui->keybinds, ACTION_DUMMY_HOOK);
  bool dummy_aim = keybinds_is_action_down(&ts->ui->keybinds, ACTION_DUMMY_AIM);

  for (int i = 0; i < ts->player_track_count; ++i) {
    if (i == ts->selected_player_track_index) continue;

    player_track_t *track = &ts->player_tracks[i];
    if (!track->is_dummy || i >= world.m_NumCharacters) continue;
    mvec2 dummy_pos = world.m_pCharacters[i].m_Pos;

    SPlayerInput final_input = {.m_TargetX = track->current_input.m_TargetX, .m_TargetY = track->current_input.m_TargetY};

    for (int action_idx = 0; action_idx < DUMMY_ACTION_COUNT; ++action_idx) {
      dummy_action_type_t action = ts->dummy_action_priority[action_idx];

      if (action == DUMMY_ACTION_COPY && ts->dummy_copy_input) {
        if (track->dummy_copy_flags & COPY_DIRECTION) final_input.m_Direction = source_input.m_Direction;
        if (track->dummy_copy_flags & COPY_TARGET) {
          final_input.m_TargetX = source_input.m_TargetX;
          final_input.m_TargetY = source_input.m_TargetY;
        }
        if (track->dummy_copy_flags & COPY_JUMP) final_input.m_Jump = source_input.m_Jump;
        if (track->dummy_copy_flags & COPY_FIRE) final_input.m_Fire = source_input.m_Fire;
        if (track->dummy_copy_flags & COPY_HOOK) final_input.m_Hook = source_input.m_Hook;
        if (track->dummy_copy_flags & COPY_WEAPON) final_input.m_WantedWeapon = source_input.m_WantedWeapon;

        if (track->dummy_copy_flags & COPY_MIRROR_X) {
          final_input.m_TargetX = -final_input.m_TargetX;
          final_input.m_Direction = -final_input.m_Direction;
        }
        if (track->dummy_copy_flags & COPY_MIRROR_Y) {
          final_input.m_TargetY = -final_input.m_TargetY;
        }
      } else if (action == DUMMY_ACTION_INPUTS) {
        if (dummy_left || dummy_right) final_input.m_Direction = dummy_right - dummy_left;
        if (dummy_jump) final_input.m_Jump = dummy_jump;
        if (dummy_fire) final_input.m_Fire = dummy_fire;
        if (dummy_hook) final_input.m_Hook = dummy_hook;

        if (dummy_aim) {
          final_input.m_TargetX = vgetx(recording_pos) - vgetx(dummy_pos);
          final_input.m_TargetY = vgety(recording_pos) - vgety(dummy_pos);
        }
      }
    }
    track->current_input = final_input;
  }
  wc_free(&world);
}

void interaction_update_mouse(timeline_state_t *ts) {
  if (ts->recording) {
    player_track_t *track = &ts->player_tracks[ts->selected_player_track_index];
    input_snippet_t *active_rec_snip = active_rec_snip = &track->recording_snippets[track->recording_snippet_count - 1];
    if (ts->current_tick < active_rec_snip->end_tick) {
      float speed_scale = ts->is_reversing ? 2.0f : 1.0f;
      float intra = fminf((igGetTime() - ts->last_update_time) / (1.f / (ts->playback_speed * speed_scale)), 1.f);
      if (ts->ui->timeline.is_reversing) intra = 1.f - intra;
      // idk man this is so hacky TODO: be smarter
      float div = (1.0f / (ts->ui->gfx_handler->viewport[1] / 2.f)) * 402.f;
      ts->ui->recording_mouse_pos[0] = glm_lerp(model_get_input_at_tick(ts, ts->selected_player_track_index, ts->current_tick - 1).m_TargetX / div,
                                                model_get_input_at_tick(ts, ts->selected_player_track_index, ts->current_tick).m_TargetX / div, intra);
      ts->ui->recording_mouse_pos[1] = glm_lerp(model_get_input_at_tick(ts, ts->selected_player_track_index, ts->current_tick - 1).m_TargetY / div,
                                                model_get_input_at_tick(ts, ts->selected_player_track_index, ts->current_tick).m_TargetY / div, intra);
    }
  }
}

// Main Interaction Handlers
void interaction_handle_playback_and_shortcuts(timeline_state_t *ts) {
  ts->playback_speed = ts->gui_playback_speed;

  // Detect rewind (press or hold)
  bool reverse_down =
      keybinds_is_action_down(&ts->ui->keybinds, ACTION_REWIND_HOLD) || keybinds_is_action_pressed(&ts->ui->keybinds, ACTION_REWIND_HOLD, false);

  if (reverse_down && !ts->is_reversing) ts->last_update_time = igGetTime();
  if (!reverse_down && ts->is_reversing) ts->last_update_time = igGetTime();

  ts->is_reversing = reverse_down;
  if (ts->is_reversing) ts->is_playing = false;

  // Always update inputs for ALL tracks (selected + dummies) to ensure smooth prediction rendering
  if (ts->recording) {
    interaction_update_recording_input(ts->ui);
    interaction_apply_dummy_inputs(ts->ui);
  }

  // Playback tick advancement
  if ((ts->is_playing || ts->is_reversing) && ts->playback_speed > 0) {
    double now = igGetTime();
    double tick_interval = 1.0 / ((double)ts->playback_speed * (ts->is_reversing ? 2.0 : 1.0));
    double elapsed = now - ts->last_update_time;
    if (elapsed < 0) elapsed = 0;
    if (elapsed > 5.0) elapsed = 5.0;

    int steps = (int)floor(elapsed / tick_interval);
    int dir = ts->is_reversing ? -1 : 1;
    if (steps > 0) {
      for (int i = 0; i < steps; ++i)
        model_advance_tick(ts, dir);
      if (dir > 0) {
        SWorldCore world = wc_empty();
        model_get_world_state_at_tick(ts, ts->current_tick, &world, true);
        wc_free(&world);
      }
      ts->last_update_time += (double)steps * tick_interval;
    }
  }

  if (ts->is_playing || ts->is_reversing) interaction_update_mouse(ts);

  // Abort recording
  if (input_key_pressed(GLFW_KEY_ESCAPE, false) && ts->recording) {
    ts->is_playing = 0;
    interaction_toggle_recording(ts);
  }

  // Cancel recording
  if (keybinds_is_action_pressed(&ts->ui->keybinds, ACTION_CANCEL_RECORDING, false) && ts->recording) {
    ts->is_playing = 0;
    interaction_cancel_recording(ts);
  }

  // Trim shortcut (explicit trigger only)
  bool trim_pressed = keybinds_is_action_down(&ts->ui->keybinds, ACTION_TRIM_SNIPPET);
  if (trim_pressed) interaction_trim_recording_snippet(ts);
}

void interaction_handle_header(timeline_state_t *ts, ImRect header_bb) {
  if (igGetIO_Nil()->ConfigFlags & ImGuiConfigFlags_NoMouse) return;
  ImGuiIO *io = igGetIO_Nil();
  bool is_header_hovered = igIsMouseHoveringRect(header_bb.Min, header_bb.Max, true);

  if (is_header_hovered && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) {
    ts->is_header_dragging = true;
  }
  if (ts->is_header_dragging) {
    if (igIsMouseDown_Nil(ImGuiMouseButton_Left)) {
      if (!ts->recording) {
        int mouse_tick = renderer_screen_x_to_tick(ts, io->MousePos.x, header_bb.Min.x);
        ts->current_tick = imax(0, mouse_tick);
      }
    } else {
      ts->is_header_dragging = false;
    }
  }
}

void interaction_handle_timeline_area(timeline_state_t *ts, ImRect timeline_bb) {
  if (igGetIO_Nil()->ConfigFlags & ImGuiConfigFlags_NoMouse) return;
  handle_pan_and_zoom(ts, timeline_bb);
  handle_snippet_drag_and_drop(ts, timeline_bb);
  handle_selection_box(ts, timeline_bb);
}

// Selection Helpers

void interaction_clear_selection(timeline_state_t *ts) { ts->selected_snippets.count = 0; }

void interaction_add_snippet_to_selection(timeline_state_t *ts, int snippet_id) {
  if (!snippet_id_vector_contains(&ts->selected_snippets, snippet_id)) {
    snippet_id_vector_add(&ts->selected_snippets, snippet_id);
  }
}

void interaction_remove_snippet_from_selection(timeline_state_t *ts, int snippet_id) { snippet_id_vector_remove(&ts->selected_snippets, snippet_id); }

bool interaction_is_snippet_selected(const timeline_state_t *ts, int snippet_id) {
  return snippet_id_vector_contains(&ts->selected_snippets, snippet_id);
}

void interaction_select_track(timeline_state_t *ts, int track_index) { ts->selected_player_track_index = track_index; }

// Static Interaction Helpers

static void handle_pan_and_zoom(timeline_state_t *ts, ImRect timeline_bb) {
  ImGuiIO *io = igGetIO_Nil();
  bool is_timeline_hovered = igIsMouseHoveringRect(timeline_bb.Min, timeline_bb.Max, true);

  if (!is_timeline_hovered) return;

  // Zoom with mouse wheel, read from GLFW so it is not delayed by imgui's event trickling.
  float wheel = (float)input_scroll_y();
  if (wheel != 0.0f) {
    int mouse_tick_before = renderer_screen_x_to_tick(ts, io->MousePos.x, timeline_bb.Min.x);
    float zoom_delta = (input_ctrl_down() ? 1.0f : 0.0f) * wheel * 0.1f * ts->zoom;
    ts->zoom = fmaxf(0.05f, fminf(20.0f, ts->zoom + zoom_delta));
    int mouse_tick_after = renderer_screen_x_to_tick(ts, io->MousePos.x, timeline_bb.Min.x);
    ts->view_start_tick += (mouse_tick_before - mouse_tick_after);
    if (ts->view_start_tick < 0) ts->view_start_tick = 0;
  }

  // Pan with middle mouse button
  if (igIsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
    ts->view_start_tick += (int)(-io->MouseDelta.x / ts->zoom);
    if (ts->view_start_tick < 0) ts->view_start_tick = 0;
  }
}

static void start_drag(timeline_state_t *ts, int snippet_id, ImRect timeline_bb) {
  ImGuiIO *io = igGetIO_Nil();
  input_snippet_t *snippet = model_find_snippet_by_id(ts, snippet_id, NULL);
  if (!snippet) return;

  ts->drag_state.active = true;
  ts->drag_state.dragged_snippet_id = snippet_id;
  ts->drag_state.initial_mouse_pos = io->MousePos;
  // the click turned into a drag, so the whole selection is being moved and must stay intact.
  ts->pending_single_select_id = -1;

  int mouse_tick = renderer_screen_x_to_tick(ts, io->MousePos.x, timeline_bb.Min.x);
  ts->drag_state.drag_offset_ticks = mouse_tick - snippet->start_tick;

  if (!interaction_is_snippet_selected(ts, snippet->id)) {
    interaction_clear_selection(ts);
    interaction_add_snippet_to_selection(ts, snippet_id);
  }

  ts->drag_state.drag_info_count = ts->selected_snippets.count;
  ts->drag_state.drag_infos = realloc(ts->drag_state.drag_infos, sizeof(dragged_snippet_info_t) * ts->drag_state.drag_info_count);

  int clicked_track_idx = -1;
  model_find_snippet_by_id(ts, ts->drag_state.dragged_snippet_id, &clicked_track_idx);

  for (int i = 0; i < ts->selected_snippets.count; i++) {
    int sid = ts->selected_snippets.ids[i];
    int s_track_idx = -1;
    input_snippet_t *s = model_find_snippet_by_id(ts, sid, &s_track_idx);
    if (s) {
      ts->drag_state.drag_infos[i].snippet_id = sid;
      ts->drag_state.drag_infos[i].track_offset = s_track_idx - clicked_track_idx;
      // layer offset calculation can be added if needed
    }
  }
}

void interaction_calculate_drag_destination(timeline_state_t *ts, ImRect timeline_bb, int *out_snapped_tick, int *out_base_track) {
  ImGuiIO *io = igGetIO_Nil();
  input_snippet_t *clicked_snippet = model_find_snippet_by_id(ts, ts->drag_state.dragged_snippet_id, NULL);
  if (!clicked_snippet) return;

  int mouse_tick = renderer_screen_x_to_tick(ts, io->MousePos.x, timeline_bb.Min.x);
  int desired_start_tick = mouse_tick - ts->drag_state.drag_offset_ticks;
  *out_snapped_tick = calculate_snapped_tick(ts, desired_start_tick, clicked_snippet->input_count, clicked_snippet->id);

  *out_base_track = renderer_screen_y_to_track_index(ts, io->MousePos.y);
  if (*out_base_track == -1) {
    // If mouse is above or below, clamp to first or last track
    *out_base_track = (io->MousePos.y < timeline_bb.Min.y) ? 0 : ts->player_track_count - 1;
  }
  *out_base_track = imax(0, imin(ts->player_track_count - 1, *out_base_track));
}

// Snaps a single dragged edge to the playhead and to the edges of other snippets.
static int snap_edge_tick(const timeline_state_t *ts, int desired_tick, int exclude_id) {
  float dpi_scale = gfx_get_ui_scale();
  int snapped_tick = desired_tick;
  float min_dist_px = SNAP_THRESHOLD_PX * dpi_scale;

  float dist_to_playhead_px = fabsf((float)(desired_tick - ts->current_tick) * ts->zoom);
  if (dist_to_playhead_px < min_dist_px) {
    min_dist_px = dist_to_playhead_px;
    snapped_tick = ts->current_tick;
  }

  for (int i = 0; i < ts->player_track_count; ++i) {
    for (int j = 0; j < ts->player_tracks[i].snippet_count; ++j) {
      input_snippet_t *other = &ts->player_tracks[i].snippets[j];
      if (other->id == exclude_id) continue;

      int edges[2] = {other->start_tick, other->end_tick};
      for (int e = 0; e < 2; ++e) {
        float dist = fabsf((float)(desired_tick - edges[e]) * ts->zoom);
        if (dist < min_dist_px) {
          min_dist_px = dist;
          snapped_tick = edges[e];
        }
      }
    }
  }
  return imax(0, snapped_tick);
}

// Where the dragged edge currently wants to sit, clamped so the snippet keeps at least one tick.
static int trim_target_tick(const timeline_state_t *ts, const input_snippet_t *snippet, ImRect timeline_bb) {
  ImGuiIO *io = igGetIO_Nil();
  int mouse_tick = renderer_screen_x_to_tick(ts, io->MousePos.x, timeline_bb.Min.x);
  int desired = mouse_tick - ts->trim_state.grab_offset_ticks;
  desired = snap_edge_tick(ts, desired, snippet->id);

  if (ts->trim_state.left_edge) return imin(desired, snippet->end_tick - 1);
  return imax(desired, snippet->start_tick + 1);
}

static void draw_arrow(ImDrawList *draw_list, float tip_x, float mid_y, float size, bool points_left, ImU32 col) {
  float tip = roundf(tip_x);
  float mid = roundf(mid_y);
  float base = roundf(points_left ? tip + size * 1.2f : tip - size * 1.2f);
  float half = roundf(size);

  ImVec2 v_tip = {tip, mid};
  ImVec2 v_top = {base, mid - half};
  ImVec2 v_bottom = {base, mid + half};
  if (points_left) ImDrawList_AddTriangleFilled(draw_list, v_tip, v_top, v_bottom, col);
  else ImDrawList_AddTriangleFilled(draw_list, v_tip, v_bottom, v_top, col);
}

// Draws the grab affordance on one edge: a bright bar, plus a tick of extra brightness where the
// snippet still holds source that trimming has hidden, so it reads as "there is more to pull out".
static void draw_trim_handle(const input_snippet_t *snippet, ImRect rect, bool left_edge, bool active) {
  float dpi_scale = gfx_get_ui_scale();
  ImDrawList *draw_list = igGetWindowDrawList();

  float thickness = (active ? 4.0f : 3.0f) * dpi_scale;
  float x = left_edge ? rect.Min.x : rect.Max.x - thickness;
  ImVec2 bar_min = {x, rect.Min.y};
  ImVec2 bar_max = {x + thickness, rect.Max.y};

  ImU32 col = active ? IM_COL32(255, 255, 255, 235) : IM_COL32(255, 255, 255, 150);
  ImDrawList_AddRectFilled(draw_list, bar_min, bar_max, col, 2.0f * dpi_scale, ImDrawFlags_RoundCornersAll);

  int hidden = left_edge ? snippet_source_before(snippet) : snippet_source_after(snippet);
  if (hidden <= 0) return;

  // Little arrow pointing outwards, in the direction the retained source lies.
  float mid_y = (rect.Min.y + rect.Max.y) * 0.5f;
  float size = fminf(5.0f * dpi_scale, (rect.Max.y - rect.Min.y) * 0.3f);
  if (size < 2.0f) return;

  draw_arrow(draw_list, left_edge ? bar_max.x + 1.0f : bar_min.x - 1.0f, mid_y, size, left_edge, col);
}

// Puts a grab zone on each edge of a snippet and runs the resize drag.
static void handle_trim_handles(timeline_state_t *ts, input_snippet_t *snippet, ImRect timeline_bb, ImRect rect) {
  float dpi_scale = gfx_get_ui_scale();
  float width = rect.Max.x - rect.Min.x;
  float height = rect.Max.y - rect.Min.y;
  if (height < 1.0f) return;

  // Never let the two handles eat the whole snippet, otherwise it cannot be dragged any more.
  float handle_w = fminf(TRIM_HANDLE_PX * dpi_scale, width * 0.34f);
  if (handle_w < 2.0f) return;

  bool trimming_this = ts->trim_state.active && ts->trim_state.snippet_id == snippet->id;

  for (int edge = 0; edge < 2; ++edge) {
    bool left_edge = (edge == 0);
    float x = left_edge ? rect.Min.x : rect.Max.x - handle_w;

    igSetCursorScreenPos((ImVec2){x, rect.Min.y});
    igPushID_Int(edge);
    igInvisibleButton("trim", (ImVec2){handle_w, height}, 0);

    bool hovered = igIsItemHovered(0);
    if (hovered || (trimming_this && ts->trim_state.left_edge == left_edge)) {
      igSetMouseCursor(ImGuiMouseCursor_ResizeEW);
      draw_trim_handle(snippet, rect, left_edge, trimming_this);
    }

    if (igIsItemActive() && igIsMouseDragging(ImGuiMouseButton_Left, 1.0f) && !ts->trim_state.active && !ts->drag_state.active) {
      int edge_tick = left_edge ? snippet->start_tick : snippet->end_tick;
      int mouse_tick = renderer_screen_x_to_tick(ts, igGetIO_Nil()->MousePos.x, timeline_bb.Min.x);

      ts->trim_state.active = true;
      ts->trim_state.snippet_id = snippet->id;
      ts->trim_state.left_edge = left_edge;
      ts->trim_state.grab_offset_ticks = mouse_tick - edge_tick;
      ts->trim_state.preview_tick = edge_tick;

      // Resizing acts on one snippet, so make it the selection.
      if (!interaction_is_snippet_selected(ts, snippet->id)) {
        interaction_clear_selection(ts);
        interaction_add_snippet_to_selection(ts, snippet->id);
      }
      ts->pending_single_select_id = -1;
    }
    igPopID();
  }

  if (!trimming_this) return;

  // Live feedback while dragging: the new extent as an outline, plus the resulting length.
  ts->trim_state.preview_tick = trim_target_tick(ts, snippet, timeline_bb);
  int new_start = ts->trim_state.left_edge ? ts->trim_state.preview_tick : snippet->start_tick;
  int new_end = ts->trim_state.left_edge ? snippet->end_tick : ts->trim_state.preview_tick;

  float preview_min_x = renderer_tick_to_screen_x(ts, new_start, timeline_bb.Min.x);
  float preview_max_x = renderer_tick_to_screen_x(ts, new_end, timeline_bb.Min.x);
  ImDrawList *draw_list = igGetWindowDrawList();

  // Mark where the retained source runs out on the side being dragged. With nothing hidden this
  // still marks the edge the drag started from, which is the reference for how long it was.
  bool left_edge = ts->trim_state.left_edge;
  int hidden = left_edge ? snippet_source_before(snippet) : snippet_source_after(snippet);
  int limit_tick = left_edge ? snippet->start_tick - hidden : snippet->end_tick + hidden;
  float limit_x = renderer_tick_to_screen_x(ts, limit_tick, timeline_bb.Min.x);
  float edge_x = left_edge ? preview_min_x : preview_max_x;
  bool past_limit = left_edge ? (ts->trim_state.preview_tick < limit_tick) : (ts->trim_state.preview_tick > limit_tick);

  // Between the dragged edge and that limit: source still to be revealed, or blank ticks being
  // invented once the drag goes past it.
  ImU32 region_col = past_limit ? IM_COL32(255, 180, 80, 45) : IM_COL32(255, 255, 255, 30);
  ImDrawList_AddRectFilled(draw_list, (ImVec2){fminf(edge_x, limit_x), rect.Min.y}, (ImVec2){fmaxf(edge_x, limit_x), rect.Max.y}, region_col, 0.0f, 0);

  ImDrawList_AddRectFilled(draw_list, (ImVec2){preview_min_x, rect.Min.y}, (ImVec2){preview_max_x, rect.Max.y}, IM_COL32(100, 150, 240, 70),
                           4.0f * dpi_scale, ImDrawFlags_RoundCornersAll);
  ImDrawList_AddRect(draw_list, (ImVec2){preview_min_x, rect.Min.y}, (ImVec2){preview_max_x, rect.Max.y}, IM_COL32(255, 255, 255, 200),
                     4.0f * dpi_scale, ImDrawFlags_RoundCornersAll, 1.5f * dpi_scale);

  ImU32 limit_col = IM_COL32(255, 255, 255, 190);
  ImDrawList_AddLine(draw_list, (ImVec2){roundf(limit_x), rect.Min.y}, (ImVec2){roundf(limit_x), rect.Max.y}, limit_col, 1.0f * dpi_scale);
  float arrow_size = fminf(5.0f * dpi_scale, (rect.Max.y - rect.Min.y) * 0.3f);
  if (hidden > 0 && arrow_size >= 2.0f) {
    // Points back into the material, capping the range the edge can still be pulled over.
    draw_arrow(draw_list, left_edge ? limit_x + 2.0f : limit_x - 2.0f, (rect.Min.y + rect.Max.y) * 0.5f, arrow_size, !left_edge, limit_col);
  }

  int delta = left_edge ? snippet->start_tick - new_start : new_end - snippet->end_tick;
  int beyond = left_edge ? limit_tick - ts->trim_state.preview_tick : ts->trim_state.preview_tick - limit_tick;
  if (past_limit) igSetTooltip("%d ticks  (%+d)\n%d blank", new_end - new_start, delta, beyond);
  else igSetTooltip("%d ticks  (%+d)\n%d source left", new_end - new_start, delta, -beyond);
}

static void handle_snippet_drag_and_drop(timeline_state_t *ts, ImRect timeline_bb) {
  float dpi_scale = gfx_get_ui_scale();

  // bool any_item_hovered_before = igIsAnyItemHovered();

  // Iterate through visible snippets to create invisible buttons for interaction
  for (int i = 0; i < ts->player_track_count; ++i) {
    player_track_t *track = &ts->player_tracks[i];
    for (int j = 0; j < track->snippet_count; ++j) {
      input_snippet_t *snippet = &track->snippets[j];

      float start_x = renderer_tick_to_screen_x(ts, snippet->start_tick, timeline_bb.Min.x);
      float end_x = renderer_tick_to_screen_x(ts, snippet->end_tick, timeline_bb.Min.x);
      if (end_x < timeline_bb.Min.x || start_x > timeline_bb.Max.x) continue;

      float track_top = renderer_get_track_screen_y(ts, i);

      // Mirror rendering logic to get correct hitbox for stacked snippets
      int stack_size = model_get_stack_size_at_tick_range(track, snippet->start_tick, snippet->end_tick);
      float sub_lane_height = (ts->track_height * dpi_scale) / (float)fmax(1, stack_size);

      float snippet_y_pos = track_top + snippet->layer * sub_lane_height;
      float snippet_height = sub_lane_height;

      // Add a small margin to match rendering
      snippet_y_pos += 2.0f * dpi_scale;
      snippet_height -= 4.0f * dpi_scale;

      igSetCursorScreenPos((ImVec2){start_x, snippet_y_pos});
      igPushID_Int(snippet->id);
      // The trim handles sit on top of the body, so the body has to allow being overlapped.
      igSetNextItemAllowOverlap();
      igInvisibleButton("snippet", (ImVec2){imax(end_x - start_x, 1), fmaxf(1.0f, snippet_height)}, 0);

      if (igIsItemClicked(ImGuiMouseButton_Left)) {
        if (input_shift_down()) {
          if (interaction_is_snippet_selected(ts, snippet->id)) interaction_remove_snippet_from_selection(ts, snippet->id);
          else interaction_add_snippet_to_selection(ts, snippet->id);
        } else {
          if (!interaction_is_snippet_selected(ts, snippet->id)) {
            interaction_clear_selection(ts);
            interaction_add_snippet_to_selection(ts, snippet->id);
          } else if (ts->selected_snippets.count > 1) {
            // keep the others selected for now, a group drag may still follow. Resolved on release.
            ts->pending_single_select_id = snippet->id;
          }
        }
      }

      if (igIsItemHovered(0) && igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left)) {
        // Ensure the clicked snippet is selected (should be handled by single click, but safe to ensure)
        if (!interaction_is_snippet_selected(ts, snippet->id)) {
          interaction_clear_selection(ts);
          interaction_add_snippet_to_selection(ts, snippet->id);
        }
        undo_command_t *cmd = commands_create_toggle_selected_snippets_active(ts->ui);
        if (cmd) undo_manager_register_command(&ts->ui->undo_manager, cmd);
      }

      if (igIsItemActive() && igIsMouseDragging(ImGuiMouseButton_Left, DRAG_THRESHOLD_PX * dpi_scale) && !ts->drag_state.active &&
          !ts->trim_state.active) {
        start_drag(ts, snippet->id, timeline_bb);
      }

      handle_trim_handles(ts, snippet, timeline_bb, (ImRect){{start_x, snippet_y_pos}, {end_x, snippet_y_pos + snippet_height}});
      igPopID();
    }
  }

  // If the user clicked the empty track area (not on a snippet), select that track.
  // We check for a left mouse click inside the track rows and ensure no snippet item consumed the click.
  if (igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) {
    ImVec2 mouse = igGetIO_Nil()->MousePos;
    // Only consider clicks inside the timeline bounding box
    if (mouse.x >= timeline_bb.Min.x && mouse.x <= timeline_bb.Max.x && mouse.y >= timeline_bb.Min.y && mouse.y <= timeline_bb.Max.y) {
      int clicked_track = renderer_screen_y_to_track_index(ts, mouse.y);
      if (clicked_track >= 0 && clicked_track < ts->player_track_count) {
        // If the click did NOT hit any existing item (snippet) we select the track.
        // igIsAnyItemHovered() being false is a reasonable heuristic here to mean the click landed on empty space.
        if (!igIsAnyItemHovered()) {
          interaction_select_track(ts, clicked_track);
          // Clicking a track (without Shift) should also clear snippet selection so user can start fresh
          if (!igGetIO_Nil()->KeyShift) interaction_clear_selection(ts);
        }
      }
    }
  }

  // commit a resize when the edge is let go
  if (ts->trim_state.active && igIsMouseReleased_Nil(ImGuiMouseButton_Left)) {
    input_snippet_t *snippet = model_find_snippet_by_id(ts, ts->trim_state.snippet_id, NULL);
    if (snippet) {
      int new_start = ts->trim_state.left_edge ? ts->trim_state.preview_tick : snippet->start_tick;
      int new_end = ts->trim_state.left_edge ? snippet->end_tick : ts->trim_state.preview_tick;
      undo_command_t *cmd = commands_create_trim_snippet(ts->ui, snippet->id, new_start, new_end);
      if (cmd) undo_manager_register_command(&ts->ui->undo_manager, cmd);
    }
    ts->trim_state.active = false;
  }

  // click on an already selected snippet that never became a drag collapses the selection onto it
  if (igIsMouseReleased_Nil(ImGuiMouseButton_Left)) {
    if (ts->pending_single_select_id != -1 && !ts->drag_state.active) {
      int snippet_id = ts->pending_single_select_id;
      interaction_clear_selection(ts);
      interaction_add_snippet_to_selection(ts, snippet_id);
    }
    ts->pending_single_select_id = -1;
  }

  // End drag
  if (ts->drag_state.active && igIsMouseReleased_Nil(ImGuiMouseButton_Left)) {
    int final_tick, final_track;
    interaction_calculate_drag_destination(ts, timeline_bb, &final_tick, &final_track);

    input_snippet_t *clicked_snippet = model_find_snippet_by_id(ts, ts->drag_state.dragged_snippet_id, NULL);
    if (clicked_snippet) {
      int tick_delta = final_tick - clicked_snippet->start_tick;

      MoveSnippetInfo *infos = malloc(sizeof(MoveSnippetInfo) * ts->drag_state.drag_info_count);
      int valid_moves = 0;

      for (int i = 0; i < ts->drag_state.drag_info_count; ++i) {
        dragged_snippet_info_t *d_info = &ts->drag_state.drag_infos[i];
        int s_track_idx;
        input_snippet_t *s = model_find_snippet_by_id(ts, d_info->snippet_id, &s_track_idx);
        if (!s) continue;

        int new_track = final_track + d_info->track_offset;
        if (new_track < 0 || new_track >= ts->player_track_count) continue;

        int new_tick = s->start_tick + tick_delta;
        int new_layer = model_find_available_layer(&ts->player_tracks[new_track], new_tick, new_tick + s->input_count, s->id);
        if (new_layer == -1) continue; // Collision

        infos[valid_moves++] = (MoveSnippetInfo){.snippet_id = s->id,
                                                 .old_track_index = s_track_idx,
                                                 .old_start_tick = s->start_tick,
                                                 .old_layer = s->layer,
                                                 .new_track_index = new_track,
                                                 .new_start_tick = new_tick,
                                                 .new_layer = new_layer};
      }

      if (valid_moves > 0) {
        undo_command_t *cmd = NULL;
        if (input_alt_down()) {
          cmd = commands_create_duplicate_snippets(ts->ui, infos, valid_moves);
        } else {
          cmd = commands_create_move_snippets(ts->ui, infos, valid_moves);
        }
        if (cmd) undo_manager_register_command(&ts->ui->undo_manager, cmd);
      }
      free(infos);
    }

    ts->drag_state.active = false;
  }
}

static void handle_selection_box(timeline_state_t *ts, ImRect timeline_bb) {
  ImGuiIO *io = igGetIO_Nil();
  bool is_timeline_hovered = igIsMouseHoveringRect(timeline_bb.Min, timeline_bb.Max, true);

  if (is_timeline_hovered && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false) && !igIsAnyItemHovered()) {
    ts->selection_box_active = true;
    ts->selection_box_start = io->MousePos;
    ts->selection_box_end = io->MousePos;
  }

  if (ts->selection_box_active) {
    if (igIsMouseDown_Nil(ImGuiMouseButton_Left)) {
      ts->selection_box_end = io->MousePos;
    } else {
      ImRect rect = {{fminf(ts->selection_box_start.x, ts->selection_box_end.x), fminf(ts->selection_box_start.y, ts->selection_box_end.y)},
                     {fmaxf(ts->selection_box_start.x, ts->selection_box_end.x), fmaxf(ts->selection_box_start.y, ts->selection_box_end.y)}};
      select_snippets_in_rect(ts, rect, timeline_bb);
      ts->selection_box_active = false;
    }
  }
}

static void select_snippets_in_rect(timeline_state_t *ts, ImRect rect, ImRect timeline_bb) {
  float dpi_scale = gfx_get_ui_scale();
  if (!input_shift_down()) {
    interaction_clear_selection(ts);
  }

  // Iterate over every snippet on every track
  for (int i = 0; i < ts->player_track_count; ++i) {
    player_track_t *track = &ts->player_tracks[i];

    // Get the screen Y position for the top of the current track row
    float track_top = renderer_get_track_screen_y(ts, i);

    // If the entire track is outside the selection box, we can skip it
    if (track_top + (ts->track_height * dpi_scale) < rect.Min.y || track_top > rect.Max.y) {
      continue;
    }

    for (int j = 0; j < track->snippet_count; ++j) {
      input_snippet_t *snip = &track->snippets[j];

      // Calculate the snippet's on-screen bounding box (mirroring render/hitbox logic)
      float start_x = renderer_tick_to_screen_x(ts, snip->start_tick, timeline_bb.Min.x);
      float end_x = renderer_tick_to_screen_x(ts, snip->end_tick, timeline_bb.Min.x);

      int stack_size = model_get_stack_size_at_tick_range(track, snip->start_tick, snip->end_tick);
      float sub_lane_height = (ts->track_height * dpi_scale) / (float)fmax(1, stack_size);

      float snippet_y_pos = track_top + snip->layer * sub_lane_height + 2.0f * dpi_scale;
      float snippet_height = sub_lane_height - 4.0f * dpi_scale;

      ImRect snippet_bb = {{start_x, snippet_y_pos}, {end_x, snippet_y_pos + snippet_height}};

      // AABB intersection test
      bool x_overlap = rect.Max.x >= snippet_bb.Min.x && rect.Min.x <= snippet_bb.Max.x;
      bool y_overlap = rect.Max.y >= snippet_bb.Min.y && rect.Min.y <= snippet_bb.Max.y;

      if (x_overlap && y_overlap) {
        interaction_add_snippet_to_selection(ts, snip->id);
      }
    }
  }
}

static int calculate_snapped_tick(const timeline_state_t *ts, int desired_start_tick, int duration, int exclude_id) {
  float dpi_scale = gfx_get_ui_scale();
  int snapped_tick = desired_start_tick;
  float min_dist_px = SNAP_THRESHOLD_PX * dpi_scale;

  // Snap to playhead
  float dist_to_playhead_px = fabsf((float)(desired_start_tick - ts->current_tick) * ts->zoom);
  if (dist_to_playhead_px < min_dist_px) {
    min_dist_px = dist_to_playhead_px;
    snapped_tick = ts->current_tick;
  }

  // Snap to other snippets
  for (int i = 0; i < ts->player_track_count; ++i) {
    for (int j = 0; j < ts->player_tracks[i].snippet_count; ++j) {
      input_snippet_t *other = &ts->player_tracks[i].snippets[j];
      if (other->id == exclude_id) continue;

      // Snap start to other start
      float dist = fabsf((float)(desired_start_tick - other->start_tick) * ts->zoom);
      if (dist < min_dist_px) {
        min_dist_px = dist;
        snapped_tick = other->start_tick;
      }

      // Snap start to other end
      dist = fabsf((float)(desired_start_tick - other->end_tick) * ts->zoom);
      if (dist < min_dist_px) {
        min_dist_px = dist;
        snapped_tick = other->end_tick;
      }

      // Snap end to other start
      dist = fabsf((float)((desired_start_tick + duration) - other->start_tick) * ts->zoom);
      if (dist < min_dist_px) {
        min_dist_px = dist;
        snapped_tick = other->start_tick - duration;
      }

      // Snap end to other end
      dist = fabsf((float)((desired_start_tick + duration) - other->end_tick) * ts->zoom);
      if (dist < min_dist_px) {
        min_dist_px = dist;
        snapped_tick = other->end_tick - duration;
      }
    }
  }
  return imax(0, snapped_tick);
}

// Recording Helpers Implementation

static void interaction_start_recording_on_track(timeline_state_t *ts, int track_index) {
  if (track_index < 0 || track_index >= ts->player_track_count) return;
  player_track_t *track = &ts->player_tracks[track_index];

  // Create a new snippet to record into
  input_snippet_t new_snippet = {0};
  new_snippet.id = ts->next_snippet_id++;
  new_snippet.start_tick = ts->current_tick;
  new_snippet.end_tick = ts->current_tick;
  new_snippet.is_active = true;
  new_snippet.layer = 0;

  // Add it to the recording track
  model_insert_snippet_into_recording_track(track, &new_snippet);

  // Find the pointer to the newly inserted snippet and add it to our recording list
  input_snippet_t *recording_target = &track->recording_snippets[track->recording_snippet_count - 1];
  if (ts->recording_snippets.count >= ts->recording_snippets.capacity) {
    ts->recording_snippets.capacity = ts->recording_snippets.capacity == 0 ? 4 : ts->recording_snippets.capacity * 2;
    ts->recording_snippets.snippets = realloc(ts->recording_snippets.snippets, sizeof(input_snippet_t *) * ts->recording_snippets.capacity);
  }
  ts->recording_snippets.snippets[ts->recording_snippets.count++] = recording_target;
}

void interaction_toggle_recording(timeline_state_t *ts) {
  ts->recording = !ts->recording;

  if (ts->recording) {
    ts->recording_snippets.count = 0;
    bool any_recording_started = false;

    for (int i = 0; i < ts->player_track_count; ++i) {
      player_track_t *track = &ts->player_tracks[i];
      bool is_selected = (i == ts->selected_player_track_index);
      bool is_dummy = track->is_dummy;

      if (is_selected || is_dummy) {
        interaction_start_recording_on_track(ts, i);
        any_recording_started = true;
      }
    }

    if (!any_recording_started) {
      ts->recording = false;
      return;
    }
  } else {
    // STOP RECORDING
    undo_command_t *cmd = commands_create_commit_recording(ts->ui);
    if (cmd) {
      undo_manager_register_command(&ts->ui->undo_manager, cmd);
    }

    model_clear_all_recording_buffers(ts);
    ts->recording_snippets.count = 0;
  }
}

void interaction_cancel_recording(timeline_state_t *ts) {
  if (!ts->recording) return;
  ts->recording = false;
  model_clear_all_recording_buffers(ts);
  ts->recording_snippets.count = 0;
  model_recalc_physics(ts, 0);
}

void interaction_trim_recording_snippet(timeline_state_t *ts) {
  if (!ts->recording) return;

  for (int i = 0; i < ts->player_track_count; ++i) {
    player_track_t *track = &ts->player_tracks[i];

    if (track->recording_snippet_count == 0) continue;

    for (int j = 0; j < track->recording_snippet_count; ++j) {
      input_snippet_t *rec = &track->recording_snippets[j];
      if (!rec) continue;

      int trim_to = ts->current_tick;
      if (trim_to < rec->start_tick) {
        model_free_snippet_inputs(rec);
        memmove(&track->recording_snippets[j], &track->recording_snippets[j + 1], (track->recording_snippet_count - j - 1) * sizeof(input_snippet_t));
        track->recording_snippet_count--;
        j--;
        continue;
      }

      int new_duration = trim_to - rec->start_tick;
      if (new_duration < rec->input_count) {
        model_resize_snippet_inputs(ts, rec, new_duration);
      }

      if (rec->input_count <= 0) {
        model_free_snippet_inputs(rec);
        memmove(&track->recording_snippets[j], &track->recording_snippets[j + 1], (track->recording_snippet_count - j - 1) * sizeof(input_snippet_t));
        track->recording_snippet_count--;
        j--;
      }
    }
  }

  ts->recording_snippets.count = 0;

  for (int i = 0; i < ts->player_track_count; ++i) {
    player_track_t *track = &ts->player_tracks[i];

    bool should_record = (i == ts->selected_player_track_index) || track->is_dummy;
    if (!should_record && track->recording_snippet_count == 0) continue;

    input_snippet_t *target = NULL;

    for (int j = 0; j < track->recording_snippet_count; ++j) {
      if (track->recording_snippets[j].end_tick == ts->current_tick) {
        target = &track->recording_snippets[j];
        break;
      }
    }

    if (!target) {
      interaction_start_recording_on_track(ts, i);
    } else {
      if (ts->recording_snippets.count >= ts->recording_snippets.capacity) {
        ts->recording_snippets.capacity = ts->recording_snippets.capacity == 0 ? 4 : ts->recording_snippets.capacity * 2;
        ts->recording_snippets.snippets = realloc(ts->recording_snippets.snippets, sizeof(input_snippet_t *) * ts->recording_snippets.capacity);
      }
      ts->recording_snippets.snippets[ts->recording_snippets.count++] = target;
    }
  }
}

void interaction_switch_recording_target(timeline_state_t *ts, int new_track_index) {
  if (ts->recording && new_track_index >= 0 && new_track_index < ts->player_track_count) {
    ts->selected_player_track_index = new_track_index;
    if (!ts->player_tracks[new_track_index].is_dummy) {
      interaction_start_recording_on_track(ts, new_track_index);
    }
  }
}

void interaction_update_recording_input(ui_handler_t *ui) {
  timeline_state_t *ts = &ui->timeline;
  keybind_manager_t *kb = &ui->keybinds;

  if (!ts->recording) return;
  if (ts->selected_player_track_index == -1 || ts->selected_player_track_index >= ts->player_track_count) return;

  SPlayerInput *input = &ts->player_tracks[ts->selected_player_track_index].current_input;

  input->m_Direction = keybinds_is_action_down(kb, ACTION_RIGHT) - keybinds_is_action_down(kb, ACTION_LEFT);
  input->m_Jump = keybinds_is_action_down(kb, ACTION_JUMP);
  input->m_Fire = keybinds_is_action_down(kb, ACTION_FIRE);
  input->m_Hook = keybinds_is_action_down(kb, ACTION_HOOK);
  set_flag_kill(input, keybinds_is_action_down(kb, ACTION_KILL));

  if (keybinds_is_action_pressed(kb, ACTION_HAMMER, false)) input->m_WantedWeapon = WEAPON_HAMMER;
  if (keybinds_is_action_pressed(kb, ACTION_GUN, false)) input->m_WantedWeapon = WEAPON_GUN;
  if (keybinds_is_action_pressed(kb, ACTION_SHOTGUN, false)) input->m_WantedWeapon = WEAPON_SHOTGUN;
  if (keybinds_is_action_pressed(kb, ACTION_GRENADE, false)) input->m_WantedWeapon = WEAPON_GRENADE;
  if (keybinds_is_action_pressed(kb, ACTION_LASER, false)) input->m_WantedWeapon = WEAPON_LASER;

  float div = (1.0f / (ui->gfx_handler->viewport[1] / 2.f)) * 402.f;
  input->m_TargetX = (int)ui->recording_mouse_pos[0] * div;
  input->m_TargetY = (int)ui->recording_mouse_pos[1] * div;
  if (!input->m_TargetX && !input->m_TargetY)
    input->m_TargetX = 1;
}

SPlayerInput interaction_predict_input(ui_handler_t *ui, SWorldCore *world, int track_idx) {
  timeline_state_t *ts = &ui->timeline;

  if (ts->recording) {
    if (track_idx < 0 || track_idx >= ts->player_track_count) return (SPlayerInput){0};

    // Force update of recording state to ensure smooth visuals at frame rate
    // This is safe because it only updates the current_input struct,
    // it does not commit to the timeline buffer.

    if (track_idx == ts->selected_player_track_index) {
      interaction_update_recording_input(ui);
    } else {
      // Re-evaluate dummy logic for this specific frame
      // We can call apply_dummy_inputs which updates ALL dummies,
      // or we could extract specific logic. Calling the bulk update is safer.
      interaction_apply_dummy_inputs(ui);
    }
    if (track_idx == ts->selected_player_track_index) return ts->player_tracks[track_idx].current_input;
  }

  return model_get_input_at_tick(ts, track_idx, world->m_GameTick);
}

void interaction_handle_context_menu(timeline_state_t *ts) {
  if (igGetIO_Nil()->ConfigFlags & ImGuiConfigFlags_NoMouse) return;
  if (igBeginPopup("TimelineContextMenu", 0)) {
    if (igMenuItem_Bool("Add Snippet", NULL, false, ts->selected_player_track_index != -1)) {
      undo_command_t *cmd = commands_create_add_snippet(ts->ui, ts->selected_player_track_index, ts->current_tick, 50);
      if (cmd) undo_manager_register_command(&ts->ui->undo_manager, cmd);
    }
    igSeparator();
    if (igMenuItem_Bool("Split Selected", "Ctrl+R", false, ts->selected_snippets.count > 0)) {
      undo_command_t *cmd = commands_create_split_selected(ts->ui);
      if (cmd) undo_manager_register_command(&ts->ui->undo_manager, cmd);
    }
    if (igMenuItem_Bool("Merge Selected", "Ctrl+M", false, ts->selected_snippets.count > 1)) {
      undo_command_t *cmd = commands_create_merge_selected(ts->ui);
      if (cmd) undo_manager_register_command(&ts->ui->undo_manager, cmd);
    }
    if (igMenuItem_Bool("Delete Selected", "Del", false, ts->selected_snippets.count > 0)) {
      undo_command_t *cmd = commands_create_delete_selected(ts->ui);
      if (cmd) undo_manager_register_command(&ts->ui->undo_manager, cmd);
    }
    igEndPopup();
  }
}
