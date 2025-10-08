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

add_library(${PROJECT_NAME} SHARED
    physics_profiler.cpp
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    TRACY_ENABLE
    TRACY_ON_DEMAND
    "TRACY_NO_SAMPLING=0"
    "TRACY_CALLSTACK=10"
)

if(OpenMP_FOUND)
    target_compile_options(${PROJECT_NAME} PRIVATE ${OpenMP_CXX_FLAGS})
endif()


target_include_directories(${PROJECT_NAME} PRIVATE
    "${tracy_SOURCE_DIR}/public"
    "${HOST_APP_DIR}/src"
    "${HOST_APP_DIR}/src/plugins"
    "${HOST_APP_DIR}/libs/cimgui"
    "${HOST_APP_DIR}/libs/cimgui/imgui"
    "${HOST_APP_DIR}/libs/ddnet_physics/include"
    "${HOST_APP_DIR}/libs/cglm/include"
)

set(CMAKE_C_FLAGS_DEBUG "-g -ffast-math -DDEBUG -fsanitize=address,undefined")
set(CMAKE_C_FLAGS_RELEASE "-g -O3 -ffast-math -funroll-loops -mfpmath=sse -fomit-frame-pointer -fno-trapping-math -fno-signed-zeros")
set(CMAKE_CXX_FLAGS_DEBUG "-g -ffast-math -DDEBUG -fsanitize=address,undefined")
set(CMAKE_CXX_FLAGS_RELEASE "-g -O3 -ffast-math -funroll-loops -mfpmath=sse -fomit-frame-pointer -fno-trapping-math -fno-signed-zeros")

if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        "${HOST_APP_DIR}/build/$<IF:$<BOOL:${CMAKE_CONFIGURATION_TYPES}>,$<CONFIG>,${CMAKE_BUILD_TYPE}>/ddnet_frametee.lib"
        DbgHelp
        Tracy::TracyClient
    )
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE Tracy::TracyClient)
endif()

get_filename_component(HOST_APP_ABS_DIR ${HOST_APP_DIR} REALPATH BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:${PROJECT_NAME}>
        "${HOST_APP_ABS_DIR}/build/plugins"
    VERBATIM
)
