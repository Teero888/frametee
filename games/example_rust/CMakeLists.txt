# Builds the Rust game module through cargo, when a Rust toolchain is present.
#
# This subdirectory is here to keep the bundled modules building together. A
# Rust module needs none of it: Cargo resolves the pinned Bevy crates and
# produces a loadable module on its own. Dropping that .so/.dll into games/ is
# the entire install step.

cmake_minimum_required(VERSION 3.14)

find_program(CARGO_EXECUTABLE cargo)

if(NOT CARGO_EXECUTABLE)
    message(STATUS "cargo not found; skipping the Rust example game module")
    return()
endif()

if(WIN32)
    set(RUST_MODULE_BUILT "${CMAKE_CURRENT_SOURCE_DIR}/target/release/example_bouncer.dll")
elseif(APPLE)
    set(RUST_MODULE_BUILT "${CMAKE_CURRENT_SOURCE_DIR}/target/release/libexample_bouncer.dylib")
else()
    set(RUST_MODULE_BUILT "${CMAKE_CURRENT_SOURCE_DIR}/target/release/libexample_bouncer.so")
endif()

get_filename_component(RUST_MODULE_EXT "${RUST_MODULE_BUILT}" EXT)
set(RUST_MODULE_INSTALLED "${CMAKE_BINARY_DIR}/games/example_bouncer${RUST_MODULE_EXT}")

add_custom_command(
    OUTPUT "${RUST_MODULE_INSTALLED}"
    COMMAND ${CARGO_EXECUTABLE} build --release --locked --quiet
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/games"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${RUST_MODULE_BUILT}" "${RUST_MODULE_INSTALLED}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/lib.rs" "${CMAKE_CURRENT_SOURCE_DIR}/src/abi.rs"
            "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml" "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.lock"
    COMMENT "Building the Rust example game module"
    VERBATIM
)

add_custom_target(example_rust_game ALL DEPENDS "${RUST_MODULE_INSTALLED}")
