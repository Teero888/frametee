cmake_minimum_required(VERSION 3.16)
project(physics_profiler_plugin CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)

if(NOT HOST_APP_DIR)
    message(FATAL_ERROR "Please provide the path to the host application root via -DHOST_APP_DIR=../..")
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(TRACY_ENABLE "Enable Tracy profiler" ON)
option(TRACY_ON_DEMAND "Enable on-demand connection" ON)
# Not a preference. Tracy's default lifetime is the shared object's: the client
# starts its worker threads while this library attaches and joins them while it
# detaches, and on Windows both run under the loader lock, which a thread needs
# to start or exit. The host loads and unloads every plugin it finds just to
# read its name, so that deadlock hung the editor on startup whenever this
# plugin was present. Manual lifetime hands both ends to plugin_init and
# plugin_shutdown; see physics_profiler.cpp. Tracy defines this PUBLIC on
# TracyClient, so the plugin's own translation unit agrees with the client on
# the config-mangled symbol names.
option(TRACY_MANUAL_LIFETIME "Start and stop the Tracy client from the plugin's entry points" ON)

include(FetchContent)
FetchContent_Declare(
    tracy
    GIT_REPOSITORY https://github.com/wolfpld/tracy.git
    GIT_TAG master
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(tracy)

find_package(OpenMP)
find_package(Vulkan REQUIRED)

add_library(${PROJECT_NAME} SHARED
    physics_profiler.cpp
)

if(UNIX AND NOT APPLE)
    # Relative to plugins/<name>/, which is two levels below the install root.
    set_target_properties(${PROJECT_NAME} PROPERTIES
        INSTALL_RPATH "$ORIGIN/../../libs/ddnet_physics:$ORIGIN/../../libs/ddnet_physics/libs/ddnet_map_loader"
    )
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
    Vulkan::Vulkan
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    TRACY_ENABLE
    TRACY_ON_DEMAND
    "TRACY_NO_SAMPLING=0"
    "TRACY_CALLSTACK=10"
)

# The imported target carries the runtime library as well as -fopenmp. Adding
# only the compile flags leaves GOMP_parallel & co. unresolved, which ELF hides
# until dlopen() but a Windows DLL cannot link at all.
if(OpenMP_CXX_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE OpenMP::OpenMP_CXX)
endif()


target_include_directories(${PROJECT_NAME} PRIVATE
    "${tracy_SOURCE_DIR}/public"
    "${HOST_APP_DIR}/src"
    "${HOST_APP_DIR}/include"
    "${HOST_APP_DIR}/games/ddnet/include"
    "${HOST_APP_DIR}/src/plugins"
    "${HOST_APP_DIR}/libs/cimgui"
    "${HOST_APP_DIR}/libs/cimgui/imgui"
    "${HOST_APP_DIR}/libs/ddnet_physics/include"
    "${HOST_APP_DIR}/libs/ddnet_physics/include/ddnet_physics"
    "${HOST_APP_DIR}/libs/ddnet_physics/libs/ddnet_map_loader"
    "${HOST_APP_DIR}/libs/cglm/include"
)

target_compile_options(${PROJECT_NAME} PRIVATE
    $<$<OR:$<C_COMPILER_ID:GNU>,$<C_COMPILER_ID:Clang>>:-Wall -Wextra -pedantic>
    $<$<C_COMPILER_ID:MSVC>:/W4 /wd4324 /wd4244>
    
    $<$<AND:$<CONFIG:Debug>,$<OR:$<C_COMPILER_ID:GNU>,$<C_COMPILER_ID:Clang>>>:-g -DDEBUG>
    $<$<AND:$<CONFIG:Release>,$<OR:$<C_COMPILER_ID:GNU>,$<C_COMPILER_ID:Clang>>>:-g -O3 -funroll-loops -mfpmath=sse -fomit-frame-pointer -fno-trapping-math -fno-signed-zeros>

    $<$<AND:$<BOOL:${ENABLE_SANITIZERS}>,$<CONFIG:Debug>,$<OR:$<C_COMPILER_ID:GNU>,$<C_COMPILER_ID:Clang>>>:-fsanitize=address,undefined>
)

if(WIN32)
    set_source_files_properties(physics_profiler.cpp PROPERTIES
        COMPILE_DEFINITIONS "IMGUI_API=__declspec(dllimport)"
    )
    if(TARGET frametee)
        target_link_libraries(${PROJECT_NAME} PRIVATE
            frametee
            DbgHelp
            Tracy::TracyClient
        )
    else()
        target_link_libraries(${PROJECT_NAME} PRIVATE
            "${HOST_APP_DIR}/build/$<IF:$<BOOL:${CMAKE_CONFIGURATION_TYPES}>,$<CONFIG>,${CMAKE_BUILD_TYPE}>/frametee.lib"
            DbgHelp
            Tracy::TracyClient
        )
    endif()
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE Tracy::TracyClient)
endif()

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    get_filename_component(HOST_APP_ABS_DIR ${HOST_APP_DIR} REALPATH BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:${PROJECT_NAME}>
            "${HOST_APP_ABS_DIR}/build/plugins"
        VERBATIM
    )
endif()

# The engine no longer links any game's physics, so a DDNet-specific plugin
# brings its own copy of the game's library.
if(TARGET ddnet_physics)
    target_link_libraries(${PROJECT_NAME} PRIVATE ddnet_physics)
endif()
