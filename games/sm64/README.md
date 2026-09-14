# Native Super Mario 64

FrameTee runs original SM64 game logic and levels natively in-tree.
It parses vanilla SM64 ROMs directly, provides accurate physics simulation and level geometry
via `libs/sm64_physics`, snapshots mutable game state for timeline branching, and rasterizes Fast3D display lists
using an integrated Fast3D renderer with software and Vulkan backends.
No external unlock tools, encrypted binaries, emulators, or `sm64ex` builds are required.

## Player setup

1. Place a legally obtained, unmodified **US, JP, or EU** Super Mario 64 ROM (`.z64`, `.n64`, or `.v64`)
   in `data/games/sm64` (or select it when prompted in FrameTee).
2. Start the SM64 module in FrameTee.

FrameTee automatically inspects the ROM header and country code, decompresses the required segments (such as level geometry, behavior scripts, and textures), initializes the in-tree simulation engine, and renders directly.

## Features

- **Direct ROM Execution:** Runs vanilla SM64 ROM assets and physics directly without external processes or locked binaries.
- **Fast3D Renderer:** Native Fast3D command execution supporting textured triangles, lighting, fog, and combiner modes with both software rasterization and Vulkan GPU execution.
- **Camera Modes:**
  - **Native Game Camera (Mode 0):** Accurate in-game camera following Mario.
  - **Free/Orbit Cameras (Modes 1 & 2):** Editor cameras with 3D perspective and orthographic projections.
- **State Serialization & Replay:** Checkpointing, timeline branching, property editing (position, velocity, health), and M64 movie export.
