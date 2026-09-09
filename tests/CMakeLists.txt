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
