# FrameTee

FrameTee is a TAS editor for the game [DDNet](https://github.com/ddnet/ddnet). It's built using C99, Vulkan and ImGui.

This is a WIP, expect bugs, crashes and missing features. The physics and project file format may change at any time so don't expect your projects to work across versions.
There is no support for macOS currently, since I don't have a macOS installation to test it.

<img width="2560" height="1440" alt="image" src="https://github.com/user-attachments/assets/a2076aa3-eeff-4466-9ed5-602126e26dc8" />
<img width="2560" height="1440" alt="image" src="https://github.com/user-attachments/assets/80c6a17f-b476-49c8-b1a8-fc36a3b6a9d4" />
<img width="2560" height="1440" alt="image" src="https://github.com/user-attachments/assets/6d3db15d-7237-4b3b-bebf-3b772c5d8a2b" />

## Features

### Core & Rendering
- **Custom Physics:** Custom physics from https://github.com/Teero888/ddnet_physics to prevent cheating in the original game.
- **Vulkan Renderer:** High-performance rendering using the Vulkan API.
- **DDNet Map Support:** Full support for DDNet maps (as far as the physics currently allow it).
- **Skin Rendering:** Full support for DDNet skins.

### TAS Editing
- **Timeline Interface:** Intuitive timeline for managing inputs across multiple frames.
- **Multi-Track Editing:** Support for multiple tees (tracks) with independent input streams.
- **Input Snippets:** Organize inputs into movable, resizable, and editable snippets.
- **Recording:** Real-time and frame-by-frame recording capabilities.
- **Prediction:** Visual trajectory prediction to see where the tees will go.
- **Undo/Redo:** Comprehensive undo/redo system for all timeline operations.
- **Snippet Editor:** Detailed matrix editor for precise tick-by-tick input modification.
- **Bulk Editing:** Apply changes (direction, jumping, weapons) to multiple ticks at once.

### Dummy Control
- **Dummy Handling:** Dedicated controls for dummy tees.
- **Input Copying:** Configurable input mirroring and copying from the main tee (Mirror X/Y, Copy Aim/Hook/Fire).
- **Dummy Hammer:** Dummy hammering that behaves similar to the [deepfly bind](https://wiki.ddnet.org/wiki/Binds#Deepfly)

### Tools & Extensibility
- **Demo Export:** Export your TAS directly to a DDNet compatible demo file.
- **Project System:** Save and load your work with `.tasp` project files.
- **Plugin System:** Extend the editor's functionality with C/C++ plugins (DLL/SO).
- **Skin Browser:** Visual browser for managing and selecting player skins.
- **Keybinds:** Fully configurable keyboard and mouse bindings.

## Requirements
  - clang is recommended
  - Vulkan SDK
  - `zlib`

-----

## Building

### FrameTee Application

```sh
# make sure to clone recursively, we have some necessary submodules
git clone --recursive https://github.com/Teero888/frametee.git

# create and navigate to the build directory
mkdir build && cd build

# configure for a Release build
cmake .. -DCMAKE_EXPORT_COMPILE_COMMANDS=On -DCMAKE_BUILD_TYPE=Release
# OR configure for a Debug build with sanitizers enabled
cmake .. -DCMAKE_EXPORT_COMPILE_COMMANDS=On -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=On

# compile the project
make -j$(nproc)
```

### Individual Plugins

Plugins can be built individually without rebuilding the entire application. See the **Building a Plugin** section below for instructions.

-----

## Contributing

FrameTee is open-source. Contributions are accepted through issue reports and pull requests.
You can also contact me via Discord: `teero777`

-----

## Configuration

The config for this program is saved as `~/.config/frametee/config.toml` on Unix based systems or as `%appdata%/frametee/config.toml` on Windows.

## Default Controls

### Playback
- **Play/Pause:** `X`
- **Rewind (Hold):** `C`
- **Previous Frame:** `Mouse Button 4`
- **Next Frame:** `Mouse Button 5`
- **Increase TPS:** `Up Arrow`
- **Decrease TPS:** `Down Arrow`

### Timeline Editing
- **Select All Snippets:** `Ctrl + A`
- **Delete Snippet:** `Delete`
- **Split Snippet:** `Ctrl + R`
- **Merge Snippets:** `Ctrl + M`
- **Toggle Snippet Active:** `A`

### Recording
- **Move Left/Right:** `A` / `D`
- **Jump:** `Space`
- **Fire:** `Mouse Left`
- **Hook:** `Mouse Right`
- **Kill:** `K`
- **Trim Recording:** `F`
- **Cancel Recording:** `F4`
- **Weapons:** `1`-`5` (Hammer, Gun, Shotgun, Grenade, Laser)

### Dummy & Camera
- **Dummy Fire:** `V`
- **Toggle Dummy Copy:** `R`
- **Zoom In/Out:** `W` / `S`
- **Switch Track:** `Alt + 1-9`

## Plugins 

### Overview

FrameTee supports plugins in the form of shared libraries (`.dll` on Windows, `.so` on Linux) that can extend the editor's functionality. Plugins are loaded at startup from the `plugins` directory relative to the executable. They can interact with the editor, add new UI elements using ImGui, and call back into the host application to perform actions like creating timeline snippets or drawing debug information.

### Plugin Lifecycle

Each plugin is a shared library that must export four C functions which the editor calls at different stages:

  * `plugin_info_t get_plugin_info(void)`: Returns basic information about the plugin like its name, author, and version. It's called once when the plugin is loaded.
  * `void *plugin_init(tas_context_t *context, const tas_api_t *api)`: Called after the plugin is loaded. This is where you should allocate any state for your plugin. It receives pointers to the application's context and API. You must return a pointer to your plugin's state data, or `NULL` if initialization fails.
  * `void plugin_update(void *plugin_data)`: Called every frame. The `plugin_data` argument is the pointer you returned from `plugin_init`. This is where you can render UI and perform per-frame logic.
  * `void plugin_shutdown(void *plugin_data)`: Called when the plugin is about to be unloaded (e.g., on application shutdown or plugin reload). You should free any resources allocated in `plugin_init` here.

### The API

The plugin interacts with the host application through two main structs defined in `src/plugins/plugin_api.h`.

#### `tas_context_t`

This struct provides **read-only access** to the application's state.

  * `timeline`: The main timeline state, containing tracks and snippets.
  * `imgui_context`: The active ImGui context pointer. **You must call ImGui::SetCurrentContext with this this** before calling any ImGui functions from your plugin.
  * `gfx_handler`, `ui_handler`: Pointers to other high-level application state.

#### `tas_api_t`

This struct provides a set of **functions your plugin can call** to interact with the editor.

  * `get_current_tick()`: Gets the current tick of the timeline playhead.
  * `log_info()`, `log_warning()`, `log_error()`: Logs a message to the console.
  * `do_create_track()`: Adds a new player track to the timeline and returns an undo command you can
    register with the editor.
  * `do_create_snippet()`: Creates a new input snippet on a track and optionally reports the created
    snippet's ID so you can modify it further.
  * `do_set_inputs()`: Overwrites a range of inputs inside an existing snippet. Returns an undo
    command capturing the changes.
  * `register_undo_command()`: Registers a custom undo/redo action with the editor's undo manager.
  * `draw_line_world()`: A debug function to draw a line in the world view. The unit here is Blocks.

### Creating a Plugin

**1. Create Plugin Files**

Create a new directory inside `plugins/`, for example `plugins/my_plugin/`. Inside, create your source files (e.g., `my_plugin.c`) and a `CMakeLists.txt`.

**2. The C Source (`my_plugin.c`)**

Your C file must implement the four required functions (`get_plugin_info`, `plugin_init`, `plugin_update`, `plugin_shutdown`). For a complete and recommended structure, see the example implementation in `plugins/example_c/example.c`. The `plugins/random_input_filler/` plugin shows how to use the extended API to create tracks and populate snippets with custom input data.

**3. The Build Script (`CMakeLists.txt`)**

Your plugin needs its own CMake script to be compiled as a shared library. The easiest way to start is to copy and modify the one from `plugins/example_c/CMakeLists.txt`.

**4. Building a Plugin**

To compile a single plugin for development, navigate to its directory and run the following commands:

```sh
# navigate to your plugin's directory
cd plugins/my_plugin

# configure the build, pointing to the host application's root
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=On -DHOST_APP_DIR=../../

# compile the plugin
cmake --build build
```

The compiled shared library will be automatically copied to the main application's `build/plugins` directory.

### C++ Plugins

It's also possible to write plugins in C++ and use the native ImGui C++ API. Check out the `plugins/example_cpp/` directory for a complete example of this approach.
