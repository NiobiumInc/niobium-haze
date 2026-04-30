cmake_minimum_required(VERSION 3.22)
project(haze LANGUAGES C CXX VERSION 0.1.0)

# ---------------------------------------------------------------------------
# niobium-fhetch resolution
# ---------------------------------------------------------------------------
# Two-tier lookup:
#   1. If NIOBIUM_HAZE_FHETCH_DIR is set, add_subdirectory() that external
#      source tree. Parents (e.g. niobium-client) that already own a
#      niobium-fhetch checkout point haze at it via this cache var.
#   2. Else, fall back to vendor/niobium-fhetch — haze's own submodule.
#      Used for standalone builds (run `make sync` from this directory
#      to populate it).
#
# OPENFHE_INSTALL_DIR is honored by niobium-fhetch's own CMakeLists when
# we add it via either path, so injecting it here covers transitive
# OpenFHE discovery for both branches.

set(NIOBIUM_HAZE_FHETCH_DIR "" CACHE PATH
    "External niobium-fhetch source tree to use instead of haze's vendor/niobium-fhetch (empty = use the vendored submodule)")
if(NIOBIUM_HAZE_FHETCH_DIR)
  add_subdirectory(${NIOBIUM_HAZE_FHETCH_DIR}
                   ${CMAKE_BINARY_DIR}/_deps/niobium-fhetch-build)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/vendor/niobium-fhetch/CMakeLists.txt)
  add_subdirectory(vendor/niobium-fhetch)
else()
  message(FATAL_ERROR
    "haze cannot find niobium-fhetch. Either pass "
    "-DNIOBIUM_HAZE_FHETCH_DIR=<path/to/niobium-fhetch>, or run "
    "`make sync` from this directory to init vendor/niobium-fhetch.")
endif()

# OPENFHE_INSTALL_DIR may be set by:
#   - the parent build (niobium-client passes it explicitly)
#   - haze's own Makefile (standalone build)
#   - the user via -D
# When unset, niobium-fhetch's CMakeLists defaults to its own
# vendor/lib/openfhe install. The test executable below also reads it
# for the OpenFHE BigInteger reference oracle.
if(NOT DEFINED OPENFHE_INSTALL_DIR)
  message(FATAL_ERROR
    "OPENFHE_INSTALL_DIR is not set. Pass -DOPENFHE_INSTALL_DIR=<path> or "
    "use haze's Makefile, which derives it from niobium-fhetch's install tree.")
endif()

# OPENFHE_LIBRARY_PATH locates libOPENFHEcore for the haze_tests reference
# oracle (test_basis_convert.cpp). Most callers already supply it:
#   - niobium-client sets it at the parent CMake level, pointing at
#     OpenFHE's *build* output dir to dodge an extra install copy.
#   - niobium-fhetch's CMakeLists sets it to ${OPENFHE_INSTALL_DIR}/lib
#     when haze pulls fhetch in via add_subdirectory.
# This fallback only fires when haze is built against an externally-resolved
# Niobium::fhetch target whose enclosing project did not also provide
# OPENFHE_LIBRARY_PATH (a partial-parent scenario).
if(NOT DEFINED OPENFHE_LIBRARY_PATH)
  set(OPENFHE_LIBRARY_PATH "${OPENFHE_INSTALL_DIR}/lib")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Emit compile_commands.json for clangd / IDE integration.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Compiler diagnostics
# ---------------------------------------------------------------------------

add_compile_options(
  -Wall -Wextra -Werror -Wpedantic
  -Wshadow -Wconversion
)

# Clang's thread-safety analysis enforces the lock contracts annotated in
# common/thread_safety.hpp at compile time. No-op under GCC.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wthread-safety)
endif()

# ---------------------------------------------------------------------------
# Sanitizer options (mutually exclusive)
# ---------------------------------------------------------------------------

option(HAZE_SANITIZERS "Enable ASAN + UBSAN" OFF)
option(HAZE_TSAN       "Enable TSAN"         OFF)

# Selects which FBC variant the test reference oracle invokes through
# OpenFHE. Must match whichever variant the niobium-instrumented OpenFHE
# was compiled with (its own WITH_REDUCED_NOISE flag). Default ON
# matches niobium's vendored OpenFHE.
option(HAZE_FBC_REDUCED_NOISE
       "Test oracle uses ReducedNoise variant (OFF: Standard)" ON)

if(HAZE_SANITIZERS AND HAZE_TSAN)
  message(FATAL_ERROR "HAZE_SANITIZERS and HAZE_TSAN are mutually exclusive.")
endif()

if(HAZE_SANITIZERS)
  # -fno-sanitize=enum: HAZE's hazeError_t is a C enum that callers may
  # legitimately receive from older library builds with values not in the
  # current enum (forward-compat). UBSAN's strict-enum check flags those
  # legitimate values as undefined behaviour. Disable that one check; the
  # rest of UBSAN remains active.
  add_compile_options(-fsanitize=address,undefined -fno-sanitize=enum
                      -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined -fno-sanitize=enum)
endif()

if(HAZE_TSAN)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()

# ---------------------------------------------------------------------------
# libhaze shared library
# ---------------------------------------------------------------------------
# Niobium::fhetch (libnbfhetch) supplies:
#   - the niobium::compiler() singleton (recording + replay dispatch)
#   - the fhetch::* recording API used by haze's compute / epoch paths
#   - public include dirs for <niobium/...> and OpenFHE headers (via
#     BUILD_INTERFACE on the niobium-fhetch target)
# So haze itself needs no other niobium-side dep.

add_library(haze SHARED
  # extern "C" boundary — one cpp per public-header section.
  src/api/basis_convert.cpp
  src/api/compute.cpp
  src/api/config.cpp
  src/api/device.cpp
  src/api/error.cpp
  src/api/lifecycle.cpp
  src/api/memory.cpp
  src/api/stream.cpp
  # Internal implementation modules.
  src/core/allocator.cpp
  src/core/backend.cpp
  src/core/basis_convert.cpp
  src/core/config.cpp
  src/core/device.cpp
  src/core/epoch.cpp
  src/core/polynomial_io.cpp
  src/core/stream.cpp
  # Shared leaf utilities.
  src/common/errors.cpp
  src/common/log.cpp
)

target_include_directories(haze
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    # niobium-fhetch's JSON_INCLUDE_DIR cache var is PRIVATE on its
    # target, so haze (a downstream consumer) doesn't inherit it.
    # polynomial_io.cpp uses nlohmann/json — same vendored copy.
    ${JSON_INCLUDE_DIR}
)

target_compile_definitions(haze PRIVATE HAZE_BUILDING_LIBRARY)

target_link_libraries(haze PRIVATE Niobium::fhetch)

set_target_properties(haze PROPERTIES
  CXX_VISIBILITY_PRESET     hidden
  VISIBILITY_INLINES_HIDDEN ON
  OUTPUT_NAME               haze
)

# ---------------------------------------------------------------------------
# Replay bridge — OpenFHE-using helper for synthesizing the CryptoContext +
# ciphertext templates the compiler-side replay needs. Lives in its own
# subdirectory and target so libhaze stays FHETCH-only; only haze_tests
# (and any other replay-aware harness) link it in.
# ---------------------------------------------------------------------------

add_subdirectory(replay_bridge)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
# One executable, three ctest entries split by Catch2 tag + HAZE_TARGET:
#   - haze_unit_tests       : runs everything not tagged [integration]
#                             with HAZE_TARGET=local. Unit tests do not
#                             trigger replay so the target value is
#                             effectively a recording-only setting.
#   - haze_sim_tests        : runs [integration]-tagged cases through
#                             libnbfhetch's in-process FHETCH simulator
#                             (HAZE_TARGET=local). No external binary
#                             required; default test target.
#   - haze_transport_tests  : runs [integration]-tagged cases via the
#                             FHETCH transport (forwarder + server) so
#                             the niobium-compiler-built nbcc_fhetch_replay
#                             executes the recorded project. Opt-in via
#                             NIOBIUM_CLIENT_HAZE_WITH_TRANSPORT_TESTS.

enable_testing()
find_package(Catch2 3 REQUIRED)

# Test reference oracle (in test_basis_convert.cpp) calls into OpenFHE
# BigInteger primitives directly. These come from the parent project's
# OpenFHE install tree.
set(_openfhe_include "${OPENFHE_INSTALL_DIR}/include/openfhe")
set(_openfhe_core_lib_so   "${OPENFHE_LIBRARY_PATH}/libOPENFHEcore.so")
set(_openfhe_core_lib_dylib "${OPENFHE_LIBRARY_PATH}/libOPENFHEcore.dylib")
if(EXISTS "${_openfhe_core_lib_dylib}")
  set(_openfhe_core_lib "${_openfhe_core_lib_dylib}")
elseif(EXISTS "${_openfhe_core_lib_so}")
  set(_openfhe_core_lib "${_openfhe_core_lib_so}")
else()
  message(FATAL_ERROR
    "libOPENFHEcore not found under ${OPENFHE_LIBRARY_PATH}. The haze test "
    "oracle links it directly; ensure niobium-client built OpenFHE first.")
endif()

add_executable(haze_tests
  test/test_build.cpp
  test/test_memory.cpp
  test/test_config.cpp
  test/test_compute.cpp
  test/test_basis_convert.cpp
  test/test_ring_dim_consistency.cpp
)

target_include_directories(haze_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/test
)
# OpenFHE BigInteger headers — used by test_basis_convert.cpp's reference
# oracle. Marked SYSTEM so OpenFHE's internal warnings don't trip our -Werror.
target_include_directories(haze_tests SYSTEM PRIVATE
  ${_openfhe_include}
  ${_openfhe_include}/core
)

target_link_libraries(haze_tests PRIVATE
  haze
  haze_replay_bridge
  Catch2::Catch2WithMain
  ${_openfhe_core_lib}
)

# Pass the FBC variant choice through to the test oracle.
target_compile_definitions(haze_tests PRIVATE
  HAZE_FBC_REDUCED_NOISE=$<BOOL:${HAZE_FBC_REDUCED_NOISE}>)

# ---------------------------------------------------------------------------
# ctest entries
# ---------------------------------------------------------------------------

option(NIOBIUM_CLIENT_HAZE_WITH_TRANSPORT_TESTS
       "Register haze_transport_tests (transport round-trip via nbcc_fhetch_replay). Default OFF so config-client-release works without NIOBIUM_COMPILER_ROOT. Turn ON to add the ctest entry; the Makefile's test-transport target invokes scripts/test_haze_integration_standalone.sh directly without needing this flag."
       OFF)

# Unit suite: in-process, no server, no compiler-side binary.
# HAZE_TARGET=local pins the compiler target so haze never attempts to
# dispatch to nbcc_fhetch_replay during unit tests. Unit tests observe
# state-machine and recording behaviour only -- they do not call
# hazeReplay() and do not validate FHE math results.
add_test(NAME haze_unit_tests
         COMMAND haze_tests "~[integration]")
set_tests_properties(haze_unit_tests PROPERTIES
    ENVIRONMENT "HAZE_TARGET=local")

# Sim suite: in-process FHETCH simulator. No transport setup needed;
# always registered so `ctest` (or the Makefile's `make test`) exercises
# real FHE math without external dependencies.
#
# Target literal must match haze::kLocalTarget in src/core/config.hpp
# AND the test-sim recipe in Makefile.
add_test(NAME haze_sim_tests
         COMMAND haze_tests "[integration]")
set_tests_properties(haze_sim_tests PROPERTIES
    ENVIRONMENT "HAZE_TARGET=local")

if(NIOBIUM_CLIENT_HAZE_WITH_TRANSPORT_TESTS)
  set(NIOBIUM_COMPILER_ROOT "" CACHE PATH
      "Path to a niobium-compiler checkout with build/nbcc_fhetch_replay. Required when NIOBIUM_CLIENT_HAZE_WITH_TRANSPORT_TESTS=ON.")
  if(NOT NIOBIUM_COMPILER_ROOT)
    message(FATAL_ERROR
      "NIOBIUM_CLIENT_HAZE_WITH_TRANSPORT_TESTS=ON requires "
      "-DNIOBIUM_COMPILER_ROOT=<path/to/niobium-compiler>")
  endif()

  add_test(NAME haze_transport_tests
           COMMAND
             ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_haze_integration.sh)
  set_tests_properties(haze_transport_tests PROPERTIES
    ENVIRONMENT
      "NIOBIUM_COMPILER_ROOT=${NIOBIUM_COMPILER_ROOT};NIOBIUM_CLIENT_BUILD=${CMAKE_BINARY_DIR};HAZE_TEST_BIN=$<TARGET_FILE:haze_tests>;OPENFHE_LIB=${OPENFHE_LIBRARY_PATH};HAZE_TARGET=FUNC_SIM")
endif()
