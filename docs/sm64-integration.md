# SM64 integration verification

The full SM64 integration is still in progress. Passing the tests below does
not establish complete editor workflows, project portability, performance, or
cross-platform parity.

## Verified foundations

- Simulation branches survive loading another setup backed by the same native
  image. The restore baseline owns its snapshot instead of retaining a dangling
  pointer when a world is copied or destroyed.
- Rendering restores the native memory it changes, including Fast3D pointers
  and viewport dimensions. Simulation continuation is independent of repeated
  rendering and viewport resizing.
- Position, velocity, and health can be edited through reflection and offered
  as starting overrides. Save format `FTSM6403` replays ordered edits at their
  original frames. Input-only `FTSM6402` saves remain readable. Tests compare
  actual position, velocity, action, health, and continuation, not just inputs.
- Plugin ABI 4 exposes owned-world clone/copy/step, reflection, and serialization
  without a graphics device. A headless plugin integration test exercises the
  engine callbacks with SM64.
- Vulkan presentation uses the correct binding and 2D sampler. The original
  texture-array/binding-1 declaration could produce a black viewport even when
  SM64's GPU target contained a correctly rendered scene.
- Vulkan texture IDs are reused per frame, unchanged texture uploads are
  skipped, and resized images survive until submitted commands finish. Buffer
  exhaustion rejects the GPU frame instead of overwriting pending geometry or
  texture uploads. Split render passes explicitly synchronize attachments.
- Fast3D SPIR-V headers are generated from `games/sm64/shaders` during the build.
- Course card descriptions wrap inside their boxes in the inspected two-column
  splash layout at a 1280-pixel window width.
- Native scene regeneration supports editor perspective and orthographic
  cameras without advancing simulation. CPU tests check distinct, repeatable
  frames, camera matrix round trips, and unchanged simulation continuation.
  GPU captures exercise Game, Freecam and Top-down modes at first/repeated
  paused frames with rebuilt US, JP and EU Linux runtimes.
- The directed view uses the original camera pose, lens and roll. Game ABI 20
  adds an explicit matrix override used by engine rendering and picking; the
  Rust ABI mirror is updated too. Cached camera data survives copy/save replay
  without needing a graphics device.
- Headless simulation now supplies canonical 320x240 dimensions. The native
  object culler and skybox generator previously read zero dimensions before a
  renderer was attached. Temporary editor dimensions do not affect simulation.
- EU course startup skips the empty-save language menu only when skip-intro
  startup is requested. Normal movie playback keeps that menu. An allocated
  but uninitialized Mario state no longer counts as an active player.
- The C/Rust camera and containing frame layouts match in a compiled ABI
  comparison; this also corrected the Rust mirror's missing `forward` and
  `orthographic` fields.

## Running the current checks

Provide a local `.sm64` setup pointing to a supported ROM and the current
FrameTee runtime (older exploratory libraries without `-Bsymbolic` can crash
inside the engine due to symbol collisions).

```sh
cmake -S . -B build -DFRAMETEE_SM64_TEST_SETUP=/absolute/path/test.sm64
cmake --build build --target frametee sm64_simulation_test simulation_probe -j6
ctest --test-dir build -R '^sm64_' --output-on-failure
```

For a machine with Vulkan presentation and a display, add
`-DFRAMETEE_SM64_GPU_TESTS=ON`. The render test checks a nonblank viewport and
identical first/repeated paused frames. A direct invocation is:

```sh
python3 tests/sm64_render.py --exe build/frametee --setup /absolute/path/test.sm64
```

Use `--editor-cameras` with a rebuilt runtime to include Freecam and Top-down.
The CPU camera checks skip explicitly on legacy runtimes without scene hooks;
passing those legacy tests does not verify camera support. On a camera
round-trip failure, `SM64_TEST_ARTIFACT_DIR` saves diagnostic PPM pairs.

Current manual evidence used the US runtime with Castle Grounds and Bob-omb
Battlefield. Address/undefined sanitizers passed the simulation test with leak
detection disabled because LeakSanitizer cannot run under the sandbox's ptrace
environment. Native Mario layout assertions also passed compilation against
the vendored headers.

The current CPU isolation/camera/save suite passes on US, JP and EU Linux
runtimes. All six native libraries compile for Linux and Windows, including
the EU startup correction. Windows execution and the updated encrypted release
bundles are not yet verified/staged. These tests use separate scratch runtime
paths and do not overwrite a user's cached runtime or existing saves.

## Remaining completion requirements

- Verify prediction overlays through actual editor interactions, including
  occlusion: the composited native scene does not yet share its depth buffer
  with the engine's overlay primitives. Verify camera behavior during cutscenes
  and area transitions, and additional aspect ratios and regional lenses.
- Implement collision visualization (currently a setting with no backend
  behavior), and decide how inactive timeline groups appear in 3D.
- Verify starting overrides through the actual editor, including immediate
  rendered transforms. Current edits modify simulation state; the existing
  display list may still contain the previous transform until a step.
- Make project level/setup data portable and independent of an external cache
  setup path. `level_serialize` / `level_load_memory` are not implemented.
- Correct course-start M64 export semantics: setup warps and skip-intro changes
  cannot be reproduced by the exported controller prefix alone. Explicit
  state edits now reject M64 export instead of silently dropping them.
- Verify snippet editing, recording/key conflicts, all 3D modes, prediction
  rules, undo/redo, full project round trips, and start overrides together.
- Verify splash course cards at narrower widths and additional UI scales.
- Benchmark CPU/GPU frame time and snapshot/branch throughput on representative
  courses, including moving cameras and long timelines. GPU error handling,
  device resource recreation, and larger geometry/upload buffers need review.
- Execute the Windows regional tests and stage verified encrypted release
  bundles after native hooks settle. The Windows unlock-tool dependency build
  also needs attention: its libsodium archive download was not a valid gzip.
  EU's playback rate still needs review because the module currently declares
  a fixed 30 ticks/second.
