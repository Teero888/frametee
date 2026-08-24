cmake_minimum_required(VERSION 3.14)

# Prefer Clang for a default build when both C and C++ drivers are available.
# Explicit compiler selections and toolchain files always take precedence.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE
   AND NOT DEFINED ENV{CMAKE_TOOLCHAIN_FILE}
   AND NOT DEFINED CMAKE_C_COMPILER
   AND NOT DEFINED CMAKE_CXX_COMPILER
   AND NOT DEFINED ENV{CC}
   AND NOT DEFINED ENV{CXX})
    find_program(FRAMETEE_CLANG_C_COMPILER NAMES clang)
    find_program(FRAMETEE_CLANG_CXX_COMPILER NAMES clang++)

    if(FRAMETEE_CLANG_C_COMPILER AND FRAMETEE_CLANG_CXX_COMPILER)
        set(CMAKE_C_COMPILER "${FRAMETEE_CLANG_C_COMPILER}" CACHE FILEPATH "C compiler")
        set(CMAKE_CXX_COMPILER "${FRAMETEE_CLANG_CXX_COMPILER}" CACHE FILEPATH "C++ compiler")
        message(STATUS "Clang found; using it as the default FrameTee compiler")
    endif()

    unset(FRAMETEE_CLANG_C_COMPILER CACHE)
    unset(FRAMETEE_CLANG_CXX_COMPILER CACHE)
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED TRUE)

# Option to enable sanitizers
option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

# source files
project(frametee)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	message(STATUS "No build type selected, defaulting to Release")
	set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type (default Release)" FORCE)
endif()

option(ENABLE_AGGRESSIVE_OPTIM "Enable aggressive optimizations with LTO and native tuning" OFF)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PGO)

if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
    add_compile_options(
        /wd4018 # signed/unsigned mismatch
        /wd4068 # unknown pragma
        /wd4100 # unreferenced parameter
        /wd4152 # function/data pointer conversion
        /wd4201 # nameless struct/union
        /wd4242 # conversion from type1 to type2, possible loss of data
        /wd4244 # conversion from type1 to type2, possible loss of data
        /wd4267 # conversion from size_t to type, possible loss of data
        /wd4324 # structure padded due to alignment specifier
        /wd4456 # declaration hides previous local declaration
        /wd4710 # function not inlined
        /wd4820 # padding added after data member
        /wd4996 # deprecated functions
        /wd5045 # Spectre mitigation info
        /wd5287 # operands are different enum types
    )
endif()


# The engine. Nothing here knows about any particular game: physics, level
# formats, visuals and per-game panels all live in games/<id>/ behind
# include/frametee/game_abi.h.
set(SRCS 
	src/main.c
	src/system/save.c
	src/system/config.c
	src/system/input.c
	src/system/fs.c
	src/logger/logger.c
	src/engine/game_host.c
	src/engine/engine_api.c
	src/engine/input_record.c
	src/engine/prediction.c
	src/plugins/api_impl.c
	src/plugins/plugin_manager.c
	src/scripting/script_engine.c
	src/renderer/graphics_backend.c
	src/renderer/renderer.c
	src/user_interface/keybinds.c
	src/user_interface/player_info.c
	src/user_interface/snippet_editor.c
	src/user_interface/timeline_events.c
	src/user_interface/undo_redo.c
	src/user_interface/entity_inspector.c
	src/user_interface/user_interface.c
	src/user_interface/timeline/timeline.c
	src/user_interface/timeline/timeline_commands.c
	src/user_interface/timeline/timeline_interaction.c
	src/user_interface/timeline/timeline_model.c
	src/user_interface/timeline/timeline_renderer.c
)

# find dependencies
find_package(Vulkan REQUIRED)

# Try to find an existing glfw3 installation first; fall back to fetching it if unavailable.
# 3.4 is a hard minimum: raylib (games/example_platformer) uses GLFW 3.4-only API
# such as GLFW_MOUSE_PASSTHROUGH and glfwGetPlatform.
find_package(glfw3 3.4 CONFIG QUIET)
if(NOT glfw3_FOUND)
    find_package(glfw3 3.4 QUIET)
endif()

if(NOT glfw3_FOUND)
    message(STATUS "glfw3 >= 3.4 not found via CMake package registry, fetching from source")
    include(FetchContent)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    if(UNIX AND NOT APPLE)
        # keeps the fallback buildable with only the X11 dev packages installed
        set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
    endif()
    FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.4
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(glfw)
    # the fetched library is static and ends up inside loadable game modules
    set_target_properties(glfw PROPERTIES POSITION_INDEPENDENT_CODE ON)
    # raylib's cmake/GlfwImport.cmake keys off glfw3_FOUND; without this it also
    # adds its own bundled glfw and collides with the target created above.
    set(glfw3_FOUND TRUE)
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

set_source_files_properties(libs/cimgui/imgui/backends/imgui_impl_glfw.cpp PROPERTIES
    COMPILE_DEFINITIONS "IMGUI_IMPL_API=extern \"C\";glfwGetMonitors=mock_glfwGetMonitors;glfwGetMonitorPos=mock_glfwGetMonitorPos;glfwGetVideoMode=mock_glfwGetVideoMode;glfwGetMonitorWorkarea=mock_glfwGetMonitorWorkarea;glfwGetMonitorContentScale=mock_glfwGetMonitorContentScale"
)
set_source_files_properties(libs/cimgui/imgui/backends/imgui_impl_vulkan.cpp PROPERTIES
    COMPILE_DEFINITIONS "IMGUI_IMPL_API=extern \"C\""
)

if(UNIX)
    # On Linux/macOS, this flag adds all symbols to the dynamic symbol table
    target_link_options(${PROJECT_NAME} PRIVATE -rdynamic)
elseif(WIN32)
    # On Windows, this enables the executable to export symbols like a DLL
    set_target_properties(${PROJECT_NAME} PROPERTIES ENABLE_EXPORTS ON)
    set_property(SOURCE ${IMGUI_SOURCES} PROPERTY COMPILE_DEFINITIONS "IMGUI_API=__declspec(dllexport)")
    set_property(SOURCE libs/cimgui/imgui/backends/imgui_impl_glfw.cpp APPEND PROPERTY COMPILE_DEFINITIONS "IMGUI_IMPL_API=extern \"C\";glfwGetMonitors=mock_glfwGetMonitors;glfwGetMonitorPos=mock_glfwGetMonitorPos;glfwGetVideoMode=mock_glfwGetVideoMode;glfwGetMonitorWorkarea=mock_glfwGetMonitorWorkarea;glfwGetMonitorContentScale=mock_glfwGetMonitorContentScale")
    set_property(SOURCE libs/cimgui/imgui/backends/imgui_impl_vulkan.cpp APPEND PROPERTY COMPILE_DEFINITIONS "IMGUI_IMPL_API=extern \"C\"")
endif()

set(IMGUI_STATIC "yes" CACHE STRING "Build as a static library" FORCE)
# cimgui configuration
add_subdirectory(libs/cimgui)
# Override cimgui's interface compile definitions so it doesn't leak escaped quotes to C files
set_target_properties(cimgui PROPERTIES INTERFACE_COMPILE_DEFINITIONS "IMGUI_DISABLE_OBSOLETE_FUNCTIONS=1")

target_compile_definitions(${PROJECT_NAME} PRIVATE 
    CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1
    CIMGUI_USE_GLFW=1
    CIMGUI_USE_VULKAN=1
    CIMGUI_NO_EXPORT=1
    # APP_USE_VULKAN_DEBUG_REPORT=1
)

# add and link libs
add_subdirectory(libs/cglm)
add_subdirectory(libs/nfd)

target_link_libraries(${PROJECT_NAME} PRIVATE
    nfd
    stb
    toml
    cglm
    cimgui
    ${GLFW_LINK_TARGET}
    ${PLATFORM_LIBS}
    Vulkan::Vulkan
)

target_compile_options(${PROJECT_NAME} PRIVATE
    $<$<OR:$<C_COMPILER_ID:GNU>,$<C_COMPILER_ID:Clang>>: -Wall -Wextra -pedantic>
    $<$<C_COMPILER_ID:MSVC>: /W4 /wd4324>
    $<$<AND:$<CONFIG:Debug>,$<OR:$<C_COMPILER_ID:GNU>,$<C_COMPILER_ID:Clang>>>: -g -DDEBUG>
    $<$<AND:$<CONFIG:Debug>,$<C_COMPILER_ID:MSVC>>: /Zi /Od /DDEBUG>

    $<$<AND:$<CONFIG:Release>,$<OR:$<C_COMPILER_ID:GNU>,$<C_COMPILER_ID:Clang>>>: -g -O3 -funroll-loops -mfpmath=sse -fomit-frame-pointer -fno-trapping-math -fno-signed-zeros>
    $<$<AND:$<CONFIG:Release>,$<C_COMPILER_ID:MSVC>>: /Ox /Ob2 /Oi /GL /DNDEBUG>

    $<$<AND:$<BOOL:${ENABLE_SANITIZERS}>,$<CONFIG:Debug>,$<OR:$<C_COMPILER_ID:GNU>,$<C_COMPILER_ID:Clang>>>:-fsanitize=address,undefined>
)


target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR}/src)
# The game module ABI. Kept in its own root so a game module can consume it
# without seeing any engine internals.
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR}/include)
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
        set(WRAPPER_SCRIPT "${CMAKE_BINARY_DIR}/scripts/frametee.sh")
        file(WRITE ${WRAPPER_SCRIPT}
            "#!/bin/sh\n"
            "export LSAN_OPTIONS=suppressions=${CMAKE_BINARY_DIR}/asan_suppressions.txt\n"
            "exec ${CMAKE_BINARY_DIR}/frametee \"$@\"\n"
        )
        # make the wrapper script executable
        execute_process(COMMAND chmod +x ${WRAPPER_SCRIPT})
        message(STATUS "Sanitizer wrapper script created at ${WRAPPER_SCRIPT}. Use './frametee.sh' to run with suppressions.")
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
    COMMAND ${CMAKE_COMMAND} -E copy_if_different 
        ${SHADER_OUTPUTS} 
        ${CMAKE_SOURCE_DIR}/data/shaders/
    COMMENT "Copying compiled shaders to build directory"
)

# =================================================================================================
# GAME MODULE ASSETS
# =================================================================================================
# Each game keeps its assets under data/games/<id>/, which is what
# engine->resolve_data_path() hands back to a module. Shaders there are compiled
# the same way the engine's own are.
file(GLOB GAME_SHADER_SOURCES
    "${CMAKE_SOURCE_DIR}/data/games/*/shaders/*.vert.glsl"
    "${CMAKE_SOURCE_DIR}/data/games/*/shaders/*.frag.glsl"
)
set(GAME_SHADER_OUTPUTS "")
foreach(SHADER ${GAME_SHADER_SOURCES})
    file(RELATIVE_PATH SHADER_REL "${CMAKE_SOURCE_DIR}/data" "${SHADER}")
    string(REPLACE ".glsl" ".spv" SHADER_REL_SPV "${SHADER_REL}")
    set(SHADER_OUTPUT "${CMAKE_BINARY_DIR}/data/${SHADER_REL_SPV}")
    get_filename_component(SHADER_OUT_DIR "${SHADER_OUTPUT}" DIRECTORY)
    list(APPEND GAME_SHADER_OUTPUTS ${SHADER_OUTPUT})
    add_custom_command(
        OUTPUT ${SHADER_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUT_DIR}"
        COMMAND ${GLSLANG_VALIDATOR} -V ${SHADER} -o ${SHADER_OUTPUT}
        DEPENDS ${SHADER}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Compiling game shader ${SHADER_REL}"
    )
endforeach()
add_custom_target(compile_game_shaders ALL DEPENDS ${GAME_SHADER_OUTPUTS})
add_dependencies(${PROJECT_NAME} compile_game_shaders)

# copy the non-shader game assets (textures, skins, ...) next to them
add_custom_command(
    TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/data/games
        ${CMAKE_BINARY_DIR}/data/games
    COMMENT "Copying game module assets to build directory"
)

# =================================================================================================
# GAME MODULES
# =================================================================================================
# Each subdirectory of games/ is an independent shared library implementing
# include/frametee/game_abi.h. They are built here purely for convenience: a
# module has no build-time dependency on the engine and can be compiled
# out-of-tree, in any language that can produce a C-callable shared library.
option(BUILD_GAMES "Build the bundled game modules" ON)
# The game this build starts with when the user has no saved preference. Purely
# a packaging choice: the engine itself knows nothing about any game, and an
# empty value makes it take the first module in id order.
set(FRAMETEE_DEFAULT_GAME "" CACHE STRING "Game module id to start with by default; empty selects the first installed module")
target_compile_definitions(${PROJECT_NAME} PRIVATE FRAMETEE_DEFAULT_GAME_ID="${FRAMETEE_DEFAULT_GAME}")
set(FRAMETEE_ABI_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/include" CACHE PATH "Location of the FrameTee game ABI header")

add_custom_command(
    TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/games
    COMMENT "Creating games directory in build folder"
)

if(BUILD_GAMES)
    file(GLOB GAME_ENTRIES "${CMAKE_SOURCE_DIR}/games/*")
    foreach(GAME_DIR ${GAME_ENTRIES})
        if(IS_DIRECTORY "${GAME_DIR}" AND EXISTS "${GAME_DIR}/CMakeLists.txt")
            add_subdirectory("${GAME_DIR}")
            get_property(GAME_TARGETS DIRECTORY "${GAME_DIR}" PROPERTY BUILDSYSTEM_TARGETS)
            foreach(GAME_TARGET ${GAME_TARGETS})
                set_target_properties(${GAME_TARGET} PROPERTIES
                    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/games"
                    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/games"
                )
                # On Windows the modules link the executable's import library,
                # so the dependency runs the other way and adding it here would
                # close a cycle. They are ALL targets either way.
                if(NOT WIN32)
                    add_dependencies(${PROJECT_NAME} ${GAME_TARGET})
                endif()
            endforeach()
        endif()
    endforeach()
endif()

# =================================================================================================
# PLUGINS INTEGRATION
# =================================================================================================
option(BUILD_PLUGINS "Build all default plugins" ON)
if(BUILD_PLUGINS)
    set(HOST_APP_DIR "${CMAKE_SOURCE_DIR}" CACHE PATH "Path to the FrameTee project root" FORCE)
    file(GLOB PLUGIN_ENTRIES "${CMAKE_SOURCE_DIR}/plugins/*")
    foreach(PLUGIN_DIR ${PLUGIN_ENTRIES})
        if(IS_DIRECTORY "${PLUGIN_DIR}" AND EXISTS "${PLUGIN_DIR}/CMakeLists.txt")
            add_subdirectory("${PLUGIN_DIR}")
            get_property(PLUGIN_TARGETS DIRECTORY "${PLUGIN_DIR}" PROPERTY BUILDSYSTEM_TARGETS)
            foreach(TARGET ${PLUGIN_TARGETS})
                set_target_properties(${TARGET} PROPERTIES
                    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
                    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
                )
            endforeach()

        endif()
    endforeach()
endif()

# =================================================================================================
# TESTS
# =================================================================================================
include(CTest)
if(BUILD_TESTING AND BUILD_GAMES)
    add_library(semver_invalid_game MODULE tests/semver_invalid_game.c)
    set_target_properties(semver_invalid_game PROPERTIES PREFIX "" OUTPUT_NAME "semver_invalid_game")
    target_include_directories(semver_invalid_game PRIVATE ${FRAMETEE_ABI_INCLUDE_DIR})
    add_library(input_stride_game MODULE tests/input_stride_game.c)
    set_target_properties(input_stride_game PROPERTIES PREFIX "" OUTPUT_NAME "input_stride_game")
    target_include_directories(input_stride_game PRIVATE ${FRAMETEE_ABI_INCLUDE_DIR})
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test-config-bouncer")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test-config-platformer")
    add_test(
        NAME project_v12_bouncer_roundtrip
        COMMAND $<TARGET_FILE:${PROJECT_NAME}> --game example-bouncer --auto "${CMAKE_SOURCE_DIR}/tests/v12_bouncer_roundtrip.auto"
    )
    add_test(
        NAME project_v12_platformer_roundtrip
        COMMAND $<TARGET_FILE:${PROJECT_NAME}> --game example-platformer --auto "${CMAKE_SOURCE_DIR}/tests/v12_platformer_roundtrip.auto"
    )
    add_test(
        NAME game_switch_and_quick_save_lifetime
        COMMAND $<TARGET_FILE:${PROJECT_NAME}> --game example-bouncer --auto "${CMAKE_SOURCE_DIR}/tests/game_switch_lifetime.auto"
    )
    add_test(
        NAME config_preserves_inactive_games
        COMMAND ${CMAKE_COMMAND}
            -DAPP=$<TARGET_FILE:${PROJECT_NAME}>
            -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DBUILD_DIR=${CMAKE_BINARY_DIR}
            -P "${CMAKE_SOURCE_DIR}/tests/config_preserve.cmake"
    )
    add_test(
        NAME rejects_invalid_game_semver
        COMMAND ${CMAKE_COMMAND}
            -DAPP=$<TARGET_FILE:${PROJECT_NAME}>
            -DMODULE=$<TARGET_FILE:semver_invalid_game>
            -DBUILD_DIR=${CMAKE_BINARY_DIR}
            -P "${CMAKE_SOURCE_DIR}/tests/semver_rejection.cmake"
    )
    add_test(
        NAME game_input_records_use_declared_stride
        COMMAND ${CMAKE_COMMAND}
            -DAPP=$<TARGET_FILE:${PROJECT_NAME}>
            -DMODULE=$<TARGET_FILE:input_stride_game>
            -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DBUILD_DIR=${CMAKE_BINARY_DIR}
            -P "${CMAKE_SOURCE_DIR}/tests/input_stride.cmake"
    )
    add_test(
        NAME ddnet_demo_exporter_writes_demo
        COMMAND ${CMAKE_COMMAND}
            -DAPP=$<TARGET_FILE:${PROJECT_NAME}>
            -DMODULE=$<TARGET_FILE:ddnet_game>
            -DMAP=${CMAKE_SOURCE_DIR}/libs/ddnet_demo/Aip-Gores.map
            -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DBUILD_DIR=${CMAKE_BINARY_DIR}
            -P "${CMAKE_SOURCE_DIR}/tests/ddnet_export.cmake"
    )
    set_tests_properties(project_v12_bouncer_roundtrip PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-config-bouncer;APPDATA=${CMAKE_BINARY_DIR}/test-config-bouncer"
    )
    set_tests_properties(project_v12_platformer_roundtrip PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-config-platformer;APPDATA=${CMAKE_BINARY_DIR}/test-config-platformer"
    )
    set_tests_properties(game_switch_and_quick_save_lifetime PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-config-switch;APPDATA=${CMAKE_BINARY_DIR}/test-config-switch"
        PASS_REGULAR_EXPRESSION "Quick-saved current project to 'frametee-switch-lifetime.tasp'."
    )
endif()
