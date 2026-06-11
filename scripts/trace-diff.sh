#!/usr/bin/env bash
# trace-diff.sh - capture / compare recorded trace artifacts across builds.
#
# Usage:
#   scripts/trace-diff.sh capture <out-dir>            # run suites, snapshot runs/
#   scripts/trace-diff.sh compare <baseline> <candidate>
#   scripts/trace-diff.sh --help
#
# The conformance harness for refactors that must not change what haze
# records: `capture` runs each local suite against a clean runs dir and
# snapshots the artifacts per suite under <out-dir>/<suite>/; `compare`
# diffs two snapshots and exits non-zero on any divergence.
#
# Suites and their comparison policy:
#   unit    haze_tests "~[integration]"          strict
#   simdet  haze_tests "[integration]~[e2e]"     strict
#   sim     haze_tests "[integration]"           structural
#   e2e     haze_e2e_tests (all)                 structural
#
# strict:     every file byte-compared (after the normalizations below) —
#             the suite records only synthetic, seeded data.
# structural: *.fhetch and fhetch_replay.json byte-compared; payload
#             files (captured inputs *.bin/.ids, serialized_probes *.ct,
#             cryptocontext, templates) inventory-only — these suites run
#             OpenFHE keygen/encrypt, whose RNG lands in the payloads.
#             Polynomial *values* never enter the trace itself, so the
#             .fhetch stays strict even here.
#
# Normalizations (sole known run-to-run nondeterminism):
#   *.fhetch            drop the "# Generated: <epoch-ms>" comment line
#   fhetch_replay.json  drop "generated_timestamp", map the snapshot root
#                       in absolute paths to <ROOT>
#
# Tree-level: missing or extra files in either direction always fail.
#
# Env:
#   BUILD_DIR   build tree with haze_tests/haze_e2e_tests (default: build)
#   HAZE_SUITES suites to run during capture (default: all four)

set -euo pipefail

repo_root() {
    git rev-parse --show-toplevel 2>/dev/null ||
        (cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
}

ROOT="$(repo_root)"
BUILD_DIR="${BUILD_DIR:-build}"
RUNS_DIR="$ROOT/$BUILD_DIR/runs"

usage() {
    sed -n '/^# Usage:/,/^$/p' "$0" | sed 's/^# \?//'
}

suite_filter() {
    case "$1" in
        unit) echo '~[integration]' ;;
        simdet) echo '[integration]~[e2e]' ;;
        sim) echo '[integration]' ;;
        e2e) echo '' ;;
        *) return 1 ;;
    esac
}

suite_policy() {
    case "$1" in
        unit | simdet) echo strict ;;
        sim | e2e) echo structural ;;
        *) return 1 ;;
    esac
}

# Print a normalized view of a content-compared file. $2 is the snapshot
# root to strip from absolute paths.
normalized() {
    local f="$1" snap_root="$2"
    case "$f" in
        *.fhetch)
            grep -v '^# Generated:' "$f"
            ;;
        *.json)
            # generated_timestamp (fhetch_replay.json) and timestamp
            # (haze.inputs.json / haze.outputs.json) are wall-clock
            # stamps; absolute paths embed the snapshot location.
            sed -e '/"generated_timestamp"/d' -e '/"timestamp"/d' \
                -e "s|$snap_root|<ROOT>|g" "$f"
            ;;
        *)
            cat "$f"
            ;;
    esac
}

capture() {
    local out="$1"
    [[ -n "$out" ]] || { usage >&2; exit 2; }
    local bin="$ROOT/$BUILD_DIR/haze_tests"
    local e2e_bin="$ROOT/$BUILD_DIR/haze_e2e_tests"
    [[ -x "$bin" ]] || { echo "ERROR: $bin not built" >&2; exit 2; }

    rm -rf "$out"
    mkdir -p "$out"

    local suites="${HAZE_SUITES:-unit simdet sim e2e}"
    for suite in $suites; do
        local filter exe="$bin"
        filter="$(suite_filter "$suite")" || { echo "ERROR: unknown suite '$suite'" >&2; exit 2; }
        [[ "$suite" == e2e ]] && exe="$e2e_bin"
        if [[ "$suite" == e2e && ! -x "$e2e_bin" ]]; then
            echo "trace-diff: skipping e2e suite ($e2e_bin not built)"
            continue
        fi
        echo "trace-diff: running $suite suite"
        # Each suite starts from an empty runs dir so its snapshot is
        # self-contained. --order lex: Catch2 shuffles case order per
        # run by default, and the first case of an invocation skips the
        # serialized_probes wipe, so a fixed order is required for
        # run-to-run determinism.
        rm -rf "$RUNS_DIR"
        mkdir -p "$RUNS_DIR"
        if [[ -n "$filter" ]]; then
            (cd "$RUNS_DIR" && HAZE_TARGET=local "$exe" "$filter" --order lex)
        else
            (cd "$RUNS_DIR" && HAZE_TARGET=local "$exe" --order lex)
        fi
        mkdir -p "$out/$suite"
        cp -R "$RUNS_DIR/." "$out/$suite/"
    done

    git -C "$ROOT" rev-parse HEAD > "$out/.trace-diff-rev" 2>/dev/null || true
    echo "trace-diff: captured $(find "$out" -type f | wc -l | tr -d ' ') files to $out"
}

compare() {
    local base="$1" cand="$2"
    [[ -d "$base" && -d "$cand" ]] || { usage >&2; exit 2; }
    local fail=0

    # Tree-level inventory, both directions (metadata file excluded).
    local base_list cand_list
    base_list="$(cd "$base" && find . -type f ! -name '.trace-diff-rev' | sort)"
    cand_list="$(cd "$cand" && find . -type f ! -name '.trace-diff-rev' | sort)"
    if [[ "$base_list" != "$cand_list" ]]; then
        echo "FAIL: file inventory differs"
        comm -23 <(echo "$base_list") <(echo "$cand_list") | sed 's/^/  only in baseline: /'
        comm -13 <(echo "$base_list") <(echo "$cand_list") | sed 's/^/  only in candidate: /'
        fail=1
    fi

    local rel suite policy checked=0 skipped=0
    while IFS= read -r rel; do
        rel="${rel#./}"
        [[ -f "$cand/$rel" ]] || continue
        suite="${rel%%/*}"
        policy="$(suite_policy "$suite" 2>/dev/null || echo strict)"
        case "$rel" in
            *.fhetch | */fhetch_replay.json) ;; # always content-checked
            *)
                if [[ "$policy" == structural ]]; then
                    skipped=$((skipped + 1))
                    continue
                fi
                ;;
        esac
        if ! diff -q <(normalized "$base/$rel" "$base") \
                     <(normalized "$cand/$rel" "$cand") >/dev/null; then
            echo "FAIL: content differs: $rel"
            fail=1
        fi
        checked=$((checked + 1))
    done <<< "$base_list"

    if [[ "$fail" -ne 0 ]]; then
        echo "trace-diff: DIVERGED ($checked content-checked, $skipped inventory-only)"
        return 1
    fi
    echo "trace-diff: identical ($checked content-checked, $skipped inventory-only, inventories match)"
}

case "${1:-}" in
    capture) shift; capture "${1:-}" ;;
    compare) shift; compare "${1:-}" "${2:-}" ;;
    -h | --help) usage ;;
    *) usage >&2; exit 2 ;;
esac
