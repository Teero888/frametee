#include "physics.h"

void physics_init(ph_t *h, const char *path) {
  physics_free(h);
  if (!init_collision(&h->collision, path))
    return;
  init_config(&h->config);
  wc_init(&h->world, &h->collision, &h->config);
}

void physics_tick(ph_t *h) {
  for (int i = 0; i < h->world.m_NumCharacters; ++i)
    cc_on_input(&h->world.m_pCharacters[i], &h->world.m_pCharacters[i].m_Input);
  wc_tick(&h->world);
}

void physics_free(ph_t *h) {
  wc_free(&h->world);
  free_collision(&h->collision);
}
