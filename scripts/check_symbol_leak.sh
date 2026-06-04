#!/usr/bin/env bash
# Fail if libhaze exports any defined dynamic symbol outside the haze* C ABI:
# the proof that its statically-absorbed OpenFHE cannot collide with another
# OpenFHE in one process. Arg is libhaze.so / libhaze.dylib, or a dir holding one.
set -euo pipefail

if [ $# -ne 1 ]; then
    echo "usage: check_symbol_leak.sh PATH (libhaze.so/.dylib, or its directory)" >&2
    exit 2
fi
arg=$1

if [ -d "$arg" ]; then
    lib=
    for cand in "$arg/libhaze.so" "$arg/libhaze.dylib"; do
        if [ -f "$cand" ]; then
            lib=$cand
            break
        fi
    done
    if [ -z "$lib" ]; then
        echo "no libhaze.so or libhaze.dylib under $arg" >&2
        exit 1
    fi
else
    lib=$arg
fi
if [ ! -f "$lib" ]; then
    echo "libhaze not found: $lib" >&2
    exit 1
fi

if ! nm=$(command -v nm || command -v llvm-nm); then
    echo "nm not found" >&2
    exit 1
fi

# Mach-O prefixes C names with '_' and wants -gU; ELF's long flags suit GNU nm
# and llvm-nm alike. Weak-coalesced C++ vtables/typeinfo are external+defined,
# so they surface here too -- which is exactly what must be caught.
if [ "$(uname -s)" = Darwin ]; then
    syms=$("$nm" -gU "$lib")
    prefix=_haze
else
    syms=$("$nm" --dynamic --defined-only "$lib")
    prefix=haze
fi

# Match on the name, not the type letter, so an unexpected type still counts.
# Skip undefined (U) and absolute (A/a, e.g. the HAZE_1.0 version node), and
# strip any @@HAZE_1.0 version suffix before comparing.
leaks=$(awk -v re="^$prefix" '
    /^[0-9a-fA-F]+[ \t]+[A-Za-z][ \t]+/ {
        type = $2; name = $3; sub(/@.*/, "", name)
        if (type == "A" || type == "a" || type == "U") next
        if (name !~ re) print "  " type " " name
    }' <<<"$syms")

if [ -n "$leaks" ]; then
    echo "libhaze leaks non-${prefix}* exported symbol(s) -- isolation broken:" >&2
    echo "$leaks" >&2
    exit 1
fi
echo "symbol-leak check OK: $lib exports only ${prefix}*"
