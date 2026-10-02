/* spawn MAP...: each map's start (the car at time 0): position and the
   direction it faces, for writing refcompare views */
#include <tmuf_physics/tmuf_physics.h>

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  const char *dir = getenv("TMUF_PACKS");
  char err[512];
  tmuf_packs *packs = tmuf_packs_open(dir ? dir : "data/games/tmuf/Packs", err, sizeof err);
  if (!packs) {
    fprintf(stderr, "packs: %s\n", err);
    return 1;
  }
  for (int i = 1; i < argc; i++) {
    FILE *f = fopen(argv[i], "rb");
    if (!f)
      continue;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *map = malloc((size_t)n);
    if (fread(map, 1, (size_t)n, f) != (size_t)n)
      return 1;
    fclose(f);
    tmuf_track *t = tmuf_track_load(packs, map, (size_t)n, NULL, err, sizeof err);
    if (!t) {
      printf("%s: %s\n", argv[i], err);
      continue;
    }
    tmuf_world w = tmuf_world_empty();
    tmuf_world_init(&w, t);
    const tmuf_dyna_state *s = &w.sim.body.state;
    printf("%s  pos %.3f %.3f %.3f  forward %.3f %.3f %.3f\n", argv[i], s->pos.x, s->pos.y, s->pos.z,
           s->rot.m[0][2], s->rot.m[1][2], s->rot.m[2][2]);
    tmuf_world_free(&w);
    tmuf_track_free(t);
    free(map);
  }
  tmuf_packs_close(packs);
  return 0;
}
