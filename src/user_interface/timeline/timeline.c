#include "timeline.h"
#include "../user_interface.h"
#include "cimgui.h"
#include "timeline_interaction.h"
#include "timeline_model.h"
#include "timeline_renderer.h"
#include <string.h>

// Public API Implementation

void timeline_init(ui_handler_t *ui) {
  ui->timeline = (timeline_state_t){};
  model_init(&ui->timeline, ui);
}

void timeline_cleanup(timeline_state_t *ts) { model_cleanup(ts); }

void render_timeline(ui_handler_t *ui) {
  timeline_state_t *ts = &ui->timeline;

  interaction_handle_playback_and_shortcuts(ts);

  igSetNextWindowClass(&((ImGuiWindowClass){.DockingAllowUnclassed = false}));
  igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2){8, 8});

  if (igBegin("Timeline", NULL, ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar)) {
    igPopStyleVar(1);
    ImDrawList *draw_list = igGetWindowDrawList();
    ImDrawList *overlay_draw_list = igGetForegroundDrawList_WindowPtr(igGetCurrentWindow());

    // Render top controls
    renderer_draw_controls(ts);
    igSeparator();

    // Calculate layout for header and tracks area
    float header_height = igGetTextLineHeightWithSpacing() * 2.0f;
    float track_header_width = 120.0f;
    ImVec2 content_start_pos;
    igGetCursorScreenPos(&content_start_pos);
    ImVec2 available_space;
    igGetContentRegionAvail(&available_space);

    // Bounding box for the tick marks and labels
    ImRect header_bb = {{content_start_pos.x + track_header_width, content_start_pos.y},
                        {content_start_pos.x + available_space.x, content_start_pos.y + header_height}};
    // Bounding box for the main snippet area (RHS)
    ImRect timeline_bb = {{header_bb.Min.x, header_bb.Max.y}, {header_bb.Max.x, content_start_pos.y + available_space.y}};

    // Handle header interaction and render it
    interaction_handle_header(ts, header_bb);
    renderer_draw_header_and_playhead(ts, draw_list, header_bb, content_start_pos.y + available_space.y);
    igDummy((ImVec2){0, header_height}); // Advance cursor

    // Create a child window for the vertically scrollable track area
    ImVec2 tracks_area_pos;
    igGetCursorScreenPos(&tracks_area_pos);
    igBeginChild_Str("TracksArea", (ImVec2){available_space.x, timeline_bb.Max.y - timeline_bb.Min.y}, false, ImGuiWindowFlags_NoScrollWithMouse);

    float tracks_scroll_y = igGetScrollY();

    // Render the track headers and snippets inside the child window
    renderer_draw_tracks_area(ts, timeline_bb);

    // Handle mouse interactions for the main timeline area (panning, selection, drag-drop)
    interaction_handle_timeline_area(ts, timeline_bb, tracks_scroll_y);

    // Handle context menu
    if (igIsMouseReleased_Nil(ImGuiMouseButton_Right) && igIsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !igIsAnyItemHovered()) {
      igOpenPopup_Str("TimelineContextMenu", 0);
    }
    interaction_handle_context_menu(ts);

    igEndChild();

    // Render overlays (drag preview, selection box)
    renderer_draw_selection_box(ts, overlay_draw_list);
    renderer_draw_drag_preview(ts, overlay_draw_list, timeline_bb, tracks_scroll_y);

  } else {
    igPopStyleVar(1);
  }
  igEnd();
}

// Other Public Functions

void timeline_switch_recording_target(timeline_state_t *ts, int new_track_index) {
  // This logic is primarily interaction-based
  interaction_switch_recording_target(ts, new_track_index);
}
