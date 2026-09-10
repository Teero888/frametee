#!/usr/bin/env python3
"""Build FrameTee's native, full-game libsm64 from the vendored SM64EX source."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import tempfile
from pathlib import Path


SCRIPT_DIRECTORY = Path(__file__).resolve().parent
ROOT = SCRIPT_DIRECTORY.parents[1]
# Installed FrameTee packages keep the builder, its SM64EX source, and the
# headless platform layer together in data/games/sm64/builder.  The fallback
# keeps the same script convenient to run from the source checkout.
SOURCE = SCRIPT_DIRECTORY / "sm64ex"
if not SOURCE.is_dir():
    SOURCE = ROOT / "libs" / "sm64ex"
HEADLESS_MAIN = SCRIPT_DIRECTORY / "full_game" / "main.c"

ROM_VERSIONS = {
    ("635a2bff", ord("E")): "us",
    ("4eaa3d0e", ord("J")): "jp",
    ("a03cf036", ord("P")): "eu",
}


def normalize_rom(data: bytes) -> bytes:
    if len(data) != 8 * 1024 * 1024:
        raise ValueError("expected an 8 MiB vanilla SM64 ROM")
    if data[:4] == b"\x80\x37\x12\x40":
        return data
    out = bytearray(data)
    if data[:4] == b"\x37\x80\x40\x12":
        for index in range(0, len(out), 2):
            out[index], out[index + 1] = out[index + 1], out[index]
        return bytes(out)
    if data[:4] == b"\x40\x12\x37\x80":
        for index in range(0, len(out), 4):
            out[index:index + 4] = reversed(out[index:index + 4])
        return bytes(out)
    raise ValueError("unrecognized N64 ROM byte order")


def rom_version(data: bytes) -> str:
    version = ROM_VERSIONS.get((data[0x10:0x14].hex(), data[0x3E]))
    if version is None:
        raise ValueError("unsupported ROM; expected original US, JP, or EU SM64")
    return version


def patch_makefile(path: Path) -> None:
    makefile = path.read_text()
    # The temporary copy deliberately has no .git directory.  SM64EX checks
    # for nightly builds at makefile parse time, so silence that optional
    # lookup instead of printing a misleading fatal message during every
    # regular FrameTee build.
    makefile = makefile.replace(
        "$(shell git rev-parse --abbrev-ref HEAD)",
        "$(shell git rev-parse --abbrev-ref HEAD 2>/dev/null)",
    )
    makefile = makefile.replace(
        "$(shell git rev-parse --short HEAD)",
        "$(shell git rev-parse --short HEAD 2>/dev/null)",
    )
    makefile = makefile.replace(
        "SRC_DIRS := src src/engine src/game src/audio src/menu src/buffers actors levels bin data assets src/pc src/pc/gfx src/pc/audio src/pc/controller src/pc/fs src/pc/fs/packtypes\n",
        "SRC_DIRS := src src/engine src/game src/audio src/menu src/buffers actors levels bin data assets src/pc src/pc/gfx src/pc/audio src/pc/controller src/pc/fs src/pc/fs/packtypes\n"
        "ifeq ($(HEADLESS),1)\n"
        "SRC_DIRS += src/pc_headless\n"
        "RENDER_API := NULL\nWINDOW_API := NULL\nAUDIO_API := NULL\nCONTROLLER_API :=\n"
        "endif\n",
    )
    makefile = makefile.replace(
        "C_FILES := $(filter-out src/game/main.c,$(C_FILES))\n",
        "C_FILES := $(filter-out src/game/main.c,$(C_FILES))\n"
        "ifeq ($(HEADLESS),1)\n"
        "C_FILES := $(filter-out src/pc/pc_main.c src/pc/controller/controller_entry_point.c src/pc/gfx/gfx_opengl.c src/pc/gfx/gfx_opengl_legacy.c src/pc/gfx/gfx_sdl.c src/pc/gfx/gfx_sdl1.c src/pc/gfx/gfx_sdl2.c,$(C_FILES))\n"
        "CXX_FILES := $(filter-out src/pc/gfx/gfx_direct3d11.cpp src/pc/gfx/gfx_direct3d12.cpp,$(CXX_FILES))\n"
        "endif\n",
    )
    makefile = makefile.replace(
        "ELF := $(BUILD_DIR)/$(TARGET).elf\n",
        "ifeq ($(HEADLESS),1)\n"
        "ifeq ($(WINDOWS_BUILD),1)\nEXE := $(BUILD_DIR)/frametee_sm64.dll\n"
        "else\nEXE := $(BUILD_DIR)/frametee_sm64.so\n"
        "endif\n"
        "endif\n\nELF := $(BUILD_DIR)/$(TARGET).elf\n",
    )
    makefile = makefile.replace(
        "ASFLAGS := -I include -I $(BUILD_DIR) $(VERSION_ASFLAGS)\n",
        "ifeq ($(HEADLESS),1)\n"
        "CFLAGS += -DFRAMETEE\nifneq ($(WINDOWS_BUILD),1)\nCFLAGS += -fPIC\nendif\n"
        "endif\n\nASFLAGS := -I include -I $(BUILD_DIR) $(VERSION_ASFLAGS)\n",
    )
    makefile = makefile.replace(
        "# Prevent a crash with -sopt\n",
        "ifeq ($(HEADLESS),1)\n"
        "ifeq ($(WINDOWS_BUILD),1)\nLDFLAGS += -shared -Wl,--export-all-symbols\n"
        "else\n"
        "# FrameTee exports engine symbols such as fs_open. Bind internal SM64\n"
        "# calls to this image so incompatible same-named engine functions\n"
        "# cannot interpose when the library is loaded with dlopen.\n"
        "LDFLAGS += -shared -Wl,-z,defs -Wl,-Bsymbolic\n"
        "endif\n"
        "endif\n\n# Prevent a crash with -sopt\n",
    )
    if "src/pc_headless" not in makefile or "frametee_sm64.so" not in makefile or "frametee_sm64.dll" not in makefile:
        raise RuntimeError("SM64EX Makefile layout changed; update build_full_libsm64.py")
    path.write_text(makefile)


def patch_simulation_sources(root: Path) -> None:
    """Apply the small FrameTee-only simulation/presentation split.

    Keep this in the builder as well as in the developer checkout: release
    builds copy a clean SM64EX tree to a temporary directory first.
    """
    game_init = root / "src" / "game" / "game_init.c"
    source = game_init.read_text()
    if "extern int sm64_simulating" not in source:
        source = source.replace('#include "memory.h"\n', '#include "memory.h"\n#ifdef FRAMETEE\nextern int sm64_simulating;\nextern int sm64_starting;\n#endif\n', 1)
        source = source.replace('    audio_game_loop_tick();\n    config_gfx_pool();', '''#ifndef FRAMETEE
    audio_game_loop_tick();
#else
    if (!sm64_simulating) audio_game_loop_tick();
#endif
    config_gfx_pool();''', 1)
        source = source.replace('    levelCommandAddr = level_script_execute(levelCommandAddr);\n    display_and_vsync();', '''    levelCommandAddr = level_script_execute(levelCommandAddr);
#ifdef FRAMETEE
    if (sm64_simulating && !sm64_starting) {
        if (++frameBufferIndex == 3) frameBufferIndex = 0;
        ++gGlobalTimer;
        return;
    }
#endif
    display_and_vsync();''', 1)
        source = source.replace('#endif\n    display_and_vsync();', '#endif\n    if (sm64_starting) {\n        display_and_vsync();\n        return;\n    }\n    display_and_vsync();', 1)
        game_init.write_text(source)

    elif "extern int sm64_starting" not in source:
        source = source.replace("extern int sm64_simulating;", "extern int sm64_simulating;\nextern int sm64_starting;", 1)
    source = source.replace("if (sm64_simulating) {", "if (sm64_simulating && !sm64_starting) {", 1)
    if "if (sm64_starting)" not in source:
        source = source.replace("#endif\n    display_and_vsync();", "#endif\n    if (sm64_starting) {\n        display_and_vsync();\n        return;\n    }\n    display_and_vsync();", 1)
    game_init.write_text(source)

    # A clean SM64EX checkout does not contain the animation gate that lives
    # in the FrameTee vendored checkout. Keep generated runtimes reproducible
    # when the builder is pointed at such a checkout.
    source = game_init.read_text()
    if "extern int sm64_process_animations;" not in source:
        source = source.replace(
            "extern int sm64_starting;",
            "extern int sm64_starting;\nextern int sm64_process_animations;\n"
            "void frametee_update_mario_animation(void);\n"
            "void frametee_update_object_animations(void);",
            1,
        )
    if "if (sm64_process_animations) frametee_update_object_animations();" not in source:
        marker = "    if (sm64_simulating && !sm64_starting) {\n"
        if source.count(marker) != 1:
            raise RuntimeError("SM64EX simulation branch changed; update animation hook")
        source = source.replace(
            marker,
            marker + "        if (sm64_process_animations) frametee_update_object_animations();\n"
            "        else frametee_update_mario_animation();\n",
            1,
        )
    game_init.write_text(source)

    object_header = root / "src" / "game" / "object_list_processor.h"
    source = object_header.read_text()
    if "void frametee_update_object_animations(void);" not in source:
        marker = "\n#endif // OBJECT_LIST_PROCESSOR_H"
        if source.count(marker) != 1:
            raise RuntimeError("SM64EX object-list header changed; update animation hook")
        source = source.replace(
            marker,
            "\n#ifdef FRAMETEE\n"
            "void frametee_update_mario_animation(void);\n"
            "void frametee_update_object_animations(void);\n"
            "#endif\n" + marker,
            1,
        )
    object_source = root / "src" / "game" / "object_list_processor.c"
    object_text = object_source.read_text()
    if "void frametee_update_object_animations(void)" not in object_text:
        marker = "\n/**\n * Unload deactivated objects"
        if object_text.count(marker) != 1:
            raise RuntimeError("SM64EX object-list processor changed; update animation hook")
        object_text = object_text.replace(
            marker,
            """
#ifdef FRAMETEE
static void frametee_update_one_object_animation(struct Object *object) {
    struct GraphNodeObject_sub *animation;

    if (gCurrentArea == NULL || object == NULL
        || object->header.gfx.unk18 != gCurrentArea->index
        || !(object->header.gfx.node.flags & GRAPH_RENDER_HAS_ANIMATION)) {
        return;
    }
    animation = &object->header.gfx.unk38;
    if (animation->curAnim == NULL) return;
    animation->animFrame = geo_update_animation_frame(
        animation, &animation->animFrameAccelAssist);
    animation->animTimer = gAreaUpdateCounter;
}

void frametee_update_mario_animation(void) {
    frametee_update_one_object_animation(gMarioObject);
}

void frametee_update_object_animations(void) {
    s32 listIndex;

    if (gObjectLists == NULL) return;
    for (listIndex = 0; listIndex < NUM_OBJ_LISTS; ++listIndex) {
        struct ObjectNode *list = &gObjectLists[listIndex];
        struct ObjectNode *node = list->next;
        while (node != list) {
            frametee_update_one_object_animation((struct Object *) node);
            node = node->next;
        }
    }
}
#endif
""" + marker,
            1,
        )
    object_header.write_text(source)
    object_source.write_text(object_text)

    level_script = root / "src" / "engine" / "level_script.c"
    source = level_script.read_text()
    if "extern int sm64_simulating" not in source:
        source = source.replace('#include "surface_load.h"\n', '#include "surface_load.h"\n#ifdef FRAMETEE\nextern int sm64_simulating;\nextern int sm64_starting;\n#endif\n', 1)
        source = source.replace('    profiler_log_thread5_time(LEVEL_SCRIPT_EXECUTE);\n    init_render_image();', '''    profiler_log_thread5_time(LEVEL_SCRIPT_EXECUTE);
#ifdef FRAMETEE
    if (sm64_simulating && !sm64_starting) return sCurrentCmd;
#endif
    init_render_image();''', 1)
    elif "extern int sm64_starting" not in source:
        source = source.replace("extern int sm64_simulating;", "extern int sm64_simulating;\nextern int sm64_starting;", 1)
    source = source.replace("#ifdef FRAMETEE\n    if (sm64_simulating && !sm64_starting) return sCurrentCmd;\n#endif\n", "", 1)
    level_script.write_text(source)

    area = root / "src" / "game" / "area.c"
    source = area.read_text()
    if "extern int sm64_simulating" not in source:
        source = source.replace('#include "level_table.h"\n', '#include "level_table.h"\n#ifdef FRAMETEE\nextern int sm64_simulating;\n#endif\n', 1)
    if "Keep menu, cutscene, and transition state machines alive" not in source:
        source = source.replace('void render_game(void) {', 'void render_game(void) {\n#ifdef FRAMETEE\n    if (sm64_simulating) {\n        // Keep menu, cutscene, and transition state machines alive for the\n        // automatic startup path without traversing the render graph.\n        do_cutscene_handler();\n        gPauseScreenMode = render_menus_and_dialogs();\n        if (gWarpTransition.isActive && gWarpTransDelay == 0) {\n            gWarpTransition.isActive = !render_screen_transition(0, gWarpTransition.type, gWarpTransition.time, &gWarpTransition.data);\n        }\n        return;\n    }\n#endif', 1)
    area.write_text(source)

    graph = root / "src" / "game" / "rendering_graph_node.c"
    source = graph.read_text()
    if "extern int sm64_simulating" not in source:
        source = source.replace('#include "sm64.h"\n', '#include "sm64.h"\nvoid geo_try_process_children(struct GraphNode *node);\n#ifdef FRAMETEE\nextern int sm64_simulating;\n#else\n#define sm64_simulating 0\n#endif\n', 1)
        source = source.replace('static void geo_process_master_list_sub(struct GraphNodeMasterList *node) {', 'static void geo_process_master_list_sub(struct GraphNodeMasterList *node) {\n    if (sm64_simulating) return;', 1)
        source = source.replace('static void geo_append_display_list(void *displayList, s16 layer) {', 'static void geo_append_display_list(void *displayList, s16 layer) {\n    if (sm64_simulating) return;', 1)
        source = source.replace('static void geo_process_background(struct GraphNodeBackground *node) {', 'static void geo_process_background(struct GraphNodeBackground *node) {\n    if (sm64_simulating) { geo_try_process_children(&node->fnNode.node); return; }', 1)
        source = source.replace('static void geo_process_shadow(struct GraphNodeShadow *node) {', 'static void geo_process_shadow(struct GraphNodeShadow *node) {\n    if (sm64_simulating) { geo_try_process_children(&node->node); return; }', 1)
        source = source.replace('static void geo_process_object_parent(struct GraphNodeObjectParent *node) {', '#ifdef FRAMETEE\nextern void sm64_draw_ghosts(void (*draw)(struct Object *));\n#endif\nstatic void geo_process_object_parent(struct GraphNodeObjectParent *node) {', 1)
        source = source.replace('        node->sharedChild->parent = NULL;\n    }\n    if (node->node.children != NULL)', '        node->sharedChild->parent = NULL;\n#ifdef FRAMETEE\n        sm64_draw_ghosts(geo_process_object);\n#endif\n    }\n    if (node->node.children != NULL)', 1)
        graph.write_text(source)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom", required=True, type=Path, help="vanilla US, JP, or EU SM64 ROM (.z64, .n64, or .v64)")
    parser.add_argument("--output", required=True, type=Path, help="destination native full-game library")
    parser.add_argument("--target", choices=("linux", "windows"), default="linux", help="native library target")
    parser.add_argument(
        "--normalized-rom-output",
        type=Path,
        help="optional destination for the normalized ROM used by the runtime",
    )
    parser.add_argument("--metadata-output", type=Path, help="optional JSON file with ROM and library SHA-256 hashes")
    parser.add_argument("--source", default=SOURCE, type=Path, help="SM64EX source checkout")
    parser.add_argument("--jobs", type=int, default=None, help="parallel make jobs")
    args = parser.parse_args()

    source = args.source.resolve()
    if not (source / "Makefile").is_file() or not HEADLESS_MAIN.is_file():
        raise SystemExit("SM64EX source or FrameTee headless platform layer is missing")
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    normalized_rom_output = args.normalized_rom_output.resolve() if args.normalized_rom_output else None
    with tempfile.TemporaryDirectory(prefix="frametee-sm64-build-") as temporary:
        work = Path(temporary) / "sm64ex"
        shutil.copytree(source, work, ignore=shutil.ignore_patterns(".git", "build"))
        rom = normalize_rom(args.rom.read_bytes())
        version = rom_version(rom)
        (work / f"baserom.{version}.z64").write_bytes(rom)
        if normalized_rom_output:
            normalized_rom_output.parent.mkdir(parents=True, exist_ok=True)
            normalized_rom_output.write_bytes(rom)
        # SM64EX's EU-only audio port calls audio_reset_session() without
        # including the header that declares its no-argument EU signature.
        # Modern C compilers reject that implicit declaration.
        if version == "eu":
            eu_audio = work / "src" / "audio" / "port_eu.c"
            eu_audio.write_text(eu_audio.read_text().replace('#include "synthesis.h"\n', '#include "synthesis.h"\n#include "heap.h"\n'))
        destination_main = work / "src" / "pc_headless" / "main.c"
        destination_main.parent.mkdir(parents=True)
        shutil.copy2(HEADLESS_MAIN, destination_main)
        shutil.copy2(SCRIPT_DIRECTORY / "include" / "sm64" / "sm64_physics.h", destination_main.with_name("sm64_physics.h"))
        shutil.copy2(HEADLESS_MAIN.with_name("scene.h"), destination_main.with_name("scene.h"))
        patch_simulation_sources(work)
        scene = work / "src" / "game" / "rendering_graph_node.c"
        scene_source = scene.read_text()
        patches = {
            "static void geo_process_ortho_projection(":
                "extern void sm64_scene_projection(Mtx *, u16 *);\n"
                "extern void sm64_scene_view(struct GraphNodeCamera *, Mat4, Mtx *);\n"
                "extern int sm64_scene_object_visible(Mat4, float);\n\n"
                "static void geo_process_ortho_projection(",
            "        gSPPerspNormalize(gDisplayListHead++, perspNorm);":
                "        sm64_scene_projection(mtx, &perspNorm);\n"
                "        gSPPerspNormalize(gDisplayListHead++, perspNorm);",
            "    mtxf_lookat(cameraTransform, node->pos, node->focus, node->roll);":
                "    mtxf_lookat(cameraTransform, node->pos, node->focus, node->roll);\n"
                "    sm64_scene_view(node, cameraTransform, rollMtx);",
            "    // Don't render if the object is close to or behind the camera":
                "    int editor_visible = sm64_scene_object_visible(matrix, cullingRadius);\n"
                "    if (editor_visible >= 0) return editor_visible;\n\n"
                "    // Don't render if the object is close to or behind the camera",
        }
        for original, replacement in patches.items():
            if scene_source.count(original) != 1:
                raise RuntimeError(f"SM64EX scene hook changed: {original}")
            scene_source = scene_source.replace(original, replacement)
        scene.write_text(scene_source)
        # The course browser already requests skip-intro startup. PAL also
        # opens language settings for an empty save file; leave that menu in
        # normal movie playback, but skip it during automatic course startup.
        if version == "eu":
            menu = work / "src" / "menu" / "file_select.c"
            menu_source = menu.read_text()
            original = "            sOpenLangSettings = TRUE;"
            if menu_source.count(original) != 1:
                raise RuntimeError("SM64EX PAL language-menu startup hook changed")
            menu.write_text('#include "pc/configfile.h"\n' + menu_source.replace(
                original, "            sOpenLangSettings = !configSkipIntro;"))
        patch_makefile(work / "Makefile")
        if args.target == "windows":
            # Asset extraction invokes these executables while make parses
            # the main Makefile.  Build them for the build host first; the
            # subsequent Windows build sees them as current and only cross
            # compiles the game library.
            subprocess.run(
                ["make", "-C", "tools", "CC=gcc", "CXX=g++", "-j1"],
                cwd=work,
                check=True,
            )
        command = ["make", "HEADLESS=1", f"VERSION={version}", "COMPARE=0", "NO_PIE=0"]
        extension = ".so"
        if args.target == "windows":
            # SM64EX uses ?= for CC/CXX, so a developer's host compiler in
            # the environment would otherwise win over CROSS.  Pass every
            # tool explicitly to ensure this produces a PE DLL.
            command.extend(
                [
                    "WINDOWS_BUILD=1",
                    "CROSS=x86_64-w64-mingw32-",
                    "CC=x86_64-w64-mingw32-gcc",
                    "CXX=x86_64-w64-mingw32-g++",
                    "LD=x86_64-w64-mingw32-gcc",
                    "CPP=x86_64-w64-mingw32-cpp -P",
                    "OBJCOPY=x86_64-w64-mingw32-objcopy",
                    "TARGET_ARCH=x86-64",
                ]
            )
            extension = ".dll"
        if args.jobs:
            command.append(f"-j{args.jobs}")
        subprocess.run(command, cwd=work, check=True)
        built = work / "build" / f"{version}_pc" / f"frametee_sm64{extension}"
        if not built.is_file():
            raise SystemExit("SM64EX did not produce the native library")
        shutil.copy2(built, output)
    metadata = {
        "format": 1,
        "version": version,
        "target": args.target,
        "rom_sha256": hashlib.sha256(rom).hexdigest(),
        "library_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
        "library": output.name,
    }
    if args.metadata_output:
        args.metadata_output.parent.mkdir(parents=True, exist_ok=True)
        args.metadata_output.write_text(json.dumps(metadata, indent=2) + "\n")
    print(json.dumps(metadata, sort_keys=True))


if __name__ == "__main__":
    main()
