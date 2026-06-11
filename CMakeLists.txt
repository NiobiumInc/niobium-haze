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

# haze ships a single, symbol-isolated libhaze.so that statically ABSORBS
# libnbfhetch + OpenFHE and exports only the haze* C ABI (see the libhaze
# section below). Force the in-tree niobium-fhetch build to be a static archive
# that links OpenFHE's *_static.a, so both are absorbed into libhaze.so rather
# than referenced as separate shared objects. These set() calls precede
# fhetch's option() declarations, so under CMP0077 they take effect without any
# -D on the command line. (No effect in the prebuilt-fhetch branch.)
set(BUILD_SHARED_LIBS OFF)
set(NIOBIUM_FHETCH_OPENFHE_STATIC ON)

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

# Resolve the concrete fhetch library target — the one with a TARGET_FILE
# (static archive) and usage requirements — across both resolution modes:
#   * in-tree  (add_subdirectory)         -> target `niobium_fhetch`
#   * prebuilt (find_package NiobiumFhetch)-> imported `Niobium::niobium_fhetch`
# libhaze absorbs this archive whole; haze_tests pulls its include dirs /
# compile defs. (Niobium::fhetch is an INTERFACE alias/wrapper with no
# TARGET_FILE, so it can't be used for the absorb.)
if(TARGET niobium_fhetch)
  set(HAZE_FHETCH_LIB niobium_fhetch)
else()
  set(HAZE_FHETCH_LIB Niobium::niobium_fhetch)
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

# haze's own translation units, compiled once as an OBJECT library and shared
# by two consumers: the shipped libhaze.so (symbol-isolated, below) and
# haze_tests (which links these objects directly with full visibility, so the
# suite can reach haze:: internals and drive the bridge's in-process OpenFHE —
# see the test section). A version script on libhaze.so would otherwise hide
# the internal C++ symbols the tests link against.
add_library(haze_objs OBJECT
  # extern "C" boundary — one cpp per public-header section.
  src/api/basis_convert.cpp
  src/api/compute.cpp
  src/api/config.cpp
  src/api/device.cpp
  src/api/error.cpp
  src/api/graph.cpp
  src/api/kernel.cpp
  src/api/lifecycle.cpp
  src/api/memory.cpp
  src/api/stream.cpp
  # Internal implementation modules.
  src/core/allocator.cpp
  src/core/backend.cpp
  src/core/basis_convert.cpp
  src/core/config.cpp
  src/core/device.cpp
  src/core/graph.cpp
  src/core/kernel_cache.cpp
  src/core/lower.cpp
  src/core/mrp_polymap.cpp
  src/core/record.cpp
  src/core/polynomial_io.cpp
  src/core/stream.cpp
  # Shared leaf utilities.
  src/common/errors.cpp
  src/common/log.cpp
)

target_include_directories(haze_objs
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    # api/lifecycle.cpp includes <haze/replay_bridge.h> (the bridge's pure-C
    # control surface). The bridge target isn't defined until the
    # add_subdirectory below, so name its public include dir directly here.
    ${CMAKE_CURRENT_SOURCE_DIR}/replay_bridge/include
    # niobium-fhetch's JSON_INCLUDE_DIR cache var is PRIVATE on its
    # target, so haze (a downstream consumer) doesn't inherit it.
    # polynomial_io.cpp uses nlohmann/json — same vendored copy.
    ${JSON_INCLUDE_DIR}
)

target_compile_definitions(haze_objs PRIVATE HAZE_BUILDING_LIBRARY)

set_target_properties(haze_objs PROPERTIES
  CXX_VISIBILITY_PRESET     hidden
  VISIBILITY_INLINES_HIDDEN ON
  POSITION_INDEPENDENT_CODE  ON
)

# haze_objs compiles <niobium/...> includes from fhetch (e.g. epoch.cpp); pull
# fhetch's include-dir usage requirements. OBJECT libraries only compile, so no
# link happens here — the actual archive link is on the haze target below.
target_link_libraries(haze_objs PRIVATE Niobium::fhetch)

# ---------------------------------------------------------------------------
# Replay bridge — OpenFHE-using helper that synthesizes CryptoContext +
# ciphertext templates the compiler-side replay needs, and provides the
# niobium::fhetch::result(...) overloads libhaze references from epoch.cpp.
# Built as an OBJECT library (see replay_bridge/CMakeLists.txt) so all its TUs
# (incl. result()) are absorbed unconditionally; its OpenFHE includes stay out
# of libhaze's own sources.
# ---------------------------------------------------------------------------

add_subdirectory(replay_bridge)

# ---------------------------------------------------------------------------
# libhaze — the single shipped artifact: a DYNAMIC shared object that
# statically absorbs libnbfhetch + the bridge + OpenFHE's *_static.a and
# exports ONLY the haze* C ABI. FIDESlib links this .so dynamically and calls
# the C ABI; FIDESlib's own (different, ABI-incompatible) OpenFHE is the static
# side in the same process. The symbol isolation below is what makes that safe.
# ---------------------------------------------------------------------------

add_library(haze SHARED
  $<TARGET_OBJECTS:haze_objs>
  $<TARGET_OBJECTS:haze_replay_bridge>
)

set_target_properties(haze PROPERTIES OUTPUT_NAME haze)

target_include_directories(haze PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

# Header-only typed kernel layer (include/haze/cxx/). Consumer floor is
# C++20 — everything lowers to the C ABI, so libhaze's internal C++23
# never reaches the consumer's toolchain.
add_library(haze_cxx INTERFACE)
add_library(haze::cxx ALIAS haze_cxx)
target_include_directories(haze_cxx INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(haze_cxx INTERFACE cxx_std_20)
target_link_libraries(haze_cxx INTERFACE haze)

# Floor check: the global standard is C++23, so this object target pins
# -std=c++20 and compiles a TU that includes only the public cxx
# headers. A C++23-ism leaking into include/haze/cxx/ fails the BUILD.
add_library(haze_cxx_floor20 OBJECT test/cxx/floor20_check.cpp)
target_link_libraries(haze_cxx_floor20 PRIVATE haze_cxx)
set_target_properties(haze_cxx_floor20 PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

find_package(Threads REQUIRED)

# The niobium FHE stack to absorb: fhetch's archive + OpenFHE's *_static.a.
# OpenFHE must be built with -DBUILD_STATIC=ON -DCMAKE_POSITION_INDEPENDENT_CODE=ON
# (haze's Makefile / flake / build-matrix do this; a parent consumer such as
# niobium-client must do the same in its OpenFHE build, AND force the fhetch
# subtree static — set(BUILD_SHARED_LIBS OFF) + NIOBIUM_FHETCH_OPENFHE_STATIC=ON
# before its add_subdirectory(niobium-fhetch) — or these archives won't exist).
# Fail fast with a clear message rather than a raw linker error if they're absent.
set(_haze_openfhe_static
  ${OPENFHE_LIBRARY_PATH}/libOPENFHEpke_static.a
  ${OPENFHE_LIBRARY_PATH}/libOPENFHEbinfhe_static.a
  ${OPENFHE_LIBRARY_PATH}/libOPENFHEcore_static.a
)
# OpenFHE static archives are libOPENFHE*_static.a on Linux AND macOS, so the
# existence check applies to both platforms.
foreach(_a IN LISTS _haze_openfhe_static)
  if(NOT EXISTS "${_a}")
    message(FATAL_ERROR
      "libhaze absorbs OpenFHE statically but ${_a} is missing. Build OpenFHE "
      "with -DBUILD_STATIC=ON -DCMAKE_POSITION_INDEPENDENT_CODE=ON (see the "
      "Makefile's OPENFHE_CMAKE_FLAGS), or point OPENFHE_LIBRARY_PATH at an "
      "install that has the *_static.a archives.")
  endif()
endforeach()

# NOTE: the archives are absorbed by raw path (below), which bypasses OpenFHE's
# imported-target INTERFACE_LINK_LIBRARIES. That is complete ONLY because the
# niobium OpenFHE build keeps WITH_NTL / WITH_TCM / WITH_OPENMP = OFF (no gmp /
# ntl / tcmalloc / omp transitive deps). If any of those are enabled upstream,
# add the corresponding runtime here or libhaze will hit undefined symbols at
# RUNTIME (a .so link tolerates undefined symbols; the crash surfaces later).
set(_haze_fhe_archives
  $<TARGET_FILE:${HAZE_FHETCH_LIB}>
  ${_haze_openfhe_static}
)

# Absorb that stack WHOLE: fhetch and the instrumented OpenFHE are mutually
# recursive (OpenFHE's CPROBES + auto hooks resolve back into fhetch), so plain
# archive linking would drop needed TUs / fail to resolve on a single pass.
# `_haze_absorb_fhe` is reused by both libhaze.so and haze_tests. macOS uses
# -force_load per archive; GNU ld / LLD use a --whole-archive group. It is
# applied via target_link_options (not _libraries) so CMake cannot reorder or
# de-duplicate the archive paths out of the --whole-archive/--no-whole-archive
# pair. (A clang+lld Windows build also takes the else path.)
if(APPLE)
  set(_haze_absorb_fhe "")
  foreach(_lib ${_haze_fhe_archives})
    list(APPEND _haze_absorb_fhe "-Wl,-force_load,${_lib}")
  endforeach()
else()
  set(_haze_absorb_fhe
    -Wl,--whole-archive ${_haze_fhe_archives} -Wl,--no-whole-archive)
endif()

# Fail the LINK (not at runtime) if absorbing the stack leaves any strong symbol
# undefined -- e.g. GOMP_*/gmp/ntl from an OpenFHE built WITH_OPENMP/NTL/TCM=ON
# (see the NTL/TCM/OPENMP=OFF note above). A .so otherwise tolerates undefined
# symbols and defers the crash. macOS ld64 already defaults to -undefined error.
if(APPLE)
  set(_haze_no_undef -Wl,-undefined,error)
else()
  set(_haze_no_undef -Wl,--no-undefined)
endif()

target_link_libraries(haze PRIVATE Threads::Threads)
target_link_options(haze PRIVATE ${_haze_absorb_fhe} ${_haze_no_undef})

# Localize every non-haze* symbol so the absorbed OpenFHE cannot collide with a
# second OpenFHE (e.g. FIDESlib's 1.5.1) in the same process. Hidden visibility
# alone is NOT enough — objects pulled from a static archive keep the default
# visibility OpenFHE's build gave them — so --exclude-libs + the version script
# (allowlisting only the haze* C ABI) are required.
if(APPLE)
  # macOS: -exported_symbols_list makes every symbol not matching the allowlist
  # local. Empirically this localizes weak-coalesced defs too (C++ vtables /
  # typeinfo): a release libhaze.dylib exports exactly the haze* symbols and no
  # lbcrypto/cprobe symbols (verified with Apple/llvm nm). The symbol-leak check
  # backstops it on every build -- build-matrix runs it on macos-14 and the
  # flake `isolation` check on aarch64-darwin, both with llvm/Apple nm (GNU
  # binutils nm misreads Mach-O private-extern symbols and must not be used). If
  # the leak check ever fires on macOS, switch to per-archive -load_hidden.
  target_link_options(haze PRIVATE
    -Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/linker/haze_exports.sym)
else()
  target_link_options(haze PRIVATE
    -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/linker/haze_exports.map
    -Wl,--exclude-libs,ALL)
endif()

# When fhetch is built in-tree, the $<TARGET_FILE:> path reference doesn't carry
# a build-order edge; make it explicit. (No-op/invalid for the imported
# prebuilt target, which is already built.)
if(TARGET niobium_fhetch)
  add_dependencies(haze niobium_fhetch)
endif()

# ---------------------------------------------------------------------------
# Tests (opt-out)
# ---------------------------------------------------------------------------
# Everything below builds the test suite + the isolation symbol-leak check and
# needs Catch2 v3. Turn OFF to build just libhaze.so (e.g. the build-only CI
# matrix on platforms without Catch2). Default ON preserves `make test`.
option(HAZE_BUILD_TESTS "Build the haze test suite + isolation symbol-leak check (needs Catch2 v3)." ON)
if(NOT HAZE_BUILD_TESTS)
  return()
endif()

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

# Black-box e2e suite (opt-in). Builds haze_e2e_tests, which links the SHIPPED
# libhaze.so (haze* C ABI only) + a SEPARATE stock OpenFHE. Default OFF so plain
# `-DHAZE_BUILD_TESTS=ON` consumers (e.g. niobium-client via add_subdirectory)
# are unaffected and need not know about the stock OpenFHE; haze's own Makefile
# and flake turn it ON and supply HAZE_TEST_OPENFHE_DIR.
option(HAZE_BUILD_E2E_TESTS
       "Build the black-box e2e suite (haze_e2e_tests; needs HAZE_TEST_OPENFHE_DIR)." OFF)

if(HAZE_BUILD_E2E_TESTS)
  # Stock (unmodified upstream) OpenFHE for haze_e2e_tests' reference crypto.
  # Built SHARED and linked ONLY into that exe, NEVER absorbed into libhaze.so.
  # It MUST be a different install than OPENFHE_INSTALL_DIR (the instrumented
  # 1.4.2 fork libhaze absorbs) so the two can never collide in one image.
  set(HAZE_TEST_OPENFHE_DIR "" CACHE PATH
      "Install prefix of the stock OpenFHE for haze_e2e_tests (distinct from OPENFHE_INSTALL_DIR).")
  if(NOT HAZE_TEST_OPENFHE_DIR)
    message(FATAL_ERROR
      "HAZE_BUILD_E2E_TESTS=ON requires -DHAZE_TEST_OPENFHE_DIR=<stock-openfhe-install> "
      "(haze's Makefile builds vendor/openfhe into vendor/lib/openfhe-stock).")
  endif()
  # Compare resolved real paths so a trailing slash / symlink / relative form
  # cannot alias the instrumented install past this guard.
  get_filename_component(_haze_test_ofhe_real "${HAZE_TEST_OPENFHE_DIR}" REALPATH)
  get_filename_component(_haze_instr_ofhe_real "${OPENFHE_INSTALL_DIR}" REALPATH)
  if(_haze_test_ofhe_real STREQUAL _haze_instr_ofhe_real)
    message(FATAL_ERROR
      "HAZE_TEST_OPENFHE_DIR must differ from OPENFHE_INSTALL_DIR: the stock "
      "OpenFHE must never share an install with the instrumented fork libhaze.so "
      "absorbs (both resolved to ${_haze_test_ofhe_real}).")
  endif()
endif()

# haze_tests links the SAME compiled objects as libhaze.so (haze_objs + the
# bridge) PLUS the niobium FHE stack absorbed whole (${_haze_absorb_fhe}), but
# WITHOUT the version script — so internal haze:: symbols, the bridge C++ API,
# and the BigInteger reference oracle (test_basis_convert.cpp) all resolve at
# static-link time against one in-exe OpenFHE + one niobium::compiler(). Only
# the OpenFHE headers are needed explicitly here.
set(_openfhe_include "${OPENFHE_INSTALL_DIR}/include/openfhe")

add_executable(haze_tests
  test/test_build.cpp
  test/test_tape.cpp
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
  test/test_d2d_copy.cpp
  test/test_mrp_group_reuse.cpp
  test/test_hardware_format.cpp
  test/test_threaded_record.cpp
  test/test_kernel_memo.cpp
  test/cxx/test_kernel_guards.cpp
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
  # Same compiled code as libhaze.so, linked directly — NOT the shipped
  # libhaze.so, whose version script hides the internal symbols these tests use.
  $<TARGET_OBJECTS:haze_objs>
  $<TARGET_OBJECTS:haze_replay_bridge>
)

target_include_directories(haze_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/test
  # replay_bridge.h — the bridge target is consumed as objects (not linked), so
  # name its public include dir directly.
  ${CMAKE_CURRENT_SOURCE_DIR}/replay_bridge/include
)
# Headers for the test sources, SYSTEM so neither set's internal warnings trip
# our -Werror. haze_tests absorbs the fhetch archive via a file path (no usage
# requirements), so its include dirs are named explicitly:
#   * The explicit ${_openfhe_include}{,/core,/pke,/binfhe} entries are
#     LOAD-BEARING: in prebuilt (find_package) mode the installed fhetch target
#     carries only its own include dir — OpenFHE's dirs are BUILD_INTERFACE-only
#     on fhetch and do NOT survive install. In-tree mode they'd be covered by
#     the property below, but don't rely on that; keep these.
#   * The fhetch interface include dirs supply <niobium/compiler.h> (used by
#     integration_helpers.hpp) in both modes.
target_include_directories(haze_tests SYSTEM PRIVATE
  ${_openfhe_include}
  ${_openfhe_include}/core
  ${_openfhe_include}/pke
  ${_openfhe_include}/binfhe
  $<TARGET_PROPERTY:${HAZE_FHETCH_LIB},INTERFACE_INCLUDE_DIRECTORIES>
)

target_link_libraries(haze_tests PRIVATE
  Threads::Threads
  Catch2::Catch2WithMain
)
# Absorb the same fhetch+OpenFHE stack as libhaze (via options, to pin the
# --whole-archive ordering). haze_tests is an executable, so it gets no version
# script (it needs the internal haze:: symbols) and -undefined error by default.
target_link_options(haze_tests PRIVATE ${_haze_absorb_fhe})
# target_link_options drops the $<TARGET_FILE:> auto-dependency, so make the
# in-tree fhetch build-order edge explicit (matches the haze target above).
if(TARGET niobium_fhetch)
  add_dependencies(haze_tests niobium_fhetch)
endif()

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

# The niobium-instrumented OpenFHE headers (cryptocontext.h etc.) only declare
# g_replay_mode / the niobium_auto hooks under OPENFHE_CPROBES. haze_tests
# absorbs the fhetch archive by file path (no usage requirements), so propagate
# fhetch's PUBLIC compile definitions (incl. OPENFHE_CPROBES) explicitly.
target_compile_definitions(haze_tests PRIVATE
  $<TARGET_PROPERTY:${HAZE_FHETCH_LIB},INTERFACE_COMPILE_DEFINITIONS>)

# ---------------------------------------------------------------------------
# haze_e2e_tests — black-box suite: links the SHIPPED libhaze.so (haze* C ABI
# only) + a SEPARATE, stock OpenFHE. This is the "a consumer runs its own
# current OpenFHE alongside haze" proof: the exe sees only the haze* C ABI
# (libhaze hides its absorbed OpenFHE 1.4.2) plus its own stock OpenFHE 1.5.1.
# Deliberately NO src/, NO fhetch includes, NO OPENFHE_CPROBES. The stock
# OpenFHE is SHARED, so it is structurally ineligible for libhaze's *_static.a
# whole-archive absorb. ops.cpp is shared with the internal exe; built here
# without OPENFHE_CPROBES, its PausedRecording guard compiles away to a no-op.
# ---------------------------------------------------------------------------
if(HAZE_BUILD_E2E_TESTS)
set(_stock_openfhe_include "${HAZE_TEST_OPENFHE_DIR}/include/openfhe")
set(_stock_openfhe_lib "${HAZE_TEST_OPENFHE_DIR}/lib")

add_executable(haze_e2e_tests
  test/e2e/ops.cpp
  test/e2e/public/test_simple_program.cpp
  test/e2e/public/test_interleave.cpp
  test/e2e/public/test_cxx_simple_program.cpp
)

# Public headers only: haze C ABI + the bridge's public C ABI + the test
# helpers + ops.hpp; plus the stock OpenFHE headers. No src/, no fhetch.
target_include_directories(haze_e2e_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/replay_bridge/include
  ${CMAKE_CURRENT_SOURCE_DIR}/test
  ${CMAKE_CURRENT_SOURCE_DIR}/test/e2e
)
target_include_directories(haze_e2e_tests SYSTEM PRIVATE
  ${_stock_openfhe_include}
  ${_stock_openfhe_include}/core
  ${_stock_openfhe_include}/pke
  ${_stock_openfhe_include}/binfhe
)

# Link the shipped libhaze.so as a black box (the `haze` target) + the stock
# OpenFHE SHARED libs by path (the unversioned .so/.dylib symlink). Shared =>
# never absorbed, never --whole-archive'd. Explicitly the .so/.dylib, never .a.
target_link_libraries(haze_e2e_tests PRIVATE
  haze
  Threads::Threads
  Catch2::Catch2WithMain
  ${_stock_openfhe_lib}/${CMAKE_SHARED_LIBRARY_PREFIX}OPENFHEpke${CMAKE_SHARED_LIBRARY_SUFFIX}
  ${_stock_openfhe_lib}/${CMAKE_SHARED_LIBRARY_PREFIX}OPENFHEbinfhe${CMAKE_SHARED_LIBRARY_SUFFIX}
  ${_stock_openfhe_lib}/${CMAKE_SHARED_LIBRARY_PREFIX}OPENFHEcore${CMAKE_SHARED_LIBRARY_SUFFIX}
)

# Same designated-initializer relaxation as the internal exe (ops.hpp CtxParams).
target_compile_options(haze_e2e_tests PRIVATE -Wno-missing-field-initializers)

# Resolve the stock OpenFHE .so from the build tree (the stock libs are linked
# by raw path, not a CMake target, so they are not auto-added to the RPATH;
# libhaze's dir IS auto-added via the linked `haze` target). The INSTALL_RPATH
# is left to the global CMAKE_INSTALL_RPATH (the nix build appends the stock lib
# dir there) so it can also reach the installed libhaze under $ORIGIN/../lib.
set_target_properties(haze_e2e_tests PROPERTIES BUILD_RPATH "${_stock_openfhe_lib}")
endif() # HAZE_BUILD_E2E_TESTS

# ---------------------------------------------------------------------------
# ctest entries
# ---------------------------------------------------------------------------

option(NIOBIUM_CLIENT_HAZE_WITH_TRANSPORT_TESTS
       "Register haze_transport_tests (transport round-trip via nbcc_fhetch_replay). Default OFF so config-client-release works without NIOBIUM_COMPILER_ROOT. Turn ON to add the ctest entry; the Makefile's test-transport target invokes scripts/test_haze_integration_standalone.sh directly without needing this flag."
       OFF)

# Unit suite: in-process, no server, no compiler-side binary.
# HAZE_TARGET=local pins the compiler target so haze never attempts to
# dispatch to nbcc_fhetch_replay during unit tests. Unit tests observe
# state-machine and recording behaviour only -- they do not flush a
# recording (which would dispatch replay) and do not validate FHE math results.
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

# E2E suite: the public-C-ABI + stock-OpenFHE black-box exe. HAZE_TARGET=local
# runs the in-process FHETCH simulator inside libhaze.so. WORKING_DIRECTORY
# (created at configure) keeps probe/trace artifacts under build/runs and out of
# the build root when invoked via `ctest` directly; the Makefile/flake cd here too.
if(HAZE_BUILD_E2E_TESTS)
  file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/runs)
  add_test(NAME haze_e2e_tests
           COMMAND haze_e2e_tests)
  set_tests_properties(haze_e2e_tests PROPERTIES
      ENVIRONMENT "HAZE_TARGET=local"
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runs")
endif()

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

# Isolation proof: assert the shipped libhaze exports ONLY the haze* C ABI, so
# its absorbed OpenFHE 1.4.2 cannot collide with another OpenFHE in one process.
if(NOT WIN32)
  add_test(NAME haze_isolated_symbol_leak
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_symbol_leak.sh
                   $<TARGET_FILE:haze>)
endif()
