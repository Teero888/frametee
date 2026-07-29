#include "physics.h"
#include <string.h>

void physics_init_from_memory(physics_handler_t *h, const unsigned char *map_buffer, size_t size) {
  physics_free(h);
  map_data_t map = load_map_from_memory((unsigned char *)map_buffer, size);
  if (!init_collision(&h->collision, &map)) {
    // if init fails, free_map_data will free the layer data and the map_buffer.
    free_map_data(&map);
    return;
  }
  init_config(&h->config);
  h->grid = tg_empty();
  tg_init(&h->grid, h->collision.m_MapData.width, h->collision.m_MapData.height);
  wc_init(&h->world, &h->collision, &h->grid, &h->config);
  h->loaded = true;
}

void physics_init(physics_handler_t *h, const char *path) {
  physics_free(h);
  map_data_t map = load_map(path);
  if (!init_collision(&h->collision, &map)) {
    free_map_data(&map);
    return;
  }
  init_config(&h->config);
  h->grid = tg_empty();
  tg_init(&h->grid, h->collision.m_MapData.width, h->collision.m_MapData.height);
  wc_init(&h->world, &h->collision, &h->grid, &h->config);
  h->loaded = true;
}

void physics_enable_fastcap(physics_handler_t *h) {
  if (!h->loaded) return;

  h->config.m_SvFastcap = 1;
  h->config.m_SvHealthAndAmmo = 1;
  h->config.m_SvDestroyBulletsOnDeath = 1;
  h->config.m_SvSoloServer = 1;
  h->world.m_UniqueRace = true;

  for (int zone = 0; zone < NUM_TUNE_ZONES; ++zone) {
    h->collision.m_aTuningList[zone].m_PlayerCollision = 0.0f;
    h->collision.m_aTuningList[zone].m_PlayerHooking = 0.0f;
  }
}

void physics_tick(physics_handler_t *h) {
  for (int i = 0; i < h->world.m_NumCharacters; ++i)
    cc_on_input(&h->world.m_pCharacters[i], &h->world.m_pCharacters[i].m_Input);
  wc_tick(&h->world);
}

void physics_free(physics_handler_t *h) {
  tg_destroy(&h->grid);
  wc_free(&h->world);
  free_collision(&h->collision);
  memset(h, 0, sizeof(physics_handler_t));
}

float physics_character_race_time(const SCharacterCore *character, float game_tick) {
  if (character->m_RaceTime >= 0.0f) return character->m_RaceTime;
  if (character->m_StartTick < 0) return -1.0f;

  const float time = (game_tick - (float)character->m_StartTime - character->m_StartTickOffset) / GAME_TICK_SPEED;
  return time > 0.0f ? time : 0.0f;
}

bool physics_pickup_on_cooldown(const SWorldCore *world, int character_id, int cooldown_key) {
  if (!world->m_pPickupCooldowns || character_id < 0 || character_id >= world->m_NumCharacters) return false;

  const SPickupCooldownList *cooldowns = &world->m_pPickupCooldowns[character_id];
  for (int i = 0; i < cooldowns->m_NumEntries; ++i) {
    if (cooldowns->m_pEntries[i].m_Key == cooldown_key && world->m_GameTick <= cooldowns->m_pEntries[i].m_EndTick) return true;
  }
  return false;
}
