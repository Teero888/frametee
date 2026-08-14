# The 3D reference game. Pure C against the ABI header: no renderer, no shaders
# and no graphics library, because a 3D game gets depth-tested primitives from
# the engine the same way a 2D one gets rects and circles.

cmake_minimum_required(VERSION 3.14)

add_library(example_cube_game MODULE cube_game.c)

set_target_properties(example_cube_game PROPERTIES
    PREFIX ""
    OUTPUT_NAME "example_cube"
    C_STANDARD 11
    C_VISIBILITY_PRESET hidden
)

target_include_directories(example_cube_game PRIVATE
    ${FRAMETEE_ABI_INCLUDE_DIR}
    # Only the headers: the ig* symbols resolve against the host, as they do for
    # every other module that draws its own panels.
    ${CMAKE_SOURCE_DIR}/libs/cimgui
    ${CMAKE_SOURCE_DIR}/libs/cimgui/imgui
)

target_compile_definitions(example_cube_game PRIVATE CIMGUI_DEFINE_ENUMS_AND_STRUCTS)

# Windows resolves every symbol at link time, so the ig* calls above cannot be
# left for the loader the way they are on ELF.
if(WIN32 AND TARGET frametee)
    target_link_libraries(example_cube_game PRIVATE frametee)
endif()

if(NOT MSVC)
    target_compile_options(example_cube_game PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
