# FrameTee

FrameTee is a game-agnostic **Tool-Assisted Speedrun (TAS) editor and simulation engine** for games whose physics can be reproduced outside the original game.
The core engine is written in **C99** using **Dear ImGui** and **Vulkan**. Game-specific physics, inputs, rendering, level loading and exporting are provided through separate game modules.
Currently supported games are [Teeworlds](https://teeworlds.com/) / [DDNet](https://ddnet.org/) and [TrackMania Nations Forever](https://store.steampowered.com/app/11020/TrackMania_Nations_Forever/). Super Mario 64 support is currently in development.

> FrameTee is a **Work In Progress**. Expect bugs, crashes and breaking changes. Physics integrations, APIs and project file formats may change between versions. macOS is currently not supported.

<p align="center">
<img width="45%" height="350" alt="ddnet_gif" src="https://github.com/user-attachments/assets/40258457-b4ea-45a1-8a4b-d95f2fdd2f36" />
<img align="top" width="48%" alt="tmnf_gif" src="https://github.com/user-attachments/assets/420f16f3-83ca-4129-9c5c-5cadec0f87c6" />
<img height="530px" alt="ddnet_image" src="https://github.com/user-attachments/assets/91449a7d-6be8-4d88-b9ff-294106f90a28" />
<p/>
  
## Games

Game integrations are implemented as modules behind a versioned C ABI. A module provides simulation, input definitions, state reflection, level loading, rendering and exporters while FrameTee handles the editor and TAS workflow.

### DDNet / Teeworlds

The DDNet module uses [`ddnet_physics`](https://github.com/Teero888/ddnet_physics) and loads regular DDNet `.map` files.
The following gamemodes are supported: DDRace, Race, FastCap, FastCap no weapons.
Exporting to .demo or ghost files is also supported. `ddnet_physics` is still incomplete so some features such as draggers or plasma turrets won't work but will be implemented in the future.
To prevent cheating on official DDNet servers, the physics have been slightly altered and exporting input sequences to cheat clients is **NOT** supported and never will be. Don't even try.

### TrackMania Nations Forever

TrackMania support is built around my fork of [ForeverValidator](https://github.com/Skycrafter-dev/ForeverValidator).
FrameTee can load TrackMania Forever challenges, simulate and render the car in the 3D editor, and export runs as native `.Replay.Gbx` files.
TrackMania's proprietary assets are not distributed with FrameTee and must come from an existing TrackMania installation. See [installation guide](https://github.com/Teero888/frametee/blob/master/data/games/tmnf/README.txt).

## Timeline

The timeline is built around tracks and input snippets.
Snippets contain authored input and can be moved, resized, split, duplicated and combined without flattening the run into a single input stream. The snippet editor exposes the active game's inputs per tick and supports direct editing of the underlying input data. Snippets can be manually created or be recorded live at variable speed. Snippets are non-destructive, resizing them or splitting them won't delete any information, just as in any video editor.

<img height="300" alt="image" src="https://github.com/user-attachments/assets/33ffbf90-fe4b-475b-9370-e3aff492c803" />

### Snippet Effects

Snippet effects apply non-destructive transformations to authored input.
Effects are processed in order and can be reordered, disabled or removed without modifying the original input data. Game modules define the available effects while FrameTee handles their storage, evaluation and undo history.

<img height="300" alt="DDNet snippet effects image" src="https://github.com/user-attachments/assets/20427b2c-f38c-49be-846d-ee4e592edec6" />

## Prediction

Prediction lines simulate future movement from the current state without modifying the timeline.
The primary line follows the authored inputs. Additional lines can override selected inputs, allowing several possible continuations to be compared from the same simulation state.
Prediction uses the same game simulation interface as normal playback, so it works across both 2D and 3D modules.

<img align="top" height="350" alt="image" src="https://github.com/user-attachments/assets/570483fb-b86b-4dd3-82cc-499ae5bcf5d7" />
<img height="350" alt="image" src="https://github.com/user-attachments/assets/17ed1ee7-6845-4a99-8e3f-8ecfde112941" />
<img height="350" alt="image" src="https://github.com/user-attachments/assets/59a03977-95f2-48fe-a463-3f8f020711b8" />

## Groups

Groups represent independent simulation worlds.
Tracks inside the same group share a world and interact normally. Separate groups maintain separate world states, which allows multiple independent runs to exist in the same project.

<img height="230" alt="image" src="https://github.com/user-attachments/assets/9df14f5d-4da0-41fc-89d3-0ba8e4948e15" />
<img height="230" alt="image" src="https://github.com/user-attachments/assets/fbcaeb56-9ba1-4c41-b0e7-9973e5449e07" />
<img height="230" alt="image" src="https://github.com/user-attachments/assets/2a03a444-1a57-45c9-97c0-7d837286bea4" />

## Starting-State Overrides

Game modules can expose selected entity properties as editable starting-state values.
These overrides are applied before timeline playback begins and can be used to change state such as position, velocity or other game-specific properties supported by the module.

<img height="400" alt="image" src="https://github.com/user-attachments/assets/5dda3e0e-3760-49b2-b40f-d05956b763ae" />
<img align="top" height="400" alt="image" src="https://github.com/user-attachments/assets/d69e8d54-7732-41dc-a0c0-9169d0ce89d4" />

## Plugins

Plugins extend FrameTee without modifying the core engine.
They can inspect and modify timeline data, generate inputs, add UI, draw viewport overlays, participate in undo/redo and expose command-line functionality.
Plugins use a versioned C API and are configured through `plugin.toml`.
See [`docs/plugins.md`](docs/plugins.md) for the plugin API and examples.

## Command Line

FrameTee can be started directly with a selected game and level:

```bash
./frametee --game ddnet --level path/to/map.map
```

Installed game modules can be listed with:

```bash
./frametee --list-games
```

Headless mode runs the simulation without creating a window and can be combined with plugins:

```bash
./frametee \
    --headless \
    --game ddnet \
    --level path/to/map.map \
    --plugin my_plugin
```

Additional arguments can be forwarded to active plugins.

Use:

```bash
./frametee --help
```

for the options supported by the current build.

## Undo / Redo

FrameTee uses a command-based undo system for timeline and state editing.
Native editor operations and plugin-generated changes can participate in the same history, with simulation caches invalidated as required when changes are reverted or reapplied.

## Game Modules

New games are integrated through [`include/frametee/game_abi.h`](include/frametee/game_abi.h).
The ABI covers simulation, input schemas, reflected state, rendering, level loading, cameras, exporters and other game-specific functionality. Modules are compiled as shared libraries and discovered at runtime.
See [`docs/game-modules.md`](docs/game-modules.md) for more information.

## Building

Clone the repository with its submodules:

```bash
git clone --recursive https://github.com/Teero888/frametee.git
cd frametee
```

Configure and build with CMake:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
```

FrameTee requires CMake, a suitable C/C++ compiler, Vulkan development libraries and `glslangValidator`.

Windows and Linux are currently supported. (not msvc since it's garbage. use mingw)

## Contributing

Bug reports, fixes, game integrations, plugins and editor improvements are welcome.

For simulation issues, include the game, level, FrameTee revision and a minimal project or input sequence that reproduces the problem where possible.

### Contact

Discord: `teero777`
Matrix: `@teero888:matrix.org`
