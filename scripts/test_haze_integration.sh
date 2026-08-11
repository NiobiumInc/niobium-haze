#!/bin/bash
# ============================================================================
# test_haze_integration.sh -- run [integration]-tagged haze_tests against a
# niobium-compiler-built nbcc_fhetch_replay.
#
# Two dispatch modes, selected by --mode (no default; the caller must say
# which one it wants):
#
#   --mode=direct   libnbfhetch's Compiler::replay() spawns the compiler
#                   binary itself via NBCC_FHETCH_REPLAY. No HTTP, no
#                   niobium-client transport binaries. This is what haze's
#                   own `make test-transport` uses -- from haze's checkout we
#                   don't have (and don't want to require) the forwarder or
#                   the server.
#
#   --mode=http     Full transport round trip. Spawns
#                   nbcc_fhetch_replay_server via niobium-client's
#                   fhetch_server.sh, waits on /healthz, and puts the
#                   client-side forwarder (also named nbcc_fhetch_replay)
#                   first on PATH so Compiler::replay()'s
#                   system("nbcc_fhetch_replay ...") hits the forwarder,
#                   which packs the project and ships it to the server.
#                   Used by the haze_transport_tests ctest entry when
#                   niobium-client owns both halves of the transport.
#
#   --check         Resolve and validate the compiler binary, print it, exit.
#                   Lets `make test-transport` fail fast before building haze
#                   without re-implementing the path logic.
#
# Required env:
#   NIOBIUM_COMPILER_ROOT  Path to a niobium-compiler checkout containing
#                          build/nbcc_fhetch_replay (release) or
#                          dbuild/nbcc_fhetch_replay (debug).
#   HAZE_TEST_BIN          Absolute path to the haze_tests executable
#                          (cmake $<TARGET_FILE:haze_tests>). Not needed by
#                          --check.
#   OPENFHE_LIB            OpenFHE shared-library dir, prepended to
#                          DYLD_LIBRARY_PATH (macOS) and LD_LIBRARY_PATH
#                          (Linux) so haze_tests resolves libOPENFHEcore at
#                          runtime. Not needed by --check.
#   NIOBIUM_CLIENT_BUILD   niobium-client build dir (cmake binary dir), used
#                          to locate the transport binaries. --mode=http only.
#
# Optional env:
#   NIOBIUM_COMPILER_BUILD Compiler build dir to use verbatim, skipping the
#                          build/dbuild search. Same knob name as
#                          niobium-client/scripts/fhetch_server.sh.
#   HAZE_MODE              debug | release. Orders the build/dbuild search
#                          when NIOBIUM_COMPILER_BUILD is unset; the other
#                          flavour is still accepted as a fallback.
#                          Default: release.
#   HAZE_TRANSPORT_TARGET  Replay target, exported as HAZE_TARGET. Must be a
#                          non-"local" value or Compiler::replay() runs the
#                          in-process simulator instead of dispatching to the
#                          compiler. Default: FUNC_SIM.
#   HAZE_TEST_FILTER       Catch2 filter. Default: [integration].
#   HAZE_RUNS_DIR          Working dir haze_tests cd's into before recording.
#                          Per-test program dirs (haze, haze_*, epoch_*) and
#                          replay artifacts land here and are cleaned on EXIT.
#                          Default: a mktemp dir, removed entirely on EXIT.
#   HAZE_KEEP_RUNS         1 skips runs-dir cleanup (post-mortem debugging).
#   PORT                   Server port. --mode=http only.
#                          Default: ephemeral via python3.
# ============================================================================
set -euo pipefail

SELF="$(basename "$0")"
readonly SELF
readonly REPLAY_BIN_NAME="nbcc_fhetch_replay"

# Exit codes: 2 = missing/invalid prerequisite, 3 = server never became healthy.
readonly EXIT_PREREQ=2
readonly EXIT_SERVER=3

die() {
    echo "[$SELF] error: $*" >&2
    exit "$EXIT_PREREQ"
}

log() {
    echo "[$SELF] $*"
}

usage() {
    cat >&2 <<EOF
usage: $SELF --mode=direct|http
       $SELF --check

  --mode=direct  spawn $REPLAY_BIN_NAME directly (no HTTP transport)
  --mode=http    round trip via niobium-client's forwarder + server
  --check        resolve and print the compiler binary, then exit
EOF
    exit "$EXIT_PREREQ"
}

# ----------------------------------------------------------------------------
# Argument parsing
# ----------------------------------------------------------------------------

MODE=""
CHECK_ONLY=0

while [[ $# -gt 0 ]]; do
    case "$1" in
    --mode=direct | --mode=http) MODE="${1#--mode=}" ;;
    --check) CHECK_ONLY=1 ;;
    -h | --help) usage ;;
    *)
        echo "[$SELF] error: unrecognised argument: $1" >&2
        usage
        ;;
    esac
    shift
done

if [[ "$CHECK_ONLY" -eq 0 && -z "$MODE" ]]; then
    echo "[$SELF] error: --mode is required" >&2
    usage
fi

# ----------------------------------------------------------------------------
# Compiler binary resolution -- the ONLY place the nbcc_fhetch_replay path is
# spelled out. Sets NBCC_FHETCH_REPLAY (the binary, honoured by libnbfhetch's
# Compiler::replay() as an absolute override before its PATH lookup) and
# NIOBIUM_COMPILER_BUILD (its directory, which fhetch_server.sh consumes).
# ----------------------------------------------------------------------------

resolve_nbcc_replay() {
    if [[ -z "${NIOBIUM_COMPILER_ROOT:-}" ]]; then
        echo "[$SELF] error: NIOBIUM_COMPILER_ROOT is not set" >&2
        echo "  standalone: make test-transport NIOBIUM_COMPILER_ROOT=/path/to/niobium-compiler" >&2
        echo "  parent:     cmake -DNIOBIUM_CLIENT_HAZE_WITH_TRANSPORT_TESTS=ON" \
            "-DNIOBIUM_COMPILER_ROOT=/path/to/niobium-compiler" >&2
        exit "$EXIT_PREREQ"
    fi

    # Explicit override wins outright: use it verbatim, no fallback.
    if [[ -n "${NIOBIUM_COMPILER_BUILD:-}" ]]; then
        local explicit="$NIOBIUM_COMPILER_BUILD/$REPLAY_BIN_NAME"
        if [[ ! -x "$explicit" ]]; then
            echo "[$SELF] error: not an executable: $explicit" >&2
            echo "  (NIOBIUM_COMPILER_BUILD=$NIOBIUM_COMPILER_BUILD was set explicitly," \
                "so build/ and dbuild/ were not searched)" >&2
            exit "$EXIT_PREREQ"
        fi
        NBCC_FHETCH_REPLAY="$explicit"
        log "compiler = $NBCC_FHETCH_REPLAY (NIOBIUM_COMPILER_BUILD)"
        return
    fi

    # Otherwise search both flavours, preferring the one matching HAZE_MODE.
    # The compiler's build dirs mirror haze's own convention: build/ is
    # release, dbuild/ is debug.
    local -a candidates
    case "${HAZE_MODE:-release}" in
    debug) candidates=(dbuild build) ;;
    release) candidates=(build dbuild) ;;
    *) die "invalid HAZE_MODE='${HAZE_MODE:-}' (expected 'debug' or 'release')" ;;
    esac

    local dir flavour
    for dir in "${candidates[@]}"; do
        if [[ -x "$NIOBIUM_COMPILER_ROOT/$dir/$REPLAY_BIN_NAME" ]]; then
            NBCC_FHETCH_REPLAY="$NIOBIUM_COMPILER_ROOT/$dir/$REPLAY_BIN_NAME"
            NIOBIUM_COMPILER_BUILD="$NIOBIUM_COMPILER_ROOT/$dir"
            if [[ "$dir" == "dbuild" ]]; then
                flavour=debug
            else
                flavour=release
            fi
            log "compiler = $NBCC_FHETCH_REPLAY ($flavour)"
            return
        fi
    done

    echo "[$SELF] error: no executable $REPLAY_BIN_NAME found. Probed:" >&2
    for dir in "${candidates[@]}"; do
        echo "    $NIOBIUM_COMPILER_ROOT/$dir/$REPLAY_BIN_NAME" >&2
    done
    echo "  Build it with: (cd $NIOBIUM_COMPILER_ROOT && make build-release)" >&2
    echo "  Or point NIOBIUM_COMPILER_BUILD at the build dir that holds it." >&2
    exit "$EXIT_PREREQ"
}

resolve_nbcc_replay
export NBCC_FHETCH_REPLAY NIOBIUM_COMPILER_BUILD

if [[ "$CHECK_ONLY" -eq 1 ]]; then
    exit 0
fi

# ----------------------------------------------------------------------------
# Shared prerequisites and environment
# ----------------------------------------------------------------------------

: "${HAZE_TEST_BIN:?HAZE_TEST_BIN must be set (path to haze_tests executable)}"
: "${OPENFHE_LIB:?OPENFHE_LIB must be set (OpenFHE shared-library dir)}"
: "${HAZE_TRANSPORT_TARGET:=FUNC_SIM}"
: "${HAZE_TEST_FILTER:=[integration]}"

[[ -x "$HAZE_TEST_BIN" ]] || die "not an executable: $HAZE_TEST_BIN"

# Track ownership so a caller-supplied runs dir is only pruned of program
# dirs, while a tempdir we created is removed outright.
RUNS_DIR_IS_OURS=0
if [[ -z "${HAZE_RUNS_DIR:-}" ]]; then
    HAZE_RUNS_DIR="$(mktemp -d -t haze_runs.XXXXXX)"
    RUNS_DIR_IS_OURS=1
fi

# HAZE_TARGET must be a non-"local" value (haze::kLocalTarget in
# src/core/config.hpp) or Compiler::replay() runs the in-process simulator
# instead of dispatching to the compiler binary.
export HAZE_TARGET="$HAZE_TRANSPORT_TARGET"
export DYLD_LIBRARY_PATH="$OPENFHE_LIB${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
export LD_LIBRARY_PATH="$OPENFHE_LIB${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

SERVER_LOG=""
SERVER_PID=""

# Server teardown runs before runs-dir cleanup: the server's cwd is the runs
# dir, so it has to be gone first.
cleanup() {
    set +e
    if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill -TERM "$SERVER_PID" 2>/dev/null
        for _ in 1 2 3 4 5; do
            kill -0 "$SERVER_PID" 2>/dev/null || break
            sleep 1
        done
        kill -0 "$SERVER_PID" 2>/dev/null && kill -KILL "$SERVER_PID" 2>/dev/null
    fi
    if [[ -n "$SERVER_LOG" ]]; then
        echo
        echo "=== fhetch_server log ==="
        cat "$SERVER_LOG"
        rm -f "$SERVER_LOG"
    fi

    if [[ "${HAZE_KEEP_RUNS:-0}" == "1" ]]; then
        echo "[$SELF] HAZE_KEEP_RUNS=1: leaving $HAZE_RUNS_DIR in place"
        return
    fi
    if [[ ! -d "$HAZE_RUNS_DIR" ]]; then
        return
    fi
    if [[ "$RUNS_DIR_IS_OURS" -eq 1 ]]; then
        rm -rf "$HAZE_RUNS_DIR"
        return
    fi
    # Prune only haze's per-program directories, leaving the caller's runs dir
    # itself in place so re-invocations don't have to recreate it. find rather
    # than glob expansion so an unmatched pattern is a real no-op instead of a
    # literal "haze_*" argument.
    find "$HAZE_RUNS_DIR" -mindepth 1 -maxdepth 1 \
        \( -name "haze" -o -name "haze_*" -o -name "epoch_*" \) \
        -exec rm -rf {} + 2>/dev/null
}
trap cleanup EXIT

mkdir -p "$HAZE_RUNS_DIR"

# ----------------------------------------------------------------------------
# --mode=http: bring up the transport pair
# ----------------------------------------------------------------------------

if [[ "$MODE" == "http" ]]; then
    : "${NIOBIUM_CLIENT_BUILD:?NIOBIUM_CLIENT_BUILD must be set (niobium-client cmake binary dir)}"

    # niobium-client's fhetch_server.sh expects to live two levels under the
    # client root and computes paths relative to itself. Walk up from the
    # build dir to locate the source tree.
    CLIENT_ROOT="$(cd "$NIOBIUM_CLIENT_BUILD/.." && pwd)"
    FHETCH_SERVER_SH="$CLIENT_ROOT/scripts/fhetch_server.sh"
    TRANSPORT_DIR="$NIOBIUM_CLIENT_BUILD/src/fhetch_transport"

    for path in \
        "$TRANSPORT_DIR/nbcc_fhetch_replay_server" \
        "$TRANSPORT_DIR/$REPLAY_BIN_NAME" \
        "$FHETCH_SERVER_SH"; do
        [[ -e "$path" ]] || die "not found: $path (is NIOBIUM_CLIENT_WITH_FHETCH_TRANSPORT=ON?)"
    done

    # Pick an ephemeral port unless one is forced.
    : "${PORT:=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')}"
    SERVER_LOG="$(mktemp)"

    echo "=== [1/2] starting fhetch_server (port $PORT) ==="
    # The server's cwd determines where its per-request working files
    # (nbcc_fhetch_replay_source_*) land. Anchor it to the runs dir so the
    # repo isn't polluted wherever this script was invoked from.
    (
        cd "$HAZE_RUNS_DIR"
        PORT="$PORT" BIND=127.0.0.1 \
            NIOBIUM_COMPILER_ROOT="$NIOBIUM_COMPILER_ROOT" \
            NIOBIUM_COMPILER_BUILD="$NIOBIUM_COMPILER_BUILD" \
            "$FHETCH_SERVER_SH" >"$SERVER_LOG" 2>&1
    ) &
    SERVER_PID=$!

    # Wait for /healthz, mirroring niobium-client/scripts/test_transport_mult.sh.
    for _ in $(seq 1 50); do
        if curl -sf "http://127.0.0.1:$PORT/healthz" >/dev/null 2>&1; then
            log "fhetch_server up (pid=$SERVER_PID)"
            break
        fi
        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            echo "[$SELF] error: server exited before /healthz responded" >&2
            exit "$EXIT_SERVER"
        fi
        sleep 0.1
    done
    if ! curl -sf "http://127.0.0.1:$PORT/healthz" >/dev/null 2>&1; then
        echo "[$SELF] error: server did not become healthy within 5s" >&2
        exit "$EXIT_SERVER"
    fi

    # Forwarder first on PATH so libnbfhetch's system("nbcc_fhetch_replay")
    # resolves to it rather than to the compiler binary directly. Drop the
    # absolute override for the same reason -- it would win over PATH and
    # bypass the transport this mode exists to exercise.
    export PATH="$TRANSPORT_DIR:$PATH"
    export NBCC_FHETCH_SERVER="http://127.0.0.1:$PORT"
    unset NBCC_FHETCH_REPLAY

    echo
    echo "=== [2/2] running haze_tests $HAZE_TEST_FILTER through transport ==="
else
    echo "=== running haze_tests $HAZE_TEST_FILTER (direct $REPLAY_BIN_NAME invocation) ==="
fi

# ----------------------------------------------------------------------------
# Run
# ----------------------------------------------------------------------------

log "target = $HAZE_TARGET"
log "runs   = $HAZE_RUNS_DIR"

# cd into the runs dir so libnbfhetch's get_program_directory() resolves
# program artifacts under HAZE_RUNS_DIR rather than the caller's cwd.
cd "$HAZE_RUNS_DIR"
"$HAZE_TEST_BIN" "$HAZE_TEST_FILTER"
