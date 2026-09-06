#include <engine/game_host.h>
#include <engine/input_record.h>

#include <assert.h>
#include <stdlib.h>
#include <string.h>

struct ft_game { int index; };
typedef struct browser_state { int index; } browser_state;

static game_host_t host;
static int creates[2], destroys[2], resource_creates[2], browser_draws[2], browser_destroys[2];

static ft_game *create(const ft_engine_api *engine) {
  assert(engine == host.engine_api);
  ft_game *game = malloc(sizeof(*game));
  assert(game);
  game->index = host.active;
  assert(strcmp(game_host_calling_id(&host), host.slots[game->index].id) == 0);
  ++creates[game->index];
  return game;
}

static void destroy(ft_game *game) {
  ++destroys[game->index];
  free(game);
}

static bool resources_create(ft_game *game) {
  ++resource_creates[game->index];
  return true;
}

static void splash(const ft_engine_api *engine, void **context, const ft_ui_frame *frame) {
  assert(engine == host.engine_api && frame->slot == FT_UI_SPLASH);
  const int index = game_host_browsed_index(&host);
  assert(strcmp(game_host_calling_id(&host), host.slots[index].id) == 0);
  browser_state *browser = *context;
  if (!browser) {
    browser = malloc(sizeof(*browser));
    assert(browser);
    browser->index = index;
    *context = browser;
  }
  assert(browser->index == index);
  ++browser_draws[index];
}

static void splash_destroy(void *context) {
  browser_state *browser = context;
  assert(strcmp(game_host_calling_id(&host), host.slots[browser->index].id) == 0);
  ++browser_destroys[browser->index];
  free(browser);
}

int main(void) {
  const ft_engine_api engine = {0};
  const ft_game_variant variants[] = {{"normal", "Normal"}, {"alternate", "Alternate"}};
  const ft_input_field field = {.id = "cursor", .flags = FT_INPUT_FLAG_RECORDING_CURSOR};
  const ft_input_schema schema = {.record_size = 8, .fields = &field, .field_count = 1};
  const ft_game_module module = {.constraints = {.variants = variants, .variant_count = 2},
                                  .input_schema = &schema, .create = create, .destroy = destroy,
                                  .resources_create = resources_create, .splash = splash, .splash_destroy = splash_destroy};
  game_host_init(&host, &engine);
  host.slots = calloc(2, sizeof(*host.slots));
  assert(host.slots);
  host.count = host.capacity = 2;
  for (int i = 0; i < 2; ++i) {
    host.slots[i].usable = true;
    host.slots[i].module = &module;
    strcpy(host.slots[i].id, i ? "second" : "first");
  }
  const ft_ui_frame frame = {.struct_size = sizeof(frame), .slot = FT_UI_SPLASH};

  // First launch and game selection do not create or bind a runtime.
  assert(!game_host_ready(&host) && host.active == -1);
  assert(game_host_browse(&host, 0));
  assert(!game_host_ready(&host) && !creates[0] && engine_input_cursor_field() == -1);
  gh_browsed_ui(&host, &frame);
  assert(browser_draws[0] == 1 && !creates[0] && !resource_creates[0]);
  assert(game_host_browse(&host, 1));
  assert(!browser_destroys[0]); // UI selection does not free a context mid-callback.
  game_host_browser_sync(&host);
  assert(browser_destroys[0] == 1);
  gh_browsed_ui(&host, &frame);
  assert(!creates[1] && !game_host_ready(&host));
  game_host_browse_clear(&host);
  game_host_browser_sync(&host);
  assert(browser_destroys[1] == 1 && !creates[0] && !creates[1]);

  // An actual open creates the runtime. Reopening its splash never replaces it.
  assert(game_host_activate_index(&host, 0));
  assert(gh_resources_create(&host));
  ft_game *running = host.instance;
  assert(engine_input_cursor_field() == 0 && creates[0] == 1 && resource_creates[0] == 1);
  assert(game_host_browse(&host, 0));
  game_host_browsed_set_variant(&host, "alternate");
  gh_browsed_ui(&host, &frame);
  assert(strcmp(game_host_variant(&host), "normal") == 0);
  assert(strcmp(game_host_browsed_variant(&host), "alternate") == 0);
  assert(host.instance == running && creates[0] == 1 && !destroys[0]);

  // Browsing a different game and cancelling leaves runtime/input/ruleset intact.
  assert(game_host_browse(&host, 1));
  game_host_browser_sync(&host);
  gh_browsed_ui(&host, &frame);
  assert(host.instance == running && host.active == 0 && !creates[1]);
  assert(strcmp(game_host_calling_id(&host), "first") == 0);
  game_host_browse_clear(&host);
  game_host_browser_sync(&host);
  assert(host.instance == running && !destroys[0] && engine_input_cursor_field() == 0);

  // Only explicit activation of the selected game's level changes the runtime.
  assert(game_host_browse(&host, 1));
  game_host_browsed_set_variant(&host, "alternate");
  char chosen_variant[FT_ID_MAX];
  strcpy(chosen_variant, game_host_browsed_variant(&host));
  assert(game_host_activate_index(&host, game_host_browsed_index(&host)));
  game_host_set_variant(&host, chosen_variant);
  assert(creates[1] == 1 && destroys[0] == 1 && host.active == 1);
  assert(strcmp(game_host_variant(&host), "alternate") == 0);
  assert(game_host_activate_index(&host, 1) && creates[1] == 1);
  game_host_shutdown(&host);
  assert(destroys[1] == 1);
  return 0;
}
