# FrameTee

**FrameTee** is a Tool-Assisted Speedrun (TAS) editor built with C99, Vulkan, and ImGui.

The editor itself is game-agnostic: it owns the timeline, snippets, undo/redo,
projects and rendering, while a **game module** supplies the physics, the level
format, the visuals and the rules. Modules are shared libraries that implement
one C ABI, so they can be written in any language that compiles to one — the
bundled examples are in C, C++ and Rust. [DDNet](https://github.com/ddnet/ddnet)
is the game FrameTee ships with. See [docs/game-modules.md](docs/game-modules.md).

> **Note:** This project is a Work In Progress (WIP). Expect bugs, crashes, and missing features. Physics and project file formats are subject to change.
> Currently, there is **no macOS support**.

---

## Screenshots

<p align="center">
  <img width="48%" alt="FrameTee Editor View" src="https://github.com/user-attachments/assets/a2076aa3-eeff-4466-9ed5-602126e26dc8" />
  <img width="48%" alt="Skin Browser" src="https://github.com/user-attachments/assets/80c6a17f-b476-49c8-b1a8-fc36a3b6a9d4" />
  <img width="48%" alt="Controls" src="https://github.com/user-attachments/assets/6d3db15d-7237-4b3b-bebf-3b772c5d8a2b" />
</p>

---

## Features

### Core & Rendering
*   **Game Modules:** Games plug in as shared libraries through a versioned C ABI (C, C++, Rust, ...), and each one tells the editor what it allows: player counts, tick rate, whether worlds can be forked.
*   **Custom Physics:** The DDNet module uses [ddnet_physics](https://github.com/Teero888/ddnet_physics) to prevent cheating.
*   **Vulkan Renderer:** High-performance rendering pipeline, with sprite atlases and custom pipelines available to game modules.
*   **DDNet Support:** Compatibility with DDNet maps and skins.

### TAS Editing
*   **Timeline Interface:** Multi-track timeline for managing inputs.
*   **Recording:** Real-time and frame-by-frame recording capabilities.
*   **Input Snippets:** Organize inputs into movable, resizable, and editable snippets.
*   **Prediction:** Visual trajectory prediction.
*   **Snippet Editor:** Detailed matrix editor for precise tick-by-tick modification.
*   **Undo/Redo:** Comprehensive system for timeline operations.
*   **Bulk Editing:** Apply changes (direction, jumping, weapons) to multiple ticks simultaneously.

### Advanced Control
*   **Linked Inputs:** Games opt into schema-driven input mirroring. The DDNet module uses it for dummy-tee controls; unrelated games do not expose them.
*   **Deepfly Support:** Dummy fire mechanics similar to the standard [deepfly bind](https://wiki.ddnet.org/wiki/Binds#Deepfly).

### Tools & Extensibility
*   **Exporters:** Games provide their own; DDNet exports directly to DDNet-compatible demo files.
*   **Plugin System:** C/C++ plugin support (DLL/SO) for custom functionality, scoped either globally or to one game.
*   **Project System:** `.tasp` project files for saving/loading work.
*   **Keybinds:** Fully configurable keyboard and mouse bindings.
*   **Player Identity:** Names, colours and an appearance id the active game resolves however it likes.

---

## Building

### Requirements
*   **Compiler:** `clang` (recommended)
*   **SDK:** Vulkan SDK
*   **Libraries:** `zlib`

### Build Instructions

1.  **Clone the repository:**
    ```sh
    # Make sure to clone recursively for submodules
    git clone --recursive https://github.com/Teero888/frametee.git
    cd frametee
    ```

2.  **Configure and Compile:**
    ```sh
    mkdir build && cd build

    # For Release
    cmake .. -DCMAKE_BUILD_TYPE=Release

    # For optimized builds
    cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_AGGRESSIVE_OPTIM=On

    # OR for Debug (with sanitizers)
    # cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=On

    make -j$(nproc)
    ```

---

## Controls & Configuration

**Configuration File:**
*   **Linux/Unix:** `~/.config/frametee/config.toml`
*   **Windows:** `%appdata%/frametee/config.toml`

### Default Key Bindings

The engine owns playback, timeline, project, camera and track-selection binds.
Recording controls are declared by the active game and appear in the same
keybind editor. These are the bundled DDNet module's defaults:

| Category | Action | Default Key |
| :--- | :--- | :--- |
| **Playback** | Play/Pause | `X` |
| | Rewind (Hold) | `C` |
| | Previous Frame | `Left Arrow`, `Mouse Button 4` |
| | Next Frame | `Right Arrow`, `Mouse Button 5` |
| | Adjust TPS | `Up` / `Down` Arrows |
| **Timeline** | Select All Snippets | `Ctrl + A` |
| | Delete Snippet | `Delete` |
| | Split Snippet | `Ctrl + R` |
| | Merge Snippets | `Ctrl + M` |
| | Toggle Active | `A` |
| **DDNet Recording** | Move | `A` / `D` |
| | Jump | `Space` |
| | Fire | `Mouse Left` |
| | Hook | `Mouse Right` |
| | Kill | `K` |
| | Weapons | `1`-`5` (Hammer, Gun, Shotgun, Grenade, Laser) |
| **Recording** | Trim Recording | `F` |
| | Cancel Recording | `F4` |
| | Toggle Linked Copy | `R` |
| **Camera** | Zoom | `=` / `-` |
| **Tracks** | Switch Track | `Alt + 1-9` |

---

## Plugin System

FrameTee supports extensions via shared libraries (`.dll` / `.so`) loaded from the `plugins/` directory. Plugins can interact with the editor, add UI elements via ImGui, and manipulate timeline data.

### Plugin Lifecycle

Plugins must export four C functions:

*   `plugin_info_t get_plugin_info(void)`: Returns metadata (name, author, version).
*   `void *plugin_init(tas_context_t *context, const tas_api_t *api)`: Initialize state.
*   `void plugin_update(void *plugin_data)`: Called every frame (UI/Logic).
*   `void plugin_shutdown(void *plugin_data)`: Cleanup resources.

### API Access

Plugins interact with the host via `src/plugins/plugin_api.h`:

*   **`tas_context_t`**: The shared ImGui context, headless flag, and active game id.
    *   *Note: UI plugins must set the ImGui context using `imgui_context`.*
*   **`tas_api_t`**: Function pointers for actions:
    *   `do_create_track()`, `do_create_snippet()`, `do_set_inputs()`
    *   `input_field_count()`, `input_field()`, and the typed input accessors
    *   `log()`, `draw_line_world()`
    *   `register_undo_command()`

### Building a Plugin

Plugins can be built independently without recompiling the main application.

1.  **Create Directory:** `plugins/my_plugin/`
2.  **Source File:** Implement the lifecycle functions.
3.  **CMakeLists.txt:** Configure as a shared library.

**Build Command:**
```sh
cd plugins/my_plugin
cmake -S . -B build -DHOST_APP_DIR=../../
cmake --build build
```

The output binary will be copied to `build/plugins` automatically if configured like the examples. See `plugins/example_c/` and `plugins/example_cpp/` for reference.

### Plugin Scope

A plugin is global by default and stays loaded whichever game is active. To bind
one to a single game, export:

```c
FT_API const char *plugin_game_id(void) { return "ddnet"; }
```

The host then only loads it while that game is active. Anything that reads or
writes a game's world or input records should do this, since those bytes mean
nothing under a different game.

---

## Game Modules

Games live in `games/`, are built to `build/games/`, and are discovered at
startup:

```sh
./frametee --list-games              # installed games and what each one allows
./frametee --game example-platformer # start under a specific game
```

The bundled modules are `ddnet` (C), a raylib-backed C++ platformer, and a
Bevy ECS-backed Rust bouncer. Writing your own means implementing
`include/frametee/game_abi.h` and exporting `ft_game_module_entry` — no engine
sources, no Vulkan, no ImGui required. Full guide:
[docs/game-modules.md](docs/game-modules.md).

---

## Contributing

FrameTee is open-source. Contributions are accepted through issue reports and pull requests.

**Contact:**
*   Discord: `teero777`
*   Matrix: `@teero888:matrix.org`
