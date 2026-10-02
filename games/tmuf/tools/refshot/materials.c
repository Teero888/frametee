/* materials MAP...: each material of the maps' scenery and car (as
   tmuf_track_visuals and tmuf_track_vehicle_visuals give them): its shader
   flags, render state and textures (sampler, uv set, generation, file), for
   matching the game's draw calls to them (trace_materials.py) */
#include <tmuf_physics/tmuf_physics.h>

#include <stdio.h>
#include <stdlib.h>

static void dump(const char *what, const tmuf_visuals *v) {
  for (uint32_t i = 0; i < v->material_count; i++) {
    const tmuf_visual_material *m = &v->materials[i];
    printf("%s %u %s flags %08x %08x rs %08x\n", what, i, m->name[0] ? m->name : "-", m->shader_flags[0],
           m->shader_flags[1], m->has_render_state ? m->render_state[0] : 0u);
    for (uint32_t k = 0; k < m->texture_count; k++) {
      const tmuf_visual_texture *t = &m->textures[k];
      printf("  %s uv %u gen %u %s%s\n", t->sampler && t->sampler[0] ? t->sampler : "-", t->texcoord, t->generate,
             t->file ? t->file : t->pack_file ? t->pack_file : "-", t->unbound ? " unbound" : "");
    }
  }
}

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
    if (!f) continue;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *map = malloc((size_t)n);
    if (fread(map, 1, (size_t)n, f) != (size_t)n) return 1;
    fclose(f);
    const tmuf_track_options options = {.flags = TMUF_TRACK_VISUALS};
    tmuf_track *t = tmuf_track_load(packs, map, (size_t)n, &options, err, sizeof err);
    if (!t) {
      fprintf(stderr, "%s: %s\n", argv[i], err);
      continue;
    }
    dump("scene", tmuf_track_visuals(t));
    const tmuf_vehicle_visuals *car = tmuf_track_vehicle_visuals(t);
    if (car) dump("car", &car->visuals);
    tmuf_track_free(t);
    free(map);
  }
  tmuf_packs_close(packs);
  return 0;
}
