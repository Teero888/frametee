# DDNet game module.
#
# A shared library built against the ABI header. It links ddnet_physics because
# that is this game's physics, cglm for its own maths and stb for image decoding;
# the engine neither knows nor cares what a module links.

cmake_minimum_required(VERSION 3.14)

# The physics is ddnet_physics: its optimized backend, with its event build
# (ddnet_ev_*) for the worlds whose effects are shown, heard or exported.
#
# One copy of it at runtime rather than one per binary: game modules are
# dlopened privately, so anything that wants this game's physics would link its
# own static copy, with its own count of worlds that ddnet_world_copy relies on.
# A shared library gives the module and every plugin written for this game the
# same one. It exports only its own functions (ddnet_*).
#
# Windows keeps the static arrangement.
set(FRAMETEE_DDNET_PHYSICS_DIR "${CMAKE_SOURCE_DIR}/libs/ddnet_physics" CACHE PATH
    "The ddnet_physics checkout the DDNet game is built with")
if(NOT TARGET ddnet_physics)
    set(DDNET_PHYSICS_BACKEND optimized CACHE STRING "" FORCE)
    set(DDNET_PHYSICS_EVENTS ON CACHE BOOL "" FORCE)
    set(DDNET_PHYSICS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(DDNET_PHYSICS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    if(NOT WIN32)
        set(DDNET_PHYSICS_SHARED ON CACHE BOOL "" FORCE)
    endif()
    # The CPUs the old physics was built for already (AVX2 and FMA): the
    # library's SSE4.1 and AVX paths, with the same results.
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64" AND NOT MSVC)
        set(DDNET_PHYSICS_ARCH "x86-64-v3" CACHE STRING "" FORCE)
    endif()
    add_subdirectory(${FRAMETEE_DDNET_PHYSICS_DIR} ${CMAKE_BINARY_DIR}/libs/ddnet_physics)
endif()

if(NOT TARGET ddnet_ghost)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(SAVED_SHARED_LIB ${SHARED_LIB})
    set(SHARED_LIB OFF CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_SOURCE_DIR}/libs/ddnet_ghost ${CMAKE_BINARY_DIR}/libs/ddnet_ghost)
    set(SHARED_LIB ${SAVED_SHARED_LIB} CACHE BOOL "" FORCE)
endif()
if(TARGET ddnet_ghost)
    set_target_properties(ddnet_ghost PROPERTIES
        POSITION_INDEPENDENT_CODE ON
    )
endif()

# The map loader is a static library that the module links too (it loads the
# maps it hands the physics), so it has to be usable inside a shared object.
set_target_properties(ddnet_physics PROPERTIES BUILD_RPATH_USE_ORIGIN ON)
if(TARGET ddnet_map_loader)
    set_target_properties(ddnet_map_loader PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

add_library(ddnet_game MODULE
    ddnet_game.c
    dd_input_effects.c
    dd_gfx.c
    dd_text.c
    dd_render.c
    dd_overlays.c
    dd_particles.c
    dd_anim_data.c
    dd_anim_system.c
    dd_map.c
    dd_map_design.c
    dd_map_math.c
    dd_maps.c
    dd_imgui.c
    dd_skin_browser.c
    dd_player_panel.c
    dd_profile.c
    dd_events.c
    dd_audio.c
    dd_export.c
    dd_demo.c
    dd_ghost.c
    dd_recording.c
    dd_recording_impl.c
)

set_target_properties(ddnet_game PROPERTIES
    PREFIX ""
    OUTPUT_NAME "ddnet"
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_VISIBILITY_PRESET hidden
)
if(UNIX AND NOT APPLE)
    set_target_properties(ddnet_game PROPERTIES
        INSTALL_RPATH "$ORIGIN/../libs/ddnet_physics:$ORIGIN/../libs/ddnet_physics/libs/ddnet_map_loader"
    )
endif()

target_include_directories(ddnet_game PRIVATE
    ${FRAMETEE_ABI_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/libs/stb
    ${CMAKE_SOURCE_DIR}/libs/cjson
    ${CMAKE_SOURCE_DIR}/libs
    ${CMAKE_SOURCE_DIR}/libs/ddnet_ghost/include
    # This game draws its own panels with the editor's ImGui. Only the headers
    # are needed: the ig* symbols resolve against the host executable at load
    # time, exactly as the bundled plugins do.
    ${CMAKE_SOURCE_DIR}/libs/cimgui
    ${CMAKE_SOURCE_DIR}/libs/cimgui/imgui
)

target_compile_definitions(ddnet_game PRIVATE CIMGUI_DEFINE_ENUMS_AND_STRUCTS)

find_package(CURL REQUIRED)
find_package(Freetype REQUIRED)
target_link_libraries(ddnet_game PRIVATE ddnet_physics ddnet_ghost cglm CURL::libcurl Freetype::Freetype)
# The demo library is header-only; its reconstruction replays DDNet's physics bit for bit, so its
# file must never be built with anything that changes float results.
if(NOT MSVC)
    set_source_files_properties(dd_recording_impl.c PROPERTIES COMPILE_OPTIONS "-ffp-contract=off;-fno-fast-math")
endif()

# cJSON is compiled straight in; it is one file and the editor's copy is not a
# library a module can link against.
target_sources(ddnet_game PRIVATE ${CMAKE_SOURCE_DIR}/libs/cjson/cJSON.c)

if(NOT MSVC)
    target_compile_options(ddnet_game PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# Windows resolves every symbol at link time, so the ig* calls above cannot be
# left dangling for the loader the way they are on ELF. The module links the
# host executable's import library instead, exactly as the bundled plugins do.
if(WIN32 AND TARGET frametee)
    target_link_libraries(ddnet_game PRIVATE frametee)
endif()
