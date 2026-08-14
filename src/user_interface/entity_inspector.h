#ifndef ENTITY_INSPECTOR_H
#define ENTITY_INSPECTOR_H

// A property inspector for whatever the active game exposes.
//
// It used to model DDNet's projectiles and lasers field by field, which meant
// the editor knew what a laser was. It now walks the entity classes and
// property descriptors the game publishes, so it shows a Trackmania car or a
// Mario the same way it shows a tee.

#include <engine/game_host.h>
#include <stdbool.h>
#include <stdint.h>

struct gfx_handler_t;

#define ENTITY_INSPECTOR_MAX_PROPS 64

typedef struct {
  char label[64];
  char group[32];
  char unit[16];
  ft_value value;
  char text[64]; // formatted for display, so rendering does no work
} entity_prop_view_t;

typedef struct {
  bool valid;
  bool show;

  // What is being inspected, in the game's own numbering.
  uint32_t entity_class;
  int32_t entity_index;
  char class_name[64];

  int timeline_tick;
  int world_tick;
  ft_vec2 position;
  // A 3D game reports its position as one vec3, so the viewport marker needs
  // the third component to sit in the right place.
  ft_vec3 position3;
  bool position_is_3d;

  entity_prop_view_t props[ENTITY_INSPECTOR_MAX_PROPS];
  int prop_count;
} entity_inspector_t;

void entity_inspector_clear(entity_inspector_t *inspector);
// Picks the entity nearest the cursor, across every class the game exposes.
bool entity_inspector_pick(entity_inspector_t *inspector, const ft_world *world, struct gfx_handler_t *gfx, float intra, float mouse_x,
                           float mouse_y);
void entity_inspector_render(entity_inspector_t *inspector);
void entity_inspector_render_highlight(const entity_inspector_t *inspector, struct gfx_handler_t *gfx);

#endif
