cmake_minimum_required(VERSION 3.22)
# Do not build this module by default, as it is still in development and not fully functional yet. It is also not a part of the official Frametee release.
option(FRAMETEE_BUILD_SM64 "Build the native SM64 game module" OFF)
if(NOT FRAMETEE_BUILD_SM64)
    return()
endif()

add_library(sm64_game MODULE
    sm64_game.cpp
    sm64_backend.cpp
    sm64_vulkan.cpp
    fast3d/gfx_pc.c
)

set(SM64_SHADER_HEADERS "")
foreach(SM64_SHADER_STAGE vert frag)
    if(SM64_SHADER_STAGE STREQUAL "vert")
        set(SM64_SHADER_SYMBOL kFast3dVertSpv)
    else()
        set(SM64_SHADER_SYMBOL kFast3dFragSpv)
    endif()
    set(SM64_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/fast3d_${SM64_SHADER_STAGE}_spv.h")
    list(APPEND SM64_SHADER_HEADERS "${SM64_SHADER_HEADER}")
    add_custom_command(OUTPUT "${SM64_SHADER_HEADER}"
        COMMAND "${GLSLANG_VALIDATOR}" -V --vn "${SM64_SHADER_SYMBOL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fast3d.${SM64_SHADER_STAGE}" -o "${SM64_SHADER_HEADER}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fast3d.${SM64_SHADER_STAGE}"
        VERBATIM)
endforeach()

add_custom_target(sm64_fast3d_shaders DEPENDS ${SM64_SHADER_HEADERS})
add_dependencies(sm64_game sm64_fast3d_shaders)

set_target_properties(sm64_game PROPERTIES PREFIX "" OUTPUT_NAME "sm64" CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_VISIBILITY_PRESET hidden)
target_include_directories(sm64_game PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${FRAMETEE_ABI_INCLUDE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/fast3d"
    "${CMAKE_SOURCE_DIR}/libs/cimgui"
    "${CMAKE_SOURCE_DIR}/libs/cimgui/imgui"
    "${CMAKE_SOURCE_DIR}/libs/sm64_physics/include"
    "${CMAKE_SOURCE_DIR}/libs/sm64_physics/upstream/include"
)
target_include_directories(sm64_game PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(sm64_game PRIVATE CIMGUI_DEFINE_ENUMS_AND_STRUCTS _LANGUAGE_C)

find_package(Threads REQUIRED)
find_package(Vulkan REQUIRED)
target_link_libraries(sm64_game PRIVATE sm64_physics_core Threads::Threads Vulkan::Vulkan ${CMAKE_DL_LIBS})

if(WIN32)
    target_link_libraries(sm64_game PRIVATE frametee ws2_32 userenv bcrypt ntdll opengl32 gdi32 setupapi cfgmgr32)
elseif(APPLE)
    target_link_libraries(sm64_game PRIVATE "-framework Metal" "-framework QuartzCore" "-framework Foundation")
else()
    target_link_libraries(sm64_game PRIVATE m)
    target_compile_options(sm64_game PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

install(TARGETS sm64_game RUNTIME DESTINATION games COMPONENT Runtime LIBRARY DESTINATION games COMPONENT Runtime)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/" DESTINATION "include" COMPONENT Development)
