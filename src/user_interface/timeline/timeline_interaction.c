#include "timeline_interaction.h"
#include "timeline_commands.h"
#include "timeline_model.h"
#include "timeline_renderer.h"
#include <GLFW/glfw3.h>
#include <ddnet_physics/gamecore.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <user_interface/user_interface.h>

#define SNAP_THRESHOLD_PX 5.0f
#define DRAG_THRESHOLD_PX 5.0f

// Forward Declarations for Static Interaction Helpers
static void handle_pan_and_zoom(timeline_state_t *ts, ImRect timeline_bb);
static void handle_snippet_drag_and_drop(timeline_state_t *ts, ImRect timeline_bb, float tracks_scroll_y);
static void handle_selection_box(timeline_state_t *ts, ImRect timeline_bb, float tracks_scroll_y);
static void select_snippets_in_rect(timeline_state_t *ts, ImRect rect, ImRect timeline_bb, float scroll_y);
static int calculate_snapped_tick(const timeline_state_t *ts, int desired_start_tick, int duration, int exclude_id);
static void interaction_start_recording_on_track(timeline_state_t *ts, int track_index);

void interaction_apply_dummy_inputs(struct ui_handler *ui) {
  timeline_state_t *ts = &ui->timeline;
  if (!ts->recording || ts->selected_player_track_index == -1) return;

  SWorldCore world = wc_empty();
  model_get_world_state_at_tick(ts, ts->current_tick, &world);

  if (ts->selected_player_track_index >= world.m_NumCharacters) {
    wc_free(&world);
    return;
  }

  SCharacterCore *recording_char = &world.m_pCharacters[ts->selected_player_track_index];
  mvec2 recording_pos = recording_char->m_Pos;

  SPlayerInput source_input = ts->player_tracks[ts->selected_player_track_index].current_input;
  bool dummy_fire_active = is_key_combo_down(&ts->ui->keybinds.bindings[ACTION_DUMMY_FIRE].combo);

  for (int i = 0; i < ts->player_track_count; ++i) {
    if (i == ts->selected_player_track_index) continue;

    player_track_t *track = &ts->player_tracks[i];
    if (!track->is_dummy || i >= world.m_NumCharacters) continue;

    SCharacterCore *dummy_char = &world.m_pCharacters[i];
    mvec2 dummy_pos = dummy_char->m_Pos;

    SPlayerInput final_input = {0};

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
      } else if (action == DUMMY_ACTION_FIRE && dummy_fire_active && track->allow_dummy_fire) {
        final_input.m_Fire = 1;
        if (track->dummy_fire_aimbot) {
          final_input.m_TargetX = vgetx(recording_pos) - vgetx(dummy_pos);
          final_input.m_TargetY = vgety(recording_pos) - vgety(dummy_pos);
        }
      }
    }
    track->current_input = final_input;
  }
  wc_free(&world);
}

// Main Interaction Handlers
void interaction_handle_playback_and_shortcuts(timeline_state_t *ts) {
  ts->playback_speed = ts->gui_playback_speed;

  // Detect rewind (press or hold)
  bool reverse_down = is_key_combo_down(&ts->ui->keybinds.bindings[ACTION_REWIND_HOLD].combo) ||
                      is_key_combo_pressed(&ts->ui->keybinds.bindings[ACTION_REWIND_HOLD].combo, false);

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
      for (int i = 0; i < steps; ++i) {
        // model_advance_tick now uses the already-updated current_input values
        model_advance_tick(ts, dir);
      }
      ts->last_update_time += (double)steps * tick_interval;
    }
  }

  // Abort recording
  if (igIsKeyPressed_Bool(ImGuiKey_Escape, false) && ts->recording) interaction_toggle_recording(ts);

  // Cancel recording
  if (is_key_combo_pressed(&ts->ui->keybinds.bindings[ACTION_CANCEL_RECORDING].combo, false) && ts->recording) {
    interaction_cancel_recording(ts);
  }

  // Undo / Redo
  if (igIsKeyDown_Nil(ImGuiKey_LeftCtrl) || igIsKeyDown_Nil(ImGuiKey_RightCtrl)) {
    if (igIsKeyPressed_Bool(ImGuiKey_Z, false)) undo_manager_undo(&ts->ui->undo_manager, ts);
    if (igIsKeyPressed_Bool(ImGuiKey_Y, false)) undo_manager_redo(&ts->ui->undo_manager, ts);
  }

  // Trim shortcut (explicit trigger only)
  bool trim_pressed = is_key_combo_down(&ts->ui->keybinds.bindings[ACTION_TRIM_SNIPPET].combo);
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

void interaction_handle_timeline_area(timeline_state_t *ts, ImRect timeline_bb, float tracks_scroll_y) {
  if (igGetIO_Nil()->ConfigFlags & ImGuiConfigFlags_NoMouse) return;
  handle_pan_and_zoom(ts, timeline_bb);
  handle_snippet_drag_and_drop(ts, timeline_bb, tracks_scroll_y);
  handle_selection_box(ts, timeline_bb, tracks_scroll_y);
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

  // Zoom with mouse wheel
  if (io->MouseWheel != 0) {
    int mouse_tick_before = renderer_screen_x_to_tick(ts, io->MousePos.x, timeline_bb.Min.x);
    float zoom_delta = io->KeyCtrl * io->MouseWheel * 0.1f * ts->zoom;
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

  int mouse_tick = renderer_screen_x_to_tick(ts, io->MousePos.x, timeline_bb.Min.x);
  ts->drag_state.drag_offset_ticks = mouse_tick - snippet->start_tick;

  if (!interaction_is_snippet_selected(ts, snippet->id)) {
    interaction_clear_selection(ts);
    interaction_add_snippet_to_selection(ts, snippet_id);
  }

  ts->drag_state.drag_info_count = ts->selected_snippets.count;
  ts->drag_state.drag_infos = realloc(ts->drag_state.drag_infos, sizeof(DraggedSnippetInfo) * ts->drag_state.drag_info_count);

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

void interaction_calculate_drag_destination(timeline_state_t *ts, ImRect timeline_bb, float scroll_y, int *out_snapped_tick, int *out_base_track) {
  ImGuiIO *io = igGetIO_Nil();
  input_snippet_t *clicked_snippet = model_find_snippet_by_id(ts, ts->drag_state.dragged_snippet_id, NULL);
  if (!clicked_snippet) return;

  int mouse_tick = renderer_screen_x_to_tick(ts, io->MousePos.x, timeline_bb.Min.x);
  int desired_start_tick = mouse_tick - ts->drag_state.drag_offset_ticks;
  *out_snapped_tick = calculate_snapped_tick(ts, desired_start_tick, clicked_snippet->input_count, clicked_snippet->id);

  *out_base_track = renderer_screen_y_to_track_index(ts, timeline_bb, io->MousePos.y, scroll_y);
  if (*out_base_track == -1) {
    // If mouse is above or below, clamp to first or last track
    *out_base_track = (io->MousePos.y < timeline_bb.Min.y) ? 0 : ts->player_track_count - 1;
  }
  *out_base_track = imax(0, imin(ts->player_track_count - 1, *out_base_track));
}

static void handle_snippet_drag_and_drop(timeline_state_t *ts, ImRect timeline_bb, float tracks_scroll_y) {
  ImGuiIO *io = igGetIO_Nil();

  // bool any_item_hovered_before = igIsAnyItemHovered();

  // Iterate through visible snippets to create invisible buttons for interaction
  for (int i = 0; i < ts->player_track_count; ++i) {
    player_track_t *track = &ts->player_tracks[i];
    for (int j = 0; j < track->snippet_count; ++j) {
      input_snippet_t *snippet = &track->snippets[j];

      float start_x = renderer_tick_to_screen_x(ts, snippet->start_tick, timeline_bb.Min.x);
      float end_x = renderer_tick_to_screen_x(ts, snippet->end_tick, timeline_bb.Min.x);
      if (end_x < timeline_bb.Min.x || start_x > timeline_bb.Max.x) continue;

      float track_top = renderer_get_track_screen_y(ts, timeline_bb, i, tracks_scroll_y);

      // Mirror rendering logic to get correct hitbox for stacked snippets
      int stack_size = model_get_stack_size_at_tick_range(track, snippet->start_tick, snippet->end_tick);
      float sub_lane_height = ts->track_height / (float)fmax(1, stack_size);

      float snippet_y_pos = track_top + snippet->layer * sub_lane_height;
      float snippet_height = sub_lane_height;

      // Add a small margin to match rendering
      snippet_y_pos += 2.0f;
      snippet_height -= 4.0f;

      igSetCursorScreenPos((ImVec2){start_x, snippet_y_pos});
      igPushID_Int(snippet->id);
      igInvisibleButton("snippet", (ImVec2){imax(end_x - start_x, 1), fmaxf(1.0f, snippet_height)}, 0);

      if (igIsItemClicked(ImGuiMouseButton_Left)) {
        if (io->KeyShift) {
          if (interaction_is_snippet_selected(ts, snippet->id)) interaction_remove_snippet_from_selection(ts, snippet->id);
          else interaction_add_snippet_to_selection(ts, snippet->id);
        } else {
          if (!interaction_is_snippet_selected(ts, snippet->id)) {
            interaction_clear_selection(ts);
            interaction_add_snippet_to_selection(ts, snippet->id);
          }
        }
      }
      if (igIsItemActive() && igIsMouseDragging(ImGuiMouseButton_Left, DRAG_THRESHOLD_PX) && !ts->drag_state.active) {
        start_drag(ts, snippet->id, timeline_bb);
      }
      igPopID();
    }
  }

  // If the user clicked the empty track area (not on a snippet), select that track.
  // We check for a left mouse click inside the track rows and ensure no snippet item consumed the click.
  if (igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) {
    ImVec2 mouse = igGetIO_Nil()->MousePos;
    // Only consider clicks inside the timeline bounding box
    if (mouse.x >= timeline_bb.Min.x && mouse.x <= timeline_bb.Max.x && mouse.y >= timeline_bb.Min.y && mouse.y <= timeline_bb.Max.y) {
      int clicked_track = renderer_screen_y_to_track_index(ts, timeline_bb, mouse.y, tracks_scroll_y);
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

  // End drag
  if (ts->drag_state.active && igIsMouseReleased_Nil(ImGuiMouseButton_Left)) {
    int final_tick, final_track;
    interaction_calculate_drag_destination(ts, timeline_bb, tracks_scroll_y, &final_tick, &final_track);

    input_snippet_t *clicked_snippet = model_find_snippet_by_id(ts, ts->drag_state.dragged_snippet_id, NULL);
    if (clicked_snippet) {
      int tick_delta = final_tick - clicked_snippet->start_tick;

      MoveSnippetInfo *infos = malloc(sizeof(MoveSnippetInfo) * ts->drag_state.drag_info_count);
      int valid_moves = 0;

      for (int i = 0; i < ts->drag_state.drag_info_count; ++i) {
        DraggedSnippetInfo *d_info = &ts->drag_state.drag_infos[i];
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
        if (io->KeyAlt) {
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

static void handle_selection_box(timeline_state_t *ts, ImRect timeline_bb, float tracks_scroll_y) {
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
      select_snippets_in_rect(ts, rect, timeline_bb, tracks_scroll_y);
      ts->selection_box_active = false;
    }
  }
}

static void select_snippets_in_rect(timeline_state_t *ts, ImRect rect, ImRect timeline_bb, float scroll_y) {
  ImGuiIO *io = igGetIO_Nil();
  if (!io->KeyShift) {
    interaction_clear_selection(ts);
  }

  // Iterate over every snippet on every track
  for (int i = 0; i < ts->player_track_count; ++i) {
    player_track_t *track = &ts->player_tracks[i];

    // Get the screen Y position for the top of the current track row
    float track_top = renderer_get_track_screen_y(ts, timeline_bb, i, scroll_y);

    // If the entire track is outside the selection box, we can skip it
    if (track_top + ts->track_height < rect.Min.y || track_top > rect.Max.y) {
      continue;
    }

    for (int j = 0; j < track->snippet_count; ++j) {
      input_snippet_t *snip = &track->snippets[j];

      // Calculate the snippet's on-screen bounding box (mirroring render/hitbox logic)
      float start_x = renderer_tick_to_screen_x(ts, snip->start_tick, timeline_bb.Min.x);
      float end_x = renderer_tick_to_screen_x(ts, snip->end_tick, timeline_bb.Min.x);

      int stack_size = model_get_stack_size_at_tick_range(track, snip->start_tick, snip->end_tick);
      float sub_lane_height = ts->track_height / (float)fmax(1, stack_size);

      float snippet_y_pos = track_top + snip->layer * sub_lane_height + 2.0f;
      float snippet_height = sub_lane_height - 4.0f;

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
  int snapped_tick = desired_start_tick;
  float min_dist_px = SNAP_THRESHOLD_PX;

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
    // STOP RECORDING: Merge recording buffers into main snippets
    for (int i = 0; i < ts->player_track_count; ++i) {
      player_track_t *track = &ts->player_tracks[i];
      for (int j = 0; j < track->recording_snippet_count; ++j) {
        input_snippet_t *rec_snip = &track->recording_snippets[j];
        for (int k = 0; k < rec_snip->input_count; ++k) {
          int tick = rec_snip->start_tick + k;
          model_apply_input_to_main_buffer(ts, track, tick, &rec_snip->inputs[k]);
        }
      }
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
    if (track->recording_snippet_count == 0) continue;

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
    interaction_start_recording_on_track(ts, new_track_index);
  }
}

void interaction_update_recording_input(struct ui_handler *ui) {
  timeline_state_t *ts = &ui->timeline;
  keybind_manager_t *kb = &ui->keybinds;

  if (!ts->recording) return;
  if (ts->selected_player_track_index == -1 || ts->selected_player_track_index >= ts->player_track_count) return;

  SPlayerInput *input = &ts->player_tracks[ts->selected_player_track_index].current_input;

  input->m_Direction = is_key_combo_down(&kb->bindings[ACTION_RIGHT].combo) - is_key_combo_down(&kb->bindings[ACTION_LEFT].combo);
  input->m_Jump = is_key_combo_down(&kb->bindings[ACTION_JUMP].combo);
  input->m_Fire = is_key_combo_down(&kb->bindings[ACTION_FIRE].combo);
  input->m_Hook = is_key_combo_down(&kb->bindings[ACTION_HOOK].combo);
  set_flag_kill(input, is_key_combo_down(&kb->bindings[ACTION_KILL].combo));

  if (is_key_combo_pressed(&kb->bindings[ACTION_HAMMER].combo, false)) input->m_WantedWeapon = WEAPON_HAMMER;
  if (is_key_combo_pressed(&kb->bindings[ACTION_GUN].combo, false)) input->m_WantedWeapon = WEAPON_GUN;
  if (is_key_combo_pressed(&kb->bindings[ACTION_SHOTGUN].combo, false)) input->m_WantedWeapon = WEAPON_SHOTGUN;
  if (is_key_combo_pressed(&kb->bindings[ACTION_GRENADE].combo, false)) input->m_WantedWeapon = WEAPON_GRENADE;
  if (is_key_combo_pressed(&kb->bindings[ACTION_LASER].combo, false)) input->m_WantedWeapon = WEAPON_LASER;

  input->m_TargetX = (int)ui->recording_mouse_pos[0];
  input->m_TargetY = (int)ui->recording_mouse_pos[1];
}

SPlayerInput interaction_predict_input(struct ui_handler *ui, SWorldCore *world, int track_idx) {
  timeline_state_t *ts = &ui->timeline;

  if (ts->recording) {
    if (track_idx < 0 || track_idx >= ts->player_track_count) return (SPlayerInput){0};

    // Force update of recording state to ensure smooth visuals at frame rate
    // This is safe because it only updates the `current_input` struct,
    // it does not commit to the timeline buffer.

    if (track_idx == ts->selected_player_track_index) {
      interaction_update_recording_input(ui);
    } else {
      // Re-evaluate dummy logic for this specific frame
      // We can call apply_dummy_inputs which updates ALL dummies,
      // or we could extract specific logic. Calling the bulk update is safer.
      interaction_apply_dummy_inputs(ui);
    }

    return ts->player_tracks[track_idx].current_input;
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
