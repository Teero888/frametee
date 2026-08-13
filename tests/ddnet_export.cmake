if(NOT DEFINED APP OR NOT DEFINED MODULE OR NOT DEFINED MAP OR NOT DEFINED SOURCE_DIR OR NOT DEFINED BUILD_DIR)
    message(FATAL_ERROR "APP, MODULE, MAP, SOURCE_DIR and BUILD_DIR are required")
endif()

set(TEST_ROOT "${BUILD_DIR}/test-ddnet-export")
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/games" "${TEST_ROOT}/config")
file(COPY "${MODULE}" DESTINATION "${TEST_ROOT}/games")
file(COPY "${BUILD_DIR}/data" DESTINATION "${TEST_ROOT}")
file(COPY "${MAP}" DESTINATION "${TEST_ROOT}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "XDG_CONFIG_HOME=${TEST_ROOT}/config"
        "APPDATA=${TEST_ROOT}/config"
        "${APP}" --game ddnet --auto "${SOURCE_DIR}/tests/ddnet_export.auto"
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE RESULT
    OUTPUT_VARIABLE OUTPUT
    ERROR_VARIABLE ERROR_OUTPUT
)
if(NOT RESULT EQUAL 0)
    message(FATAL_ERROR "DDNet demo export crashed (${RESULT})\n${OUTPUT}\n${ERROR_OUTPUT}")
endif()

set(DEMO "${TEST_ROOT}/Aip-Gores.demo")
if(NOT EXISTS "${DEMO}")
    message(FATAL_ERROR "DDNet exporter did not create ${DEMO}\n${OUTPUT}\n${ERROR_OUTPUT}")
endif()
file(SIZE "${DEMO}" DEMO_SIZE)
if(DEMO_SIZE LESS 1024)
    message(FATAL_ERROR "DDNet exporter created an implausibly small demo (${DEMO_SIZE} bytes)")
endif()
