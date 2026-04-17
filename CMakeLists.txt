cmake_minimum_required(VERSION 3.22)
project(haze LANGUAGES C CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

# ---------------------------------------------------------------------------
# Sanitizer options (mutually exclusive)
# ---------------------------------------------------------------------------

option(HAZE_SANITIZERS "Enable ASAN + UBSAN" OFF)
option(HAZE_TSAN       "Enable TSAN"         OFF)

if(HAZE_SANITIZERS AND HAZE_TSAN)
  message(FATAL_ERROR "HAZE_SANITIZERS and HAZE_TSAN are mutually exclusive.")
endif()

if(HAZE_SANITIZERS)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
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

# ---------------------------------------------------------------------------
# libhaze shared library
# ---------------------------------------------------------------------------

add_library(haze SHARED
  src/haze_device.cpp
  src/haze_memory.cpp
  src/haze_stream.cpp
  src/haze_compute.cpp
  src/haze_config.cpp
  src/haze_error.cpp
)

target_include_directories(haze
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
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

add_executable(haze_tests test/test_build.cpp)

target_include_directories(haze_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(haze_tests PRIVATE haze Catch2::Catch2WithMain)

add_test(NAME haze_tests COMMAND haze_tests)
