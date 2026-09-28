#!/usr/bin/env bash
# Fail if libhaze exports any defined dynamic symbol outside the haze* C ABI, or
# exports none at all: the proof that its statically-absorbed OpenFHE cannot
# collide with another OpenFHE in one process. Arg is libhaze.so / libhaze.dylib,
# or a dir holding one.
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

# Mach-O export tables use private-extern bits (N_PEXT) that GNU binutils nm
# misreports as global, so on macOS require llvm-nm / Apple nm. ELF is fine with
# GNU nm. (The flake's isolation check supplies the right nm per platform.)
if [ "$(uname -s)" = Darwin ]; then
    nm=$(command -v llvm-nm || command -v nm) || { echo "nm not found" >&2; exit 1; }
    nmflags=(-gU)
    prefix=_haze
else
    nm=$(command -v nm || command -v llvm-nm) || { echo "nm not found" >&2; exit 1; }
    nmflags=(--dynamic --defined-only)
    prefix=haze
fi

if ! syms=$("$nm" "${nmflags[@]}" "$lib"); then
    echo "$nm failed on $lib" >&2
    exit 1
fi

# Classify each exported symbol by NAME, not type letter, so an unexpected type
# still counts. Skip undefined (U) and absolute (A/a, e.g. a HAZE_1.x version
# node), and strip any @@HAZE_1.0 version suffix before comparing. `leaks` holds
# non-prefix names; `kept` counts prefix matches (to assert the ABI is present).
classify() {
    awk -v re="^$prefix" -v want="$1" '
        /^[0-9a-fA-F]+[ \t]+[A-Za-z][ \t]+/ {
            type = $2; name = $3; sub(/@.*/, "", name)
            if (type == "A" || type == "a" || type == "U") next
            if (name ~ re) { kept++; next }
            if (want == "leaks") print "  " type " " name
        }
        END { if (want == "kept") print kept+0 }' <<<"$syms"
}
leaks=$(classify leaks)
kept=$(classify kept)

if [ -n "$leaks" ]; then
    echo "libhaze leaks non-${prefix}* exported symbol(s) -- isolation broken:" >&2
    echo "$leaks" >&2
    exit 1
fi
if [ "$kept" -eq 0 ]; then
    echo "libhaze exports no ${prefix}* symbols -- not a valid haze library: $lib" >&2
    exit 1
fi

# Exhaustive golden name@@VERSION check (ELF/GNU-ld only -- Mach-O has no
# symbol-versioning concept at all: linker/haze_exports.sym, the macOS
# analogue of linker/haze_exports.map, is a plain _haze* allowlist with no
# version nodes, so `nm -gU` never emits an `@@VERSION` suffix to check).
#
# Diffs the full set of exported name@@VERSION pairs against
# scripts/haze_symbol_versions.txt, both sorted in byte order (LC_ALL=C, so
# the result depends on neither the caller's locale nor the golden file's
# line order), so every exported symbol must sit in the version node the
# golden file names: a symbol in the wrong node, or one missing from or extra
# to the golden file, fails the check. Regenerate the golden file after a
# deliberate ABI change with:
#   nm --dynamic --defined-only <libhaze.so> \
#     | awk '/^[0-9a-fA-F]+[ \t]+[A-Za-z][ \t]+/ { if ($2!="A" && $2!="a" && $2!="U" && $3 ~ /^haze/) print $3 }' \
#     | LC_ALL=C sort > scripts/haze_symbol_versions.txt
if [ "$(uname -s)" != Darwin ]; then
    script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
    golden_file="$script_dir/haze_symbol_versions.txt"
    if [ ! -f "$golden_file" ]; then
        echo "golden symbol-version file not found: $golden_file" >&2
        exit 1
    fi
    actual=$(awk -v re="^$prefix" '
        /^[0-9a-fA-F]+[ \t]+[A-Za-z][ \t]+/ {
            type = $2; name = $3
            if (type == "A" || type == "a" || type == "U") next
            if (name ~ re) print name
        }' <<<"$syms" | LC_ALL=C sort)
    diff_output=$(diff -u <(LC_ALL=C sort "$golden_file") <(echo "$actual") || true)
    if [ -n "$diff_output" ]; then
        echo "libhaze symbol-version set does not match $golden_file:" >&2
        echo "$diff_output" >&2
        exit 1
    fi
fi

echo "symbol-leak check OK: $lib exports only ${prefix}* ($kept symbol(s))"
