if(NOT DEFINED APP OR NOT DEFINED SOURCE_DIR OR NOT DEFINED BUILD_DIR)
    message(FATAL_ERROR "APP, SOURCE_DIR and BUILD_DIR are required")
endif()

set(TEST_ROOT "${BUILD_DIR}/test-config-preserve")
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
file(COPY "${SOURCE_DIR}/tests/config_preserve/frametee" DESTINATION "${TEST_ROOT}")

function(run_editor)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            "XDG_CONFIG_HOME=${TEST_ROOT}"
            "APPDATA=${TEST_ROOT}"
            "${APP}" --game example-bouncer --auto "${SOURCE_DIR}/tests/empty.auto"
        WORKING_DIRECTORY "${BUILD_DIR}"
        RESULT_VARIABLE RESULT
        OUTPUT_VARIABLE OUTPUT
        ERROR_VARIABLE ERROR_OUTPUT
    )
    if(NOT RESULT EQUAL 0)
        message(FATAL_ERROR "FrameTee config round-trip failed (${RESULT})\n${OUTPUT}\n${ERROR_OUTPUT}")
    endif()
endfunction()

run_editor()
file(READ "${TEST_ROOT}/frametee/config.toml" CONFIG)
foreach(EXPECTED
        "\"game_ddnet_jump\" = \"J\""
        "\"future_setting\" = [\"kept\", \"seven\"]"
        "[game.\"ddnet\"]"
        "[game.\"example-bouncer\"]"
        "editor_camera_mode = \"free\"")
    string(FIND "${CONFIG}" "${EXPECTED}" FOUND)
    if(FOUND EQUAL -1)
        message(FATAL_ERROR "Saved config lost: ${EXPECTED}\n${CONFIG}")
    endif()
endforeach()

# A second pass proves that the normalized quoted-table output parses too.
run_editor()
