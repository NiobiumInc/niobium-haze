#!/bin/bash
# ============================================================================
# test_haze_integration_standalone.sh -- run [integration]-tagged haze_tests
# against an externally-supplied nbcc_fhetch_replay binary, with no HTTP
# transport in the loop.
#
# Standalone counterpart to test_haze_integration.sh. The HTTP-transport
# version is what niobium-client uses to drive haze tests when it owns both
# the forwarder and the server. From haze's own checkout we don't have
# (and don't want to require) the niobium-client transport binaries, so
# libnbfhetch's Compiler::replay() is pointed straight at the
# compiler-built binary via NBCC_FHETCH_REPLAY.
#
# Required env (the standalone Makefile test-integration-release target
# plumbs all of these in):
#   HAZE_TEST_BIN          Absolute path to the haze_tests executable.
#   NIOBIUM_COMPILER_ROOT  Path to a niobium-compiler checkout containing
#                          build/nbcc_fhetch_replay. The binary must be
#                          executable.
#   OPENFHE_LIB            Path to the OpenFHE shared-library directory.
#                          Loaded via DYLD_LIBRARY_PATH (macOS) and
#                          LD_LIBRARY_PATH (Linux) so haze_tests resolves
#                          libOPENFHEcore at runtime.
#   HAZE_RUNS_DIR          Working directory haze_tests cd's into before
#                          recording. Per-test program dirs (haze, haze_*,
#                          epoch_*) land here and are cleaned on EXIT.
# ============================================================================
set -euo pipefail

: "${HAZE_TEST_BIN:?HAZE_TEST_BIN must be set (path to haze_tests executable)}"
: "${NIOBIUM_COMPILER_ROOT:?NIOBIUM_COMPILER_ROOT must be set (path to niobium-compiler checkout)}"
: "${OPENFHE_LIB:?OPENFHE_LIB must be set (OpenFHE shared-library dir)}"
: "${HAZE_RUNS_DIR:?HAZE_RUNS_DIR must be set (per-test artifact root)}"

NBCC_FHETCH_REPLAY="$NIOBIUM_COMPILER_ROOT/build/nbcc_fhetch_replay"

for path in "$HAZE_TEST_BIN" "$NBCC_FHETCH_REPLAY"; do
  if [[ ! -e "$path" ]]; then
    echo "[test_haze_integration_standalone.sh] error: not found: $path" >&2
    exit 2
  fi
done
if [[ ! -x "$NBCC_FHETCH_REPLAY" ]]; then
  echo "[test_haze_integration_standalone.sh] error: not executable: $NBCC_FHETCH_REPLAY" >&2
  echo "  (run \`make build-release\` in $NIOBIUM_COMPILER_ROOT first)" >&2
  exit 2
fi
if [[ ! -x "$HAZE_TEST_BIN" ]]; then
  echo "[test_haze_integration_standalone.sh] error: not executable: $HAZE_TEST_BIN" >&2
  exit 2
fi

cleanup() {
  set +e
  # Per-program directories land directly under HAZE_RUNS_DIR. Leave the
  # runs dir itself in place so re-invocations don't recreate it.
  # Use find rather than glob expansion so unmatched patterns surface as
  # a real no-op rather than silently expanding to literal "haze_*" etc.
  if [[ -d "$HAZE_RUNS_DIR" ]]; then
    find "$HAZE_RUNS_DIR" -mindepth 1 -maxdepth 1 \
      \( -name "haze" -o -name "haze_*" -o -name "epoch_*" \) \
      -exec rm -rf {} + 2>/dev/null
  fi
}
trap cleanup EXIT

mkdir -p "$HAZE_RUNS_DIR"
cd "$HAZE_RUNS_DIR"

echo "=== running haze_tests [integration] (direct nbcc_fhetch_replay invocation) ==="
echo "    binary: $NBCC_FHETCH_REPLAY"
echo "    runs:   $HAZE_RUNS_DIR"

# libnbfhetch's Compiler::replay() honors NBCC_FHETCH_REPLAY as an absolute
# override before falling back to PATH lookup, which is what lets us bypass
# the HTTP forwarder entirely.
NBCC_FHETCH_REPLAY="$NBCC_FHETCH_REPLAY" \
DYLD_LIBRARY_PATH="$OPENFHE_LIB${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
LD_LIBRARY_PATH="$OPENFHE_LIB${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  "$HAZE_TEST_BIN" "[integration]"
