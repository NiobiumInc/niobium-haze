# Isolation guard for the shipped libhaze.
#
# libhaze statically absorbs the niobium OpenFHE 1.4.2 fork + libnbfhetch. For
# it to coexist in one process with another OpenFHE (e.g. FIDESlib's 1.5.1),
# none of those absorbed symbols may be exported — only haze's C ABI (haze*).
# This script fails if libhaze exports any defined dynamic symbol whose name is
# not haze* (Mach-O: _haze*). Run as a ctest; see CMakeLists.txt.
#
# Usage: cmake -DHAZE_LIB=<path/to/libhaze.{so,dylib}> -P check_symbol_leak.cmake

if(NOT DEFINED HAZE_LIB OR HAZE_LIB STREQUAL "")
  message(FATAL_ERROR "HAZE_LIB is not set (or empty)")
endif()
if(NOT EXISTS "${HAZE_LIB}")
  message(FATAL_ERROR "HAZE_LIB does not exist: ${HAZE_LIB}")
endif()

# Prefer the platform's native nm (GNU on Linux, Apple/LLVM on macOS); the flag
# sets below are chosen per platform. llvm-nm is only a last resort.
find_program(NM_EXE NAMES nm llvm-nm)
if(NOT NM_EXE)
  message(WARNING "nm not found; skipping libhaze symbol-leak check")
  return()
endif()

if(APPLE)
  # -g external only, -U defined only. Mach-O prefixes C names with '_'.
  # Weak-coalesced defs (vtables/typeinfo) are external+defined, so they show
  # here too — which is exactly what we want to catch.
  execute_process(COMMAND ${NM_EXE} -gU "${HAZE_LIB}"
                  OUTPUT_VARIABLE _out RESULT_VARIABLE _rc)
  set(_prefix "_haze")
else()
  # GNU/long flags so this works with both GNU nm and llvm-nm.
  execute_process(COMMAND ${NM_EXE} --dynamic --defined-only "${HAZE_LIB}"
                  OUTPUT_VARIABLE _out RESULT_VARIABLE _rc)
  set(_prefix "haze")
endif()
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "${NM_EXE} failed on ${HAZE_LIB} (rc=${_rc})")
endif()

string(REPLACE "\n" ";" _lines "${_out}")
set(_leaks "")
foreach(_line IN LISTS _lines)
  # "<addr> <type> <name>". Decide by NAME, not by type letter, so a leak with
  # an unexpected type (Mach-O 'S' section syms, GNU 'u' unique, weak 'V/W',
  # etc.) is still caught. Skip undefined (U/u-as-undefined handled by flags)
  # and absolute 'A/a' (the version-node pseudo-symbol, e.g. HAZE_1.0).
  if(_line MATCHES "^[0-9a-fA-F]+[ \t]+([A-Za-z])[ \t]+([^ \t]+.*)$")
    set(_type "${CMAKE_MATCH_1}")
    set(_name "${CMAKE_MATCH_2}")
    string(REGEX REPLACE "@.*$" "" _name "${_name}") # strip @@HAZE_1.0
    if(_type STREQUAL "A" OR _type STREQUAL "a" OR _type STREQUAL "U")
      continue()
    endif()
    if(NOT _name MATCHES "^${_prefix}")
      list(APPEND _leaks "${_type} ${_name}")
    endif()
  endif()
endforeach()

list(LENGTH _leaks _n)
if(_n GREATER 0)
  string(REPLACE ";" "\n  " _pretty "${_leaks}")
  message(FATAL_ERROR
    "libhaze leaks ${_n} non-${_prefix}* exported symbol(s) — isolation broken:\n  ${_pretty}")
endif()

message(STATUS "symbol-leak check OK: ${HAZE_LIB} exports only ${_prefix}*")
