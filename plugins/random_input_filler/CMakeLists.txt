cmake_minimum_required(VERSION 3.10)
project(random_input_filler_plugin C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED TRUE)

set(HOST_APP_DIR "" CACHE PATH "Path to the FrameTee project root")
if(NOT HOST_APP_DIR)
    message(FATAL_ERROR "Please provide the path to the host application root via -DHOST_APP_DIR=../..")
endif()

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
    "${HOST_APP_DIR}/include"
    "${HOST_APP_DIR}/src/plugins"
    "${HOST_APP_DIR}/libs/cglm/include"
    "${HOST_APP_DIR}/libs/cimgui"
    "${HOST_APP_DIR}/libs/cimgui/imgui"
)

target_compile_options(${PROJECT_NAME} PRIVATE
    $<$<AND:$<COMPILE_LANGUAGE:C>,$<OR:$<C_COMPILER_ID:GNU>,$<C_COMPILER_ID:Clang>>>:-Wall;-pedantic-errors>
    $<$<C_COMPILER_ID:MSVC>:/W4>
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME random_input_filler
)

install(TARGETS ${PROJECT_NAME}
    LIBRARY DESTINATION "${HOST_APP_DIR}/build/plugins"
)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> "${HOST_APP_DIR}/build/plugins/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
    )
endif()

if(WIN32)
    if(TARGET frametee)
        target_link_libraries(${PROJECT_NAME} PRIVATE frametee)
    else()
        target_link_libraries(${PROJECT_NAME} PRIVATE
            "${HOST_APP_DIR}/build/$<IF:$<BOOL:${CMAKE_CONFIGURATION_TYPES}>,$<CONFIG>,${CMAKE_BUILD_TYPE}>/frametee.lib"
        )
    endif()
endif()
