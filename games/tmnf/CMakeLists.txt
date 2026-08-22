# TrackMania Nations Forever.
#
# Off by default, and deliberately so: ForeverValidator is over three hundred
# C++17 translation units, which is a large addition to a build that otherwise
# compiles in seconds, and CI has no game data to run it against anyway. Turn it
# on with -DFRAMETEE_BUILD_TMNF=ON.
#
# The module additionally needs an installed TrackMania Packs directory at
# runtime; see FRAMETEE_TMNF_PACKS in tmnf_sandbox.cpp.
#
# ForeverValidator is a submodule pinned to its own upstream. Nothing here
# patches it -- how it is built is set from this file, but what it compiles is
# whatever the pinned commit says.

cmake_minimum_required(VERSION 3.14)

option(FRAMETEE_BUILD_TMNF "Build the TrackMania Nations Forever game module" ON)
if(NOT FRAMETEE_BUILD_TMNF)
    return()
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ForeverValidator/CMakeLists.txt")
    message(WARNING "FRAMETEE_BUILD_TMNF is ON but games/tmnf/ForeverValidator is missing. "
                    "Run: git submodule update --init --recursive")
    return()
endif()

# Only the libraries; the validator's own CLI, tests and benchmarks are no use
# inside the editor and would roughly double the build.
set(FOREVERVALIDATOR_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(FOREVERVALIDATOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FOREVERVALIDATOR_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)

# Interprocedural optimization for the validator.
#
# The submodule turns its own IPO on only for GCC -- see
# FOREVERVALIDATOR_ENABLE_RELEASE_IPO in its CMakeLists -- so a Clang build,
# which is what this project recommends, gets none. That is expensive here in a
# way it would not be for most libraries: the validator is three hundred
# translation units of small leaf math (GmMath::TransformPoint, GmIso4::Mult,
# Binary32::FromDouble, CIsqrt) that the physics calls millions of times a
# second, and it is assembled below into one shared object with default
# visibility, so without IPO every one of those is a real PLT-indirected call.
# Roughly a third of the simulation's retired branches were call/ret.
#
# Deliberately not folded into ENABLE_AGGRESSIVE_OPTIM: that also turns on
# -march=native, and the module has to keep running on machines this one was not
# built on. This part is portable and bit-exact -- the submodule compiles with
# -ffp-contract=off, so no fused multiply-add can be formed no matter how much
# gets inlined, and IPO changes only inlining, layout and branch order.
#
# Full -flto rather than -flto=thin: ThinLTO promotes the validator's
# anonymous-namespace thread_locals to global names, and bfd's LLVM plugin then
# rejects the result ("TLS definition ... mismatches non-TLS reference"). With
# lld installed, -flto=thin links a good deal faster and is equivalent here.
option(FRAMETEE_TMNF_VALIDATOR_LTO
       "Build ForeverValidator with link-time optimization (Clang; GCC has its own)" ON)

set(TMNF_VALIDATOR_LTO OFF)
if(FRAMETEE_TMNF_VALIDATOR_LTO AND NOT ENABLE_SANITIZERS AND
   CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Clang's -flto emits LLVM bitcode objects, which only llvm-ar can index.
    # Set for this directory's scope alone, so the rest of the build keeps the
    # archiver it was configured with.
    find_program(FRAMETEE_LLVM_AR NAMES llvm-ar)
    find_program(FRAMETEE_LLVM_RANLIB NAMES llvm-ranlib)
    if(FRAMETEE_LLVM_AR AND FRAMETEE_LLVM_RANLIB)
        set(CMAKE_CXX_ARCHIVE_CREATE "${FRAMETEE_LLVM_AR} qc <TARGET> <LINK_FLAGS> <OBJECTS>")
        set(CMAKE_CXX_ARCHIVE_APPEND "${FRAMETEE_LLVM_AR} q <TARGET> <LINK_FLAGS> <OBJECTS>")
        set(CMAKE_CXX_ARCHIVE_FINISH "${FRAMETEE_LLVM_RANLIB} <TARGET>")
        set(TMNF_VALIDATOR_LTO ON)
    else()
        message(STATUS "TMNF: validator LTO off (llvm-ar/llvm-ranlib not found)")
    endif()
endif()

add_subdirectory(ForeverValidator ${CMAKE_BINARY_DIR}/forevervalidator EXCLUDE_FROM_ALL)

# The validator's objects are linked into a loadable module.
set_target_properties(forevervalidator_core forevervalidator_native PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The validator is where this game spends essentially all of its time, so it is
# where both of these are worth having: see the note above -flto, and note that
# before this the two PGO stages profiled the host and its plugins but never the
# library underneath them. cmake/PGO.cmake's function is a no-op unless
# PGO_STAGE is set; scripts/pgo_build.sh drives the two stages.
# ENABLE_AGGRESSIVE_OPTIM means "build for this machine" and already says so
# for the host and its plugins; the validator is where the time actually goes,
# so it belongs here too. Kept separate from the LTO above because that stays
# portable and this does not: a module built with it will not start on a CPU
# without the instructions it was compiled for. Worth about three percent on
# this simulation, and bit-exact -- -ffp-contract=off still forbids the fused
# multiply-add that a wider instruction set would otherwise make available.
set(TMNF_VALIDATOR_NATIVE_OPTIONS)
if(ENABLE_AGGRESSIVE_OPTIM AND NOT ENABLE_SANITIZERS AND
   CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # -mno-fma is load-bearing, and the claim above that -ffp-contract=off is
    # sufficient on its own is not true. That flag governs contraction of
    # expressions written in the source; it does not stop clang synthesising
    # fmod as x - trunc(x/y)*y and fusing its own multiply-add. Building this
    # module with -march=native and without -mno-fma puts 20 fused instructions
    # into Binary32::TruncateToUint32Modulo and GmMap2::IsInside, where a plain
    # x86-64 baseline has none -- so the results are not the same. Denying the
    # instruction restores the bit-exactness the comment above promises, and
    # measures no slower.
    set(TMNF_VALIDATOR_NATIVE_OPTIONS
        $<$<CONFIG:Release>:-march=native -mtune=native -mno-fma>)
endif()

foreach(fv_target forevervalidator_core forevervalidator_native)
    if(TMNF_VALIDATOR_LTO)
        target_compile_options(${fv_target} PRIVATE $<$<CONFIG:Release>:-flto>)
        target_link_options(${fv_target} PRIVATE $<$<CONFIG:Release>:-flto>)
    endif()
    if(TMNF_VALIDATOR_NATIVE_OPTIONS)
        target_compile_options(${fv_target} PRIVATE ${TMNF_VALIDATOR_NATIVE_OPTIONS})
    endif()
    if(COMMAND frametee_enable_pgo)
        frametee_enable_pgo(${fv_target})
    endif()
endforeach()

# One copy of the validator at runtime rather than one per binary.
#
# Game modules are dlopened privately, so a plugin that wants to simulate this
# game cannot borrow the module's validator and would otherwise link its own
# static copy -- sixty megabytes of it, with its own set of every global, vtable
# and typeinfo the library has. Restoring a capture the module produced into a
# sandbox built by a second copy works only because those layouts happen to
# agree; sharing one library removes the question.
#
# The submodule builds static archives only and carries no export decoration, so
# the shared library is assembled here out of them rather than by patching it,
# which is also why this is a POSIX-only arrangement: on Windows nothing in the
# archives is marked for export and the module keeps linking them statically.
if(NOT WIN32)
    set(FV_SHARED_STUB "${CMAKE_CURRENT_BINARY_DIR}/forevervalidator_shared.cpp")
    file(WRITE "${FV_SHARED_STUB}"
        "// Assembled from the validator's archives; see games/tmnf/CMakeLists.txt.\n")

    add_library(forevervalidator_shared SHARED "${FV_SHARED_STUB}")
    add_library(ForeverValidator::shared ALIAS forevervalidator_shared)
    add_dependencies(forevervalidator_shared forevervalidator_native forevervalidator_core)

    # Whole-archive by file rather than by target: the archives have to land
    # inside the flags, in this order, and naming the targets would let the link
    # line's de-duplication move them out from between them.
    target_link_options(forevervalidator_shared PRIVATE
        "-Wl,--whole-archive"
        "$<TARGET_FILE:forevervalidator_native>"
        "$<TARGET_FILE:forevervalidator_core>"
        "-Wl,--no-whole-archive")

    # The archives are LTO bitcode, so the code generation that would contract
    # a*b+c into an FMA happens here, at the link, not where -ffp-contract=off
    # was given. On a plain x86-64 baseline that never showed, because there is
    # no FMA to contract into. Raising the baseline makes it reachable, so both
    # flags have to be restated on the link line or results move.
    if(FOREVERVALIDATOR_RAISE_ISA_BASELINE)
        target_link_options(forevervalidator_shared PRIVATE
            "-ffp-contract=off"
            "-march=x86-64-v3"
            "-mno-fma")
    endif()

    # Linked again by target, privately: that is what carries the archives' own
    # dependencies -- OpenSSL and zlib, whose imported targets belong to the
    # submodule's scope and cannot be named from here. Naming them a second time
    # costs nothing, since the whole-archive pass above has already taken every
    # object out of them.
    target_link_libraries(forevervalidator_shared PRIVATE
        forevervalidator_native
        forevervalidator_core)

    # Private linkage keeps the archives out of a consumer's link line, so the
    # one usage requirement they did carry is restated here.
    target_include_directories(forevervalidator_shared INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/ForeverValidator/include")

    # Bind the library's own calls to its own definitions.
    #
    # Everything above is exported at default visibility on purpose -- that is
    # what lets the module and a plugin share one copy -- but the side effect is
    # that a call from one validator translation unit to another goes out to the
    # PLT and can be interposed by whatever was loaded first. Only functions are
    # bound, so every global, vtable and typeinfo stays as interposable as it
    # was and the one-copy arrangement is untouched. Nothing about code
    # generation changes, so physics results cannot move.
    #
    # It also closes a hazard: this module and the UltraForce plugin both
    # compile tmnf_sim.cpp against the validator's headers, with different
    # floating-point flags (see plugins/ultraforce/CMakeLists.txt), and the
    # inline functions that produces are weak definitions of symbols the
    # validator also defines. Without this the validator can end up calling the
    # plugin's copy, compiled under flags the physics was never certified for.
    target_link_options(forevervalidator_shared PRIVATE "-Wl,-Bsymbolic-functions")

    if(TMNF_VALIDATOR_LTO)
        target_compile_options(forevervalidator_shared PRIVATE $<$<CONFIG:Release>:-flto>)
        target_link_options(forevervalidator_shared PRIVATE $<$<CONFIG:Release>:-flto>)
    endif()
    if(TMNF_VALIDATOR_NATIVE_OPTIONS)
        target_compile_options(forevervalidator_shared PRIVATE ${TMNF_VALIDATOR_NATIVE_OPTIONS})
        target_link_options(forevervalidator_shared PRIVATE ${TMNF_VALIDATOR_NATIVE_OPTIONS})
    endif()
    if(COMMAND frametee_enable_pgo)
        frametee_enable_pgo(forevervalidator_shared)
    endif()

    set(TMNF_VALIDATOR_LIBS ForeverValidator::shared)
else()
    set(TMNF_VALIDATOR_LIBS ForeverValidator::native ForeverValidator::core)
endif()

add_library(tmnf_game MODULE
    tmnf_game.cpp
    tmnf_sim.cpp
    tmnf_sandbox.cpp
    tmnf_level.cpp
    tmnf_render.cpp
    tmnf_car.cpp
    tmnf_camera.cpp
    tmnf_ui.cpp
    tmnf_vehicle_model.cpp
    tmnf_pack.cpp
    tmnf_material.cpp
    tmnf_texture.cpp
    tmnf_scene.cpp
)

set_target_properties(tmnf_game PROPERTIES
    PREFIX ""
    OUTPUT_NAME "tmnf"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_VISIBILITY_PRESET hidden
)

target_include_directories(tmnf_game PRIVATE
    ${FRAMETEE_ABI_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    # What this game promises its own plugins. The module compiles against the
    # same header they do, so the two cannot disagree about a layout.
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/libs/cimgui
    ${CMAKE_SOURCE_DIR}/libs/cimgui/imgui
    # Track thumbnails are JPEGs embedded in the challenge files.
    ${CMAKE_SOURCE_DIR}/libs/stb
    # The validator's internal headers, which it only exposes to itself. Reading
    # them changes nothing in the submodule. Two files need them: the car's
    # visual model (tmnf_vehicle_model.cpp) and the simulation itself
    # (tmnf_sim.cpp), which drives ReplaySimulationSession rather than the
    # published sandbox -- see tmnf/tmnf_sim.h for why.
    ${CMAKE_CURRENT_SOURCE_DIR}/ForeverValidator/src
)

target_compile_definitions(tmnf_game PRIVATE CIMGUI_DEFINE_ENUMS_AND_STRUCTS)

target_link_libraries(tmnf_game PRIVATE ${TMNF_VALIDATOR_LIBS})

if(NOT MSVC)
    target_compile_options(tmnf_game PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# tmnf_sim.cpp is the module's simulation and is on the hot path for anything
# that drives the game headlessly, so it wants the same treatment the validator
# gets.
if(COMMAND frametee_enable_pgo)
    frametee_enable_pgo(tmnf_game)
endif()

# On Windows the ig* calls must resolve at link time against the host.
if(WIN32 AND TARGET frametee)
    target_link_libraries(tmnf_game PRIVATE frametee)
endif()
