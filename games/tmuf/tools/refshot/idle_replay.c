/* idle_replay MAP.Challenge.Gbx OUT.Replay.Gbx [SECONDS]
   A replay of the car standing on the start for SECONDS (default 20): what
   refshot.py has the game show while it holds its camera poses. */
#include <tmuf_physics/tmuf_physics.h>

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: idle_replay MAP OUT [SECONDS]\n");
    return 2;
  }
  const char *packs_dir = getenv("TMUF_PACKS");
  char err[512];
  tmuf_packs *packs = tmuf_packs_open(packs_dir ? packs_dir : "data/games/tmuf/Packs", err, sizeof err);
  if (!packs) {
    fprintf(stderr, "packs: %s\n", err);
    return 1;
  }
  FILE *f = fopen(argv[1], "rb");
  if (!f) {
    fprintf(stderr, "cannot read %s\n", argv[1]);
    return 1;
  }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  void *map = malloc((size_t)n);
  if (!map || fread(map, 1, (size_t)n, f) != (size_t)n)
    return 1;
  fclose(f);
  tmuf_track *track = tmuf_track_load(packs, map, (size_t)n, NULL, err, sizeof err);
  if (!track) {
    fprintf(stderr, "track: %s\n", err);
    return 1;
  }
  const uint32_t count = (uint32_t)((argc > 3 ? atof(argv[3]) : 20.0) * 1000.0 / TMUF_TICK_MS) +
                         TMUF_RACE_START_MS / TMUF_TICK_MS;
  tmuf_input *inputs = calloc(count, sizeof *inputs);
  size_t size = 0;
  void *gbx = tmuf_replay_write(track, inputs, count, NULL, &size, err, sizeof err);
  if (!gbx) {
    fprintf(stderr, "replay: %s\n", err);
    return 1;
  }
  f = fopen(argv[2], "wb");
  if (!f || fwrite(gbx, 1, size, f) != size)
    return 1;
  fclose(f);
  tmuf_free(gbx);
  free(inputs);
  tmuf_track_free(track);
  tmuf_packs_close(packs);
  return 0;
}
