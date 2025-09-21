#include "physics.h"
#include <string.h>

void physics_init(ph_t *h, const char *path) {
  physics_free(h);
  if (!init_collision(&h->collision, path))
    return;
  init_config(&h->config);

  h->grid = tg_empty();
  tg_init(&h->grid, h->collision.m_MapData.width, h->collision.m_MapData.height);

  wc_init(&h->world, &h->collision, &h->grid, &h->config);
  h->loaded = true;
}

void physics_tick(ph_t *h) {
  for (int i = 0; i < h->world.m_NumCharacters; ++i)
    cc_on_input(&h->world.m_pCharacters[i], &h->world.m_pCharacters[i].m_Input);
  wc_tick(&h->world);
}

void physics_free(ph_t *h) {
  tg_destroy(&h->grid);
  wc_free(&h->world);
  free_collision(&h->collision);
  memset(h, 0, sizeof(ph_t));
}
