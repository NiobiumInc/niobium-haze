// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Helpers for [integration]-tagged Catch2 cases. Integration tests:
//   1. Record FHETCH instructions through haze.
//   2. Bridge auto-synthesizes a CryptoContext + per-input cereal-binary
//      .bin files + per-output ciphertext templates at recording-finalize
//      time (see haze_replay_bridge's post_recording_hook).
//   3. Call hazeReplay() to dispatch the recorded project to a
//      niobium-compiler-built nbcc_fhetch_replay over the FHETCH HTTP
//      transport.
//   4. The simulator-computed values flow back as serialized_probes/*.ct,
//      which fhetch::result reads and replay_and_populate writes into the
//      haze shadow buffers.
//   5. hazeMemcpy(D2H) returns the materialized values from shadow.
//
// Orchestration of the server + client-side forwarder lives in
// scripts/test_haze_integration.sh.

#pragma once

#include <catch2/catch_test_macros.hpp>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>

#include <cstdint>

namespace haze::test {

// Combined integration setup: reset, ring dim, target=FUNC_SIM, bridge
// CryptoContext init (which writes <program_dir>/cryptocontext.dat),
// align haze's ciphertext modulus with OpenFHE's picked tower prime,
// then configure the device. Returns the picked modulus so callers can
// keep using it consistently with what haze recorded.
//
// Modulus alignment: OpenFHE's CKKS GenCryptoContext picks a prime near
// 2^bits(desired). The picked value rarely matches `desired_modulus`
// exactly. To keep haze's recording-side modulus arithmetic in lockstep
// with the .ct round-trip, we feed the picked value back into
// hazeSetCiphertextModulus before any compute.
//
// After this call returns, tests do compute + hazeReplay() + hazeMemcpy(D2H).
// The bridge's post_recording_hook auto-writes the .bin / .ids / .template
// artifacts during stop(); tests do not need to call WriteTemplate or
// WriteInputBin explicitly.
inline uint64_t setup_integration_compute_config(
    uint64_t ring_dim, uint64_t desired_modulus, int mod_idx) {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(ring_dim) == HAZE_SUCCESS);
    REQUIRE(hazeSetTarget("FUNC_SIM") == HAZE_SUCCESS);
    uint64_t picked = 0;
    REQUIRE(hazeReplayBridgeInitCryptoContext(ring_dim, desired_modulus,
                                              &picked) == HAZE_SUCCESS);
    REQUIRE(picked != 0);
    REQUIRE(hazeSetCiphertextModulus(mod_idx, picked) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);
    return picked;
}

} // namespace haze::test
