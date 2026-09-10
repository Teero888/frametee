cmake_minimum_required(VERSION 3.22)

find_program(CARGO_EXECUTABLE cargo REQUIRED)

file(GLOB_RECURSE SM64_LOCK_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.rs")
set(SM64_LOCK_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/cargo-target")
set(SM64_LOCK_BUILT "${SM64_LOCK_TARGET_DIR}/release/sm64_lock${CMAKE_EXECUTABLE_SUFFIX}")
set(SM64_LOCK_OUTPUT "${CMAKE_BINARY_DIR}/data/games/sm64/runtime/sm64_lock${CMAKE_EXECUTABLE_SUFFIX}")

add_custom_command(
    OUTPUT "${SM64_LOCK_OUTPUT}"
    COMMAND ${CARGO_EXECUTABLE} build --release --locked --bin sm64_lock --target-dir "${SM64_LOCK_TARGET_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/data/games/sm64/runtime"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SM64_LOCK_BUILT}" "${SM64_LOCK_OUTPUT}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    DEPENDS ${SM64_LOCK_SOURCES}
            "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
            "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.lock"
    COMMENT "Building sm64_lock unlock tool"
    VERBATIM
)

add_custom_target(sm64_lock ALL DEPENDS "${SM64_LOCK_OUTPUT}")

install(PROGRAMS "${SM64_LOCK_OUTPUT}"
    DESTINATION "data/games/sm64/runtime"
    COMPONENT Runtime
)
