#include "entity_inspector.h"

#include <math.h>
#include <renderer/graphics_backend.h>
#include <stdio.h>
#include <string.h>
#include <system/include_cimgui.h>

void entity_inspector_clear(entity_inspector_t *inspector) {
  const bool show = inspector->show;
  memset(inspector, 0, sizeof(*inspector));
  inspector->show = show;
  inspector->entity_index = -1;
}

static void format_value(const ft_value *value, const char *unit, char *out, size_t out_size) {
  const char *suffix = (unit && *unit) ? unit : "";
  switch (value->kind) {
  case FT_VALUE_BOOL: snprintf(out, out_size, "%s", value->as.b ? "yes" : "no"); break;
  case FT_VALUE_INT: snprintf(out, out_size, "%lld%s%s", (long long)value->as.i, *suffix ? " " : "", suffix); break;
  case FT_VALUE_FLOAT: snprintf(out, out_size, "%.4f%s%s", value->as.f, *suffix ? " " : "", suffix); break;
  case FT_VALUE_VEC2: snprintf(out, out_size, "%.4f, %.4f%s%s", value->as.v.x, value->as.v.y, *suffix ? " " : "", suffix); break;
  case FT_VALUE_STRING: snprintf(out, out_size, "%s", value->as.s ? value->as.s : ""); break;
  default: snprintf(out, out_size, "?"); break;
  }
}

// Copies every property the game reports for one entity into the inspector, so
// the panel can be drawn later without holding on to the world.
static void snapshot_entity(entity_inspector_t *inspector, game_host_t *host, const ft_world *world, uint32_t entity_class, int32_t index) {
  const ft_entity_class *desc = gh_entity_class(host, entity_class);
  if (!desc) return;

  inspector->valid = true;
  inspector->entity_class = entity_class;
  inspector->entity_index = index;
  inspector->world_tick = gh_world_tick(host, world);
  snprintf(inspector->class_name, sizeof(inspector->class_name), "%s", desc->display_name ? desc->display_name : desc->id);

  inspector->prop_count = 0;
  for (uint32_t i = 0; i < desc->prop_count && inspector->prop_count < ENTITY_INSPECTOR_MAX_PROPS; ++i) {
    ft_value value;
    if (!gh_entity_prop_get(host, world, entity_class, index, i, &value)) continue;

    // Remember where it is, so the viewport can ring the selection.
    if (value.kind == FT_VALUE_VEC2 && desc->props[i].id && strcmp(desc->props[i].id, "position") == 0) inspector->position = value.as.v;

    entity_prop_view_t *view = &inspector->props[inspector->prop_count++];
    snprintf(view->label, sizeof(view->label), "%s", desc->props[i].display_name ? desc->props[i].display_name : desc->props[i].id);
    snprintf(view->group, sizeof(view->group), "%s", desc->props[i].group ? desc->props[i].group : "");
    snprintf(view->unit, sizeof(view->unit), "%s", desc->props[i].unit ? desc->props[i].unit : "");
    view->value = value;
    format_value(&value, view->unit, view->text, sizeof(view->text));
  }
}

bool entity_inspector_pick(entity_inspector_t *inspector, const ft_world *world, struct gfx_handler_t *gfx, float intra, float mouse_x,
                           float mouse_y) {
  (void)intra;
  if (!world || !gfx) return false;
  game_host_t *host = &gfx->game_host;

  float world_x, world_y;
  screen_to_world(gfx, mouse_x, mouse_y, &world_x, &world_y);

  // Players have their own selection handling in the viewport, so the inspector
  // only claims the classes beyond them.
  const uint32_t class_count = gh_entity_class_count(host);
  float best_distance = 1.0f;
  uint32_t best_class = 0;
  int best_index = -1;

  for (uint32_t entity_class = 1; entity_class < class_count; ++entity_class) {
    const int count = gh_entity_count(host, world, entity_class);
    const ft_entity_class *desc = gh_entity_class(host, entity_class);
    if (!desc) continue;

    // Position is found by name: a class that does not expose one cannot be
    // picked in the viewport, which is a reasonable thing for a game to decide.
    int position_prop = -1;
    for (uint32_t i = 0; i < desc->prop_count; ++i) {
      if (desc->props[i].id && strcmp(desc->props[i].id, "position") == 0) {
        position_prop = (int)i;
        break;
      }
    }
    if (position_prop < 0) continue;

    for (int index = 0; index < count; ++index) {
      ft_value value;
      if (!gh_entity_prop_get(host, world, entity_class, index, (uint32_t)position_prop, &value)) continue;
      if (value.kind != FT_VALUE_VEC2) continue;

      const float dx = value.as.v.x - world_x;
      const float dy = value.as.v.y - world_y;
      const float distance = sqrtf(dx * dx + dy * dy);
      if (distance < best_distance) {
        best_distance = distance;
        best_class = entity_class;
        best_index = index;
      }
    }
  }

  if (best_index < 0) return false;

  entity_inspector_clear(inspector);
  snapshot_entity(inspector, host, world, best_class, best_index);
  inspector->show = true;
  return true;
}

void entity_inspector_render(entity_inspector_t *inspector) {
  if (!inspector->show) return;

  igSetNextWindowSize((ImVec2){360, 420}, ImGuiCond_FirstUseEver);
  if (igBegin("Entity Inspector", &inspector->show, 0)) {
    if (!inspector->valid) {
      igTextDisabled("Click an entity in the viewport to inspect it.");
      igEnd();
      return;
    }

    igText("%s #%d", inspector->class_name, inspector->entity_index);
    igTextDisabled("tick %d", inspector->world_tick);
    igSeparator();

    // Properties are grouped by whatever heading the game gave them.
    const char *current_group = NULL;
    for (int i = 0; i < inspector->prop_count; ++i) {
      const entity_prop_view_t *prop = &inspector->props[i];
      if (prop->group[0] && (!current_group || strcmp(current_group, prop->group) != 0)) {
        current_group = prop->group;
        igSeparatorText(prop->group);
      }
      igText("%s", prop->label);
      igSameLine(0, 8.f);
      igTextDisabled("%s", prop->text);
    }
  }
  igEnd();
}

void entity_inspector_render_highlight(const entity_inspector_t *inspector, struct gfx_handler_t *gfx) {
  if (!inspector->valid || !inspector->show || !gfx) return;
  renderer_submit_circle_filled(gfx, 9.5f, (vec2){inspector->position.x, inspector->position.y}, 0.35f, (vec4){1.f, 0.9f, 0.2f, 0.5f}, 12);
}
