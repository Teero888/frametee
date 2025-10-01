# FrameTee

FrameTee is a TAS editor for the game [DDNet](https://github.com/ddnet/ddnet). It's built using C99, Vulkan and ImGui.

## Features
  - TAS editing
  - Vulkan graphics
  - ImGui user interface

## Requirements
  - `clang` compiler
  - Vulkan SDK
  - `zlib`

-----

## Building

### FrameTee Application

```sh
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
