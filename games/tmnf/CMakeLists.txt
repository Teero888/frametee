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

# One copy of the validator at runtime rather than one per binary.
#
# Game modules are dlopened privately, so a plugin that wants to simulate this
# game cannot borrow the module's validator and would otherwise link its own
# static copy -- sixty megabytes of it, with its own set of every global, vtable
# and typeinfo the library has. Restoring a capture the module produced into a
# sandbox built by a second copy works only because those layouts happen to
# agree; sharing one library removes the question.
#
# The submodule builds static archives only and carries no export decoration, so
# the shared library is assembled here out of them rather than by patching it,
# which is also why this is a POSIX-only arrangement: on Windows nothing in the
# archives is marked for export and the module keeps linking them statically.
if(NOT WIN32)
    set(FV_SHARED_STUB "${CMAKE_CURRENT_BINARY_DIR}/forevervalidator_shared.cpp")
    file(WRITE "${FV_SHARED_STUB}"
        "// Assembled from the validator's archives; see games/tmnf/CMakeLists.txt.\n")

    add_library(forevervalidator_shared SHARED "${FV_SHARED_STUB}")
    add_library(ForeverValidator::shared ALIAS forevervalidator_shared)
    add_dependencies(forevervalidator_shared forevervalidator_native forevervalidator_core)

    # Whole-archive by file rather than by target: the archives have to land
    # inside the flags, in this order, and naming the targets would let the link
    # line's de-duplication move them out from between them.
    target_link_options(forevervalidator_shared PRIVATE
        "-Wl,--whole-archive"
        "$<TARGET_FILE:forevervalidator_native>"
        "$<TARGET_FILE:forevervalidator_core>"
        "-Wl,--no-whole-archive")

    # Linked again by target, privately: that is what carries the archives' own
    # dependencies -- OpenSSL and zlib, whose imported targets belong to the
    # submodule's scope and cannot be named from here. Naming them a second time
    # costs nothing, since the whole-archive pass above has already taken every
    # object out of them.
    target_link_libraries(forevervalidator_shared PRIVATE
        forevervalidator_native
        forevervalidator_core)

    # Private linkage keeps the archives out of a consumer's link line, so the
    # one usage requirement they did carry is restated here.
    target_include_directories(forevervalidator_shared INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/ForeverValidator/include")

    set(TMNF_VALIDATOR_LIBS ForeverValidator::shared)
else()
    set(TMNF_VALIDATOR_LIBS ForeverValidator::native ForeverValidator::core)
endif()

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
    tmnf_material.cpp
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
    # What this game promises its own plugins. The module compiles against the
    # same header they do, so the two cannot disagree about a layout.
    ${CMAKE_CURRENT_SOURCE_DIR}/include
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

target_link_libraries(tmnf_game PRIVATE ${TMNF_VALIDATOR_LIBS})

if(NOT MSVC)
    target_compile_options(tmnf_game PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# On Windows the ig* calls must resolve at link time against the host.
if(WIN32 AND TARGET frametee)
    target_link_libraries(tmnf_game PRIVATE frametee)
endif()
