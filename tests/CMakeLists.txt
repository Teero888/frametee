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
