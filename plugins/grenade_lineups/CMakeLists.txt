cmake_minimum_required(VERSION 3.10)
project(grenade_lineups_plugin CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)

if(NOT HOST_APP_DIR)
    message(FATAL_ERROR "Please provide the path to the host application root via -DHOST_APP_DIR=../..")
endif()
get_filename_component(GL_HOST_DIR "${HOST_APP_DIR}" REALPATH BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(GL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

find_package(Threads REQUIRED)

# One source set, a library per backend of DDNet (DD_UNIQUE comes from the physics target).
function(gl_add_plugin target physics)
    add_library(${target} SHARED
        "${GL_SOURCE_DIR}/grenade_lineups.cpp"
        "${GL_SOURCE_DIR}/gl_table.cpp"
        "${GL_SOURCE_DIR}/gl_fast.cpp"
    )
    target_include_directories(${target} PRIVATE
        "${GL_HOST_DIR}/src"
        "${GL_HOST_DIR}/include"
        "${GL_HOST_DIR}/games/ddnet/include"
        "${GL_HOST_DIR}/src/plugins"
        "${GL_HOST_DIR}/libs/cglm/include"
        "${GL_HOST_DIR}/libs/cimgui"
        "${GL_HOST_DIR}/libs/cimgui/imgui"
        "${GL_SOURCE_DIR}"
    )
    # the flight is the physics' float math: no contraction into FMA
    target_compile_options(${target} PRIVATE
        $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-Wall -ffp-contract=off -fno-fast-math>
        $<$<AND:$<CONFIG:Release>,$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>>:-O3>
    )
    target_link_libraries(${target} PRIVATE ${physics} Threads::Threads)
    if(WIN32)
        set_source_files_properties("${GL_SOURCE_DIR}/grenade_lineups.cpp" PROPERTIES
            COMPILE_DEFINITIONS "IMGUI_API=__declspec(dllimport)"
        )
        if(TARGET frametee)
            target_link_libraries(${target} PRIVATE frametee)
        else()
            target_link_libraries(${target} PRIVATE
                "${GL_HOST_DIR}/build/$<IF:$<BOOL:${CMAKE_CONFIGURATION_TYPES}>,$<CONFIG>,${CMAKE_BUILD_TYPE}>/frametee.lib"
            )
        endif()
    endif()
endfunction()

if(NOT TARGET ddnet_physics)
    return()
endif()

gl_add_plugin(grenade_lineups_plugin ddnet_physics)
if(NOT WIN32 AND NOT APPLE)
    set_target_properties(grenade_lineups_plugin PROPERTIES
        INSTALL_RPATH "$ORIGIN/../../libs/ddnet_physics;$ORIGIN/../../libs/ddnet_physics/libs/ddnet_map_loader"
    )
endif()

# In directories of their own: the host names every target of this one after the plugin.
add_subdirectory(unique)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
