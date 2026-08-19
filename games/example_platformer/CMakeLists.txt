# Reference game module, written in C++ and backed by raylib.
#
# Note what it does not need: no engine sources, no engine libraries, no Vulkan,
# and no engine implementation. raylib owns this example's game-side math and
# collision, while FrameTee remains the window and renderer host. cimgui is only
# used for the module-owned start screen.

cmake_minimum_required(VERSION 3.14)

option(FRAMETEE_BUILD_PLATFORMER_EXAMPLE "Build the Platformer example game module" OFF)
if(NOT FRAMETEE_BUILD_PLATFORMER_EXAMPLE)
    return()
endif()

find_package(raylib 5.5 QUIET)
if(NOT raylib_FOUND)
    include(FetchContent)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        raylib
        GIT_REPOSITORY https://github.com/raysan5/raylib.git
        GIT_TAG 5.5
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(raylib)
    # The bundled fallback is static and is linked into a loadable game module.
    set_target_properties(raylib PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

add_library(example_platformer_game MODULE
    platformer_game.cpp
)

set_target_properties(example_platformer_game PROPERTIES
    PREFIX ""
    OUTPUT_NAME "example_platformer"
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_VISIBILITY_PRESET hidden
)

target_include_directories(example_platformer_game PRIVATE
    ${FRAMETEE_ABI_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/libs/cimgui
    ${CMAKE_SOURCE_DIR}/libs/cimgui/imgui
)

target_compile_definitions(example_platformer_game PRIVATE CIMGUI_DEFINE_ENUMS_AND_STRUCTS)
target_link_libraries(example_platformer_game PRIVATE raylib)

if(NOT MSVC)
    target_compile_options(example_platformer_game PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# The start screen calls into the editor's ImGui; on Windows those symbols have
# to be resolved at link time against the host executable's import library.
if(WIN32 AND TARGET frametee)
    target_link_libraries(example_platformer_game PRIVATE frametee)
endif()
