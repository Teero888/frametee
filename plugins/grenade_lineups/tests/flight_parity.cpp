// Fires grenades in the physics and checks that gl::Sim::Fly explodes them on
// the same float position, from random positions and aims of a map.
#include "gl_fast.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <random>
#include <thread>
#include <vector>

namespace {

struct Explosion {
  bool m_Done = false;
  ddnet_vec2_t m_Pos{};
};

void OnSound(ddnet_vec2_t Pos, int Sound, int ClientId, void *pUser) {
  (void)ClientId;
  Explosion *p = (Explosion *)pUser;
  if (Sound == DDNET_SOUND_GRENADE_EXPLODE && !p->m_Done) {
    p->m_Done = true;
    p->m_Pos = Pos;
  }
}

#if defined(DDNET_PHYSICS_GAME_UNIQUE)
ddnet_character_t *Character(ddnet_world_t *w) { return ddnet_ev_world_character(w); }
ddnet_input_t *Input(ddnet_world_t *w) { return &w->player.input; }
void Changed(ddnet_world_t *w) { ddnet_ev_character_changed(w); }
bool Join(ddnet_world_t *w) { return ddnet_ev_player_spawn(w); }
const ddnet_gamemode_t MODE = DDNET_MODE_UNIQUE_RACE;
#else
ddnet_character_t *Character(ddnet_world_t *w) { return ddnet_ev_world_character(w, 0); }
ddnet_input_t *Input(ddnet_world_t *w) { return &w->players[0].input; }
void Changed(ddnet_world_t *w) { ddnet_character_changed(w, 0); }
bool Join(ddnet_world_t *w) { return ddnet_ev_player_join(w, 0); }
const ddnet_gamemode_t MODE = DDNET_MODE_DDRACE;
#endif

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s map [samples]\n", argv[0]);
    return 2;
  }
  const int Samples = argc > 2 ? atoi(argv[2]) : 3000;
  map_data_t Map = load_map(argv[1]);
  static ddnet_collision_t s_Col;
  if (!ddnet_collision_init(&s_Col, &Map)) {
    fprintf(stderr, "cannot load %s\n", argv[1]);
    return 2;
  }
  free_map_data(&Map);

  const ddnet_config_t Config = ddnet_config_default(MODE);
  static ddnet_world_t s_Base, s_World;
  if (!ddnet_ev_world_init(&s_Base, &s_Col, &Config) || !ddnet_ev_world_init(&s_World, &s_Col, &Config) || !Join(&s_Base))
    return 2;
  for (int i = 0; i < 5 && !Character(&s_Base); i++) ddnet_ev_world_tick(&s_Base);
  if (!Character(&s_Base)) {
    fprintf(stderr, "no tee\n");
    return 2;
  }

  gl::Sim Sim;
  if (!Sim.Init(&s_Base)) return 2;
  gl::Layout Layout;
  Layout.Build(&s_Col);

  std::mt19937_64 Rng(1234);
  int Checked = 0, Mismatches = 0, Skipped = 0, Random = 0, Teleported = 0, Tuned = 0;
  std::vector<ddnet_vec2_t> vPath;
  for (int s = 0; s < Samples; s++) {
    const uint64_t Pos = Rng() % Layout.Positions();
    int x, y;
    Layout.PositionXY(Pos, &x, &y);
    const ddnet_vec2_t Tee = {(float)x, (float)y};
    int tx = (int)(Rng() % 2001) - 1000, ty = (int)(Rng() % 2001) - 1000;
    if (!tx && !ty) ty = 1;

    ddnet_ev_world_copy(&s_World, &s_Base);
    ddnet_character_t *pChr = Character(&s_World);
    pChr->pos = pChr->core.pos = Tee;
    pChr->core.vel = {0.0f, 0.0f};
    pChr->prev_pos = Tee;
    pChr->core.weapons[DDNET_WEAPON_GRENADE].got = true;
    pChr->core.weapons[DDNET_WEAPON_GRENADE].ammo = -1;
    pChr->core.active_weapon = DDNET_WEAPON_GRENADE;
    pChr->reload_timer = 0;
    pChr->freeze_time = 0;
    Changed(&s_World);
    Explosion Ex;
    s_World.sound = OnSound;
    s_World.user_data = &Ex;
    ddnet_input_t *pIn = Input(&s_World);
    memset(pIn, 0, sizeof(*pIn));
    pIn->target_x = tx, pIn->target_y = ty;
    pIn->fire = 1;
    ddnet_ev_world_tick(&s_World);
    pIn->fire = 2; // released: one grenade
    for (int t = 0; t < 300 && !Ex.m_Done; t++) ddnet_ev_world_tick(&s_World);

    ddnet_vec2_t Land;
    vPath.clear();
    if (Sim.Fly(Tee, gl::Sim::Direction(tx, ty), &Land, &vPath) == gl::OUTCOME_RANDOM_TELE) {
      Random++;
      continue;
    }
    if (!Ex.m_Done) {
      Skipped++; // the tee could not fire (frozen, dead)
      continue;
    }
    Checked++;
    for (const ddnet_vec2_t &P : vPath) Teleported += P.x != P.x;
    Tuned += Sim.TuneZone(vPath[0].x, vPath[0].y) != 0 || Sim.TuneZone(Tee.x, Tee.y) != 0;
    if (memcmp(&Land, &Ex.m_Pos, sizeof(Land)) != 0) {
      if (Mismatches++ < 10)
        printf("mismatch: tee %d,%d aim %d,%d: physics %.9g,%.9g sim %.9g,%.9g\n", x, y, tx, ty, Ex.m_Pos.x, Ex.m_Pos.y,
               Land.x, Land.y);
    }
  }
  ddnet_ev_world_free(&s_World);
  ddnet_ev_world_free(&s_Base);

  // the table (fast landings, compressed) and its lookup against flying every shot again, on random tiles
  int QueryErrors = 0;
  {
    std::vector<uint32_t> vTiles;
    for (int i = 0; i < 40; i++) vTiles.push_back(Layout.m_vSlotTile[Rng() % Layout.m_vSlotTile.size()]);
    const gl::Layout Slice = Layout.Subset(vTiles);
    gl::Table Table;
    std::string Error;
    if (!Table.Start(Sim, Slice, 64, 4, &Error)) {
      fprintf(stderr, "table: %s\n", Error.c_str());
      return 1;
    }
    while (!Table.Poll()) std::this_thread::yield();

    int x, y;
    ddnet_vec2_t Land;
    uint64_t Wrong = 0;
    for (uint32_t Slot = 0; Slot < Slice.m_vSlotTile.size(); Slot++)
      for (int a = 0; a < Table.NumAngles(); a++) {
        uint32_t aLand[gl::TILE_POSITIONS];
        Table.Decode(Slot, a, aLand);
        for (int u = 0; u < gl::TILE_POSITIONS; u++) {
          Slice.PositionXY((uint64_t)Slot * gl::TILE_POSITIONS + u, &x, &y);
          const uint32_t Want = Sim.Fly({(float)x, (float)y}, Table.Aims()[(size_t)a].m_Dir, &Land) == gl::OUTCOME_RANDOM_TELE
                                    ? gl::LAND_RANDOM
                                    : gl::LandIndex(Slice.m_W, Slice.m_H, Land);
          if (aLand[u] != Want && Wrong++ < 5)
            printf("table: tee %d,%d aim %d: %u, flying %u\n", x, y, a, aLand[u], Want);
        }
      }
    const uint64_t Entries = Slice.Positions() * (uint64_t)Table.NumAngles();
    printf("table: %llu landings, %llu wrong, %.3f bytes each\n", (unsigned long long)Entries, (unsigned long long)Wrong,
           (double)Table.Bytes() / (double)Entries);
    QueryErrors += Wrong != 0;

    Slice.PositionXY(Slice.Positions() / 2, &x, &y);
    Sim.Fly({(float)x, (float)y}, Table.Aims()[3].m_Dir, &Land);
    gl::Target Tgt;
    Tgt.m_X = (int)Land.x, Tgt.m_Y = (int)Land.y;
    gl::Query Query;
    Query.Start(&Table, Tgt, 3);
    while (!Query.Poll()) std::this_thread::yield();
    const gl::QueryResult R = Query.Take();

    std::vector<uint64_t> vPos;
    std::vector<uint16_t> vCount;
    for (uint64_t Pos = 0; Pos < Slice.Positions(); Pos++) {
      Slice.PositionXY(Pos, &x, &y);
      int Count = 0;
      for (const gl::Aim &A : Table.Aims()) {
        if (Sim.Fly({(float)x, (float)y}, A.m_Dir, &Land) != gl::OUTCOME_EXPLODED) continue;
        Count += Tgt.Hits(Slice, gl::LandIndex(Slice.m_W, Slice.m_H, Land));
      }
      if (Count) {
        vPos.push_back(Pos);
        vCount.push_back((uint16_t)Count);
      }
    }
    QueryErrors += vPos != R.m_vPos || vCount != R.m_vCount || R.m_vSlotBegin.back() != R.m_vPos.size();
    printf("lookup around %d,%d: %zu positions (%zu flying again)%s\n", Tgt.m_X, Tgt.m_Y, R.m_vPos.size(), vPos.size(),
           QueryErrors ? " MISMATCH" : "");
    if (vPos.empty()) QueryErrors = 1;
  }
  printf("%d checked, %d mismatches, %d skipped, %d random teleports, %d teleported, %d in tune zones\n", Checked,
         Mismatches, Skipped, Random, Teleported, Tuned);
  return Mismatches == 0 && !QueryErrors && Checked > Samples / 2 ? 0 : 1;
}
