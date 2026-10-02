# TrackMania United Forever game module.
#
# A shared library built against the ABI header around libs/tmuf_physics, the
# game's physics in C, bit-exact with TmForever 2.11.26. The library also hands
# over what the game draws as plain data (tmuf_track_visuals); everything
# graphical is done here. At runtime the module needs the game's Packs and
# GameData under data/games/tmuf beside the executable.

cmake_minimum_required(VERSION 3.14)

option(FRAMETEE_BUILD_TMUF "Build the TrackMania United Forever game module" ON)
if(NOT FRAMETEE_BUILD_TMUF)
    return()
endif()

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/libs/tmuf_physics/CMakeLists.txt")
    message(WARNING "FRAMETEE_BUILD_TMUF is ON but libs/tmuf_physics is missing. "
                    "Run: git submodule update --init --recursive")
    return()
endif()

# The optimized backend: bit-identical to the reference, about twice as fast.
if(NOT TARGET tmuf_physics)
    set(TMUF_PHYSICS_BACKEND optimized CACHE STRING "" FORCE)
    set(TMUF_PHYSICS_TOOLS OFF CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_SOURCE_DIR}/libs/tmuf_physics ${CMAKE_BINARY_DIR}/libs/tmuf_physics)
endif()
set_target_properties(tmuf_physics PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The shaders, as SPIR-V in headers.
find_package(Vulkan REQUIRED)
set(TMUF_SHADER_HEADERS "")
foreach(shader track_vert track_frag block_vert block_frag fence_vert fence_frag water_vert water_frag car_vert car_frag hud_vert hud_frag present_vert present_frag blit_vert blit_frag post_vert post_frag clouds_vert clouds_frag shadow_vert shadow_frag lfm_vert lfm_frag flare_vert flare_frag groundrefl_vert groundrefl_frag pssm_vert pssm_frag pssmsolid_frag depthonly_vert depthonly_frag aodepth_vert aodepth_frag ssao_frag aoblur_frag aoquad_vert particle_vert particle_frag bakecast_vert bakecast_frag bakerecv_vert bakerecv_frag bakefinish_frag bakequad_vert grassmark_frag)
    string(REPLACE "_" "." source "${shader}")
    set(header "${CMAKE_CURRENT_BINARY_DIR}/${shader}_spv.h")
    list(APPEND TMUF_SHADER_HEADERS "${header}")
    add_custom_command(OUTPUT "${header}"
        COMMAND "${GLSLANG_VALIDATOR}" -V --vn "k_${shader}_spv"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${source}" -o "${header}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${source}"
        VERBATIM)
endforeach()
# the depth export, from a single and a multisampled depth
foreach(variant pack pack_ms)
    set(header "${CMAKE_CURRENT_BINARY_DIR}/${variant}_frag_spv.h")
    list(APPEND TMUF_SHADER_HEADERS "${header}")
    set(defines "")
    if(variant STREQUAL "pack_ms")
        set(defines -DMULTISAMPLED)
    endif()
    add_custom_command(OUTPUT "${header}"
        COMMAND "${GLSLANG_VALIDATOR}" -V ${defines} --vn "k_${variant}_frag_spv"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/pack.frag" -o "${header}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/pack.frag"
        VERBATIM)
endforeach()
add_custom_target(tmuf_shaders DEPENDS ${TMUF_SHADER_HEADERS})

add_library(tmuf_game MODULE
    tmuf_game.c
    tmuf_gpu.c
    tmuf_hud.c
    tmuf_post.c
    tmuf_weather.c
    tmuf_clouds.c
    tmuf_shadow.c
    tmuf_flares.c
    tmuf_scene.c
    tmuf_image.c
    tmuf_camera.c
    tmuf_car.c
    tmuf_replay.c
    tmuf_ui.c
    tmuf_export.c
    tmuf_imgui.c
    tmuf_video.c
    tmuf_marks.c
    tmuf_particles.c
    tmuf_leaves.c
    tmuf_bake.c
    tmuf_pssm.c
    tmuf_ssao.c
)
add_dependencies(tmuf_game tmuf_shaders)

set_target_properties(tmuf_game PROPERTIES
    PREFIX ""
    OUTPUT_NAME "tmuf"
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_VISIBILITY_PRESET hidden
)

target_include_directories(tmuf_game PRIVATE
    ${FRAMETEE_ABI_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/libs/stb
    # The module draws its own panels with the editor's ImGui. Only the
    # headers: the ig* symbols resolve against the host executable at load time.
    ${CMAKE_SOURCE_DIR}/libs/cimgui
    ${CMAKE_SOURCE_DIR}/libs/cimgui/imgui
)
target_compile_definitions(tmuf_game PRIVATE CIMGUI_DEFINE_ENUMS_AND_STRUCTS)
target_link_libraries(tmuf_game PRIVATE tmuf_physics Vulkan::Vulkan)
# the game's Bink videos (the Stadium's arrow signs), through the FFmpeg the
# editor exports videos with
if(TARGET PkgConfig::FRAMETEE_VIDEO_FFMPEG)
    target_link_libraries(tmuf_game PRIVATE PkgConfig::FRAMETEE_VIDEO_FFMPEG)
    target_compile_definitions(tmuf_game PRIVATE TM_HAS_FFMPEG=1)
endif()
if(NOT MSVC)
    target_link_libraries(tmuf_game PRIVATE m)
    target_compile_options(tmuf_game PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# Windows resolves every symbol at link time, so the ig* calls cannot be left
# for the loader the way they are on ELF.
if(WIN32 AND TARGET frametee)
    target_link_libraries(tmuf_game PRIVATE frametee)
endif()
