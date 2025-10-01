cmake_minimum_required(VERSION 3.10)
project(random_input_filler_plugin C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED TRUE)

set(HOST_APP_DIR "" CACHE PATH "Path to the root of the ddnet_frametee project")
if(NOT HOST_APP_DIR)
    message(FATAL_ERROR "Please provide the path to the host application root via -DHOST_APP_DIR=../..")
endif()

find_package(Vulkan REQUIRED)

set(PLUGIN_SOURCES
    random_input_filler.c
)

add_library(${PROJECT_NAME} SHARED
    ${PLUGIN_SOURCES}
)

set_source_files_properties(${PLUGIN_SOURCES} PROPERTIES
    COMPILE_DEFINITIONS CIMGUI_DEFINE_ENUMS_AND_STRUCTS
)

target_include_directories(${PROJECT_NAME} PRIVATE
    "${HOST_APP_DIR}/src"
    "${HOST_APP_DIR}/src/plugins"
    "${HOST_APP_DIR}/src/user_interface"
    "${HOST_APP_DIR}/libs/cglm/include"
    "${HOST_APP_DIR}/libs/cimgui"
    "${HOST_APP_DIR}/libs/cimgui/imgui"
    "${HOST_APP_DIR}/libs/ddnet_physics/include"
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Vulkan::Vulkan
)

target_compile_options(${PROJECT_NAME} PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Wall;-pedantic-errors>
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME random_input_filler
)

install(TARGETS ${PROJECT_NAME}
    LIBRARY DESTINATION "${HOST_APP_DIR}/build/plugins"
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> "${HOST_APP_DIR}/build/plugins/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
)

if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        "${HOST_APP_DIR}/build/$<IF:$<BOOL:${CMAKE_CONFIGURATION_TYPES}>,$<CONFIG>,${CMAKE_BUILD_TYPE}>/ddnet_frametee.lib"
    )
endif()
