# Native Super Mario 64

FrameTee runs the original SM64 game logic natively in an in-process library.
It supplies the N64 controller state each frame, snapshots mutable game memory
for timeline branching, and rasterizes the game’s Fast3D display lists in the
FrameTee viewport. It does not run Wafel or an emulator process.

## Player setup

1. Download FrameTee.
2. Place a legally obtained, unmodified **US, JP, or EU** Super Mario 64 ROM in
   `data/games/sm64` next to the FrameTee executable. The filename does not
   matter; `.z64`, `.n64`, and `.v64` byte orders work.
3. Start the SM64 game in FrameTee.

The release contains encrypted native libraries and a small unlock tool for
its platform. FrameTee identifies the ROM revision, decrypts only the matching
library into the user cache, and starts the game. The encryption uses the
normalized complete ROM as its key, so an altered or mismatched ROM cannot
unlock the runtime. No compiler, Python installation, Make, or manual library
selection is required from the player.

The raw ROM remains in the install’s `data/games/sm64` directory. The cached
copy and recovered native library are stored under FrameTee’s per-user game
cache. FrameTee prefers US when several supported ROMs are present, then JP,
then EU.

## Release maintenance

`build_full_libsm64.py` builds one headless full-game library from a supplied
ROM. `build_locked_runtimes.py` is the release entry point: it builds the US,
JP, and EU Linux and Windows libraries, records normalized-ROM, plaintext, and
encrypted-library SHA-256 hashes in each `manifest.json`, then locks every
library with its corresponding ROM.

For example, after building the Linux and Windows `frametee-sm64-lock` tools:

```sh
python3 games/sm64/build_locked_runtimes.py \
  --rom-dir data/games/sm64 \
  --output data/games/sm64/runtime \
  --lock-tool tools/sm64_lock/target/release/sm64_lock
```

The output layout is `runtime/linux` and `runtime/windows`. CMake installs the
directory for its target platform as `data/games/sm64/runtime` in the release.
ROMs and generated runtime artifacts are ignored locally and are never added
to this repository.

`libs/sm64ex` is the pinned full-game source base. FrameTee’s SM64 runtime
uses the full game implementation for game-accurate simulation and rendering.

