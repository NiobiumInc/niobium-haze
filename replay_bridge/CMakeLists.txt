# haze_replay_bridge — OpenFHE-using helper that synthesizes the CryptoContext
# + ciphertext templates the compiler-side nbcc_fhetch_replay needs to write
# serialized_probes/ from a haze recording. Built as an OBJECT library so its
# TUs (incl. the niobium::fhetch::result(...) overloads epoch.cpp references)
# are absorbed unconditionally into libhaze.so and into haze_tests.
#
# Note: this OBJECT library is linked into the SAME final image as haze_objs,
# which already compiles src/common/log.cpp — so the bridge must NOT compile
# its own copy (that would be a duplicate definition). It references
# haze_objs's copy instead.
add_library(haze_replay_bridge OBJECT
  src/openfhe_template.cpp
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
# BUILD_INTERFACE include dirs) the OpenFHE headers needed to COMPILE the bridge
# objects. Consumers (libhaze, haze_tests) pull these objects in via
# $<TARGET_OBJECTS:haze_replay_bridge>, which carries no usage requirements: the
# bridge's fhetch/OpenFHE symbol references resolve against the same stack the
# consumer absorbs (_haze_absorb_fhe), and libhaze's -undefined error link turns
# any gap into a link error rather than a runtime fault.
target_link_libraries(haze_replay_bridge PUBLIC Niobium::fhetch)

# OBJECT library: no linkable artifact, so OUTPUT_NAME would be a no-op.
#
# Deliberately NOT CXX_VISIBILITY_PRESET=hidden. On arm64 macOS clang encodes
# RTTI uniqueness in bit 63 of type_info::__type_name: a default-visibility class
# gets "non-unique" typeinfo (libc++ compares by strcmp), while -fvisibility=hidden
# gives "unique" typeinfo (pointer comparison, treated as a distinct type across
# image boundaries). This is the only TU in the libhaze/haze_tests image that
# instantiates cereal save<> over OpenFHE types; if it were hidden, ld64 would let
# its unique-flavored weak typeinfo win the coalesce, and cereal's shared_ptr fast
# path (typeid(*ptr) == typeid(T)) would compare a folded bit-clear name against
# the in-memory bit-set name and miss -- throwing "unregistered polymorphic type"
# for CryptoContextImpl / NativeVector during every hazeFlush (macOS-only; Linux
# libstdc++ has no uniqueness bit). The OpenFHE stack and libnbfhetch are all
# default-visibility, so this keeps the flavor uniform. Symbol isolation does not
# rely on this preset -- libhaze.so's haze*-only export surface is enforced by the
# -exported_symbols_list / version-script at link time (see CMakeLists.txt).
set_target_properties(haze_replay_bridge PROPERTIES
  VISIBILITY_INLINES_HIDDEN ON
  POSITION_INDEPENDENT_CODE  ON
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
