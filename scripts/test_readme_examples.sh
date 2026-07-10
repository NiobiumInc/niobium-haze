#!/usr/bin/env bash
# test_readme_examples.sh - extract, compile, and run the README examples.
#
# Usage:
#   scripts/test_readme_examples.sh            # extract + compile + run (the check)
#   scripts/test_readme_examples.sh --check    # alias for the default
#   scripts/test_readme_examples.sh --help
#
# Docs-as-tests: the two fenced examples in README.md (a pure-C 22-limb raw add
# and a C++ 22-limb CKKS add) are the source of truth. This script pulls each
# marked block out of README.md, compiles it against the shipped libhaze, runs
# it through the in-process FHETCH simulator (HAZE_TARGET=local), and asserts
# exit 0 plus the expected output token. It fails loudly if a marker region is
# missing or a compile/run/assert fails, so the published code cannot rot.
#
# Resolves the repo root from `git rev-parse` (falls back to the script's
# `$(dirname BASH_SOURCE)/..` outside a git checkout, e.g. inside a nix
# derivation sandbox where the source tree has no .git).
#
# Environment overrides (all have defaults resolved from the repo root):
#   BUILD_DIR                build tree name (default: build).
#   HAZE_INCLUDE_DIR         public haze headers   (default: $root/include).
#   HAZE_BRIDGE_INCLUDE_DIR  replay-bridge header  (default:
#                            $root/replay_bridge/include). The C example calls
#                            hazeReplayBridgeInitCryptoContext, declared there.
#   HAZE_LIB_DIR             dir holding libhaze.*  (default: $root/$BUILD_DIR).
#   STOCK_OPENFHE_DIR        stock OpenFHE prefix   (default:
#                            $root/vendor/lib/openfhe-stock). C++ only.
#   HAZE_RUNS_DIR            writable dir the examples run in so libnbfhetch's
#                            program_dir stays out of the source root (default:
#                            $root/$BUILD_DIR/runs).
#   CC / CXX                 compilers (default: cc / c++).

set -euo pipefail

case "${1:-}" in
    -h | --help)
        sed -n '/^# Usage:/,/^$/p' "$0" | sed 's/^# \?//'
        exit 0
        ;;
    --check | "") ;;
    *)
        printf '%s: unknown argument: %s\n' "$(basename "$0")" "$1" >&2
        exit 2
        ;;
esac

if root=$(git rev-parse --show-toplevel 2>/dev/null); then
    cd "$root"
else
    root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
    cd "$root"
fi

readme="${README:-$root/README.md}"
build_dir="${BUILD_DIR:-build}"
haze_include_dir="${HAZE_INCLUDE_DIR:-$root/include}"
haze_bridge_include_dir="${HAZE_BRIDGE_INCLUDE_DIR:-$root/replay_bridge/include}"
haze_lib_dir="${HAZE_LIB_DIR:-$root/$build_dir}"
stock_openfhe_dir="${STOCK_OPENFHE_DIR:-$root/vendor/lib/openfhe-stock}"
runs_dir="${HAZE_RUNS_DIR:-$root/$build_dir/runs}"
cc="${CC:-cc}"
cxx="${CXX:-c++}"

[[ -f "$readme" ]] || {
    printf 'error: README not found: %s\n' "$readme" >&2
    exit 1
}
[[ -d "$haze_lib_dir" ]] || {
    printf 'error: HAZE_LIB_DIR not found: %s (build libhaze first)\n' "$haze_lib_dir" >&2
    exit 1
}

scratch=$(mktemp -d "${TMPDIR:-/tmp}/readme-examples.XXXXXX")
trap 'rm -rf "$scratch"' EXIT

# Pull the fenced code of the readme-example region named $1 out of the README,
# stripping the ``` fence lines. Prints to stdout; empty output means the region
# was absent or malformed.
extract_block() {
    local name="$1"
    awk -v want="$name" '
        /^<!-- readme-example:begin / { active = ($0 ~ ("name=" want " ")); next }
        /^<!-- readme-example:end/    { if (active) exit; next }
        active && /^```/              { infence = !infence; next }
        active && infence             { print }
    ' "$readme"
}

extract_to() {
    local name="$1" out="$2"
    extract_block "$name" >"$out"
    [[ -s "$out" ]] || {
        printf 'error: README region name=%s is missing or empty\n' "$name" >&2
        exit 1
    }
}

extract_to quickstart "$scratch/quickstart.c"
extract_to ckks22 "$scratch/ckks22.cpp"

printf '[readme] compiling C example (quickstart.c)\n'
"$cc" -std=c11 -O2 \
    -I"$haze_include_dir" -I"$haze_bridge_include_dir" \
    "$scratch/quickstart.c" \
    -L"$haze_lib_dir" -lhaze \
    -Wl,-rpath,"$haze_lib_dir" \
    -o "$scratch/quickstart"

printf '[readme] compiling C++ example (ckks22.cpp)\n'
"$cxx" -std=c++17 -O2 \
    -I"$haze_include_dir" -I"$haze_bridge_include_dir" \
    -isystem "$stock_openfhe_dir/include/openfhe" \
    -isystem "$stock_openfhe_dir/include/openfhe/core" \
    -isystem "$stock_openfhe_dir/include/openfhe/pke" \
    -isystem "$stock_openfhe_dir/include/openfhe/binfhe" \
    "$scratch/ckks22.cpp" \
    -L"$haze_lib_dir" -lhaze \
    -L"$stock_openfhe_dir/lib" -lOPENFHEpke -lOPENFHEbinfhe -lOPENFHEcore \
    -pthread \
    -Wl,-rpath,"$haze_lib_dir" -Wl,-rpath,"$stock_openfhe_dir/lib" \
    -o "$scratch/ckks22"

# Run $bin from the runs dir under the local simulator; assert exit 0 and that
# $token appears in its output. Prints elapsed wall seconds and the token line.
run_example() {
    local label="$1" bin="$2" token="$3"
    mkdir -p "$runs_dir"
    local start end out rc
    start=$(date +%s)
    if out="$(cd "$runs_dir" && HAZE_TARGET=local "$bin" 2>&1)"; then rc=0; else rc=$?; fi
    end=$(date +%s)
    if [[ $rc -ne 0 ]]; then
        printf '[readme] %s FAILED: exit %d\n' "$label" "$rc" >&2
        printf '%s\n' "$out" >&2
        return 1
    fi
    if ! grep -qF "$token" <<<"$out"; then
        printf '[readme] %s FAILED: token %s not found in output\n' "$label" "$token" >&2
        printf '%s\n' "$out" >&2
        return 1
    fi
    printf '[readme] %s passed in %ds wall — %s\n' \
        "$label" "$((end - start))" "$(grep -F "$token" <<<"$out" | tail -n1)"
}

run_example "C example" "$scratch/quickstart" "readme-c: OK"
run_example "C++ example" "$scratch/ckks22" "readme-cpp: OK"

printf '[readme] all README examples compiled and passed\n'
