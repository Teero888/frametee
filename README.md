# FrameTee

**FrameTee** is a Tool-Assisted Speedrun (TAS) editor for [DDNet](https://github.com/ddnet/ddnet), built with C99, Vulkan, and ImGui.

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
*   **Custom Physics:** Using [ddnet_physics](https://github.com/Teero888/ddnet_physics) to prevent cheating.
*   **Vulkan Renderer:** High-performance rendering pipeline.
*   **DDNet Support:** Full compatibility with DDNet maps and skins.

### TAS Editing
*   **Timeline Interface:** Multi-track timeline for managing inputs.
*   **Recording:** Real-time and frame-by-frame recording capabilities.
*   **Input Snippets:** Organize inputs into movable, resizable, and editable snippets.
*   **Prediction:** Visual trajectory prediction.
*   **Snippet Editor:** Detailed matrix editor for precise tick-by-tick modification.
*   **Undo/Redo:** Comprehensive system for timeline operations.
*   **Bulk Editing:** Apply changes (direction, jumping, weapons) to multiple ticks simultaneously.

### Advanced Control
*   **Dummy Handling:** Dedicated controls for dummy tees with input mirroring and copying.
*   **Deepfly Support:** Dummy fire mechanics similar to the standard [deepfly bind](https://wiki.ddnet.org/wiki/Binds#Deepfly).

### Tools & Extensibility
*   **Demo Export:** Export directly to DDNet-compatible demo files.
*   **Plugin System:** C/C++ plugin support (DLL/SO) for custom functionality.
*   **Project System:** `.tasp` project files for saving/loading work.
*   **Keybinds:** Fully configurable keyboard and mouse bindings.
*   **Skin Browser:** Visual browser for managing player skins.

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

| Category | Action | Default Key |
| :--- | :--- | :--- |
| **Playback** | Play/Pause | `X` |
| | Rewind (Hold) | `C` |
| | Previous Frame | `Mouse Button 4` |
| | Next Frame | `Mouse Button 5` |
| | Adjust TPS | `Up` / `Down` Arrows |
| **Timeline** | Select All Snippets | `Ctrl + A` |
| | Delete Snippet | `Delete` |
| | Split Snippet | `Ctrl + R` |
| | Merge Snippets | `Ctrl + M` |
| | Toggle Active | `A` |
| **Recording** | Move | `A` / `D` |
| | Jump | `Space` |
| | Fire | `Mouse Left` |
| | Hook | `Mouse Right` |
| | Kill | `K` |
| | Weapons | `1`-`5` (Hammer, Gun, Shotgun, Grenade, Laser) |
| | Trim Recording | `F` |
| | Cancel Recording | `F4` |
| **Tools** | Dummy Fire | `V` |
| | Toggle Dummy Copy | `R` |
| | Zoom | `W` / `S` |
| | Switch Track | `Alt + 1-9` |

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

*   **`tas_context_t`**: Read-only access to application state (Timeline, ImGui Context, Graphics).
    *   *Note: Plugins must set the ImGui context using `imgui_context`.*
*   **`tas_api_t`**: Function pointers for actions:
    *   `do_create_track()`, `do_create_snippet()`, `do_set_inputs()`
    *   `log_info()`, `draw_line_world()`
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

---

## Contributing

FrameTee is open-source. Contributions are accepted through issue reports and pull requests.

**Contact:**
*   Discord: `teero777`
