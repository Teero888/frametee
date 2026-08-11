# Profile-guided optimization support for the frametee host and its plugins.
#
# Two-stage build. Configure with -DPGO_STAGE=GENERATE, build, run a
# representative workload, merge the raw profiles, then reconfigure with
# -DPGO_STAGE=USE. scripts/pgo_build.sh drives the whole sequence.
#
# Instrumented shared libraries and the executable each write their own raw
# profile, so the profile path keeps clang's %m (per-image signature) pattern
# to stop them clobbering each other.

set(PGO_STAGE "NONE" CACHE STRING "PGO stage: NONE, GENERATE or USE")
set_property(CACHE PGO_STAGE PROPERTY STRINGS NONE GENERATE USE)

set(PGO_PROFILE_DIR "${CMAKE_BINARY_DIR}/pgo" CACHE PATH
    "Directory raw profiles are written to during the GENERATE stage")
set(PGO_PROFILE_DATA "${CMAKE_BINARY_DIR}/pgo/merged.profdata" CACHE FILEPATH
    "Merged profile consumed by the USE stage")

if(PGO_STAGE STREQUAL "USE" AND NOT EXISTS "${PGO_PROFILE_DATA}")
    message(FATAL_ERROR
        "PGO_STAGE=USE but no merged profile at ${PGO_PROFILE_DATA}.\n"
        "Run the GENERATE stage and merge the raw profiles first "
        "(see scripts/pgo_build.sh).")
endif()

# Applies the flags for the current PGO_STAGE to a target. A no-op when
# PGO_STAGE is NONE, so it is safe to call unconditionally.
function(frametee_enable_pgo target)
    if(NOT PGO_STAGE OR PGO_STAGE STREQUAL "NONE")
        return()
    endif()

    if(CMAKE_C_COMPILER_ID)
        set(compiler_id "${CMAKE_C_COMPILER_ID}")
    else()
        set(compiler_id "${CMAKE_CXX_COMPILER_ID}")
    endif()

    if(compiler_id MATCHES "Clang")
        if(PGO_STAGE STREQUAL "GENERATE")
            set(link_flags -fprofile-generate=${PGO_PROFILE_DIR})
            set(warn_flags)
        else()
            set(link_flags -fprofile-use=${PGO_PROFILE_DATA})
            # The instrumented sources and the optimized sources are rarely
            # identical; downgrade the resulting noise.
            set(warn_flags -Wno-profile-instr-out-of-date
                           -Wno-profile-instr-unprofiled)
        endif()
    elseif(compiler_id STREQUAL "GNU")
        if(PGO_STAGE STREQUAL "GENERATE")
            set(link_flags -fprofile-generate=${PGO_PROFILE_DIR})
            set(warn_flags)
        else()
            set(link_flags -fprofile-use=${PGO_PROFILE_DIR} -fprofile-correction)
            set(warn_flags -Wno-missing-profile)
        endif()
    else()
        message(FATAL_ERROR "PGO_STAGE is set but ${compiler_id} is not supported")
    endif()

    target_compile_options(${target} PRIVATE ${link_flags} ${warn_flags})
    # Needed at link time as well: GENERATE to pull in the profile runtime, USE
    # so LTO codegen sees the profile. The -Wno-* flags are compile-only; passing
    # them to the driver at link time just yields unused-argument warnings.
    target_link_options(${target} PRIVATE ${link_flags})
endfunction()

if(NOT PGO_STAGE STREQUAL "NONE")
    message(STATUS "PGO stage: ${PGO_STAGE} (profiles: ${PGO_PROFILE_DIR})")
endif()
