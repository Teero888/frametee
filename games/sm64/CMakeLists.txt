# Super Mario 64 game modules.
#
# Shared libraries built against the ABI header around libsm64_physics, the
# whole game (libs/sm64_physics). One module per game version, each with the
# library of its version: "sm64" (US), "sm64_jp", "sm64_eu" and "sm64_sh"
# (the Shindou Edition). Off by default: the game needs the user's ROM.
cmake_minimum_required(VERSION 3.14)

option(FRAMETEE_BUILD_SM64 "Build the native SM64 game module" ON)
if(NOT FRAMETEE_BUILD_SM64)
    return()
endif()

# One copy of the game at runtime for a module and every plugin written for
# it: a world holds addresses of the library's code, so a world the engine
# hands a plugin only works with the same copy. The top level adds the
# libraries (with SM64_PHYSICS_SHARED) before the games.
if(NOT TARGET sm64_physics_us OR NOT TARGET sm64_physics_jp OR NOT TARGET sm64_physics_eu
   OR NOT TARGET sm64_physics_sh)
    message(FATAL_ERROR "games/sm64 needs libs/sm64_physics (FRAMETEE_BUILD_SM64 at the top level)")
endif()

# The shaders of the Fast3D interpreter's backend, as SPIR-V in headers.
find_package(Vulkan REQUIRED)
set(SM64_SHADER_HEADERS "")
foreach(stage vert frag)
    string(SUBSTRING "${stage}" 0 1 first)
    string(TOUPPER "${first}" first)
    string(SUBSTRING "${stage}" 1 -1 rest)
    set(header "${CMAKE_CURRENT_BINARY_DIR}/f3d_${stage}_spv.h")
    list(APPEND SM64_SHADER_HEADERS "${header}")
    add_custom_command(OUTPUT "${header}"
        COMMAND "${GLSLANG_VALIDATOR}" -V --vn "kF3d${first}${rest}Spv"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/f3d.${stage}" -o "${header}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/f3d.${stage}"
        VERBATIM)
endforeach()
add_custom_target(sm64_shaders DEPENDS ${SM64_SHADER_HEADERS})

foreach(version us jp eu sh)
    if(version STREQUAL "us")
        set(game_id "sm64")
    else()
        set(game_id "sm64_${version}")
    endif()
    # A game frame is two vertical interrupts: NTSC at 60, PAL (EU) at 50.
    if(version STREQUAL "eu")
        set(ticks_per_second 25)
    else()
        set(ticks_per_second 30)
    endif()
    string(TOUPPER "${version}" upper)
    set(game_name "Super Mario 64 (${upper})")
    if(version STREQUAL "sh")
        set(game_name "Super Mario 64 Shindou Edition")
    endif()

    # Drawing: the Fast3D interpreter (f3d/) and its Vulkan backend, which
    # draws a frame's display list on a device the caller hands over. A
    # library of its own so the module and tests/sm64_render share it.
    add_library(sm64_render_${version} STATIC f3d/f3d.c sm64_vulkan.c)
    add_dependencies(sm64_render_${version} sm64_shaders)
    set_target_properties(sm64_render_${version} PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON
                          C_VISIBILITY_PRESET hidden)
    target_include_directories(sm64_render_${version} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_include_directories(sm64_render_${version} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(sm64_render_${version} PUBLIC sm64_physics_${version} sm64_physics_gbi_${version} Vulkan::Vulkan m)
    if(NOT MSVC)
        target_compile_options(sm64_render_${version} PRIVATE -Wall -Wno-unused-parameter -Wno-unused-variable
                               -Wno-missing-braces)
    endif()

    # <id>_game, as every game's module: the build copies data/games/<id>/
    # only for a module that is part of it.
    add_library(${game_id}_game MODULE sm64_game.c)
    set_target_properties(${game_id}_game PROPERTIES
        PREFIX ""
        OUTPUT_NAME "${game_id}"
        C_STANDARD 11
        C_VISIBILITY_PRESET hidden
    )
    target_include_directories(${game_id}_game PRIVATE
        "${FRAMETEE_ABI_INCLUDE_DIR}"
        "${CMAKE_SOURCE_DIR}/libs/cimgui"
        "${CMAKE_SOURCE_DIR}/libs/cimgui/imgui"
    )
    # The public header for SM64 plugins: the layout of a world.
    target_include_directories(${game_id}_game PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_definitions(${game_id}_game PRIVATE CIMGUI_DEFINE_ENUMS_AND_STRUCTS
                               SM64_GAME_ID="${game_id}" SM64_GAME_NAME="${game_name}"
                               SM64_TICKS_PER_SECOND=${ticks_per_second})
    target_link_libraries(${game_id}_game PRIVATE sm64_render_${version} sm64_physics_${version} m)

    if(UNIX AND NOT APPLE)
        # The libraries are installed in libs/.
        set_target_properties(${game_id}_game PROPERTIES INSTALL_RPATH "$ORIGIN/../libs")
    endif()
    if(WIN32 AND TARGET frametee)
        target_link_libraries(${game_id}_game PRIVATE frametee)
    endif()
    if(NOT MSVC)
        target_compile_options(${game_id}_game PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
    install(TARGETS ${game_id}_game
        RUNTIME DESTINATION games COMPONENT Runtime
        LIBRARY DESTINATION games COMPONENT Runtime)
    # Windows finds a module's DLLs beside it (the engine loads modules with
    # LOAD_WITH_ALTERED_SEARCH_PATH); ELF through the module's run path.
    install(TARGETS sm64_physics_${version}
        RUNTIME DESTINATION games COMPONENT Runtime
        LIBRARY DESTINATION libs COMPONENT Runtime)
    if(WIN32)
        set_target_properties(sm64_physics_${version} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/games")
    endif()
endforeach()

# Every module reads its assets from data/games/<id>/: the other versions get
# a copy of the US one's (the compiled shaders, the thumbnail).
add_custom_target(sm64_version_data ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_BINARY_DIR}/data/games/sm64"
            "${CMAKE_BINARY_DIR}/data/games/sm64_jp"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_BINARY_DIR}/data/games/sm64"
            "${CMAKE_BINARY_DIR}/data/games/sm64_eu"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_BINARY_DIR}/data/games/sm64"
            "${CMAKE_BINARY_DIR}/data/games/sm64_sh"
    VERBATIM)

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/" DESTINATION "include" COMPONENT Development)
