# Standalone consumer project: proves haze installs as a real CMake package.
# `find_package(haze CONFIG REQUIRED)` must resolve `haze::haze`, whose
# installed INTERFACE_INCLUDE_DIRECTORIES must carry both haze.h and
# replay_bridge.h. Configured out-of-tree against a temp install prefix by
# `make test-package` (see the top-level Makefile) - not part of the main
# haze CMakeLists.txt build, and never add_subdirectory()'d from it.
cmake_minimum_required(VERSION 3.22)
project(haze_package_consumer LANGUAGES C)

find_package(haze CONFIG REQUIRED)

add_executable(haze_package_consumer main.c)
target_link_libraries(haze_package_consumer PRIVATE haze::haze)
