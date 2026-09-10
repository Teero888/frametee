cmake_minimum_required(VERSION 3.22)
option(FRAMETEE_BUILD_SM64 "Build the native SM64 game module" ON)
if(NOT FRAMETEE_BUILD_SM64)
    return()
endif()
add_library(sm64_game MODULE sm64_game.cpp sm64_backend.cpp sm64_vulkan.cpp)
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
target_include_directories(sm64_game PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
set_target_properties(sm64_game PROPERTIES PREFIX "" OUTPUT_NAME "sm64" CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_VISIBILITY_PRESET hidden)
target_include_directories(sm64_game PRIVATE "${FRAMETEE_ABI_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/libs/cimgui" "${CMAKE_SOURCE_DIR}/libs/cimgui/imgui")
target_include_directories(sm64_game PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(sm64_game PRIVATE CIMGUI_DEFINE_ENUMS_AND_STRUCTS)
find_package(Threads REQUIRED)
find_package(Vulkan REQUIRED)
target_link_libraries(sm64_game PRIVATE Threads::Threads Vulkan::Vulkan ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(sm64_game PRIVATE frametee ws2_32 userenv bcrypt ntdll opengl32 gdi32 setupapi cfgmgr32)
elseif(APPLE)
    target_link_libraries(sm64_game PRIVATE "-framework Metal" "-framework QuartzCore" "-framework Foundation")
else()
    target_link_libraries(sm64_game PRIVATE m)
    target_compile_options(sm64_game PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# Release maintainers run build_locked_runtimes.py with legally acquired ROMs
# before packaging. It produces encrypted libraries and a precompiled unlocker
# for one target platform. The resulting files contain no ROM image and are
# selected here only for the install step; ordinary source builds never read a
# ROM or require a compiler beyond FrameTee itself.
add_subdirectory("${CMAKE_SOURCE_DIR}/tools/sm64_lock" "${CMAKE_BINARY_DIR}/tools/sm64_lock")
add_dependencies(sm64_game sm64_lock)

if(WIN32)
    set(SM64_RUNTIME_PLATFORM "windows")
elseif(UNIX AND NOT APPLE)
    set(SM64_RUNTIME_PLATFORM "linux")
else()
    set(SM64_RUNTIME_PLATFORM "unsupported")
endif()
set(FRAMETEE_SM64_RUNTIME_DIRECTORY "${CMAKE_SOURCE_DIR}/data/games/sm64/runtime/${SM64_RUNTIME_PLATFORM}" CACHE PATH
    "Directory containing locked SM64 runtime files for this release target")
if(EXISTS "${FRAMETEE_SM64_RUNTIME_DIRECTORY}")
    file(GLOB SM64_RUNTIME_FILES CONFIGURE_DEPENDS "${FRAMETEE_SM64_RUNTIME_DIRECTORY}/*")
    set(SM64_RUNTIME_BUILD_OUTPUTS "")
    foreach(SM64_RUNTIME_FILE IN LISTS SM64_RUNTIME_FILES)
        if(IS_DIRECTORY "${SM64_RUNTIME_FILE}")
            continue()
        endif()
        get_filename_component(SM64_RUNTIME_NAME "${SM64_RUNTIME_FILE}" NAME)
        if(SM64_RUNTIME_NAME STREQUAL "sm64_lock" OR SM64_RUNTIME_NAME STREQUAL "sm64_lock.exe")
            continue()
        endif()
        set(SM64_RUNTIME_BUILD_FILE "${CMAKE_BINARY_DIR}/data/games/sm64/runtime/${SM64_RUNTIME_NAME}")
        list(APPEND SM64_RUNTIME_BUILD_OUTPUTS "${SM64_RUNTIME_BUILD_FILE}")
        add_custom_command(
            OUTPUT "${SM64_RUNTIME_BUILD_FILE}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/data/games/sm64/runtime"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SM64_RUNTIME_FILE}" "${SM64_RUNTIME_BUILD_FILE}"
            DEPENDS "${SM64_RUNTIME_FILE}"
            COMMENT "Staging SM64 locked runtime ${SM64_RUNTIME_NAME}"
            VERBATIM
        )
    endforeach()
    add_custom_target(sm64_runtime_assets ALL DEPENDS ${SM64_RUNTIME_BUILD_OUTPUTS})
    add_dependencies(sm64_game sm64_runtime_assets)
    install(DIRECTORY "${FRAMETEE_SM64_RUNTIME_DIRECTORY}/"
        DESTINATION "data/games/sm64/runtime"
        COMPONENT Runtime
        FILES_MATCHING
        PATTERN "*.locked"
        PATTERN "manifest.json")
else()
    message(STATUS "SM64: no locked ${SM64_RUNTIME_PLATFORM} runtime staged; the module will prompt for a packaged runtime")
endif()
install(TARGETS sm64_game RUNTIME DESTINATION games COMPONENT Runtime LIBRARY DESTINATION games COMPONENT Runtime)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/" DESTINATION "include" COMPONENT Development)
