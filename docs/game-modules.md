# The game layer

FrameTee is split into an **engine** and a **game**. The engine is a TAS editor:
it owns the timeline, snippets, undo/redo, projects, playback, plugins, the
window, the renderer and the camera. It does not know what is being TASed. A
**game module** supplies everything that is specific to one game: its physics,
its level format, how a world looks on screen, its own panels and exporters, and
the rules the editor has to respect.

The whole contract is one header, [`include/frametee/game_abi.h`](../include/frametee/game_abi.h).
A game module is a shared library (`.so` / `.dll` / `.dylib`) that exports a
single symbol. It links no engine code, shares no allocator, and needs no build
system of ours, so it can be written in any language that can produce a
C-callable shared library.

Three modules ship in [`games/`](../games), deliberately in three languages:

| Module | Language | What it shows |
| --- | --- | --- |
| `ddnet` | C | The real one: DDNet physics, four rulesets, up to 64 players |
| `example_platformer` | C++ / raylib | A complete fixed-player platformer using raylib math, colours and collision |
| `example_rust` | Rust / Bevy | A deterministic Bevy ECS world stepped through an ordered schedule |

```
$ frametee --list-games
FrameTee game modules (ABI 6)
  ddnet                    DDNet v1.0.0 by Teero
      players 0..64, 50 ticks/s, dynamic cast
      inputs: direction target jump fire hook weapon kill eyes emote sit tele_out
      controls: left right jump fire hook hammer gun shotgun grenade laser kill
  example-bouncer          Bevy Bouncer (Rust) v2.0.0 by FrameTee
      players 1..1, 50 ticks/s, fixed cast
      inputs: push_x push_y
      controls: left right up down
  example-platformer       Raylib Platformer (C++) v2.0.0 by FrameTee
      players 1..1, 60 ticks/s, fixed cast
      inputs: left right jump
      controls: left right jump
```

---

## Who owns what

| The engine owns | The game owns |
| --- | --- |
| Timeline, tracks, snippets, layers | Physics and the step function |
| Recording, playback, scrubbing, prediction | Level/map loading and its format |
| Undo/redo, project files | Everything drawn in the viewport |
| Window, Vulkan, batching, camera | Its own panels, settings and exporters |
| Plugin loading and scoping | What an input record contains |
| File dialogs, config, logging | What a world contains |

The engine never interprets a game's bytes. Input records and worlds are opaque
to it: it stores them, copies them, hands them back at the right tick, and edits
individual input fields only through the schema the game published.

---

## The module entry point

```c
FT_GAME_EXPORT const ft_game_module *ft_game_module_entry(uint32_t engine_abi_version);
```

Return `NULL` if you cannot support the engine's ABI version. Otherwise return a
pointer to a **static, immutable** `ft_game_module` that stays valid for as long
as the library is loaded.

The engine validates the module before it will talk to it: ABI version, a legal
id, a required [Semantic Versioning 2.0.0](https://semver.org/) game version,
the required entry points, a non-zero input record, a positive tick rate,
and that every advertised capability has the functions it implies. A module that
fails validation is kept in the list with the reason attached, so the game picker
can explain the problem instead of silently ignoring the file.

### Versioning

- `info.version` is the game's own required SemVer string, such as `2.1.0` or
  `2.1.0-beta.1`. It is written into `.tasp` files; this pre-release editor
  requires an exact match when reopening a project.
- `FT_GAME_ABI_VERSION` changes when an existing field changes meaning or type,
  or is removed. The engine refuses modules whose major version differs.
- This is a pre-release ABI with no binary-compatibility layer. Every struct
  carries its exact `struct_size`; bump `FT_GAME_ABI_VERSION` and rebuild the
  engine and every module whenever the contract changes.
- Optional function pointers may be `NULL`. That means "this game does not do
  that", and the engine hides the feature rather than failing.

---

## Constraints: how a game restricts the editor

`ft_game_constraints` is not advice. The engine treats it as a hard limit, clamps
its own UI to it, and refuses operations that would violate it — so a game never
has to defend against configurations it cannot represent.

```c
.constraints = {
    .caps = FT_CAP_WORLD_SERIALIZE | FT_CAP_RENDERS_LEVEL | FT_CAP_HEADLESS,
    .min_players = 1,
    .max_players = 1,      // == min_players, and no FT_CAP_DYNAMIC_PLAYERS
    .ticks_per_second = 60,
},
```

That block is how you say "this game is single player, always" — Trackmania, a
Mario run, any one-runner game. With it in place the editor:

- hides the "Add Player" control and the count field entirely,
- shows `Players: 1 (fixed by the game)`,
- draws the timeline ruler in 60 Hz seconds rather than 50 Hz.

Timeline groups are deliberately *not* constrainable. A group is another
instance of the same simulation, so any game that can create one world can
create several; only the cast inside a world is a game's business.

Other capability bits work the same way. No `FT_CAP_LINKED_INPUTS` means no
input mirroring tools. No `FT_CAP_EXPORTERS` means no export menu. No
`FT_CAP_TIMELINE_EVENTS` means no event lane.

---

## Input schema

A game defines its own input record — any layout it likes, including packed
bitfields — and describes it field by field:

```c
static const ft_input_field fields[] = {
    {.id = "direction", .display_name = "Direction", .kind = FT_INPUT_INT,
     .flags = FT_INPUT_FLAG_TIMELINE_LANE | FT_INPUT_FLAG_MIRROR_X,
     .min_value = -1, .max_value = 1},
    {.id = "jump", .display_name = "Jump", .kind = FT_INPUT_BOOL,
     .flags = FT_INPUT_FLAG_TIMELINE_LANE},
};

static const ft_input_control controls[] = {
    {.id = "left", .display_name = "Move Left", .category = "My Game",
     .default_binding = "A", .field = 0, .value = -1,
     .flags = FT_CONTROL_ADD},
    {.id = "right", .display_name = "Move Right", .category = "My Game",
     .default_binding = "D", .field = 0, .value = 1,
     .flags = FT_CONTROL_ADD},
    {.id = "jump", .display_name = "Jump", .category = "My Game",
     .default_binding = "Space", .field = 1, .value = 1},
};

static const ft_input_schema schema = {
    .struct_size = sizeof(ft_input_schema),
    .record_size = sizeof(MyInput),
    .record_align = _Alignof(MyInput),
    .fields = fields,
    .field_count = 2,
    .controls = controls,
    .control_count = 3,
};
```

The engine uses the declared size and alignment whenever it passes a packed
array to the game. Its own timeline slots are opaque padded storage. It reads
or writes fields only through `input_get` / `input_set`, the float accessors,
and the vec2 accessors. That indirection lets DDNet keep its bit-packed flags
word while the editor still draws a clean lane per button.

Recording controls belong to the game too. Each `ft_input_control` targets one
of the schema's fields and supplies its own label, category, default key and
value. Held controls are reset to the field default each frame; pressed
controls use `FT_CONTROL_PRESSED`; opposing axes combine with `FT_CONTROL_ADD`.
For mouse aim, mark a `FT_INPUT_VEC2` field with
`FT_INPUT_FLAG_RECORDING_CURSOR`. The editor never guesses semantics from ids
such as `direction`, `jump`, or `target`.

`id` strings end up in project files and scripts, so treat them as permanent.

---

## Levels and worlds

- `ft_level` is a loaded map. It owns whatever the game derives from it
  (collision, tile grids, tuning) and outlives every world built on it.
- `ft_world` is one complete simulation state. The engine treats it as a
  snapshot: it copies worlds constantly to cache, rewind and scrub, so
  `world_copy` is on the hot path and should reuse `dst`'s allocations.
- `world_step` advances exactly one tick, given one input record per player in
  player-index order.

Coordinates crossing the ABI are in the editor's world units, which is whatever
`level_info` reports bounds in. DDNet works internally in pixels and divides by
32 at the boundary, so the editor sees tiles.

`world_serialize` / `world_deserialize` store starting states in project files.
Version the blob yourself; the engine only records it next to the game id and
version. DDNet's implementation refuses to load a blob whose character struct
size differs from the running build, rather than reinterpreting stale bytes.

`level_serialize` embeds a level when it can be recreated through
`level_load_memory`; otherwise version 11 stores the original reloadable path.
`project_save` / `project_load` are an optional pair for metadata that belongs
to neither a level nor a world. Both bundled reference examples implement a
small versioned project blob, showing the size-query/write/read convention in
C++ and Rust. The engine length-delimits these blobs and never interprets them.

---

## Rendering

The engine owns the graphics API; the game owns what goes through it. A module
draws through `ft_engine_api`, which offers, in rising order of power:

1. **Primitives** — `draw_rect`, `draw_circle`, `draw_line`, `draw_triangle`,
   `draw_text`. Enough for a whole game, as the two examples show.
2. **Sprite atlases** — `texture_create` + `atlas_create`, then `draw_sprites`
   with an array of instances. The engine batches every draw sharing an atlas,
   which is how a game gets thousands of particles or tiles per frame without
   touching Vulkan.
3. **Custom pipelines** — `pipeline_create` takes your SPIR-V and an instance
   attribute layout, and `draw_instances` feeds it. This is the escape hatch for
   techniques the generic path cannot express, such as DDNet's tee-skin
   compositing. Attribute location 0 is the unit-quad corner the engine binds;
   your instance attributes start at location 1.

`render` is called once per pass (`FT_PASS_LEVEL_BACKGROUND`, `FT_PASS_ENTITIES`,
`FT_PASS_LEVEL_FOREGROUND`, `FT_PASS_OVERLAY`) per visible world. Each frame
carries the world at the current tick, the world before it, and an `alpha` so
rendering can interpolate between ticks instead of stuttering.

`z` orders draws across the whole frame, so a game can interleave its layers with
engine-drawn overlays.

## UI

`ui` is called for `FT_UI_SPLASH`, `FT_UI_MAIN_MENU`, `FT_UI_PANELS`,
`FT_UI_SETTINGS`, `FT_UI_PLAYER_ROW`, and `FT_UI_STATUS_BAR`. The engine hands
over its ImGui context through `engine->imgui_context()`; a module that wants
panels links its own ImGui/cimgui and adopts that context, exactly the way
plugins already do.

---

## Plugins: global or game-specific

Plugins are a separate extension point from games, and each one is either global
or bound to a single game.

Export the optional symbol to bind one:

```c
FT_API const char *plugin_game_id(void) { return "ddnet"; }
```

- **No symbol** → the plugin is global. It stays loaded across game switches.
  Use this for anything that only touches the timeline, the UI or the project.
- **A game id** → the host only loads the plugin while that game is active, and
  unloads it when the user switches away. Use this for anything that reads or
  writes a game's world or input records, since those bytes mean nothing under a
  different game.

The plugin manager shows the scope and the state: a plugin parked for another
game reads *Other game* rather than *Error*, and comes back on its own when its
game is active again. `tas_context_t.active_game_id` tells a global plugin which
game is running.

The public plugin context contains only the shared ImGui context, headless flag,
and active game id. Timeline access goes through `tas_api_t`; worlds are opaque,
input records use the active schema's exact byte size, and plugins never receive
engine UI, renderer, or timeline structures. Global plugins can enumerate
`input_field_count()` / `input_field()` and use the accessor matching each
field's declared kind; `log()` avoids depending on the engine's private logger.
`get_world_state_at` returns an owned copy which must be
passed to `destroy_world`; the initial world is borrowed.

---

## Writing a module

### Layout

```
games/
  my_game/
    CMakeLists.txt      # or Cargo.toml, or a Makefile, or nothing at all
    my_game.c
```

The build output goes in `<build>/games/`, next to `<build>/plugins/`. The engine
scans `games/` at startup, loads every shared library it finds, and keeps the
ones that pass validation. Assets belong in `data/games/<id>/`, which
`engine->resolve_data_path()` resolves for you.

### Build

A module's only required FrameTee build-time dependency is the ABI header; it
may link whatever game-side libraries or engines it needs:

```cmake
add_library(my_game MODULE my_game.c)
set_target_properties(my_game PROPERTIES PREFIX "" OUTPUT_NAME "my_game")
target_include_directories(my_game PRIVATE ${FRAMETEE_ABI_INCLUDE_DIR})
```

Out of tree, point the include at a copy of `include/`. Nothing else is needed —
no engine sources, no Vulkan, no ImGui unless you want panels.

Anything statically linked into a module has to be position independent, since
the module is a shared object. The DDNet module builds `ddnet_physics` with
`POSITION_INDEPENDENT_CODE` and `-ftls-model=initial-exec` for exactly that
reason.

### In C++ with raylib

[`games/example_platformer`](../games/example_platformer) is a complete
single-player platformer backed by raylib. Its world uses raylib vectors,
collision helpers, colour conversion and math. It deliberately does not call
`InitWindow`: FrameTee owns the Vulkan window, so the adapter translates
raylib-owned presentation data into `ft_engine_api` draw commands.

### In Rust with Bevy

[`games/example_rust`](../games/example_rust) mirrors the header by hand in
`src/abi.rs` (`#[repr(C)]` throughout, no bindgen) and implements the module in
`src/lib.rs`. Bevy ECS owns its entity, position/velocity components, input and
tick resources, and its explicitly ordered single-threaded schedule. FrameTee
snapshots only a small versioned value struct and restores those values into
the ECS, because a Bevy `World` itself is not byte-copyable.

`crate-type = ["cdylib"]` and
`#[no_mangle] pub extern "C" fn ft_game_module_entry` are the whole of the
plumbing:

```rust
#[no_mangle]
pub extern "C" fn ft_game_module_entry(engine_abi_version: u32) -> *const ft_game_module {
    if engine_abi_version != FT_GAME_ABI_VERSION {
        return std::ptr::null();
    }
    &MODULE
}
```

`cargo build --release` produces a loadable module; copying it into `games/` is
the entire install step. If you would rather generate the bindings, run bindgen
over `include/frametee/game_abi.h` — it is plain C with no macros in the type
definitions.

### Running

```sh
frametee --list-games                  # what is installed, and what each allows
frametee --game example-platformer     # start under a specific game
```

The choice is remembered in `config.toml` under `[game] id`. Games are selected
when creating a project; an open project cannot switch its game because doing
so would invalidate every world and input record in the timeline.

---

## What lives where, now

The split is done. `src/` contains no game code: no physics, no map format, no
tees, no particles, no skins, no map browser. Grep it for `SWorldCore`,
`SPlayerInput` or `ddnet_physics` and there are no hits at all; the word
"DDNet" survives only in comments explaining what a piece of the engine used to
be.

Concretely, the migration moved:

| Was in the engine | Now |
| --- | --- |
| `src/physics/` | `games/ddnet/ddnet_game.c` |
| `src/particles/`, `src/animation/` | `games/ddnet/dd_particles.c`, `dd_anim_*.c` |
| tee/hook/weapon/pickup rendering in `user_interface.c` | `games/ddnet/dd_render.c` |
| the tee skin pipeline and atlas manager in `renderer.c` | `games/ddnet/dd_gfx.c` |
| map layer textures and the level draw in `graphics_backend.c` | `games/ddnet/dd_map.c` |
| DDNet demo export | an `ft_exporter_desc` on the module |
| the entity inspector's projectile and laser structs | property reflection over `ft_entity_class` |
| DDNet skin discovery, fetching, previews and selection | `games/ddnet/dd_skin_browser.c` via the module UI hook |
| the player panel's game-specific stat block | `FT_PROP_SUMMARY` properties |
| DDNet chat/kill/vote event structs | generic events from `collect_events` |
| hardcoded input columns in the snippet editor | one column per `ft_input_field` |
| `[gameplay] game_mode` in config | the game's own variant list |

And the engine kept what is genuinely its own: the timeline and snippets, the
simulation cache and scrubbing, recording, undo/redo, projects, the camera, the
renderer, plugins, keybinds and the window.

Two consequences worth knowing:

- **Project files are version 11 and not backwards compatible.** Engine-owned
  values use explicit little-endian fields rather than raw C structs. Levels,
  each group's starting world, and optional module project metadata are stored
  as opaque length-delimited blobs. A project carries its game id and SemVer,
  ruleset, and a hash of the full input schema; any mismatch is refused before
  the open project is replaced. Modules without level serialization use the
  stored reloadable level path instead.
- **DDNet-specific plugins link the game's physics themselves.** The engine used
  to re-export `ddnet_physics` symbols to plugins; it no longer links it at all.
  Such a plugin includes `games/ddnet/include/ddnet/ddnet_game.h`, which is the
  DDNet module's own contract with its plugins — separate from the engine ABI,
  and free to change when that game changes. A plugin that only needs to read or
  write inputs can stay game-agnostic instead by enumerating the schema through
  `tas_api_t` (`input_field_count`, `input_field`, and the typed accessors),
  which is what the bundled Random Input Filler now does.

### Start screens

The splash runs in two halves. The editor draws the general one — the game
picker, recent projects, opening a level or a project — and then hands the panel
to the chosen game through the `ui` hook with `FT_UI_SPLASH`. Whatever a run
begins with belongs there: DDNet fills it with its map browser, which downloads
from ddnet.org and asks the editor to open what the user picks.

That browser used to be `src/user_interface/online_maps.c`, and was the last
place the editor knew a particular game existed. It now lives in
`games/ddnet/dd_maps.c`.

A game that wants panels does not compile ImGui: it includes the cimgui headers
and the `ig*` symbols resolve against the editor at load time, the same way the
bundled plugins work. The one requirement is adopting the editor's context and
allocator before the first call, which `engine->imgui_context()` and
`engine->imgui_allocators()` provide — see `games/ddnet/dd_imgui.c`, which is
the whole of it.

Three services exist for exactly this kind of panel:

- `resolve_cache_path` — a writable directory for anything downloaded or
  generated, under the user's config rather than the install.
- `request_level` — asks the editor to open a level by path, as if the user had
  picked it themselves.
- `imgui_texture_id` — an owned ImGui handle for a texture the game created, so
  its panels can draw thumbnails. Release each handle with
  `imgui_texture_release` before destroying its texture.
