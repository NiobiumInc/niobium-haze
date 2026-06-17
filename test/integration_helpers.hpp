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

// MODULUS CONTRACT (single source of truth): the moduli passed to
// hazeContextCreate ARE the .fhetch trace moduli, and both replay paths
// reconstruct under them; hazeReplayBridgeInitCryptoContext's scaffold prime is
// overwritten with the trace primes at synthesis (bridge switch_tower_modulus)
// and never consulted for results. So tests create the context with the
// intended primes and oracle against them (verified by "trace modulus
// is authoritative ..." in test_hardware_format.cpp). Modulus-less SRP ops are
// trace-authoritative too — haze recovers and binds the source modulus — except
// a copy of a never-modulus-bound (raw H2D) address, which has none.

// The current test context. recreate_ctx (via the setup_* helpers)
// destroys the previous case's context and creates a fresh one — the
// per-case isolation hazeDeviceReset used to provide for engine state;
// hazeDeviceReset itself now resets only the process globals.
inline hazeContext_t &ctx() {
    static hazeContext_t current = nullptr;
    return current;
}

inline void recreate_ctx(uint64_t ring_dim, const std::vector<uint64_t> &moduli) {
    if (ctx() != nullptr)
        REQUIRE(hazeContextDestroy(ctx()) == HAZE_SUCCESS);
    ctx() = nullptr;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeContextCreate(&ctx(), ring_dim, moduli.data(), moduli.size()) == HAZE_SUCCESS);
}

// Combined integration setup: fresh context with the modulus in slot
// `mod_idx`, process-global reset, bridge CC init. Returns the modulus so
// callers generate inputs and oracles against the same (authoritative) value.
inline uint64_t setup_integration_compute_config(uint64_t ring_dim = 4096,
                                                 uint64_t modulus = 576460752303415297ULL,
                                                 int mod_idx = 0) {
    std::vector<uint64_t> moduli(static_cast<size_t>(mod_idx) + 1, modulus);
    recreate_ctx(ring_dim, moduli);
    uint64_t scaffold = 0; // built then overwritten from the trace; not used for results
    REQUIRE(hazeReplayBridgeInitCryptoContext(ring_dim, modulus, &scaffold) == HAZE_SUCCESS);
    return modulus;
}

// Three-prime variant for MRP cases: slot 0 the requested modulus, slots 1
// and 2 the suite's NTT-friendly companion primes. Returns the configured
// base, index-aligned with the modulus slots.
inline std::vector<uint64_t>
setup_integration_mrp3_config(uint64_t ring_dim = 4096, uint64_t modulus = 576460752303415297ULL) {
    constexpr uint64_t kQ1 = 576460752303439873ULL;
    constexpr uint64_t kQ2 = 576460752303702017ULL;
    recreate_ctx(ring_dim, {modulus, kQ1, kQ2});
    uint64_t scaffold = 0; // built then overwritten from the trace; not used for results
    REQUIRE(hazeReplayBridgeInitCryptoContext(ring_dim, modulus, &scaffold) == HAZE_SUCCESS);
    return {modulus, kQ1, kQ2};
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
        REQUIRE(hazeMalloc(ctx(), &ptrs[i], bytes) == HAZE_SUCCESS);
        REQUIRE(hazeMemcpy(ctx(), ptrs[i], residues[i].data(), bytes, HAZE_MEMCPY_HOST_TO_DEVICE) ==
                HAZE_SUCCESS);
    }
    return ptrs;
}

// Allocate `count` empty polynomial slots of `bytes` bytes each.
inline std::vector<void *> allocate_dst_residues(std::size_t count, std::size_t bytes) {
    std::vector<void *> ptrs(count, nullptr);
    for (std::size_t i = 0; i < count; ++i) {
        REQUIRE(hazeMalloc(ctx(), &ptrs[i], bytes) == HAZE_SUCCESS);
    }
    return ptrs;
}

inline void free_all_residues(const std::vector<void *> &ptrs) {
    for (void *p : ptrs) {
        REQUIRE(hazeFree(ctx(), p) == HAZE_SUCCESS);
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
