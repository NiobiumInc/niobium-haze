cmake_minimum_required(VERSION 3.22)
project(haze LANGUAGES C CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_EXTENSIONS OFF)

# Emit compile_commands.json for clangd / IDE integration.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING
    "Build type: Debug, Release, RelWithDebInfo, or MinSizeRel" FORCE)
endif()

# ---------------------------------------------------------------------------
# Compiler diagnostics
# ---------------------------------------------------------------------------

add_compile_options(
  -Wall -Wextra -Werror -Wpedantic
  -Wshadow -Wconversion
)

# Clang's thread-safety analysis enforces the lock contracts annotated in
# haze_thread_safety.hpp at compile time. No-op under GCC.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wthread-safety)
endif()

# ---------------------------------------------------------------------------
# Sanitizer options (mutually exclusive)
# ---------------------------------------------------------------------------

option(HAZE_SANITIZERS "Enable ASAN + UBSAN" OFF)
option(HAZE_TSAN       "Enable TSAN"         OFF)

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
# Dependency: libnbcc (Niobium FHE compiler)
# Set CMAKE_PREFIX_PATH to the niobium-compiler install tree, e.g.:
#   cmake -B build -DCMAKE_PREFIX_PATH=/path/to/niobium-compiler/vendor/lib/niobium-compiler
# ---------------------------------------------------------------------------

find_package(niobium REQUIRED)

# niobium's cmake config does not propagate its openfhe transitive includes.
# Derive the path from the niobium install prefix.
get_target_property(_nb_includes NiobiumInc::niobium INTERFACE_INCLUDE_DIRECTORIES)
list(GET _nb_includes 0 _nb_include_dir)
get_filename_component(_nb_prefix "${_nb_include_dir}" DIRECTORY)
set(NIOBIUM_OPENFHE_INCLUDE
    "${_nb_prefix}/../openfhe/include/openfhe"
    CACHE PATH "OpenFHE include root bundled with the niobium compiler")
set(NIOBIUM_JSON_INCLUDE
    "${_nb_prefix}/../../../vendor/json/single_include"
    CACHE PATH "nlohmann/json include dir bundled with the niobium compiler")

if(NOT EXISTS "${NIOBIUM_OPENFHE_INCLUDE}")
  message(FATAL_ERROR
    "OpenFHE include dir not found at '${NIOBIUM_OPENFHE_INCLUDE}'. "
    "Set NIOBIUM_OPENFHE_INCLUDE manually or reinstall libnbcc.")
endif()
if(NOT EXISTS "${NIOBIUM_JSON_INCLUDE}")
  message(FATAL_ERROR
    "nlohmann/json include dir not found at '${NIOBIUM_JSON_INCLUDE}'. "
    "Set NIOBIUM_JSON_INCLUDE manually or reinstall libnbcc.")
endif()

# ---------------------------------------------------------------------------
# libhaze shared library
# ---------------------------------------------------------------------------

add_library(haze SHARED
  src/haze_device.cpp
  src/haze_device_api.cpp
  src/haze_allocator.cpp
  src/haze_memory_api.cpp
  src/haze_stream.cpp
  src/haze_stream_api.cpp
  src/haze_compute_api.cpp
  src/haze_basis_convert.cpp
  src/haze_basis_convert_api.cpp
  src/haze_config.cpp
  src/haze_config_api.cpp
  src/haze_error.cpp
  src/haze_errors.cpp
  src/haze_backend.cpp
  src/haze_epoch.cpp
  src/haze_lifecycle.cpp
  src/haze_polynomial_io.cpp
)

target_include_directories(haze
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${NIOBIUM_OPENFHE_INCLUDE}
    ${NIOBIUM_OPENFHE_INCLUDE}/binfhe
    ${NIOBIUM_OPENFHE_INCLUDE}/core
    ${NIOBIUM_OPENFHE_INCLUDE}/pke
    ${NIOBIUM_JSON_INCLUDE}
)

target_compile_definitions(haze PRIVATE HAZE_BUILDING_LIBRARY)

target_link_libraries(haze PRIVATE NiobiumInc::niobium)

set_target_properties(haze PROPERTIES
  CXX_VISIBILITY_PRESET     hidden
  VISIBILITY_INLINES_HIDDEN ON
  OUTPUT_NAME               haze
)

# ---------------------------------------------------------------------------
# Installation
# ---------------------------------------------------------------------------

install(TARGETS haze
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin
)
install(DIRECTORY include/ DESTINATION include)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

enable_testing()
find_package(Catch2 3 REQUIRED)

add_executable(haze_tests
  test/test_build.cpp
  test/test_memory.cpp
  test/test_config.cpp
  test/test_compute.cpp
  test/test_basis_convert.cpp
)

target_include_directories(haze_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(haze_tests PRIVATE haze Catch2::Catch2WithMain)

add_test(NAME haze_tests COMMAND haze_tests)
