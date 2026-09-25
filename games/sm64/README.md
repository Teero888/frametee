# Super Mario 64

The whole game, frame for frame as on the console from power-on, around
`libs/sm64_physics` (the game itself, verified in lockstep with an emulator).
One module per version, each with the library of its version:

| Module | ROM | Ticks per second |
|---|---|---|
| `sm64` | Super Mario 64 (USA) | 30 |
| `sm64_jp` | Super Mario 64 (Japan) | 30 |
| `sm64_eu` | Super Mario 64 (Europe) (En,Fr,De) | 25 |

Build with `-DFRAMETEE_BUILD_SM64=ON`.

## Player setup

Put your ROM (`.z64`, `.n64` or `.v64`) into `data/games/sm64/`, where every
version's start screen lists it, or open it from anywhere. The library carries
the game's code and data but nothing of the ROM: textures come from the ROM
when drawing.

## What the module does

- **Worlds** are consoles of the library (`sm64_world`), stepped one frame per
  tick with one controller read. Plugins look inside them through
  `include/sm64/sm64_game.h` and call the library directly.
- **Drawing**: tick N is drawn by stepping a copy of tick N - 1 again with the
  game drawing (`sm64_step_draw`); its display list goes through `fast3d/`, the
  Fast3D interpreter of [sm64-port](https://github.com/sm64-port/sm64-port)
  (see `fast3d/LICENSE.txt`), and `sm64_vulkan.c`, which draws on the engine's
  Vulkan device into a texture shown over the viewport. The 3D takes the
  viewport's aspect ratio, the HUD stays 4:3 in the middle.
- **Cameras**: the game camera shows the frame as the console draws it; the
  engine's freecam and top-down views draw the game's 3D scene from where they
  are (the game still decides what is drawn, from Lakitu's view).
- **Movies**: `.m64` files open as recordings and export from power-on, for
  Mupen64-rr.

No sound yet: the engine has no audio output.

`tests/sm64_render.c` draws frames of a movie offscreen into PNG files, as the
module does (`sm64_render_frames_<version>`).
