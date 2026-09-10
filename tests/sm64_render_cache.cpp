#include <frametee/game_abi.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

// Exercise the real module's render callback with a graphics-free host. The
// software renderer still produces real pixels; only the upload/draw endpoints
// are mocked so redundant rendering is observable without timing assertions.
struct ft_texture { uint32_t width, height; std::vector<uint8_t> pixels; };
struct ft_pipeline {};
struct ft_mesh {};
static ft_camera camera{};
static unsigned uploads = 0, draws = 0;
static std::vector<uint8_t> last_image;
static void require(bool condition, const char *message) {
  if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
static size_t resolve_path(const char *relative, char *out, size_t capacity) {
  return std::snprintf(out, capacity, "/tmp/%s", relative);
}
static bool read_file(const char *, void **out, size_t *size) {
  static uint32_t dummy = 0;
  *out = &dummy; *size = sizeof(dummy); return true;
}
static void log_message(ft_log_level, const char *, const char *message) { std::fprintf(stderr, "%s\n", message); }

static const ft_world *other_group = nullptr;
int main(int argc, char **argv) {
  require(argc == 3, "usage: sm64_render_cache_test module.so setup.sm64");
  // Unused splash/UI symbols are resolved lazily; no ImGui functions are called.
  void *library = dlopen(argv[1], RTLD_LAZY | RTLD_LOCAL);
  if (!library) { std::fprintf(stderr, "%s\n", dlerror()); return 1; }
  auto entry = reinterpret_cast<const ft_game_module *(*)(uint32_t)>(dlsym(library, FT_GAME_MODULE_ENTRY_NAME));
  require(entry, "module entry");
  const auto *module = entry(FT_GAME_ABI_VERSION);
  require(module, "module ABI");
  ft_engine_api engine{};
  engine.struct_size = sizeof(engine);
  engine.log = log_message;
  engine.resolve_data_path = engine.resolve_cache_path = resolve_path;
  engine.read_file = read_file;
  engine.free_file_data = [](void *) {};
  engine.pipeline_create = [](const ft_pipeline_desc *) { return new ft_pipeline; };
  engine.pipeline_destroy = [](ft_pipeline *p) { delete p; };
  engine.mesh_create = [](const void *, uint32_t, uint32_t, const uint32_t *, uint32_t) { return new ft_mesh; };
  engine.mesh_destroy = [](ft_mesh *m) { delete m; };
  engine.texture_create = [](const ft_texture_desc *d) { return new ft_texture{d->width, d->height, {}}; };
  engine.texture_destroy = [](ft_texture *t) { delete t; };
  engine.texture_update_layer = [](ft_texture *t, uint32_t, const void *pixels, uint32_t w, uint32_t h) {
    require(w == t->width && h == t->height, "upload dimensions");
    const auto *bytes = static_cast<const uint8_t *>(pixels);
    t->pixels.assign(bytes, bytes + size_t(w)*h*4); ++uploads; return true;
  };
  engine.draw_mesh = [](ft_pipeline *, float, ft_mesh *, ft_texture *const *textures, uint32_t count, const void *, size_t) {
    require(count == 1 && !textures[0]->pixels.empty(), "draw has a rendered image");
    last_image = textures[0]->pixels; ++draws;
  };
  engine.camera_get = [](ft_camera *out) { *out = camera; };
  engine.timeline_world_count = []() -> uint32_t { return other_group ? 2 : 1; };
  engine.timeline_world_pair = [](uint32_t i, int32_t, const ft_world **previous, const ft_world **current) {
    *previous=*current=i==1 ? other_group : nullptr; return *current!=nullptr;
  };
  auto *game = module->create(&engine);
  require(game && module->resources_create(game), "render resources");
  auto *level = module->level_load_path(game, argv[2], nullptr);
  require(level, "load real SM64 setup");
  ft_world_desc desc{};
  desc.struct_size = sizeof(desc); desc.level = level; desc.player_count = 1;
  auto *world = module->world_create(game, &desc);
  auto *copy = module->world_create(game, &desc);
  require(world && copy, "worlds");
  camera.struct_size = sizeof(camera); camera.viewport = {96, 72}; camera.aspect = 4.f/3.f;
  ft_render_frame frame{};
  frame.struct_size = sizeof(frame); frame.pass = FT_PASS_ENTITIES; frame.active = true; frame.world = world;
  auto render = [&] { module->render(game, &frame); };
  render();
  for (int i = 0; i < 100; ++i) render();
  require(uploads == 1 && draws == 101, "unchanged frames reuse the rendered image");
  std::vector<uint8_t> input(module->input_schema->record_size);
  module->input_default(game, input.data());
  module->world_step(game, world, input.data(), 1);
  render(); require(uploads == 2, "stepping invalidates the image");
  module->world_copy(game, copy, world); frame.world = copy;
  render(); require(uploads == 2, "an identical copy reuses the image");
  const auto before_edit = last_image;
  ft_value position{};
  require(module->entity_prop_get(game, copy, 0, 0, 0, &position), "position property");
  position.as.v3.x += 200.f; position.as.v3.y += 200.f;
  require(module->entity_prop_set(game, copy, 0, 0, 0, &position), "state edit");
  render(); require(uploads == 3, "editing invalidates the image");
  require(last_image != before_edit, "position override is visible without stepping (requires current native edit hooks)");
  const size_t size = module->world_serialize(game, copy, nullptr, 0);
  std::vector<uint8_t> saved(size);
  require(size && module->world_serialize(game, copy, saved.data(), size) == size, "serialize");
  require(module->world_deserialize(game, copy, saved.data(), size), "deserialize");
  render(); require(uploads == 4, "loading invalidates the image");
  camera.viewport = {128, 96};
  render(); require(uploads == 5, "resizing invalidates the image");
  ft_camera_frame camera_frame{}; camera_frame.struct_size = sizeof(camera_frame); camera_frame.world = copy;
  if (module->camera_update(game, &camera_frame, &camera)) {
    camera.mode = 1;
    render(); require(uploads == 6, "switching cameras invalidates the image");
    render(); require(uploads == 6, "unchanged editor camera reuses the image");
    camera.view_proj[0] *= .9f;
    render(); require(uploads == 7, "changing projection invalidates the image");
    const float valid = camera.view_proj[0];
    camera.view_proj[0] = std::numeric_limits<float>::quiet_NaN();
    render(); require(uploads == 7, "a failed render does not upload garbage");
    camera.view_proj[0] = valid;
    render(); require(uploads == 7, "a failed render does not poison the cached image");
  }
  const unsigned previous_uploads = uploads;
  module->resources_destroy(game);
  require(module->resources_create(game), "resource recreation with live simulation");
  render(); require(uploads == previous_uploads+1, "recreated resources receive a fresh image");
  require(module->world_tick(game, copy) == 1, "rendering and resource recreation preserve simulation");
  camera.mode=0;
  frame.world=world;frame.world_index=0;
  other_group=nullptr;render();
  const auto single_mario=last_image;
  other_group=copy;render();
  require(last_image!=single_mario,"inactive group Mario is composited into active scene");
  const unsigned group_uploads=uploads;
  render();require(uploads==group_uploads,"unchanged groups reuse scene");
  module->world_step(game,copy,input.data(),1);
  render();require(uploads==group_uploads+1,"inactive group tick invalidates scene");
  other_group=nullptr;render();
  require(last_image==single_mario,"removing a group restores single Mario scene");
  module->world_destroy(game, copy); module->world_destroy(game, world);
  module->level_destroy(game, level); module->resources_destroy(game); module->destroy(game);
  dlclose(library);
  std::puts("SM64 real-module render reuse, invalidation, and resource recreation passed");
}
