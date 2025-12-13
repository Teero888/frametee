cmake_minimum_required(VERSION 3.14)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED TRUE)

# Option to enable sanitizers
option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

# source files
project(ddnet_frametee)

if(UNIX AND CMAKE_C_COMPILER_ID MATCHES "MSVC")
    message(WARNING "It looks like you are using clang-cl (MSVC compatible) on Linux. This might be unintended. Consider setting CC=clang before running cmake.")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	message(STATUS "No build type selected, defaulting to Release")
	set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type (default Release)" FORCE)
endif()

set(SRCS 
	src/main.c
	src/animation/anim_data.c
	src/animation/anim_system.c
	src/system/save.c
	src/system/config.c
	src/logger/logger.c
	src/physics/physics.c
	src/plugins/api_impl.c
	src/plugins/plugin_manager.c
	src/renderer/graphics_backend.c
	src/renderer/renderer.c
	src/user_interface/skin_browser.c
	src/user_interface/demo.c
	src/user_interface/keybinds.c
	src/user_interface/player_info.c
	src/user_interface/snippet_editor.c
	src/user_interface/undo_redo.c
	src/user_interface/user_interface.c
	src/user_interface/widgets/hsl_colorpicker.c
	src/user_interface/timeline/timeline.c
	src/user_interface/timeline/timeline_commands.c
	src/user_interface/timeline/timeline_interaction.c
	src/user_interface/timeline/timeline_model.c
	src/user_interface/timeline/timeline_renderer.c
)

# find dependencies
find_package(Vulkan REQUIRED)

# Try to find an existing glfw3 installation first; fall back to fetching it if unavailable.
find_package(glfw3 CONFIG QUIET)
if(NOT glfw3_FOUND)
    find_package(glfw3 QUIET)
endif()

if(NOT glfw3_FOUND)
    message(STATUS "glfw3 not found via CMake package registry, fetching from source")
    include(FetchContent)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.3.9
    )
    FetchContent_MakeAvailable(glfw)
endif()

set(GLFW_LINK_TARGET glfw)
if(TARGET glfw::glfw)
    set(GLFW_LINK_TARGET glfw::glfw)
elseif(TARGET glfw)
    set(GLFW_LINK_TARGET glfw)
endif()

set(PLATFORM_LIBS)
if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    list(APPEND PLATFORM_LIBS X11)
endif()

# find vulkan glslangvalidator for shader compilation
find_program(GLSLANG_VALIDATOR glslangValidator HINTS ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE})
if(NOT GLSLANG_VALIDATOR)
    message(FATAL_ERROR "glslangValidator not found. Please ensure Vulkan SDK is installed.")
endif()

# stb library
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE ${CMAKE_SOURCE_DIR}/libs/stb)

# toml library
add_library(toml STATIC ${CMAKE_SOURCE_DIR}/libs/toml/src/tomlc17.c)
target_include_directories(toml PUBLIC ${CMAKE_SOURCE_DIR}/libs/toml/src)

# imgui sources
set(IMGUI_SOURCES 
    libs/cimgui/imgui/imgui.cpp
    libs/cimgui/imgui/imgui_draw.cpp
    libs/cimgui/imgui/imgui_demo.cpp
    libs/cimgui/imgui/imgui_widgets.cpp
    libs/cimgui/imgui/imgui_tables.cpp
    libs/cimgui/imgui/backends/imgui_impl_vulkan.cpp
    libs/cimgui/imgui/backends/imgui_impl_glfw.cpp
)
add_executable(${PROJECT_NAME} ${SRCS} ${IMGUI_SOURCES})

if(UNIX)
    # On Linux/macOS, this flag adds all symbols to the dynamic symbol table
    target_link_options(${PROJECT_NAME} PRIVATE -rdynamic)
elseif(WIN32)
    # On Windows, this enables the executable to export symbols like a DLL
    set_target_properties(${PROJECT_NAME} PROPERTIES ENABLE_EXPORTS ON)
endif()

set(IMGUI_STATIC "yes" CACHE STRING "Build as a static library" FORCE)
# cimgui configuration
add_subdirectory(libs/cimgui)
target_compile_definitions(${PROJECT_NAME} PRIVATE 
    CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1
    CIMGUI_USE_GLFW=1
    CIMGUI_USE_VULKAN=1
)

# add and link libs
add_subdirectory(libs/cglm)
add_subdirectory(libs/ddnet_physics)
add_subdirectory(libs/nfd)

target_link_libraries(${PROJECT_NAME} PRIVATE
    nfd
    stb
    toml
    ddnet_physics
    cglm
    cimgui
    ${GLFW_LINK_TARGET}
    ${PLATFORM_LIBS}
    Vulkan::Vulkan
)

target_compile_options(${PROJECT_NAME} PRIVATE
    $<$<OR:$<C_COMPILER_ID:GNU>,$<C_COMPILER_ID:Clang>>:-Wall -Wextra -pedantic>
    $<$<C_COMPILER_ID:MSVC>:/W4>
    
    $<$<AND:$<CONFIG:Debug>,$<OR:$<C_COMPILER_ID:GNU>,$<C_COMPILER_ID:Clang>>>:-g -DDEBUG -ffast-math>
    $<$<AND:$<CONFIG:Release>,$<OR:$<C_COMPILER_ID:GNU>,$<C_COMPILER_ID:Clang>>>:-g -O3 -ffast-math -funroll-loops -mfpmath=sse -fomit-frame-pointer -fno-trapping-math -fno-signed-zeros>

    $<$<AND:$<BOOL:${ENABLE_SANITIZERS}>,$<CONFIG:Debug>,$<OR:$<C_COMPILER_ID:GNU>,$<C_COMPILER_ID:Clang>>>:-fsanitize=address,undefined>
)

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR}/libs)

# sanitizer options
if(ENABLE_SANITIZERS)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${PROJECT_NAME} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${PROJECT_NAME} PRIVATE -fsanitize=address,undefined)
        # create suppression file for gtk, vulkan loader, pango, and fontconfig-related leaks
        set(SUPPRESSION_FILE "${CMAKE_BINARY_DIR}/asan_suppressions.txt")
        file(WRITE ${SUPPRESSION_FILE}
            "# Suppressions for GTK-related memory leaks\n"
            "leak:*gtk_*\n"
            "leak:*g_object_*\n"
            "leak:*gdk_*\n"
            "leak:*glib_*\n"
            "leak:*pango_*\n"
            "leak:*cairo_*\n"
            "leak:*GObject*\n"
            "leak:*Gdk*\n"
            "leak:*Gtk*\n"
            "leak:*GLib*\n"
            "# Suppressions for Vulkan loader-related memory leaks\n"
            "leak:*vulkan-icd-loader*\n"
            "leak:*setup_loader_term_phys_devs*\n"
            "leak:*terminator_EnumeratePhysicalDevices*\n"
            "leak:*vkEnumeratePhysicalDevices*\n"
            "# Suppressions for FontConfig-related memory leaks\n"
            "leak:*libfontconfig*\n"
            "leak:*FcFontRenderPrepare*\n"
            "leak:*FcFontSetMatch*\n"
            "leak:*FcValueSave*\n"
            "leak:*FcLangSetCopy*\n"
        )
        # create wrapper script to set lsan_options
        set(WRAPPER_SCRIPT "${CMAKE_BINARY_DIR}/ddnet_frametee.sh")
        file(WRITE ${WRAPPER_SCRIPT}
            "#!/bin/sh\n"
            "export LSAN_OPTIONS=suppressions=${CMAKE_BINARY_DIR}/asan_suppressions.txt\n"
            "exec ${CMAKE_BINARY_DIR}/ddnet_frametee \"$@\"\n"
        )
        # make the wrapper script executable
        execute_process(COMMAND chmod +x ${WRAPPER_SCRIPT})
        message(STATUS "Sanitizer wrapper script created at ${WRAPPER_SCRIPT}. Use './ddnet_frametee.sh' to run with suppressions.")
    else()
        message(WARNING "Sanitizers are only supported for GCC and Clang compilers.")
    endif()
endif()

# compile and copy shaders
# create shaders output directory in build folder
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/data/shaders)

# shader compilation
file(GLOB SHADER_SOURCES 
    "${CMAKE_SOURCE_DIR}/data/shaders/*.vert.glsl" 
    "${CMAKE_SOURCE_DIR}/data/shaders/*.frag.glsl"
)
message(STATUS "Shader sources found: ${SHADER_SOURCES}")
if(NOT SHADER_SOURCES)
    message(WARNING "No shader sources found in ${CMAKE_SOURCE_DIR}/data/shaders/. Check the directory and file extensions.")
endif()

set(SHADER_OUTPUTS "")
foreach(SHADER ${SHADER_SOURCES})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    string(REPLACE ".glsl" ".spv" SHADER_OUTPUT_NAME ${SHADER_NAME})
    set(SHADER_OUTPUT "${CMAKE_BINARY_DIR}/data/shaders/${SHADER_OUTPUT_NAME}")
    list(APPEND SHADER_OUTPUTS ${SHADER_OUTPUT})

    add_custom_command(
        OUTPUT ${SHADER_OUTPUT}
        COMMAND ${GLSLANG_VALIDATOR} -V ${SHADER} -o ${SHADER_OUTPUT}
        DEPENDS ${SHADER}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Compiling shader ${SHADER_NAME} to ${SHADER_OUTPUT}"
    )
endforeach()

# custom target for shaders
add_custom_target(
    compile_shaders
    DEPENDS ${SHADER_OUTPUTS}
    COMMENT "Compiling all GLSL shaders to SPIR-V"
)

# ensure shaders are compiled before the main target
add_dependencies(${PROJECT_NAME} compile_shaders)

# create plugins directory in build folder
add_custom_command(
    TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory 
        ${CMAKE_BINARY_DIR}/plugins
    COMMENT "Creating plugins directory in build folder"
)

# copy textures folder to build directory
add_custom_command(
    TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory 
        ${CMAKE_SOURCE_DIR}/data/textures 
        ${CMAKE_BINARY_DIR}/data/textures
    COMMENT "Copying textures to build directory"
)

# copy fonts folder to build directory
add_custom_command(
    TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory 
        ${CMAKE_SOURCE_DIR}/data/fonts 
        ${CMAKE_BINARY_DIR}/data/fonts
    COMMENT "Copying fonts to build directory"
)

# copy compiled shaders to build directory
add_custom_command(
    TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different 
        ${SHADER_OUTPUTS} 
        ${CMAKE_BINARY_DIR}/data/shaders/
    COMMENT "Copying compiled shaders to build directory"
)
