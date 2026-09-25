add_executable(camera_timeline_test camera_timeline.c ../src/user_interface/camera/camera_timeline.c)
target_include_directories(camera_timeline_test PRIVATE ../src)
if(UNIX)
    target_link_libraries(camera_timeline_test PRIVATE m)
    target_compile_options(camera_timeline_test PRIVATE -UNDEBUG -Wall -Wextra)
endif()
add_test(NAME camera_timeline COMMAND camera_timeline_test)

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

if(TARGET sm64_game_us)
    # Per version of the game: a headless SM64 plugin (the generic simulation
    # API and libsm64_physics called directly, as SM64 plugins do), and frames
    # drawn offscreen as the module draws them, into PNG files.
    foreach(version us jp eu)
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
endif()
