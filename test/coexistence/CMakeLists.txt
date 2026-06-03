# FIDESlib-coexistence test: one executable that dynamically links the shipped,
# symbol-isolated libhaze.so (hidden OpenFHE 1.4.2 inside) AND statically links
# OpenFHE 1.5.1 — the release FIDESlib pins — proving the two OpenFHE copies
# coexist in one process with no symbol collision. Built only when
# HAZE_WITH_COEXISTENCE_TEST=ON (see the top-level CMakeLists.txt).

if(NOT FIDESLIB_OPENFHE_DIR)
  message(FATAL_ERROR
    "HAZE_WITH_COEXISTENCE_TEST=ON requires -DFIDESLIB_OPENFHE_DIR=<path to an "
    "OpenFHE 1.5.1 install built with -DBUILD_STATIC=ON> (e.g. FIDESlib's "
    "references/FIDESlib/openfhe-install).")
endif()

# FIDESlib's OpenFHE 1.5.1 — found ONLY at the given path so it can never pick
# up haze's own (1.4.2) install.
find_package(OpenFHE 1.5.1 CONFIG EXACT REQUIRED
  PATHS "${FIDESLIB_OPENFHE_DIR}/lib/OpenFHE" NO_DEFAULT_PATH)

# OpenFHE 1.5.1's imported targets carry a bare `-fopenmp` link flag; provide a
# concrete OpenMP runtime (with its library path) so it resolves regardless of
# toolchain. haze's own absorbed OpenFHE is WITH_OPENMP=OFF, so this is the
# process's only OpenMP runtime.
find_package(OpenMP REQUIRED)

add_executable(haze_coexistence_test test_coexistence.cpp)

# 1.5.1 headers, SYSTEM so they don't trip the project's -Werror. The TU itself
# includes ONLY these + haze's C ABI; never a niobium-1.4.2 header.
target_include_directories(haze_coexistence_test SYSTEM PRIVATE
  ${OpenFHE_INCLUDE}
  ${OpenFHE_INCLUDE}/core
  ${OpenFHE_INCLUDE}/pke
  ${OpenFHE_INCLUDE}/binfhe
  ${OpenFHE_INCLUDE}/third-party/include
)
# haze's C ABI headers (haze.h + the pure-C replay_bridge.h).
target_include_directories(haze_coexistence_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../replay_bridge/include
)

# haze: dynamically linked (the shipped .so). OpenFHE 1.5.1: its static imported
# targets, whose interface also supplies the OpenMP / -ldl the archives need.
#
# These imported target names (OPENFHE{pke,binfhe,core}_static) are the SAME
# bare names the in-tree fhetch build links by (resolved there via a -L dir to
# the 1.4.2 archives). That's safe here ONLY because libhaze absorbs its 1.4.2
# stack by raw $<TARGET_FILE:>/path — it never links the fhetch target's
# interface — so these targets unambiguously refer to find_package's 1.5.1
# imports. Do NOT add a link to Niobium::fhetch / niobium_fhetch in this
# configuration or the 1.4.2 and 1.5.1 archives would cross.
target_link_libraries(haze_coexistence_test PRIVATE
  haze
  OPENFHEpke_static
  OPENFHEbinfhe_static
  OPENFHEcore_static
  OpenMP::OpenMP_CXX
)

# Our use of OpenFHE 1.5.1 template APIs trips the strict project warning set
# even with SYSTEM headers; relax for this TU only (mirrors replay_bridge).
target_compile_options(haze_coexistence_test PRIVATE
  -Wno-conversion -Wno-shadow -Wno-pedantic -Wno-deprecated-declarations
  -Wno-unused-parameter -Wno-ignored-qualifiers -Wno-missing-field-initializers)

add_test(NAME haze_coexistence_test COMMAND haze_coexistence_test)
set_tests_properties(haze_coexistence_test PROPERTIES
  ENVIRONMENT "HAZE_TARGET=local")
