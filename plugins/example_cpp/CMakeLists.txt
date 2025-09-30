cmake_minimum_required(VERSION 3.10)
# The project is now just CXX (C++).
project(api_example_cpp_plugin CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)

set(HOST_APP_DIR "" CACHE PATH "Path to the root of the ddnet_frametee project")
if(NOT HOST_APP_DIR)
    message(FATAL_ERROR "Please provide the path to the host application root via -DHOST_APP_DIR=../..")
endif()

add_library(${PROJECT_NAME} SHARED
    example.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE
    "${HOST_APP_DIR}/src"
    "${HOST_APP_DIR}/src/plugins"
    "${HOST_APP_DIR}/src/user_interface"
    "${HOST_APP_DIR}/libs/cglm/include"
    "${HOST_APP_DIR}/libs/cimgui"
    "${HOST_APP_DIR}/libs/cimgui/imgui"
)

install(TARGETS ${PROJECT_NAME}
    LIBRARY DESTINATION "${HOST_APP_DIR}/build/plugins"
)

# auto install on build
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> "${HOST_APP_DIR}/build/plugins/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
)
