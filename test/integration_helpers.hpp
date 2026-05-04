// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Helpers for [integration]-tagged Catch2 cases. Integration tests:
//   1. Record FHETCH instructions through haze.
//   2. Bridge auto-synthesizes a CryptoContext + per-input cereal-binary
//      .bin files + per-output ciphertext templates at recording-finalize
//      time (see haze_replay_bridge's post_recording_hook).
//   3. The first hazeMemcpy(D2H) finalises the recording, dispatches the
//      project to a niobium-compiler-built nbcc_fhetch_replay over the
//      FHETCH HTTP transport, and reads the materialized values back.
//   4. The simulator-computed values flow back as serialized_probes/*.ct,
//      which fhetch::result reads and replay_and_populate writes into the
//      haze shadow buffers; D2H then returns those values.
//
// Orchestration of the server + client-side forwarder lives in
// scripts/test_haze_integration.sh.

#pragma once

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <vector>

namespace haze::test {

// Combined integration setup: reset, ring dim, bridge CryptoContext init
// (which writes <program_dir>/cryptocontext.dat), align haze's ciphertext
// modulus with OpenFHE's picked tower prime, then configure the device.
// Returns the picked modulus so callers can keep using it consistently
// with what haze recorded.
//
// Target selection: the test Makefile (test-sim / test-transport) sets
// HAZE_TARGET to whichever string the surrounding suite needs. The
// helper honours that env var; if unset, falls back to haze's own
// default ("local" — in-process FHETCH simulator, per hazeSetTarget docs).
//
// Modulus alignment: OpenFHE's CKKS GenCryptoContext picks a prime near
// 2^bits(desired). The picked value rarely matches `desired_modulus`
// exactly. To keep haze's recording-side modulus arithmetic in lockstep
// with the .ct round-trip, we feed the picked value back into
// hazeSetCiphertextModulus before any compute.
//
// After this call returns, tests do compute + hazeMemcpy(D2H); the D2H
// triggers replay before reading the shadow buffer. The bridge's
// post_recording_hook auto-writes the .bin / .ids / .template artifacts
// during stop(); tests do not need to call WriteTemplate or
// WriteInputBin explicitly.
inline uint64_t setup_integration_compute_config(uint64_t ring_dim = 4096,
                                                 uint64_t desired_modulus = 576460752303415297ULL,
                                                 int mod_idx = 0) {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(ring_dim) == HAZE_SUCCESS);
    uint64_t picked = 0;
    REQUIRE(hazeReplayBridgeInitCryptoContext(ring_dim, desired_modulus, &picked) == HAZE_SUCCESS);
    REQUIRE(picked != 0);
    REQUIRE(hazeSetCiphertextModulus(mod_idx, picked) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);
    return picked;
}

// Negacyclic convolution mod q: c(X) = a(X) * b(X) mod (X^N + 1, q).
// O(N^2). Uses __uint128_t per-term so ~60-bit primes don't overflow the
// product. X^N = -1 in this quotient ring, so a contribution at index
// (i + j) wraps with a sign flip when i + j >= N. Intended as a host
// oracle for NTT->hazeMul->INTT integration tests, not a hot path.
//
// Preconditions: a.size() == b.size(), all coefficients in [0, q).
// Loop invariant: every c[k] stays in [0, q) after each update — the
// branches below depend on this. If a future edit reorders or extends
// the update logic, preserve this invariant or the modular arithmetic
// will silently corrupt.
inline std::vector<uint64_t> negacyclic_conv_ref(const std::vector<uint64_t> &a,
                                                 const std::vector<uint64_t> &b, uint64_t q) {
    REQUIRE(a.size() == b.size());
    const std::size_t n = a.size();
    std::vector<uint64_t> c(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        if (a[i] == 0) {
            continue;
        }
        for (std::size_t j = 0; j < n; ++j) {
            if (b[j] == 0) {
                continue;
            }
            const __uint128_t prod =
                static_cast<__uint128_t>(a[i]) * static_cast<__uint128_t>(b[j]);
            const uint64_t term = static_cast<uint64_t>(prod % q); // term in [0, q).
            const std::size_t k = (i + j) % n;
            const uint64_t cur = c[k]; // cur in [0, q) by invariant.
            if ((i + j) >= n) {
                // Sign flip: c[k] -= term  (mod q). Result is in [0, q).
                c[k] = (cur >= term) ? (cur - term) : (cur + (q - term));
            } else {
                // c[k] += term  (mod q). cur + term < 2q (both < q), so
                // one subtract suffices to land back in [0, q).
                const uint64_t sum = cur + term;
                c[k] = (sum >= q) ? (sum - q) : sum;
            }
        }
    }
    return c;
}

} // namespace haze::test
