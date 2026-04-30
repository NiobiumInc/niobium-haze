# haze_replay_bridge — OpenFHE-using helper that synthesizes the CryptoContext
# + ciphertext templates the compiler-side nbcc_fhetch_replay needs to write
# serialized_probes/ from a haze recording. Linked exclusively by haze_tests
# (and any other test/replay harness); never by libhaze itself.

add_library(haze_replay_bridge SHARED
  src/openfhe_template.cpp
  # Shared with libhaze. Each library compiles its own copy because the
  # function is hidden-visibility on both sides; no exported symbol means
  # no cross-library collision when haze_tests links both.
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/common/log.cpp
)

target_include_directories(haze_replay_bridge
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    # haze_types.h (for hazeError_t, HAZE_API, HAZE_NOEXCEPT)
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    # common/log.hpp lives under haze's src/.
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

# Niobium::fhetch supplies <niobium/compiler.h> and (transitively, via
# BUILD_INTERFACE include dirs) the OpenFHE headers. Linking publicly so
# downstream test executables that pull haze_replay_bridge in as a PRIVATE
# dep still resolve OpenFHE symbols at link time.
target_link_libraries(haze_replay_bridge PUBLIC Niobium::fhetch)

set_target_properties(haze_replay_bridge PROPERTIES
  CXX_VISIBILITY_PRESET     hidden
  VISIBILITY_INLINES_HIDDEN ON
  OUTPUT_NAME               haze_replay_bridge
)

target_compile_definitions(haze_replay_bridge PRIVATE HAZE_BUILDING_LIBRARY)

# OpenFHE headers trip the strict warning-set haze adds at the parent
# directory level (-Werror -Wpedantic -Wshadow -Wconversion -Wunused-parameter
# implicit in -Wextra). Same accommodation niobium-fhetch's own CMakeLists
# already makes for the niobium_fhetch library. We narrow the override to
# this target so haze proper keeps strict diagnostics.
target_compile_options(haze_replay_bridge PRIVATE
  -Wno-unused-parameter
  -Wno-shadow
  -Wno-conversion
  -Wno-pedantic
  -Wno-ignored-qualifiers
  -Wno-missing-field-initializers
  -Wno-deprecated-declarations
)
