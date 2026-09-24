#include "render_window.h"

#include "render_profile.h"
#include <GLFW/glfw3.h>
#include <engine/game_host.h>
#include <export/video_export.h>
#include <frametee/icons.h>
#include <math.h>
#include <nfd.h>
#include <renderer/graphics_backend.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <system/include_cimgui.h>
#include <user_interface/camera/camera_window.h>
#include <user_interface/user_interface.h>

// Preview

static void set_preview(ui_handler_t *ui, bool preview) {
  render_state_t *state = &ui->render;
  if (state->preview == preview && state->applied == (preview ? RENDER_TARGET_VIDEO : RENDER_TARGET_VIEWPORT)) return;
  state->preview = preview;
  render_apply(ui, preview ? RENDER_TARGET_VIDEO : RENDER_TARGET_VIEWPORT);
}

static float video_aspect(const ui_handler_t *ui) {
  const video_export_options_t *o = &ui->video_options;
  return o->width > 0 && o->height > 0 ? (float)o->width / (float)o->height : 16.f / 9.f;
}

void render_fit_viewport(ui_handler_t *ui, float *x, float *y, float *width, float *height) {
  if (!ui->render.preview || *width < 2.f || *height < 2.f) return;
  const float aspect = video_aspect(ui);
  if (*width / *height > aspect) {
    const float fitted = floorf(*height * aspect);
    *x += floorf((*width - fitted) * 0.5f);
    *width = fitted;
  } else {
    const float fitted = floorf(*width / aspect);
    *y += floorf((*height - fitted) * 0.5f);
    *height = fitted;
  }
}

// Layers

// The two value columns sit at the right of every row, under one caption each.
typedef struct layer_list_t {
  float left;          // screen x of the rows' left edge
  float width;         // the rows' width
  float column_width;  // each value column
  float row_height;
} layer_list_t;

static const ImVec4 VIEWPORT_ON = {0.24f, 0.52f, 0.86f, 1.f};
static const ImVec4 VIDEO_ON = {0.88f, 0.46f, 0.22f, 1.f};

static float column_x(const layer_list_t *list, render_target_t target) {
  const float gap = 6.f * gfx_get_ui_scale();
  return list->left + list->width - (target == RENDER_TARGET_VIEWPORT ? 2.f : 1.f) * (list->column_width + gap);
}

// A rounded switch showing its icon lit when on, crossed out and dim when off.
static bool toggle_pill(const char *id, bool on, const char *on_icon, const char *off_icon, ImVec4 color, float width) {
  const ImVec4 off = igGetStyle()->Colors[ImGuiCol_FrameBg];
  const ImVec4 hover = on ? (ImVec4){color.x * 1.15f, color.y * 1.15f, color.z * 1.15f, 1.f}
                          : igGetStyle()->Colors[ImGuiCol_FrameBgHovered];
  igPushStyleColor_Vec4(ImGuiCol_Button, on ? color : off);
  igPushStyleColor_Vec4(ImGuiCol_ButtonHovered, hover);
  igPushStyleColor_Vec4(ImGuiCol_ButtonActive, hover);
  igPushStyleColor_Vec4(ImGuiCol_Text, on ? (ImVec4){1.f, 1.f, 1.f, 1.f} : igGetStyle()->Colors[ImGuiCol_TextDisabled]);
  igPushStyleVar_Float(ImGuiStyleVar_FrameRounding, igGetFrameHeight() * 0.5f);
  char label[64];
  snprintf(label, sizeof(label), "%s%s", on ? on_icon : off_icon, id);
  const bool pressed = igButton(label, (ImVec2){width, 0.f});
  igPopStyleVar(1);
  igPopStyleColor(4);
  return pressed;
}

// Starts a row: a soft highlight under the pointer and the name at the left.
static void row_begin(const layer_list_t *list, const char *name, const char *tooltip, const ImVec4 *dot) {
  const float scale = gfx_get_ui_scale();
  const ImVec2 top = igGetCursorScreenPos();
  const ImVec2 bottom = {list->left + list->width, top.y + list->row_height};
  if (igIsMouseHoveringRect((ImVec2){list->left, top.y}, bottom, true) && igIsWindowHovered(ImGuiHoveredFlags_ChildWindows))
    ImDrawList_AddRectFilled(igGetWindowDrawList(), (ImVec2){list->left, top.y}, bottom,
                             igGetColorU32_Col(ImGuiCol_FrameBgHovered, 0.45f), 4.f * scale, 0);
  const float text_y = top.y + 0.5f * (list->row_height - igGetTextLineHeight());
  float x = list->left + 14.f * scale;
  if (dot) {
    ImDrawList_AddCircleFilled(igGetWindowDrawList(), (ImVec2){x + 4.f * scale, text_y + igGetTextLineHeight() * 0.5f},
                               4.f * scale, igGetColorU32_Vec4(*dot), 12);
    x += 14.f * scale;
  }
  ImDrawList_AddText_Vec2(igGetWindowDrawList(), (ImVec2){x, text_y}, igGetColorU32_Col(ImGuiCol_Text, 1.f), name, NULL);
  if (tooltip && igIsMouseHoveringRect((ImVec2){list->left, top.y}, (ImVec2){column_x(list, RENDER_TARGET_VIEWPORT), bottom.y}, true))
    igSetTooltip("%s", tooltip);
}

// Moves to one value column of the current row.
static void row_column(const layer_list_t *list, render_target_t target, float row_top) {
  igSetCursorScreenPos((ImVec2){column_x(list, target), row_top + 0.5f * (list->row_height - igGetFrameHeight())});
}

static void row_end(const layer_list_t *list, float row_top) {
  igSetCursorScreenPos((ImVec2){list->left, row_top + list->row_height});
}

static bool value_widget(const char *id, const ft_setting_desc *desc, ft_value *value, render_target_t target, float width) {
  if (value->kind == FT_VALUE_BOOL) {
    const bool video = target == RENDER_TARGET_VIDEO;
    if (!toggle_pill(id, value->as.b, video ? ICON_FA_VIDEO : ICON_FA_EYE, video ? ICON_FA_VIDEO_SLASH : ICON_FA_EYE_SLASH,
                     video ? VIDEO_ON : VIEWPORT_ON, width))
      return false;
    value->as.b = !value->as.b;
    return true;
  }
  igSetNextItemWidth(width);
  if (value->kind == FT_VALUE_INT) {
    int v = (int)value->as.i;
    const bool changed = igDragInt(id, &v, 0.5f, (int)desc->min_value, (int)desc->max_value, "%d", ImGuiSliderFlags_AlwaysClamp);
    value->as.i = v;
    return changed;
  }
  float v = (float)value->as.f;
  const bool changed = igDragFloat(id, &v, 0.01f, (float)desc->min_value, (float)desc->max_value, "%.2f",
                                   ImGuiSliderFlags_AlwaysClamp);
  value->as.f = v;
  return changed;
}

static void setting_row(ui_handler_t *ui, const layer_list_t *list, unsigned index, const ft_setting_desc *desc) {
  const float top = igGetCursorScreenPos().y;
  row_begin(list, desc->display_name ? desc->display_name : desc->id, desc->description, NULL);
  for (int target = RENDER_TARGET_VIEWPORT; target <= RENDER_TARGET_VIDEO; ++target) {
    ft_value value;
    if (!render_setting_get(ui, (render_target_t)target, index, &value)) continue;
    row_column(list, (render_target_t)target, top);
    char id[32];
    snprintf(id, sizeof(id), "##s%u_%d", index, target);
    if (value_widget(id, desc, &value, (render_target_t)target, list->column_width)) {
      render_setting_set(ui, (render_target_t)target, index, &value);
      if (target == RENDER_TARGET_VIDEO) ui_mark_unsaved(ui);
    }
  }
  row_end(list, top);
}

static bool section(const char *name) {
  // Space between sections, none above the first.
  if (igGetCursorPosY() > igGetStyle()->WindowPadding.y + 1.f) igSpacing();
  return igCollapsingHeader_TreeNodeFlags(name, ImGuiTreeNodeFlags_DefaultOpen);
}

static void game_layers(ui_handler_t *ui, const layer_list_t *list) {
  game_host_t *host = &ui->gfx_handler->game_host;
  const unsigned count = gh_setting_count(host);
  // The game's groups, once each, in the order it first mentions them.
  const char *groups[32];
  int group_count = 0;
  for (unsigned i = 0; i < count && group_count < 32; ++i) {
    const ft_setting_desc *desc = gh_setting_desc(host, i);
    if (!render_setting_is_layer(desc)) continue;
    const char *group = desc->group && *desc->group ? desc->group : "General";
    bool seen = false;
    for (int g = 0; g < group_count; ++g) seen = seen || strcmp(groups[g], group) == 0;
    if (!seen) groups[group_count++] = group;
  }
  const char *game = host->module && host->module->info.display_name ? host->module->info.display_name : "Game";
  for (int g = 0; g < group_count; ++g) {
    char label[160];
    snprintf(label, sizeof(label), "%s " ICON_FA_ANGLE_RIGHT " %s##game%d", game, groups[g], g);
    if (!section(label)) continue;
    for (unsigned i = 0; i < count; ++i) {
      const ft_setting_desc *desc = gh_setting_desc(host, i);
      const char *group = desc && desc->group && *desc->group ? desc->group : "General";
      if (render_setting_is_layer(desc) && strcmp(group, groups[g]) == 0) setting_row(ui, list, i, desc);
    }
  }
}

static void editor_layers(ui_handler_t *ui, const layer_list_t *list) {
  if (!section("Frametee overlays")) return;
  for (int layer = 0; layer < RENDER_LAYER_COUNT; ++layer) {
    const float top = igGetCursorScreenPos().y;
    row_begin(list, render_layer_name((render_layer_t)layer), NULL, NULL);
    for (int target = RENDER_TARGET_VIEWPORT; target <= RENDER_TARGET_VIDEO; ++target) {
      row_column(list, (render_target_t)target, top);
      const bool video = target == RENDER_TARGET_VIDEO;
      if (video && layer == RENDER_LAYER_CAMERA_PATH) {
        // Centred in the column, where a switch would be.
        const float dash = igCalcTextSize("-", NULL, false, 0.f).x;
        igSetCursorScreenPos((ImVec2){column_x(list, RENDER_TARGET_VIDEO) + 0.5f * (list->column_width - dash),
                                      top + 0.5f * (list->row_height - igGetTextLineHeight())});
        igTextDisabled("-");
        if (igIsItemHovered(0)) igSetTooltip("The camera's own path never appears in its video");
        continue;
      }
      const bool enabled = render_layer_get(ui, (render_target_t)target, (render_layer_t)layer);
      char id[32];
      snprintf(id, sizeof(id), "##l%d_%d", layer, target);
      if (toggle_pill(id, enabled, video ? ICON_FA_VIDEO : ICON_FA_EYE, video ? ICON_FA_VIDEO_SLASH : ICON_FA_EYE_SLASH,
                      video ? VIDEO_ON : VIEWPORT_ON, list->column_width)) {
        render_layer_set(ui, (render_target_t)target, (render_layer_t)layer, !enabled);
        if (video) ui_mark_unsaved(ui);
      }
    }
    row_end(list, top);
  }
}

// What the video is about: the Focus row, a picker as wide as both columns.
static void focus_row(ui_handler_t *ui, const layer_list_t *list) {
  timeline_state_t *ts = &ui->timeline;
  render_profile_t *video = render_video_profile(ui);
  const float top = igGetCursorScreenPos().y;
  row_begin(list, "Focus", "Whose chat, HUD and pickups the video shows. The viewport follows the group selected in the editor.",
            NULL);
  row_column(list, RENDER_TARGET_VIEWPORT, top);
  char preview[160];
  if (video->focus == RENDER_FOCUS_MERGED) snprintf(preview, sizeof(preview), "All groups (merged chat)");
  else if (video->focus >= 0 && video->focus < ts->group_count) snprintf(preview, sizeof(preview), "%s", ts->groups[video->focus]->name);
  else snprintf(preview, sizeof(preview), "Selected group");
  igSetNextItemWidth(column_x(list, RENDER_TARGET_VIDEO) + list->column_width - column_x(list, RENDER_TARGET_VIEWPORT));
  if (igBeginCombo("##focus", preview, 0)) {
    int chosen = video->focus;
    if (igSelectable_Bool("Selected group", video->focus == RENDER_FOCUS_SELECTED, 0, (ImVec2){0, 0})) chosen = RENDER_FOCUS_SELECTED;
    igSetItemTooltip("Follows the group selected in the editor");
    for (int g = 0; g < ts->group_count; ++g) {
      igPushID_Int(g);
      const float *color = ts->groups[g]->color;
      igTextColored((ImVec4){color[0], color[1], color[2], 1.f}, ICON_FA_CIRCLE);
      igSameLine(0.f, 6.f * gfx_get_ui_scale());
      if (igSelectable_Bool(ts->groups[g]->name, video->focus == g, 0, (ImVec2){0, 0})) chosen = g;
      igPopID();
    }
    if (igSelectable_Bool("All groups (merged chat)", video->focus == RENDER_FOCUS_MERGED, 0, (ImVec2){0, 0}))
      chosen = RENDER_FOCUS_MERGED;
    igSetItemTooltip("The selected group's HUD, with every group's chat together");
    if (chosen != video->focus) {
      video->focus = chosen;
      ui_mark_unsaved(ui);
    }
    igEndCombo();
  }
  row_end(list, top);
}

// An opacity in percent, the width of a column; greyed out while the group is hidden there.
static bool opacity_drag(const char *id, float *opacity, bool shown, float width) {
  if (!shown) igBeginDisabled(true);
  float percent = *opacity * 100.f;
  igSetNextItemWidth(width);
  const bool changed = igDragFloat(id, &percent, 0.5f, 0.f, 100.f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
  if (changed) *opacity = percent / 100.f;
  if (!shown) igEndDisabled();
  return changed;
}

static void group_layers(ui_handler_t *ui, const layer_list_t *list) {
  timeline_state_t *ts = &ui->timeline;
  if (!section("Groups")) return;
  focus_row(ui, list);
  const float scale = gfx_get_ui_scale();
  for (int g = 0; g < ts->group_count; ++g) {
    timeline_group_t *group = ts->groups[g];
    float top = igGetCursorScreenPos().y;
    const ImVec4 color = {group->color[0], group->color[1], group->color[2], 1.f};
    row_begin(list, group->name, NULL, &color);
    char id[32];
    snprintf(id, sizeof(id), "##gv%d", g);
    row_column(list, RENDER_TARGET_VIEWPORT, top);
    if (toggle_pill(id, group->visible, ICON_FA_EYE, ICON_FA_EYE_SLASH, VIEWPORT_ON, list->column_width))
      group->visible = !group->visible;
    snprintf(id, sizeof(id), "##gd%d", g);
    row_column(list, RENDER_TARGET_VIDEO, top);
    if (toggle_pill(id, group->video_visible, ICON_FA_VIDEO, ICON_FA_VIDEO_SLASH, VIDEO_ON, list->column_width)) {
      group->video_visible = !group->video_visible;
      ui_mark_unsaved(ui);
    }
    row_end(list, top);

    // Its opacity, a lighter row under it.
    top = igGetCursorScreenPos().y;
    const float text_y = top + 0.5f * (list->row_height - igGetTextLineHeight());
    ImDrawList_AddText_Vec2(igGetWindowDrawList(), (ImVec2){list->left + 42.f * scale, text_y},
                            igGetColorU32_Col(ImGuiCol_TextDisabled, 1.f), "Opacity", NULL);
    snprintf(id, sizeof(id), "##go%d", g);
    row_column(list, RENDER_TARGET_VIEWPORT, top);
    opacity_drag(id, &group->opacity, group->visible, list->column_width);
    snprintf(id, sizeof(id), "##gdo%d", g);
    row_column(list, RENDER_TARGET_VIDEO, top);
    if (opacity_drag(id, &group->video_opacity, group->video_visible, list->column_width)) ui_mark_unsaved(ui);
    row_end(list, top);
  }
}

static void render_layers(ui_handler_t *ui, float width, float height) {
  const float scale = gfx_get_ui_scale();
  igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2){10.f * scale, 8.f * scale});
  igBeginChild_Str("##render_layers", (ImVec2){width, height}, true, 0);
  igPopStyleVar(1);

  layer_list_t list;
  list.left = igGetCursorScreenPos().x;
  list.width = igGetContentRegionAvail().x;
  list.column_width = 64.f * scale;
  list.row_height = igGetFrameHeight() + 6.f * scale;
  game_layers(ui, &list);
  editor_layers(ui, &list);
  group_layers(ui, &list);
  igDummy((ImVec2){1.f, 4.f * scale});
  igEndChild();
}

// Output

typedef struct size_preset_t {
  const char *name;
  int width, height;
} size_preset_t;

static const size_preset_t SIZE_PRESETS[] = {
    {"720p (1280 x 720)", 1280, 720},    {"1080p (1920 x 1080)", 1920, 1080}, {"1440p (2560 x 1440)", 2560, 1440},
    {"4K (3840 x 2160)", 3840, 2160},    {"Vertical 1080 x 1920", 1080, 1920}, {"Square 1080 x 1080", 1080, 1080},
};

static void size_controls(ui_handler_t *ui, video_export_options_t *o) {
  const int preset_count = (int)(sizeof(SIZE_PRESETS) / sizeof(SIZE_PRESETS[0]));
  const char *preview = "Custom";
  for (int i = 0; i < preset_count; ++i)
    if (SIZE_PRESETS[i].width == o->width && SIZE_PRESETS[i].height == o->height) preview = SIZE_PRESETS[i].name;
  if (igBeginCombo("Size", preview, 0)) {
    for (int i = 0; i < preset_count; ++i)
      if (igSelectable_Bool(SIZE_PRESETS[i].name, preview == SIZE_PRESETS[i].name, 0, (ImVec2){0, 0})) {
        o->width = SIZE_PRESETS[i].width;
        o->height = SIZE_PRESETS[i].height;
        ui_mark_unsaved(ui);
      }
    igEndCombo();
  }
  int size[2] = {o->width, o->height};
  if (igDragInt2("Pixels", size, 2.f, 2, 16384, "%d", ImGuiSliderFlags_AlwaysClamp)) {
    // Encoders want even sizes.
    o->width = size[0] & ~1;
    o->height = size[1] & ~1;
    ui_mark_unsaved(ui);
  }
}

static void start_render(ui_handler_t *ui) {
  video_export_job_t *job = &ui->video_job;
  nfdu8char_t *path = NULL;
  nfdu8filteritem_t filter[] = {{"MP4 Video", "mp4"}};
  if (NFD_SaveDialogU8(&path, filter, 1, NULL, "render.mp4") != NFD_OKAY || !path) return;
  char output[1024];
  snprintf(output, sizeof(output), "%s", path);
  NFD_FreePathU8(path);
  const size_t length = strlen(output);
  if ((length < 4 || strcasecmp(output + length - 4, ".mp4") != 0) && length + 4 < sizeof(output)) strcat(output, ".mp4");
  render_video_profile(ui); // the snapshot carries the video's look
  if (!video_export_start(ui->gfx_handler, job, &ui->video_options, output) && !job->status[0])
    snprintf(job->status, sizeof(job->status), "Invalid settings or encoder unavailable");
}

// Labels beside the fields of one column, as wide as the widest of them.
static void push_field_width(const char *const *labels, int count) {
  float widest = 0.f;
  for (int i = 0; i < count; ++i) widest = fmaxf(widest, igCalcTextSize(labels[i], NULL, false, 0.f).x);
  igPushItemWidth(-(widest + igGetStyle()->ItemInnerSpacing.x + 2.f));
}

static void format_column(ui_handler_t *ui, video_export_options_t *o) {
  static const char *labels[] = {"Size", "Pixels", "Frame rate"};
  push_field_width(labels, 3);
  igSeparatorText("Format");
  size_controls(ui, o);
  int fps[2] = {o->fps_num, o->fps_den};
  if (igDragInt2("Frame rate", fps, 0.5f, 1, 240000, "%d", ImGuiSliderFlags_AlwaysClamp)) {
    o->fps_num = fps[0];
    o->fps_den = fps[1] > 0 ? fps[1] : 1;
    ui_mark_unsaved(ui);
  }
  if (igIsItemHovered(0)) igSetTooltip("Frames per second as a fraction, e.g. 60 / 1 or 60000 / 1001");
  igPopItemWidth();
}

static void encoding_column(ui_handler_t *ui, video_export_options_t *o) {
  static const char *labels[] = {"Codec", "Rate control", "Quality", "Bitrate", "Speed", "Color"};
  push_field_width(labels, 6);
  igSeparatorText("Encoding");
  static const char *codecs[] = {"H.264", "HEVC", "AV1"};
  if (igCombo_Str_arr("Codec", &o->codec, codecs, 3, -1)) ui_mark_unsaved(ui);
  static const char *modes[] = {"Constant quality", "Target bitrate"};
  if (igCombo_Str_arr("Rate control", &o->quality_mode, modes, 2, -1)) ui_mark_unsaved(ui);
  if (o->quality_mode == 0) {
    if (igDragInt("Quality", &o->quality, 0.2f, 0, 51, "CRF %d", ImGuiSliderFlags_AlwaysClamp)) ui_mark_unsaved(ui);
    if (igIsItemHovered(0)) igSetTooltip("Lower is better quality and larger files");
  } else if (igDragInt("Bitrate", &o->bitrate_kbps, 100.f, 100, 1000000, "%d kb/s", ImGuiSliderFlags_AlwaysClamp)) {
    ui_mark_unsaved(ui);
  }
  static const char *presets[] = {"Fast", "Medium", "Slow"};
  if (igCombo_Str_arr("Speed", &o->preset, presets, 3, -1)) ui_mark_unsaved(ui);
  if (igIsItemHovered(0)) igSetTooltip("Slower encoding makes smaller files at the same quality");
  static const char *depths[] = {"8-bit", "10-bit"};
  int depth = o->bit_depth == 10 ? 1 : 0;
  if (igCombo_Str_arr("Color", &depth, depths, 2, -1)) {
    o->bit_depth = depth ? 10 : 8;
    ui_mark_unsaved(ui);
  }
  igPopItemWidth();
}

static void output_column(ui_handler_t *ui, video_export_options_t *o) {
  video_export_job_t *job = &ui->video_job;
  igSeparatorText("Output");
  double start, end;
  camera_editor_export_range(ui, &start, &end);
  o->start_time = start;
  o->end_time = end;
  igText("%.2fs - %.2fs", start, end);
  if (igIsItemHovered(0))
    igSetTooltip("%s. Drag the brackets on the Camera tab's ruler to change it.",
                 ui->camera_timeline.range_set ? "Set in the Camera tab" : "The whole timeline");
  igSameLine(0.f, 8.f * gfx_get_ui_scale());
  igTextDisabled("%lld frames", (long long)video_export_frame_count(o));

  if (job->active) {
    igProgressBar(job->frame_count ? (float)job->frame_index / (float)job->frame_count : 0.f, (ImVec2){-1.f, 0.f}, NULL);
    if (!job->cancelled && igButton(ICON_FA_XMARK " Cancel", (ImVec2){-1.f, 0.f})) video_export_cancel(ui->gfx_handler, job);
    if (job->worker_process) {
      igTextDisabled("Keeps going if you close Frametee.");
      if (igIsItemHovered(0))
        igSetTooltip("Rendering runs in its own process from a snapshot of the project, so you can keep editing.\n"
                     "It writes %s when it is done.",
                     job->path);
    }
    return;
  }
  const bool ready = video_export_available() && ui->gfx_handler->level;
  if (!ready) igBeginDisabled(true);
  if (igButton(ICON_FA_VIDEO " Render MP4...", (ImVec2){-1.f, 0.f})) start_render(ui);
  if (!ready) igEndDisabled();
  if (!video_export_available()) igTextDisabled("Needs FFmpeg at build time.");
  else if (job->status[0]) igTextWrapped("%s", job->status);
}

static void output_panel(ui_handler_t *ui, float width, float height) {
  video_export_options_t *o = &ui->video_options;
  igBeginChild_Str("##render_output", (ImVec2){width, height}, true,
                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  // Three short columns side by side, so everything fits a low panel.
  if (igBeginTable("##output", 3, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX, (ImVec2){0.f, 0.f}, 0.f)) {
    igTableNextRow(0, 0.f);
    igTableSetColumnIndex(0);
    const bool busy = ui->video_job.active;
    if (busy) igBeginDisabled(true);
    format_column(ui, o);
    igTableSetColumnIndex(1);
    encoding_column(ui, o);
    if (busy) igEndDisabled();
    igTableSetColumnIndex(2);
    output_column(ui, o);
    igEndTable();
  }
  igEndChild();
}

// Window

void render_window_render(ui_handler_t *ui) {
  igSetNextWindowClass(&((ImGuiWindowClass){.DockingAllowUnclassed = false}));
  igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2){8, 8});
  const bool visible = igBegin("Render", NULL, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  igPopStyleVar(1);
  // The viewport previews the video while this tab is the one in use.
  set_preview(ui, visible && ui->gfx_handler->level && !ui->timeline.recording);
  if (!visible || !ui->gfx_handler->level) {
    if (visible) igTextDisabled("Open a level to render it.");
    igEnd();
    return;
  }
  const float scale = gfx_get_ui_scale();
  camera_editor_transport(ui);
  igSpacing();

  const ImVec2 avail = igGetContentRegionAvail();
  // The layer list only needs room for a name and two switches; the output
  // settings get the rest.
  const float layers_width = fminf(480.f * scale, fmaxf(380.f * scale, avail.x * 0.35f + 100.f * scale));
  // The output settings mirror the list at the right edge, with the space
  // between them left open. Three columns want a little more room than the list.
  const float gap = 8.f * scale;
  const float output_width = fminf(avail.x - layers_width - gap, fmaxf(layers_width, 950.f * scale));
  const float left = igGetCursorPosX();
  render_layers(ui, layers_width, avail.y);
  igSameLine(left + avail.x - output_width, 0.f);
  output_panel(ui, output_width, avail.y);
  igEnd();
}

// Quitting

bool render_allow_quit(ui_handler_t *ui) {
  const video_export_job_t *job = &ui->video_job;
  if (!job->active || !job->worker_process || ui->quit_confirmed) return true;
  ui->show_quit_render_prompt = true;
  return false;
}

void render_quit_prompt(ui_handler_t *ui) {
  video_export_job_t *job = &ui->video_job;
  if (ui->show_quit_render_prompt) {
    igOpenPopup_Str("A video is still rendering", 0);
    ui->show_quit_render_prompt = false;
  }
  if (!igBeginPopupModal("A video is still rendering", NULL, ImGuiWindowFlags_AlwaysAutoResize)) return;
  igText("%lld of %lld frames are done.", (long long)job->frame_index, (long long)job->frame_count);
  igTextWrapped("The render runs in its own process and keeps going after Frametee closes. It writes:");
  igTextDisabled("%s", job->path);
  igSpacing();
  if (igButton("Quit, keep rendering", (ImVec2){0, 0}) || !job->active) {
    ui->quit_confirmed = true;
    glfwSetWindowShouldClose(ui->gfx_handler->window, GLFW_TRUE);
    igCloseCurrentPopup();
  }
  igSameLine(0.f, 8.f);
  if (igButton("Stop the render and quit", (ImVec2){0, 0})) {
    video_export_cancel(ui->gfx_handler, job);
    ui->quit_confirmed = true;
    glfwSetWindowShouldClose(ui->gfx_handler->window, GLFW_TRUE);
    igCloseCurrentPopup();
  }
  igSameLine(0.f, 8.f);
  if (igButton("Keep Frametee open", (ImVec2){0, 0})) igCloseCurrentPopup();
  igEndPopup();
}
