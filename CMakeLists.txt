cmake_minimum_required(VERSION 3.22)
project(haze LANGUAGES C CXX VERSION 0.1.0)

# ---------------------------------------------------------------------------
# niobium-fhetch resolution
# ---------------------------------------------------------------------------
# Three-tier lookup, tried in order:
#   1. HAZE_USE_PREBUILT_FHETCH=ON  → find_package(NiobiumFhetch CONFIG).
#      Used by nix flake builds where fhetch is its own derivation.
#   2. NIOBIUM_HAZE_FHETCH_DIR set  → add_subdirectory(<that source tree>).
#      Used by parents (e.g. niobium-client) with their own checkout.
#   3. vendor/niobium-fhetch exists → add_subdirectory(vendor/niobium-fhetch).
#      Used by standalone Makefile builds.

option(HAZE_USE_PREBUILT_FHETCH
  "Use find_package(NiobiumFhetch CONFIG) instead of an in-tree add_subdirectory build. OFF (default) matches the Makefile flow; ON is for nix flake builds."
  OFF)

set(NIOBIUM_HAZE_FHETCH_DIR "" CACHE PATH
    "External niobium-fhetch source tree (empty = use vendor/niobium-fhetch)")

if(HAZE_USE_PREBUILT_FHETCH)
  find_package(NiobiumFhetch CONFIG REQUIRED)
  # TODO(niobium-fhetch): export Niobium::fhetch directly. fhetch's
  # install(EXPORT) names its target after the library
  # (Niobium::niobium_fhetch); the Niobium::fhetch alias only exists
  # in-tree. Wrap it here so the rest of haze can link Niobium::fhetch
  # uniformly across branches. GLOBAL is required so the wrapper is
  # visible from add_subdirectory(replay_bridge).
  if(NOT TARGET Niobium::fhetch)
    add_library(Niobium::fhetch INTERFACE IMPORTED GLOBAL)
    target_link_libraries(Niobium::fhetch INTERFACE Niobium::niobium_fhetch)
  endif()
elseif(NIOBIUM_HAZE_FHETCH_DIR)
  add_subdirectory(${NIOBIUM_HAZE_FHETCH_DIR}
                   ${CMAKE_BINARY_DIR}/_deps/niobium-fhetch-build)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/vendor/niobium-fhetch/CMakeLists.txt)
  add_subdirectory(vendor/niobium-fhetch)
else()
  message(FATAL_ERROR
    "haze cannot find niobium-fhetch. Either pass "
    "-DHAZE_USE_PREBUILT_FHETCH=ON (with NiobiumFhetch on CMAKE_PREFIX_PATH), "
    "-DNIOBIUM_HAZE_FHETCH_DIR=<path/to/niobium-fhetch>, or run "
    "`make sync` from this directory to init vendor/niobium-fhetch.")
endif()

# Required in every branch: the in-tree fhetch CMakeLists reads it for
# OpenFHE discovery, and the test oracle (test_basis_convert.cpp)
# reads it for the libOPENFHEcore BigInteger reference link.
if(NOT DEFINED OPENFHE_INSTALL_DIR)
  message(FATAL_ERROR
    "OPENFHE_INSTALL_DIR is not set. Pass -DOPENFHE_INSTALL_DIR=<path> or "
    "use haze's Makefile, which derives it from niobium-fhetch's install tree.")
endif()

# TODO(niobium-fhetch): expose OpenFHE includes via $<INSTALL_INTERFACE>
# so they propagate through find_package. Today they're
# $<BUILD_INTERFACE> only and don't survive the install-tree handoff,
# so re-attach them here. No-op in the add_subdirectory branches.
if(HAZE_USE_PREBUILT_FHETCH)
  set_property(TARGET Niobium::fhetch APPEND PROPERTY
    INTERFACE_INCLUDE_DIRECTORIES
      "${OPENFHE_INSTALL_DIR}/include/openfhe"
      "${OPENFHE_INSTALL_DIR}/include/openfhe/core"
      "${OPENFHE_INSTALL_DIR}/include/openfhe/pke"
      "${OPENFHE_INSTALL_DIR}/include/openfhe/binfhe"
  )
endif()

# OPENFHE_LIBRARY_PATH is used by haze_tests's libOPENFHEcore reference
# oracle (test_basis_convert.cpp). Parents (e.g. niobium-client) may
# set it explicitly to point at OpenFHE's build dir; otherwise default
# to the install layout.
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
  src/api/graph.cpp
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
  src/core/mrp_polymap.cpp
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

set_target_properties(haze PROPERTIES
  CXX_VISIBILITY_PRESET     hidden
  VISIBILITY_INLINES_HIDDEN ON
  OUTPUT_NAME               haze
)

# ---------------------------------------------------------------------------
# Replay bridge — OpenFHE-using helper that synthesizes CryptoContext +
# ciphertext templates the compiler-side replay needs, and provides the
# niobium::fhetch::result(...) overloads libhaze references from epoch.cpp.
# Lives in its own subdirectory and target so the OpenFHE includes stay
# out of libhaze sources; libhaze pulls the bridge in for symbol resolution.
# ---------------------------------------------------------------------------

add_subdirectory(replay_bridge)

target_link_libraries(haze PRIVATE Niobium::fhetch haze_replay_bridge)

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
  test/test_mul_rotation.cpp
  test/test_ring_dim_consistency.cpp
  test/test_integration_helpers.cpp
  test/test_user_crypto_context.cpp
  test/test_zero_copy_promotion.cpp
  test/test_bridge_key_extract.cpp
  # End-to-end OpenFHE-pipeline tests (real encrypt/decrypt around haze ops).
  # Tagged [integration][e2e] in Catch2 so they run under haze_sim_tests.
  # `ops.cpp` is the shared per-mode operation library these tests build on.
  test/e2e/ops.cpp
  test/e2e/test_openfhe_addition_e2e.cpp
  test/e2e/test_openfhe_sub_e2e.cpp
  test/e2e/test_openfhe_scalar_mult_e2e.cpp
  test/e2e/test_openfhe_mul_no_relin_e2e.cpp
  test/e2e/test_openfhe_mul_e2e.cpp
  test/e2e/test_openfhe_rotate_e2e.cpp
  test/e2e/test_basic_operations_sequential_e2e.cpp
  test/e2e/test_ops_ctx_params_e2e.cpp
)

target_include_directories(haze_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/test
)
# OpenFHE BigInteger headers — used by test_basis_convert.cpp's reference
# oracle. Marked SYSTEM so OpenFHE's internal warnings don't trip our -Werror.
target_include_directories(haze_tests SYSTEM PRIVATE
  ${_openfhe_include}
  ${_openfhe_include}/core
  ${_openfhe_include}/pke
  ${_openfhe_include}/binfhe
)

target_link_libraries(haze_tests PRIVATE
  haze
  haze_replay_bridge
  Catch2::Catch2WithMain
  ${_openfhe_core_lib}
)

# Test helpers (e.g. test/e2e/ops.hpp's CtxParams) use C++20 designated
# initializers that omit fields with default member initializers. clang's
# -Wmissing-field-initializers flags those omissions even though they're
# well-defined — silence here so tests can use the clean form
# `make_ctx({.mode=..., .mult_depth=..., ...})` without spelling out the
# defaulted keyswitch-key fields they don't need.
target_compile_options(haze_tests PRIVATE -Wno-missing-field-initializers)

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
# state-machine and recording behaviour only -- they do not perform a
# D2H (which would trigger replay) and do not validate FHE math results.
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
