#!/bin/bash
# ============================================================================
# test_haze_integration.sh -- run [integration]-tagged haze_tests through the
# FHETCH transport round trip.
#
# 1. Verifies the niobium-compiler-built nbcc_fhetch_replay exists.
# 2. Verifies the niobium-client transport binaries (server + forwarder)
#    were built (NIOBIUM_CLIENT_WITH_FHETCH_TRANSPORT=ON).
# 3. Spawns nbcc_fhetch_replay_server in the background via the niobium-client
#    fhetch_server.sh wrapper. Waits on /healthz.
# 4. Puts the client-side forwarder (also named nbcc_fhetch_replay) first on
#    PATH so libnbfhetch's Compiler::replay() -> dispatch_to_compiler_target's
#    system("nbcc_fhetch_replay ...") hits the forwarder.
# 5. Runs haze_tests with the [integration] Catch2 filter.
# 6. Tears the server down on exit. Dumps the server log on failure.
#
# Required env (set by CMake set_tests_properties ENVIRONMENT):
#   NIOBIUM_COMPILER_ROOT  Path to a niobium-compiler checkout containing
#                          build/nbcc_fhetch_replay.
#   NIOBIUM_CLIENT_BUILD   niobium-client build directory (cmake binary dir).
#                          Used to find the transport binaries.
#   HAZE_TEST_BIN          Absolute path to the haze_tests executable
#                          (cmake $<TARGET_FILE:haze_tests>).
#   OPENFHE_LIB            Path to the OpenFHE shared-library directory.
#
# Optional:
#   PORT                   Server port (default: ephemeral via python3).
#   HAZE_RUNS_DIR          Working directory haze_tests cd's into before
#                          recording. Replay artifacts (program dirs,
#                          .fhetch traces, ciphertext_templates/,
#                          serialized_probes/, epoch_*/) land here under
#                          the program name. Defaults to a tempdir.
#                          Per-test program dirs are cleaned on EXIT.
# ============================================================================
set -euo pipefail

: "${NIOBIUM_COMPILER_ROOT:?NIOBIUM_COMPILER_ROOT must be set (path to niobium-compiler checkout)}"
: "${NIOBIUM_CLIENT_BUILD:?NIOBIUM_CLIENT_BUILD must be set (niobium-client cmake binary dir)}"
: "${HAZE_TEST_BIN:?HAZE_TEST_BIN must be set (path to haze_tests executable)}"
: "${OPENFHE_LIB:?OPENFHE_LIB must be set (OpenFHE shared-library dir)}"
: "${HAZE_RUNS_DIR:=$(mktemp -d -t haze_runs.XXXXXX)}"

# niobium-client's fhetch_server.sh expects to live two levels under the
# client root and computes paths relative to itself. Walk up from the
# build dir to locate the source tree.
CLIENT_ROOT="$(cd "$NIOBIUM_CLIENT_BUILD/.." && pwd)"
FHETCH_SERVER_SH="$CLIENT_ROOT/scripts/fhetch_server.sh"
TRANSPORT_DIR="$NIOBIUM_CLIENT_BUILD/src/fhetch_transport"

NBCC_FHETCH_REPLAY="$NIOBIUM_COMPILER_ROOT/build/nbcc_fhetch_replay"
SERVER_BIN="$TRANSPORT_DIR/nbcc_fhetch_replay_server"
FORWARDER_BIN="$TRANSPORT_DIR/nbcc_fhetch_replay"

for path in "$NBCC_FHETCH_REPLAY" "$SERVER_BIN" "$FORWARDER_BIN" "$HAZE_TEST_BIN" "$FHETCH_SERVER_SH"; do
  if [[ ! -e "$path" ]]; then
    echo "[test_haze_integration.sh] error: not found: $path" >&2
    exit 2
  fi
done
if [[ ! -x "$NBCC_FHETCH_REPLAY" ]]; then
  echo "[test_haze_integration.sh] error: not executable: $NBCC_FHETCH_REPLAY" >&2
  echo "  (run \`make build-release\` in $NIOBIUM_COMPILER_ROOT first)" >&2
  exit 2
fi

# Pick an ephemeral port unless one is forced.
: "${PORT:=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')}"
echo "[test_haze_integration.sh] port=$PORT"

SERVER_LOG="$(mktemp)"
SERVER_PID=""

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
  echo
  echo "=== fhetch_server log ==="
  cat "$SERVER_LOG"
  rm -f "$SERVER_LOG"

  # Clean haze's per-program directories (haze, haze_*, epoch_*) under the
  # runs dir. Leaves the runs dir itself in place so the make target's
  # caller can re-use it across invocations without recreating.
  # Skip cleanup when HAZE_KEEP_RUNS=1 — useful for post-mortem debugging.
  if [[ -d "$HAZE_RUNS_DIR" && "${HAZE_KEEP_RUNS:-0}" != "1" ]]; then
    rm -rf "$HAZE_RUNS_DIR"/haze "$HAZE_RUNS_DIR"/haze_* "$HAZE_RUNS_DIR"/epoch_* 2>/dev/null
  fi
}
trap cleanup EXIT

echo "=== [1/2] starting fhetch_server (port $PORT) ==="
# Server's cwd determines where its working files (replay sources, etc.)
# land. Anchor it to HAZE_RUNS_DIR so the repo isn't polluted on each
# run — without this the per-request `nbcc_fhetch_replay_source_*` dirs
# appear wherever the script was invoked from.
mkdir -p "$HAZE_RUNS_DIR"
(
  cd "$HAZE_RUNS_DIR"
  PORT="$PORT" BIND=127.0.0.1 \
    NIOBIUM_COMPILER_ROOT="$NIOBIUM_COMPILER_ROOT" \
    NIOBIUM_COMPILER_BUILD="$NIOBIUM_COMPILER_ROOT/build" \
    "$FHETCH_SERVER_SH" > "$SERVER_LOG" 2>&1
) &
SERVER_PID=$!

# Wait for /healthz, mirroring niobium-client/scripts/test_transport_mult.sh.
for i in $(seq 1 50); do
  if curl -sf "http://127.0.0.1:$PORT/healthz" >/dev/null 2>&1; then
    echo "fhetch_server up (pid=$SERVER_PID)"
    break
  fi
  if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "[test_haze_integration.sh] error: server exited before /healthz responded" >&2
    exit 3
  fi
  sleep 0.1
done
if ! curl -sf "http://127.0.0.1:$PORT/healthz" >/dev/null 2>&1; then
  echo "[test_haze_integration.sh] error: server did not become healthy within 5s" >&2
  exit 3
fi

echo
echo "=== [2/2] running haze_tests [integration] through transport ==="
# Forwarder first on PATH so libnbfhetch's system("nbcc_fhetch_replay")
# resolves to the client-side forwarder, which packs the project and
# ships it to the server.
export PATH="$TRANSPORT_DIR:$PATH"
export NBCC_FHETCH_SERVER="http://127.0.0.1:$PORT"

# cd into the runs dir so libnbfhetch's get_program_directory() resolves
# program artifacts under HAZE_RUNS_DIR/haze rather than the caller's cwd.
mkdir -p "$HAZE_RUNS_DIR"
cd "$HAZE_RUNS_DIR"

: "${HAZE_TEST_FILTER:=[integration]}"
echo "[test_haze_integration.sh] filter=$HAZE_TEST_FILTER"
LD_LIBRARY_PATH="$OPENFHE_LIB${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  "$HAZE_TEST_BIN" "$HAZE_TEST_FILTER"
