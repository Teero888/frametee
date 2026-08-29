cmake_minimum_required(VERSION 3.10)
# The project is now just CXX (C++).
project(api_example_cpp_plugin CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)

set(HOST_APP_DIR "" CACHE PATH "Path to the FrameTee project root")
if(NOT HOST_APP_DIR)
    message(FATAL_ERROR "Please provide the path to the host application root via -DHOST_APP_DIR=../..")
endif()

add_library(${PROJECT_NAME} SHARED
    example.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE
    "${HOST_APP_DIR}/include"
    "${HOST_APP_DIR}/src/plugins"
    "${HOST_APP_DIR}/libs/cglm/include"
    "${HOST_APP_DIR}/libs/cimgui"
    "${HOST_APP_DIR}/libs/cimgui/imgui"
)

# Installing this subdirectory as part of FrameTee is handled centrally by the
# host. Keep the legacy host-build-tree destination confined to an explicitly
# standalone plugin build so `cmake --install` of FrameTee never writes back
# into its own build directory.
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    install(TARGETS ${PROJECT_NAME}
        RUNTIME DESTINATION "${HOST_APP_DIR}/build/plugins"
        LIBRARY DESTINATION "${HOST_APP_DIR}/build/plugins"
    )
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> "${HOST_APP_DIR}/build/plugins/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
    )
endif()

if(WIN32)
    # On Windows, we must explicitly link against the host executable's
    # import library (.lib) to resolve the ImGui symbols at link-time.
    set_source_files_properties(example.cpp PROPERTIES
        COMPILE_DEFINITIONS "IMGUI_API=__declspec(dllimport)"
    )
    if(TARGET frametee)
        target_link_libraries(${PROJECT_NAME} PRIVATE frametee)
    else()
        target_link_libraries(${PROJECT_NAME} PRIVATE
            "${HOST_APP_DIR}/build/frametee.lib"
        )
    endif()
endif()
