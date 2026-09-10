#include "plugin_api.h"
#include <sm64/sm64_game.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

FT_PLUGIN_ABI_EXPORT()
FT_API const char *plugin_game_id(void) { return "sm64"; }
FT_API void *plugin_init(tas_context_t *context, const tas_api_t *api) {
  return context->is_headless ? (void *)api : NULL;
}
FT_API void plugin_shutdown(void *data) { (void)data; }

FT_API int plugin_cli(void *data, int argc, const char **argv) {
  (void)argc; (void)argv;
  const tas_api_t *api = data;
  if (!api) return 1;
  ft_world *a = NULL, *b = NULL;
  void *inputs = NULL, *state = NULL;
  int result = 1;
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "simulation plugin: %s failed\n", #condition); goto cleanup; } } while (0)
  const ft_world *initial = api->get_initial_world();
  CHECK(initial);
  CHECK(api->entity_class_count() > 0);
  const ft_entity_class *player = api->entity_class(0);
  CHECK(player && player->prop_count >= 4);
  a = api->clone_world(initial);
  b = api->get_world_state_at(0);
  CHECK(a && b && api->world_player_count(a) == 1);
  CHECK(api->entity_count(a, 0) == 1);
  inputs = calloc(1, api->input_record_size());
  CHECK(inputs);
  api->input_default(inputs);
  const int stick_y = api->input_field_index("stick_y");
  CHECK(stick_y >= 0);
  api->input_set(inputs, stick_y, 80);
  ft_value pos;
  CHECK(api->entity_prop_get(a, 0, 0, 0, &pos) && pos.kind == FT_VALUE_VEC3);
  pos.as.v3.y += 100.f;
  CHECK(api->entity_prop_set(a, 0, 0, 0, &pos));
  api->copy_world(b, a);
  CHECK(!api->step_world(a, inputs, 2));
  for (int tick = 0; tick < 40; ++tick) {
    CHECK(api->step_world(a, inputs, 1));
    CHECK(api->step_world(b, inputs, 1));
    ft_value x, y;
    CHECK(api->entity_prop_get(a, 0, 0, 0, &x));
    CHECK(api->entity_prop_get(b, 0, 0, 0, &y));
    CHECK(memcmp(&x.as.v3, &y.as.v3, sizeof(x.as.v3)) == 0);
  }
  CHECK(api->world_tick(a) == 40 && api->world_tick(initial) == 0);
  ft_player_view view = {.struct_size = sizeof(view)};
  CHECK(api->world_player_view(a, 0, &view));
  const size_t size = api->serialize_world(a, NULL, 0);
  state = malloc(size);
  CHECK(size && state && api->serialize_world(a, state, size) == size);
  CHECK(api->deserialize_world(b, state, size));
  CHECK(api->step_world(a, inputs, 1) && api->step_world(b, inputs, 1));
  ft_value x, y;
  CHECK(api->entity_prop_get(a, 0, 0, 0, &x) && api->entity_prop_get(b, 0, 0, 0, &y));
  CHECK(memcmp(&x.as.v3, &y.as.v3, sizeof(x.as.v3)) == 0);
  const sm64_view *raw_view_a = sm64_world_view(a);
  const sm64_view *raw_view_b = sm64_world_view(b);
  CHECK(raw_view_a && raw_view_b);
  CHECK(raw_view_a->valid && raw_view_b->valid);
  CHECK(memcmp(raw_view_a->pos, &x.as.v3, sizeof(x.as.v3)) == 0);
  CHECK(memcmp(raw_view_b->pos, &y.as.v3, sizeof(y.as.v3)) == 0);
  const sm64_input *raw_in = sm64_input_record(inputs);
  CHECK(raw_in && raw_in->stick_y == 80);
  puts("Graphics-free plugin simulation: clone, step, reflection, overrides, copy, and save/load passed");
  result = 0;
cleanup:
  if (a) api->destroy_world(a);
  if (b) api->destroy_world(b);
  free(inputs); free(state);
  return result;
#undef CHECK
}
