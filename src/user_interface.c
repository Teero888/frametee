#include "user_interface.h"
#include "cimgui.h"
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#define MIN_TIMELINE_ZOOM 0.01f
#define MAX_TIMELINE_ZOOM 10.0f
#define TPS 50
#define DEFAULT_TRACK_HEIGHT 40.f

// --- Docking Setup ---
// Call this once after ImGui context creation and before the main loop if you want a specific layout.
// Alternatively, call it within ui_render on the first frame.
void setup_docking(ui_handler *ui) {
  ImGuiID main_dockspace_id = igGetID_Str("MainDockSpace");

  // Ensure the dockspace covers the entire viewport initially
  ImGuiViewport *viewport = igGetMainViewport();
  igSetNextWindowPos(viewport->WorkPos, ImGuiCond_Always, (ImVec2){0.0f, 0.0f});
  igSetNextWindowSize(viewport->WorkSize, ImGuiCond_Always);
  igSetNextWindowViewport(viewport->ID);

  ImGuiWindowFlags host_window_flags = 0;
  host_window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoMove;
  host_window_flags |=
      ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground;

  igPushStyleVar_Float(ImGuiStyleVar_WindowRounding, 0.0f);
  igPushStyleVar_Float(ImGuiStyleVar_WindowBorderSize, 0.0f);
  igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2){0.0f, 0.0f});
  igBegin("DockSpace Host Window", NULL,
          host_window_flags); // pass null for p_open to prevent closing the host window
  igPopStyleVar(3);

  // create the main dockspace
  igDockSpace(main_dockspace_id, (ImVec2){0.0f, 0.0f}, ImGuiDockNodeFlags_PassthruCentralNode,
              NULL); // Passthru allows seeing background
  igEnd();

  // -- Build the initial layout programmatically (optional, but good for setup) --
  // This needs to be done *after* the DockSpace call, often on the first frame or after a reset.
  static bool first_time = true;
  if (first_time) {
    first_time = false;

    igDockBuilderRemoveNode(main_dockspace_id); // Clear existing layout
    igDockBuilderAddNode(main_dockspace_id, ImGuiDockNodeFlags_DockSpace);
    igDockBuilderSetNodeSize(main_dockspace_id, viewport->WorkSize);

    // Split the main dockspace: Timeline at bottom, rest on top
    ImGuiID dock_id_top;
    ImGuiID dock_id_bottom = igDockBuilderSplitNode(main_dockspace_id, ImGuiDir_Down, 0.30f, NULL,
                                                    &dock_id_top); // Timeline takes 30%

    // Split the top area: Player list on left, properties on right
    ImGuiID dock_id_left;
    ImGuiID dock_id_right = igDockBuilderSplitNode(dock_id_top, ImGuiDir_Right, 0.80f, NULL,
                                                   &dock_id_left); // Player list takes 20% (1.0 - 0.8)
    // The remaining central node of the top split (where dock_id_right was created) will be left empty by
    // default with PassthruCentralNode

    // Assign windows to docks
    igDockBuilderDockWindow("Timeline", dock_id_bottom);
    // igDockBuilderDockWindow("Player List", dock_id_left);
    // igDockBuilderDockWindow("Properties", dock_id_right);
    // Important: Add other windows like Demo/Metrics here if you want them docked initially
    igDockBuilderDockWindow("Dear ImGui Demo", dock_id_right); // Example

    igDockBuilderFinish(main_dockspace_id);
  }
}

// --- Timeline Window ---
void render_timeline(ui_handler *ui) {
  timeline_state *ts = &ui->timeline;

  igSetNextWindowClass(&((ImGuiWindowClass){.DockingAllowUnclassed = false}));
  igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2){8, 8});
  igPushStyleVar_Float(ImGuiStyleVar_FrameRounding, 4.0f);
  if (igBegin("Timeline", NULL, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
    igPopStyleVar(2);

    ImVec2 window_pos, window_size, mouse_pos;
    igGetWindowPos(&window_pos);
    igGetWindowSize(&window_size);
    mouse_pos = igGetIO_Nil()->MousePos;

    ImDrawList *draw_list = igGetWindowDrawList();

    // --- Draw Controls (at the top) ---
    float controls_height = igGetTextLineHeightWithSpacing() * 2.0f + 20.0f;
    igPushStyleVar_Vec2(ImGuiStyleVar_ItemSpacing, (ImVec2){8, 4});
    igPushItemWidth(100);
    if (igDragInt("Current Tick", &ts->current_tick, 1, 0, 100000, "%d", ImGuiSliderFlags_None)) {
      if (ts->current_tick < 0)
        ts->current_tick = 0;
    }
    igSameLine(0, 8);
    if (igButton("|<", (ImVec2){30, 0}))
      ts->current_tick = 0;
    igSameLine(0, 4);
    if (igArrowButton("<<", ImGuiDir_Left))
      ts->current_tick = (ts->current_tick >= 50) ? ts->current_tick - 50 : 0;
    igSameLine(0, 4);
    if (igButton("Play", (ImVec2){50, 0})) { /* TODO: Playback logic */
    }
    igSameLine(0, 4);
    if (igArrowButton(">>", ImGuiDir_Right))
      ts->current_tick += 50;
    igSameLine(0, 4);
    if (igButton(">|", (ImVec2){30, 0})) { /* TODO: Go to end */
    }
    igSameLine(0, 20);
    igText("Zoom:");
    igSameLine(0, 4);
    igSetNextItemWidth(150);
    igSliderFloat("##Zoom", &ts->zoom, MIN_TIMELINE_ZOOM, MAX_TIMELINE_ZOOM, "%.2f",
                  ImGuiSliderFlags_Logarithmic);
    igPopItemWidth();
    igPopStyleVar(1);

    // --- Timeline Layout ---
    float header_height = igGetTextLineHeightWithSpacing();
    float timeline_area_height = window_size.y - controls_height - header_height;
    ImVec2 timeline_start_pos = {window_pos.x, window_pos.y + controls_height + header_height};
    ImVec2 timeline_end_pos = {window_pos.x + window_size.x,
                               window_pos.y + controls_height + header_height + timeline_area_height};
    ImRect timeline_bb = {timeline_start_pos, timeline_end_pos};

    // --- Mouse Interaction for Pan/Zoom ---
    bool is_timeline_hovered = igIsMouseHoveringRect(timeline_start_pos, timeline_end_pos, true);

    if (is_timeline_hovered && igGetIO_Nil()->MouseWheel != 0) {
      int mouse_tick_before_zoom =
          ts->view_start_tick + (int)((mouse_pos.x - timeline_start_pos.x) / ts->zoom);
      float zoom_delta = igGetIO_Nil()->MouseWheel * 0.1f * ts->zoom;
      ts->zoom = fmaxf(MIN_TIMELINE_ZOOM, fminf(MAX_TIMELINE_ZOOM, ts->zoom + zoom_delta));
      int mouse_tick_after_zoom =
          ts->view_start_tick + (int)((mouse_pos.x - timeline_start_pos.x) / ts->zoom);
      ts->view_start_tick += (mouse_tick_before_zoom - mouse_tick_after_zoom);
      if (ts->view_start_tick < 0)
        ts->view_start_tick = 0;
    }

    if (is_timeline_hovered && igIsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
      ImVec2 drag_delta;
      igGetMouseDragDelta(&drag_delta, ImGuiMouseButton_Middle, 0.0f);
      igResetMouseDragDelta(ImGuiMouseButton_Middle);
      int tick_delta = (int)(-drag_delta.x / ts->zoom);
      ts->view_start_tick += tick_delta;
      if (ts->view_start_tick < 0)
        ts->view_start_tick = 0;
    }

    // --- Draw Header (Ticks) ---
    ImU32 tick_col = igGetColorU32_Col(ImGuiCol_TextDisabled, 0.7f);
    ImU32 tick_sec_col = igGetColorU32_Col(ImGuiCol_Text, 0.9f);
    ImU32 tick_text_col = igGetColorU32_Col(ImGuiCol_Text, 1.0f);

    int min_pixels_per_tick_label = 50;
    int tick_step = 1;
    int label_tick_step = TPS / 5;

    if (ts->zoom * label_tick_step < min_pixels_per_tick_label) {
      label_tick_step = (int)ceilf(min_pixels_per_tick_label / ts->zoom);
      if (label_tick_step <= 5)
        label_tick_step = 5;
      else if (label_tick_step <= 10)
        label_tick_step = 10;
      else if (label_tick_step <= 25)
        label_tick_step = 25;
      else if (label_tick_step <= 50)
        label_tick_step = 50;
      else if (label_tick_step <= 100)
        label_tick_step = 100;
      else if (label_tick_step <= 250)
        label_tick_step = 250;
      else
        label_tick_step = (int)(ceilf(label_tick_step / 500.0f) * 500.0f);
    }
    if (ts->zoom < 2.0f)
      tick_step = 5;
    if (ts->zoom < 0.5f)
      tick_step = 10;
    if (ts->zoom < 0.1f)
      tick_step = 50;

    int max_visible_ticks = (int)(window_size.x / ts->zoom) + 1;
    int view_end_tick = ts->view_start_tick + max_visible_ticks;

    float header_y = window_pos.y + controls_height;
    for (int tick = ts->view_start_tick; tick <= view_end_tick; ++tick) {
      float x = timeline_start_pos.x + (tick - ts->view_start_tick) * ts->zoom;
      if (x < timeline_start_pos.x || x > timeline_end_pos.x)
        continue;

      bool is_second_marker = (tick % TPS == 0);
      bool is_label_marker = (tick % label_tick_step == 0);
      bool is_minor_marker = (tick % tick_step == 0);

      if (is_second_marker) {
        ImDrawList_AddLine(draw_list, (ImVec2){x, header_y}, (ImVec2){x, header_y + header_height},
                           tick_sec_col, 1.0f);
        if (is_label_marker) {
          char label[32];
          snprintf(label, sizeof(label), "%ds", tick / TPS);
          ImVec2 text_size;
          igCalcTextSize(&text_size, label, NULL, false, 0);
          ImDrawList_AddText_Vec2(draw_list,
                                  (ImVec2){x + 3.0f, header_y + (header_height - text_size.y) * 0.5f},
                                  tick_text_col, label, NULL);
        }
      } else if (is_label_marker) {
        ImDrawList_AddLine(draw_list, (ImVec2){x, header_y + header_height * 0.25f},
                           (ImVec2){x, header_y + header_height}, tick_col, 1.0f);
        char label[16];
        snprintf(label, sizeof(label), "%d", tick);
        ImVec2 text_size;
        igCalcTextSize(&text_size, label, NULL, false, 0);
        ImDrawList_AddText_Vec2(draw_list,
                                (ImVec2){x + 3.0f, header_y + (header_height - text_size.y) * 0.5f},
                                tick_text_col, label, NULL);
      } else if (is_minor_marker) {
        ImDrawList_AddLine(draw_list, (ImVec2){x, header_y + header_height * 0.6f},
                           (ImVec2){x, header_y + header_height}, tick_col, 1.0f);
      }
    }

    // --- Draw Timeline Tracks and Snippets ---
    igPushClipRect(timeline_start_pos, timeline_end_pos, true);

    float current_track_y = timeline_start_pos.y;
    for (int i = 0; i < ts->player_track_count; ++i) {
      player_track *track = &ts->player_tracks[i];

      float track_top = current_track_y;
      float track_bottom = current_track_y + ts->track_height;

      ImU32 track_bg_col = (i % 2 == 0) ? igGetColorU32_U32(igGetColorU32_Col(ImGuiCol_FrameBg, 1.0f), 0.9f)
                                        : igGetColorU32_U32(igGetColorU32_Col(ImGuiCol_WindowBg, 1.0f), 0.9f);
      ImDrawList_AddRectFilled(draw_list, (ImVec2){timeline_start_pos.x, track_top},
                               (ImVec2){timeline_end_pos.x, track_bottom}, track_bg_col, 4.0f,
                               ImDrawFlags_RoundCornersAll);
      ImDrawList_AddLine(draw_list, (ImVec2){timeline_start_pos.x, track_bottom},
                         (ImVec2){timeline_end_pos.x, track_bottom}, igGetColorU32_Col(ImGuiCol_Border, 0.3f),
                         1.0f);

      // Draw Snippets for this track
      for (int j = 0; j < track->snippet_count; ++j) {
        input_snippet *snippet = &track->snippets[j];

        float snippet_start_x = timeline_start_pos.x + (snippet->start_tick - ts->view_start_tick) * ts->zoom;
        float snippet_end_x = timeline_start_pos.x + (snippet->end_tick - ts->view_start_tick) * ts->zoom;

        if (snippet_end_x < timeline_start_pos.x || snippet_start_x > timeline_end_pos.x) {
          continue;
        }

        ImVec2 snippet_min = {snippet_start_x, track_top + 2.0f};
        ImVec2 snippet_max = {snippet_end_x, track_bottom - 2.0f};
        ImRect snippet_bb = {snippet_min, snippet_max};

        igPushID_Int(snippet->id);
        igSetCursorScreenPos(snippet_min);
        igInvisibleButton("snippet", (ImVec2){snippet_max.x - snippet_min.x, snippet_max.y - snippet_min.y},
                          ImGuiButtonFlags_MouseButtonLeft);

        if (igIsItemClicked(ImGuiMouseButton_Left)) {
          ts->selected_snippet_id = snippet->id;
          ts->selected_player_track_index = i;
        }

        // Drag source
        if (igBeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
          igSetDragDropPayload("SNIPPET", &j, sizeof(int), 0);
          ts->dragged_snippet_id = snippet->id;
          ts->dragged_player_track_index = i;
          ImDrawList_AddRectFilled(draw_list, snippet_min, snippet_max,
                                   igGetColorU32_Col(ImGuiCol_DragDropTarget, 0.5f), 4.0f,
                                   ImDrawFlags_RoundCornersAll);
          igEndDragDropSource();
        }

        // Drag handling with fixed size
        if (igIsItemActive() && igIsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
          ImVec2 drag_delta;
          igGetMouseDragDelta(&drag_delta, ImGuiMouseButton_Left, 0.0f);
          int tick_delta = (int)(drag_delta.x / ts->zoom);

          // Preserve snippet duration
          int duration = snippet->end_tick - snippet->start_tick;
          int new_start_tick = snippet->start_tick + tick_delta;

          // Update position, ensuring non-negative ticks
          snippet->start_tick = fmax(0, new_start_tick);
          snippet->end_tick = snippet->start_tick + duration;

          igResetMouseDragDelta(ImGuiMouseButton_Left);
        }

        bool is_snippet_hovered = igIsItemHovered(ImGuiHoveredFlags_RectOnly);
        if (is_snippet_hovered) {
          igSetTooltip("Snippet ID: %d", snippet->id);
        }

        igPopID();

        bool is_selected = (snippet->id == ts->selected_snippet_id);

        ImU32 snippet_col = is_selected
                                ? igGetColorU32_Col(ImGuiCol_HeaderActive, 1.0f)
                                : (is_snippet_hovered ? igGetColorU32_Col(ImGuiCol_ButtonHovered, 1.0f)
                                                      : igGetColorU32_Col(ImGuiCol_ButtonActive, 0.8f));
        ImU32 snippet_border_col = is_selected ? igGetColorU32_Col(ImGuiCol_NavWindowingHighlight, 1.0f)
                                               : igGetColorU32_Col(ImGuiCol_Border, 0.6f);
        float border_thickness = is_selected ? 2.0f : 1.0f;

        ImDrawList_AddRectFilled(draw_list, snippet_min, snippet_max, snippet_col, 4.0f,
                                 ImDrawFlags_RoundCornersAll);
        ImDrawList_AddRect(draw_list, snippet_min, snippet_max, snippet_border_col, 4.0f,
                           ImDrawFlags_RoundCornersAll, border_thickness);
      }

      current_track_y += ts->track_height;
    }

    // --- Draw Playhead ---
    float playhead_x = timeline_start_pos.x + (ts->current_tick - ts->view_start_tick) * ts->zoom;
    if (playhead_x >= timeline_start_pos.x && playhead_x <= timeline_end_pos.x) {
      ImDrawList_AddLine(draw_list, (ImVec2){playhead_x, window_pos.y + controls_height},
                         (ImVec2){playhead_x, timeline_end_pos.y},
                         igGetColorU32_Col(ImGuiCol_SeparatorActive, 1.0f), 2.0f);
    }

    igPopClipRect();
  } else {
    igPopStyleVar(2);
  }
  igEnd();
}

void ui_init(ui_handler *ui) {

  ui->show_timeline = true;

  // Initialize Players (Example: Add one player)
  timeline_state *ts = &ui->timeline;
  ts->dragged_snippet_id = -1;
  ts->dragged_player_track_index = -1;
  ts->player_track_count = 2;
  ts->player_tracks = malloc(ts->player_track_count * sizeof(player_track));
  ts->player_track_count = 0;
  // Add Player 0
  player_track *p0 = &ts->player_tracks[ts->player_track_count++];
  p0->player_id = 0;
  snprintf(p0->player_name, sizeof(p0->player_name), "Player %d", p0->player_id);
  p0->snippet_count = 0;
  p0->visible = true;

  // Example Snippet 1
  if (p0->snippet_count < MAX_SNIPPETS_PER_PLAYER) {
    input_snippet *s1 = &p0->snippets[p0->snippet_count++];
    s1->id = p0->player_id * 1000 + p0->snippet_count;
    s1->start_tick = 50;
    s1->end_tick = 150;
    s1->selected = false;
    // TODO:actually initialize the data here
  }
  // Example Snippet 2
  if (p0->snippet_count < MAX_SNIPPETS_PER_PLAYER) {
    input_snippet *s2 = &p0->snippets[p0->snippet_count++];
    s2->id = p0->player_id * 1000 + p0->snippet_count;
    s2->start_tick = 200;
    s2->end_tick = 220;
    s2->selected = false;
  }
  // Add Player 1
  player_track *p1 = &ts->player_tracks[ts->player_track_count++];
  p1->player_id = 1;
  snprintf(p1->player_name, sizeof(p1->player_name), "Player %d", p1->player_id);
  p1->snippet_count = 0;
  p1->visible = true;

  // Example Snippet 1 (Player 1)
  if (p1->snippet_count < MAX_SNIPPETS_PER_PLAYER) {
    input_snippet *s1 = &p1->snippets[p1->snippet_count++];
    s1->id = p1->player_id * 1000 + p1->snippet_count;
    s1->start_tick = 100;
    s1->end_tick = 250;
    s1->selected = false;
  }

  // Initialize Timeline State
  ts->current_tick = 0;
  ts->view_start_tick = 0;
  ts->zoom = 1.0f; // 1 pixel per tick initially
  ts->track_height = DEFAULT_TRACK_HEIGHT;
  ts->selected_player_track_index = -1; // Nothing selected initially
  ts->selected_snippet_id = -1;
}

void ui_render(ui_handler *ui) {
  setup_docking(ui);
  render_timeline(ui);
}

void ui_cleanup(ui_handler *ui) { free(ui->timeline.player_tracks); }
