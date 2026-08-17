# TrackMania Nations Forever.
#
# Off by default, and deliberately so: ForeverValidator is over three hundred
# C++17 translation units, which is a large addition to a build that otherwise
# compiles in seconds, and CI has no game data to run it against anyway. Turn it
# on with -DFRAMETEE_BUILD_TMNF=ON.
#
# The module additionally needs an installed TrackMania Packs directory at
# runtime; see FRAMETEE_TMNF_PACKS in tmnf_sandbox.cpp.
#
# ForeverValidator is a submodule pinned to its own upstream and is never
# patched from here: everything this module does goes through its published
# API, so the submodule stays byte for byte what upstream shipped.

cmake_minimum_required(VERSION 3.14)

option(FRAMETEE_BUILD_TMNF "Build the TrackMania Nations Forever game module" ON)
if(NOT FRAMETEE_BUILD_TMNF)
    return()
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ForeverValidator/CMakeLists.txt")
    message(WARNING "FRAMETEE_BUILD_TMNF is ON but games/tmnf/ForeverValidator is missing. "
                    "Run: git submodule update --init --recursive")
    return()
endif()

# Only the libraries; the validator's own CLI, tests and benchmarks are no use
# inside the editor and would roughly double the build.
set(FOREVERVALIDATOR_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(FOREVERVALIDATOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FOREVERVALIDATOR_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
add_subdirectory(ForeverValidator ${CMAKE_BINARY_DIR}/forevervalidator EXCLUDE_FROM_ALL)

# The validator's objects are linked into a loadable module.
set_target_properties(forevervalidator_core forevervalidator_native PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(tmnf_game MODULE
    tmnf_game.cpp
    tmnf_sandbox.cpp
    tmnf_level.cpp
    tmnf_render.cpp
    tmnf_car.cpp
    tmnf_camera.cpp
    tmnf_ui.cpp
    tmnf_vehicle_model.cpp
    tmnf_pack.cpp
    tmnf_texture.cpp
    tmnf_scene.cpp
)

set_target_properties(tmnf_game PROPERTIES
    PREFIX ""
    OUTPUT_NAME "tmnf"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_VISIBILITY_PRESET hidden
)

target_include_directories(tmnf_game PRIVATE
    ${FRAMETEE_ABI_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/libs/cimgui
    ${CMAKE_SOURCE_DIR}/libs/cimgui/imgui
    # Track thumbnails are JPEGs embedded in the challenge files.
    ${CMAKE_SOURCE_DIR}/libs/stb
    # The car's visual model is decoded through the validator's internal scene
    # headers, which it only exposes to itself. Reading them changes nothing in
    # the submodule; see tmnf_vehicle_model.cpp for why this is worth it.
    ${CMAKE_CURRENT_SOURCE_DIR}/ForeverValidator/src
)

target_compile_definitions(tmnf_game PRIVATE CIMGUI_DEFINE_ENUMS_AND_STRUCTS)

target_link_libraries(tmnf_game PRIVATE
    ForeverValidator::native
    ForeverValidator::core
)

if(NOT MSVC)
    target_compile_options(tmnf_game PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# On Windows the ig* calls must resolve at link time against the host.
if(WIN32 AND TARGET frametee)
    target_link_libraries(tmnf_game PRIVATE frametee)
endif()
