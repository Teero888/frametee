add_executable(input_test input.c ../src/system/input.c)
target_include_directories(input_test PRIVATE ../src ../include ../libs/cimgui
    $<TARGET_PROPERTY:${GLFW_LINK_TARGET},INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_definitions(input_test PRIVATE CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1)
if(MSVC)
    target_compile_options(input_test PRIVATE /UNDEBUG)
else()
    target_compile_options(input_test PRIVATE -UNDEBUG -Wall -Wextra)
    target_link_libraries(input_test PRIVATE m)
    if(ENABLE_SANITIZERS)
        target_compile_options(input_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(input_test PRIVATE -fsanitize=address,undefined)
    endif()
endif()
add_test(NAME input COMMAND input_test)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_program(FRAMETEE_XVFB_RUN xvfb-run)
    if(FRAMETEE_XVFB_RUN)
        add_executable(input_repeat_x11_test input_repeat_x11.c ../src/system/input_repeat.c)
        target_include_directories(input_repeat_x11_test PRIVATE ../src
            $<TARGET_PROPERTY:${GLFW_LINK_TARGET},INTERFACE_INCLUDE_DIRECTORIES>)
        target_compile_options(input_repeat_x11_test PRIVATE -UNDEBUG -Wall -Wextra)
        target_link_libraries(input_repeat_x11_test PRIVATE X11::X11 m)
        add_test(NAME input_repeat_x11 COMMAND ${FRAMETEE_XVFB_RUN} -a $<TARGET_FILE:input_repeat_x11_test>)
    endif()

    if(TARGET PkgConfig::FRAMETEE_WAYLAND)
        pkg_check_modules(FRAMETEE_WAYLAND_SERVER QUIET IMPORTED_TARGET wayland-server)
        if(TARGET PkgConfig::FRAMETEE_WAYLAND_SERVER)
            add_executable(input_repeat_wayland_test input_repeat_wayland.c ../src/system/input_repeat.c)
            target_include_directories(input_repeat_wayland_test PRIVATE ../src
                $<TARGET_PROPERTY:${GLFW_LINK_TARGET},INTERFACE_INCLUDE_DIRECTORIES>)
            target_compile_definitions(input_repeat_wayland_test PRIVATE FT_HAS_WAYLAND=1)
            target_compile_options(input_repeat_wayland_test PRIVATE -UNDEBUG -Wall -Wextra)
            target_link_libraries(input_repeat_wayland_test PRIVATE PkgConfig::FRAMETEE_WAYLAND
                PkgConfig::FRAMETEE_WAYLAND_SERVER X11::X11 m)
            add_test(NAME input_repeat_wayland COMMAND input_repeat_wayland_test)
        endif()
    endif()
endif()

add_executable(entity_match_test entity_match.c ../src/engine/entity_match.c)
target_include_directories(entity_match_test PRIVATE ../src ../include)
if(UNIX)
    target_compile_options(entity_match_test PRIVATE -UNDEBUG -Wall -Wextra)
    if(ENABLE_SANITIZERS)
        target_compile_options(entity_match_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(entity_match_test PRIVATE -fsanitize=address,undefined)
    endif()
endif()
add_test(NAME entity_match COMMAND entity_match_test)

add_executable(camera_timeline_test camera_timeline.c ../src/user_interface/camera/camera_timeline.c)
target_include_directories(camera_timeline_test PRIVATE ../src)
if(UNIX)
    target_link_libraries(camera_timeline_test PRIVATE m)
    target_compile_options(camera_timeline_test PRIVATE -UNDEBUG -Wall -Wextra)
endif()
add_test(NAME camera_timeline COMMAND camera_timeline_test)

add_executable(audio_test audio.c ../src/audio/audio.c ../src/logger/logger.c)
target_include_directories(audio_test PRIVATE ../src ../include)
target_compile_definitions(audio_test PRIVATE BUILDING_LOGGER)
target_link_libraries(audio_test PRIVATE miniaudio)
if(UNIX)
    target_compile_options(audio_test PRIVATE -UNDEBUG -Wall -Wextra)
endif()
add_test(NAME audio COMMAND audio_test)

add_executable(audio_realtime_test audio_realtime.c ../src/logger/logger.c)
target_include_directories(audio_realtime_test PRIVATE ../src ../include)
target_compile_definitions(audio_realtime_test PRIVATE BUILDING_LOGGER)
target_link_libraries(audio_realtime_test PRIVATE miniaudio)
if(MSVC)
    target_compile_options(audio_realtime_test PRIVATE /UNDEBUG)
else()
    target_compile_options(audio_realtime_test PRIVATE -UNDEBUG -Wall -Wextra)
    if(ENABLE_SANITIZERS)
        target_compile_options(audio_realtime_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(audio_realtime_test PRIVATE -fsanitize=address,undefined)
    endif()
endif()
add_test(NAME audio_realtime COMMAND audio_realtime_test)

add_executable(game_host_browse_test
    game_host_browse.c
    ../src/engine/game_host.c
    ../src/engine/input_record.c
    ../src/system/fs.c
    ../src/logger/logger.c
)
target_include_directories(game_host_browse_test PRIVATE ../src ../include)
target_compile_definitions(game_host_browse_test PRIVATE BUILDING_LOGGER)
target_link_libraries(game_host_browse_test PRIVATE ${CMAKE_DL_LIBS})
if(MSVC)
    target_compile_options(game_host_browse_test PRIVATE /UNDEBUG)
else()
    target_compile_options(game_host_browse_test PRIVATE -UNDEBUG)
    if(ENABLE_SANITIZERS)
        target_compile_options(game_host_browse_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(game_host_browse_test PRIVATE -fsanitize=address,undefined)
    endif()
endif()
add_test(NAME game_host_browse COMMAND game_host_browse_test)

if(TARGET tmuf_game)
    add_executable(tmuf_data_test tmuf_data.c ../games/tmuf/tmuf_data.c)
    target_include_directories(tmuf_data_test PRIVATE ../games/tmuf ../include)
    set_target_properties(tmuf_data_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    if(WIN32)
        target_link_libraries(tmuf_data_test PRIVATE advapi32)
    endif()
    if(MSVC)
        target_compile_options(tmuf_data_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(tmuf_data_test PRIVATE -UNDEBUG -Wall -Wextra)
        if(ENABLE_SANITIZERS)
            target_compile_options(tmuf_data_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(tmuf_data_test PRIVATE -fsanitize=address,undefined)
        endif()
    endif()
    add_test(NAME tmuf_data COMMAND tmuf_data_test)
endif()

if(TARGET sm64_game)
    # Per version of the game: a headless SM64 plugin (the generic simulation
    # API and libsm64_physics called directly, as SM64 plugins do), and frames
    # drawn offscreen as the module draws them, into PNG files.
    foreach(version us jp eu sh)
        if(version STREQUAL "us")
            set(game_id "sm64")
        else()
            set(game_id "sm64_${version}")
        endif()
        set(plugin simulation_probe_${version})
        add_library(${plugin} MODULE plugin_simulation.c)
        target_include_directories(${plugin} PRIVATE ../src/plugins ../include ../libs/cglm/include ../games/sm64/include)
        target_compile_definitions(${plugin} PRIVATE SM64_GAME_ID="${game_id}")
        target_link_libraries(${plugin} PRIVATE sm64_physics_${version})
        set_target_properties(${plugin} PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/${plugin}"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/${plugin}"
            BUILD_RPATH_USE_ORIGIN ON)
        set(SM64_PLUGIN_GAME_ID "${game_id}")
        configure_file(plugin_simulation.toml "${CMAKE_BINARY_DIR}/plugins/${plugin}/plugin.toml" @ONLY)

        add_executable(sm64_render_frames_${version} sm64_render.c)
        target_include_directories(sm64_render_frames_${version} PRIVATE ../libs/stb)
        target_link_libraries(sm64_render_frames_${version} PRIVATE sm64_render_${version})

        # The user's ROM of this version.
        string(TOUPPER "${version}" upper)
        set(FRAMETEE_SM64_TEST_ROM_${upper} "" CACHE FILEPATH "Super Mario 64 (${upper}) ROM for the SM64 integration tests")
        if(FRAMETEE_SM64_TEST_ROM_${upper})
            add_test(NAME sm64_plugin_simulation_${version} COMMAND frametee --headless --game ${game_id}
                --level "${FRAMETEE_SM64_TEST_ROM_${upper}}" --plugin ${plugin})
            set_tests_properties(sm64_plugin_simulation_${version} PROPERTIES WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
                ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-config;APPDATA=${CMAKE_BINARY_DIR}/test-config")
        endif()
    endforeach()
endif()

if(TARGET ddnet_game)
    find_package(ZLIB REQUIRED)
    add_executable(ddnet_map_test ddnet_map.c
        ../games/ddnet/dd_map_math.c ../games/ddnet/dd_map_design.c)
    set_target_properties(ddnet_map_test PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
    target_include_directories(ddnet_map_test PRIVATE ../games/ddnet ../include ../libs)
    target_link_libraries(ddnet_map_test PRIVATE ddnet_physics ddnet_map_loader cglm ZLIB::ZLIB)
    if(NOT MSVC)
        target_link_libraries(ddnet_map_test PRIVATE m)
        target_compile_options(ddnet_map_test PRIVATE -UNDEBUG -Wall -Wextra -Wno-unused-parameter)
        if(ENABLE_SANITIZERS)
            target_compile_options(ddnet_map_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(ddnet_map_test PRIVATE -fsanitize=address,undefined)
        endif()
    else()
        target_compile_options(ddnet_map_test PRIVATE /UNDEBUG)
    endif()
    add_test(NAME ddnet_map COMMAND ddnet_map_test)

    # The game headless with a plugin that steps its worlds, through the engine's simulation API and
    # through ddnet_physics directly, on a map of the physics' own tests.
    add_library(ddnet_simulation_probe MODULE ddnet_simulation.c)
    target_include_directories(ddnet_simulation_probe PRIVATE ../src/plugins ../include ../libs/cglm/include ../games/ddnet/include)
    target_link_libraries(ddnet_simulation_probe PRIVATE ddnet_physics)
    set_target_properties(ddnet_simulation_probe PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/ddnet_simulation_probe"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/ddnet_simulation_probe"
        BUILD_RPATH_USE_ORIGIN ON)
    file(WRITE "${CMAKE_BINARY_DIR}/plugins/ddnet_simulation_probe/plugin.toml"
         "name = \"DDNet simulation test\"\ndescription = \"Headless test of the DDNet game's simulation\"\ngame = \"ddnet\"\n")
    if(EXISTS "${FRAMETEE_DDNET_PHYSICS_DIR}/tests/Aip-Gores.map")
        add_test(NAME ddnet_plugin_simulation COMMAND frametee --headless --game ddnet
            --level "${FRAMETEE_DDNET_PHYSICS_DIR}/tests/Aip-Gores.map" --plugin ddnet_simulation_probe)
        set_tests_properties(ddnet_plugin_simulation PROPERTIES WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
            ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-config;APPDATA=${CMAKE_BINARY_DIR}/test-config")
    endif()
endif()
