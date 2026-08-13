if(NOT DEFINED APP OR NOT DEFINED MODULE OR NOT DEFINED BUILD_DIR)
    message(FATAL_ERROR "APP, MODULE and BUILD_DIR are required")
endif()

set(TEST_ROOT "${BUILD_DIR}/test-semver-rejection")
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/games" "${TEST_ROOT}/config")
file(COPY "${MODULE}" DESTINATION "${TEST_ROOT}/games")
file(COPY "${BUILD_DIR}/data" DESTINATION "${TEST_ROOT}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "XDG_CONFIG_HOME=${TEST_ROOT}/config"
        "APPDATA=${TEST_ROOT}/config"
        "${APP}" --list-games
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE RESULT
    OUTPUT_VARIABLE OUTPUT
    ERROR_VARIABLE ERROR_OUTPUT
)
if(NOT RESULT EQUAL 0)
    message(FATAL_ERROR "Module listing failed (${RESULT})\n${OUTPUT}\n${ERROR_OUTPUT}")
endif()
string(CONCAT LOG "${OUTPUT}" "${ERROR_OUTPUT}")
string(FIND "${LOG}" "invalid or missing SemVer 2.0.0 game version" FOUND)
if(FOUND EQUAL -1)
    message(FATAL_ERROR "Invalid SemVer module was not rejected as expected\n${LOG}")
endif()
