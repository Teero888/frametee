# SM64 physics replacement

The requested replacement is a physics-only x86 library based on
`n64decomp/sm64`, with unchanged decompiled simulation algorithms. FrameTee
must load/decode the ROM and own all graphics, following the DDNet module's
separation. The existing sm64ex implementation is to be removed, not wrapped
as the new implementation. Emulator bit parity is not required.

## Current evidence (2026-09-12)

- `games/ddnet/CMakeLists.txt` links an independent physics library and builds
  rendering/map/UI code in the game module.
- `games/sm64/sm64_backend.cpp` still loads a full-game native image, snapshots
  its writable memory and invokes native rendering through that same image.
  `games/sm64/CMakeLists.txt` still stages locked full-game runtime assets.
- `libs/sm64_physics` is a newly initialized independent Git repository whose
  origin is `https://github.com/Teero888/sm64_physics.git`. The user instructed
  that nothing be pushed. It is not yet registered as a FrameTee submodule.
- The source import pins n64decomp revision
  `9921382a68bb0c865e5e45eb594d9c64db59b1af` and records imported file hashes.
  Native compilation succeeds for Mario actions, movement, interactions,
  collision, object processing and behaviors. Building a static archive alone
  does not resolve its host-service or asset dependencies.
- Executable collision tests cover original floor/ceiling queries, dynamic
  floor precedence, the 78-unit floor tolerance, camera filtering, water and gas.
  They supply test-owned state; they do not verify a complete simulation world.
- Animation initialization, frame calculation and index lookup are extracted
  verbatim from `graph_node.c`, with original byte ranges and hashes recorded
  in `extracted.json`. `host/animation.c` performs the frame/time updates that
  previously lived in `geo_set_animation_globals` in the renderer. Native
  tests cover forward/reverse, loops/clamps, fractional speeds and tick
  ownership. The hook still needs wiring into the eventual world update.
- The build enables upstream's `NO_SEGMENTED_MEMORY` path. The host address
  adapter accepts native pointers; FrameTee must decode/relocate ROM assets
  before they cross the library boundary. No ROM decoding is implemented yet.
- Native object-node initialization and link routines are extracted verbatim.
  Math builds without the two N64 fixed-point matrix packing functions; the
  verifier checks that no other source bytes were omitted. A host-only pool
  allocator aligns native object storage correctly. Tests exercise node
  ordering/removal, spawn transforms, allocation failure and matrix transforms.
  These globals are not yet owned/switched per world, and object behaviors
  still have unresolved host and asset dependencies.
- Original main-pool functions now compile against a private header that maps
  their global names to a thread-local active owned pool. The function bodies
  are unchanged. Native tests interleave two pools and cover allocation,
  exhaustion, nested push/pop and resizing. Pool ownership is not complete
  world ownership; other globals still require adaptation. Preserve upstream
  allocator call preconditions rather than treating its stack push/pop as a
  general snapshot API.
- Owned terrain now accepts decoded triangles/regions and invokes verbatim
  surface construction/insertion and query functions. Query globals and scratch
  floor geometry are mapped to a thread-local active terrain. Tests exercise
  walls, static/dynamic floor queries, camera filtering, water/gas, independent
  instances, clone lifetime and concurrent Linux queries. The old test-only
  partition globals were removed. **The full area/object collision loader still
  uses its original globals and must be routed into the same context before
  world stepping is enabled.** This is not yet a complete world implementation.

Subsequent work routes the moving-object collision loader, dynamic clearing and
original object transform helpers into owned terrain. Its checked host entry
validates stream length/indices, the 200-vertex buffer and pool capacity.
Tests now cover translated platforms, time stop, distance culling, DDD room
assignment, malformed streams and cloning current dynamic surfaces. Full area
loading and Mario/object globals still need routing into the same world.
Terrain clones borrow object references; full-world cloning must rebind these.

Original Mario ground quarter-steps and gravity are now separately executable.
Collision helpers and the terrain sound table are copied unchanged; the terrain
context owns shell-water pseudo-floor state and the level number. Native tests
exercise ground displacement, wall response, gravity and shell-water support.
This still sets Mario state directly, without input processing, action dispatch,
airborne movement or the full world tick. A whole-archive relocatable link also
checks that extracting routines did not introduce duplicate definitions.

Original airborne movement now also runs against the owned terrain. Tests
exercise falling/landing, ceiling response, ceiling grabs, ledge grabs and
lava-wall collision without a renderer. Input processing, the action dispatcher
and complete world/level execution remain unfinished.

Original analog adjustment and Mario button/joystick processing are now
extracted. A host controller adapter owns button-edge history without N64 DMA.
Tests cover dead zones, normalization, press/hold/release behavior, input timers,
camera-relative yaw, squish behavior and independent controller histories.
Geometry input checks and the complete input/action dispatcher are still pending.

Geometry input checks and original `update_mario_inputs` have since been
extracted. Tests cover environmental flags, crushing, OOB recovery/death requests,
input reset/timers and first-person eligibility using owned camera movement
flags. Test observers cover debug output and warp requests only; production
level transitions and the action dispatcher remain unfinished.

Original `set_mario_action` and its group-specific setup helpers now link
independently. Tests cover jump initialization followed by original air steps
to landing, action timers, backwards long-jump speed, squish downgrades and
surface-dependent walking setup. Full per-frame action execution remains to be
connected; this does not establish a complete Mario/world tick.

## Next work and completion gates

Mixed source files contain presentation callbacks alongside physics. Preserve
the exact simulation bodies when separating these, and retain verifiable
upstream provenance. Animation timing, held-object transforms and any state
that affects simulation must run without a renderer. Do not substitute no-op
stubs for gameplay dependencies.

The physics API needs owned worlds, input/step, read-only state, editing and
independent clone/restore. The host must supply decoded physical assets and
consume state/events without native graphics calls. ROM assets and GPU
resources must not enter simulation snapshots.

Replace FrameTee's backend, ROM/level loader and graphics implementation around
that API. Preserve the engine-facing world, timeline, reflection, serialization
and plugin workflows. Verify complete level/project round trips, headless
continuation, interleaved worlds, render-independent stepping, moving objects
and scene rendering. Register the new physics repository as a submodule and
remove the old sm64ex submodule, runtime build/unlock tools, packaged runtime
assets and obsolete references once the replacement takes over.

The old submodule currently has local modifications. Inspect and preserve
needed reference work before removing it. Do not remove unrelated untracked
directories in the parent worktree.

The original objective remains incomplete. The separate library extraction is
a foundation, not a replacement for the full integration and verification.
