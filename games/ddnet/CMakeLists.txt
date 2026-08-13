# DDNet game module.
#
# A shared library built against the ABI header. It links ddnet_physics because
# that is this game's physics, cglm for its own maths and stb for image decoding;
# the engine neither knows nor cares what a module links.

cmake_minimum_required(VERSION 3.14)

if(NOT TARGET ddnet_physics)
    add_subdirectory(${CMAKE_SOURCE_DIR}/libs/ddnet_physics ${CMAKE_BINARY_DIR}/libs/ddnet_physics)
endif()

# The module links the physics library statically, so its objects must be
# usable inside a shared library.
set_target_properties(ddnet_physics PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
    target_compile_options(ddnet_physics PRIVATE -ftls-model=initial-exec)
endif()
if(WIN32)
    get_target_property(DDNET_PHYSICS_TYPE ddnet_physics TYPE)
    if(DDNET_PHYSICS_TYPE STREQUAL "SHARED_LIBRARY")
        target_compile_definitions(ddnet_physics PRIVATE "DDNET_PHYSICS_API=__declspec(dllexport)")
        target_compile_definitions(ddnet_physics INTERFACE "DDNET_PHYSICS_API=__declspec(dllimport)")
    else()
        set_target_properties(ddnet_physics PROPERTIES COMPILE_DEFINITIONS "")
        target_compile_definitions(ddnet_physics PUBLIC "DDNET_PHYSICS_API=__declspec(dllexport)")
    endif()
endif()

add_library(ddnet_game MODULE
    ddnet_game.c
    dd_gfx.c
    dd_render.c
    dd_particles.c
    dd_anim_data.c
    dd_anim_system.c
    dd_map.c
    dd_maps.c
    dd_imgui.c
    dd_skin_browser.c
    dd_demo.c
)

set_target_properties(ddnet_game PROPERTIES
    PREFIX ""
    OUTPUT_NAME "ddnet"
    C_STANDARD 11
    C_VISIBILITY_PRESET hidden
)

target_include_directories(ddnet_game PRIVATE
    ${FRAMETEE_ABI_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/libs/stb
    ${CMAKE_SOURCE_DIR}/libs/cjson
    ${CMAKE_SOURCE_DIR}/libs
    # This game draws its own panels with the editor's ImGui. Only the headers
    # are needed: the ig* symbols resolve against the host executable at load
    # time, exactly as the bundled plugins do.
    ${CMAKE_SOURCE_DIR}/libs/cimgui
    ${CMAKE_SOURCE_DIR}/libs/cimgui/imgui
)

target_compile_definitions(ddnet_game PRIVATE CIMGUI_DEFINE_ENUMS_AND_STRUCTS)

find_package(CURL REQUIRED)
target_link_libraries(ddnet_game PRIVATE ddnet_physics cglm CURL::libcurl)

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
