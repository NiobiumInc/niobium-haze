// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Helpers for [integration]-tagged Catch2 cases: record through haze, the
// bridge auto-synthesizes CC + .bin + templates, and hazeFlush drives replay
// back into the tagged outputs' shadow buffers (see scripts/test_haze_integration.sh
// for transport orchestration).

#pragma once

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <vector>

namespace haze::test {

// MODULUS CONTRACT:
// For ops that carry a real modulus, the .fhetch trace's modulus table is
// authoritative: both replay paths reconstruct under it — the in-process
// simulator rebuilds each NativePoly's params from the trace modulus
// (libnbfhetch auto_facade), and the transport bridge re-installs the trace
// moduli onto input .bin towers and output templates (install_trace_*_moduli).
// Verified by "trace modulus is authoritative ..." in test_hardware_format.cpp,
// which sets the trace modulus to a prime distinct from the one the bridge's
// scaffold cryptocontext.dat is built around and confirms the result tracks the
// trace.
//
// EXCEPTION — modulus-less SRP ops: an opaque SRP hazeMemcpy(D2D) and a bare
// SRP hazeAutomorph emit the COPY_MODULUS sentinel with no real modulus to
// bind, so their OUTPUT template falls back to the bridge's scaffold prime
// (the one hazeReplayBridgeInitCryptoContext "picks"). For those the trace is
// NOT authoritative, so this setup aligns slot 0 to the scaffold prime and
// returns it: tests then generate inputs and oracles against the same value
// and the modulus-less outputs stay consistent. (Tests that only use
// modulus-carrying ops would work with any prime; the MRP variants of copy /
// automorph bind base[i] and are trace-authoritative.)

// Combined integration setup: reset, ring dim, bridge CC init, align slot to
// the bridge's scaffold prime (see contract above), configure device. Returns
// that prime so callers stay in lockstep with the .ct round-trip.
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

// Three-prime variant for MRP cases: all three slots set before the single
// hazeConfigureDevice. Slot 0 is the bridge's scaffold prime (see the
// modulus-less exception in the contract above); slots 1 and 2 are the suite's
// NTT-friendly companion primes. Returns the configured base, index-aligned
// with the modulus slots.
inline std::vector<uint64_t>
setup_integration_mrp3_config(uint64_t ring_dim = 4096,
                              uint64_t desired_modulus = 576460752303415297ULL) {
    constexpr uint64_t kQ1 = 576460752303439873ULL;
    constexpr uint64_t kQ2 = 576460752303702017ULL;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(ring_dim) == HAZE_SUCCESS);
    uint64_t picked = 0;
    REQUIRE(hazeReplayBridgeInitCryptoContext(ring_dim, desired_modulus, &picked) == HAZE_SUCCESS);
    REQUIRE(picked != 0);
    REQUIRE(hazeSetCiphertextModulus(0, picked) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(1, kQ1) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(2, kQ2) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);
    return {picked, kQ1, kQ2};
}

// a + b mod q for a, b in [0, q): one conditional subtract, no overflow for
// q < 2^63 (all suite primes qualify).
inline uint64_t add_mod(uint64_t a, uint64_t b, uint64_t q) {
    const uint64_t s = a + b;
    return (s >= q) ? s - q : s;
}

// Deterministic non-trivial residue. Avoids zeros and all-equal
// coefficients across primes so per-residue bugs cannot hide behind a
// trivial input.
inline std::vector<uint64_t> make_residue(uint64_t prime, uint64_t seed, std::size_t n) {
    std::vector<uint64_t> r(n);
    for (std::size_t k = 0; k < n; ++k) {
        const __uint128_t v = (static_cast<__uint128_t>(seed) * (k + 1) * 7U) +
                              static_cast<__uint128_t>(k & 0xFFFFU) + 13U;
        r[k] = static_cast<uint64_t>(v % prime);
    }
    return r;
}

// Allocate one slot per residue and H2D each row; ownership stays with the
// caller (free with free_all_residues, REQUIRE-aborts on any haze error).
inline std::vector<void *>
allocate_and_h2d_residues(const std::vector<std::vector<uint64_t>> &residues) {
    std::vector<void *> ptrs(residues.size(), nullptr);
    for (std::size_t i = 0; i < residues.size(); ++i) {
        const std::size_t bytes = residues[i].size() * sizeof(uint64_t);
        REQUIRE(hazeMalloc(&ptrs[i], bytes) == HAZE_SUCCESS);
        REQUIRE(hazeMemcpy(ptrs[i], residues[i].data(), bytes, HAZE_MEMCPY_HOST_TO_DEVICE) ==
                HAZE_SUCCESS);
    }
    return ptrs;
}

// Allocate `count` empty polynomial slots of `bytes` bytes each.
inline std::vector<void *> allocate_dst_residues(std::size_t count, std::size_t bytes) {
    std::vector<void *> ptrs(count, nullptr);
    for (std::size_t i = 0; i < count; ++i) {
        REQUIRE(hazeMalloc(&ptrs[i], bytes) == HAZE_SUCCESS);
    }
    return ptrs;
}

inline void free_all_residues(const std::vector<void *> &ptrs) {
    for (void *p : ptrs) {
        REQUIRE(hazeFree(p) == HAZE_SUCCESS);
    }
}

// Parallel const view over a vector<void *>; useful for chaining dst slots
// as src in a recording.
inline std::vector<const void *> to_const(const std::vector<void *> &ptrs) {
    return {ptrs.begin(), ptrs.end()};
}

// Negacyclic convolution mod q (host oracle for NTT/hazeMul/INTT tests, not
// a hot path); X^N = -1 wraps with a sign flip when i+j ≥ N. Loop invariant:
// each c[k] stays in [0, q) — the branches below rely on it.
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
