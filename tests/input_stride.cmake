if(NOT DEFINED APP OR NOT DEFINED MODULE OR NOT DEFINED SOURCE_DIR OR NOT DEFINED BUILD_DIR)
    message(FATAL_ERROR "APP, MODULE, SOURCE_DIR and BUILD_DIR are required")
endif()

set(TEST_ROOT "${BUILD_DIR}/test-input-stride")
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/games" "${TEST_ROOT}/config")
file(COPY "${MODULE}" DESTINATION "${TEST_ROOT}/games")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "XDG_CONFIG_HOME=${TEST_ROOT}/config"
        "APPDATA=${TEST_ROOT}/config"
        "${APP}" --game input-stride-test --auto "${SOURCE_DIR}/tests/input_stride.auto"
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE RESULT
    OUTPUT_VARIABLE OUTPUT
    ERROR_VARIABLE ERROR_OUTPUT
)
if(NOT RESULT EQUAL 0)
    message(FATAL_ERROR "Packed input stride regression (${RESULT})\n${OUTPUT}\n${ERROR_OUTPUT}")
endif()
