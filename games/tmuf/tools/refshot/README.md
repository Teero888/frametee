# Comparing the tmuf module with the game

The real game is the reference renderer. `refcompare.py` renders views with
both and puts them side by side:

    refcompare.py views.txt OUT        # OUT/compare/NAME.png, OUT/report.txt
    refcompare.py views.txt OUT --ours-only   # reuse the game's pictures

Each view (`views.txt`) is a map, an eye, the point it looks at and a vertical
fov. For each map the game plays an idle replay of it (`idle_replay`) through
its own video shooting (`/shootvideo`), under Wine on a private Xvfb display,
with `refshot.dll` injected: it holds the scene camera at each view's pose in
turn and saves the back buffer (`refshot.py`). The module renders the same
views with `TM_TEST_CAMERA`.

Needs the tmuf_physics oracle setup (a Wine prefix with the game installed,
`launcher.exe`), Xvfb, xdotool, ffmpeg, ImageMagick, numpy. Wine must use the
game's own `d3dx9_30.dll` (refshot.py sets it): Wine's builtin one
miscompiles the game's shaders and every lit surface comes out black.

Build:

    i686-w64-mingw32-gcc -O2 -shared -static-libgcc -o refshot.dll refshot.c -ldxguid
    cc -O2 -I ../../../../libs/tmuf_physics/include -o idle_replay idle_replay.c \
        ../../../../build-tmuf/libs/tmuf_physics/libtmuf_physics.a -lm -lz
    cc -O2 -I ../../../../libs/tmuf_physics/include -o spawn spawn.c \
        ../../../../build-tmuf/libs/tmuf_physics/libtmuf_physics.a -lm -lz
    cc -O2 -I ../../../../libs/tmuf_physics/include -o materials materials.c \
        ../../../../build-tmuf/libs/tmuf_physics/libtmuf_physics.a -lm -lz

`spawn MAP...` prints where each map's car starts, for writing views.

## The game's settings

The references are the game at its maximum display settings: the prefix's
`Config/Default.SystemConfig.Gbx` (CSystemConfigDisplay, its body stored
uncompressed so it can be edited) has Antialiasing off (with it, under Wine,
the game leaves the sea out: it needs the scene's depth as a texture), ShaderQuality
4 (PC3 High), TexturesQuality 3, Shadows 5 (Complex), FilterAnisoQ 5 (16x),
PostFxEnable with the bloom and motion blur, WaterGeom(Stadium), and Preset 0
so that the game keeps these values (a preset would recompute them; the
VeryHigh one turns the post effects off on GPUs the game does not know).
LightMapQuality stays 2k, the size of the lightmap caches the game ships.
The names and offsets are the class's parameter table (0xb31268 in
TmForeverFixed.exe). refshot.dll resolves a multisampled back buffer before
reading it back, should antialiasing be turned on.

Draws without a pixel shader also get their fixed-function texture stages in
`draws.txt` ("tss": colour and alpha op and arguments per stage, texcoord
index, transform flags; "tfactor").

## What the game draws

`--trace NAME` also records the game's frame of that view, draw call by draw
call, into `OUT/trace/NAME` (refshot.c): `draws.txt` (per draw its shaders,
textures with their sampler states, render states, render target, vertex
declaration and the non-zero shader constants; clears, StretchRects and
Presents in between), the shaders disassembled by the game's own D3DX
(`vs_N.asm`, `ps_N.asm`; the constant and sampler names come with them) and
the textures (`tex_N.dds`). Every render target is saved when the game leaves
it (`rt_DRAW_sS.bmp|dds`); `--steps` saves the target after every draw
(`step_DRAW.bmp`), which shows what each draw adds. With
`TMUF_REFSHOT_TRACE_VB=1` in the environment each indexed draw's vertices are
saved too (`vb_DRAW.bin`: positions, indices and texcoord 0, in the draw's own
space; for a block drawn alone its world transform is the inverse of a
world-space draw's view-projection constants times its own).

`--trace-lightmap` records the game's lightmap computation of each map (at
load, CHmsPackLightMap::ComputeLighting) the same way into
`OUT/trace/lightmap_MAP`.

`trace_summary.py TRACE` lists the draws; `--draw N` shows one, `--sheet
OUT.png` all the textures, `--changes` (with steps) how many pixels each draw
changed.

`trace_materials.py TRACE MAP` tells which of the map's materials (the
`materials` tool's list) each of the game's shaders draws, matching the
textures a draw binds to the materials' files by their pictures: port a
shader family for exactly its materials.

The game's shader sources are in the packs (classes 0x09074000 VHlsl,
0x09077000 PHlsl, 0x09042000/0x09045000 vsh/psh text), the compiled ones in a
trace name their constants and samplers: together they give the exact
formulas.

Shot videos show no HUD (the race's countdown is ported from
CTrackManiaRace::UpdateCountDownIndex instead).
