// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Helpers for [integration]-tagged Catch2 cases: record through haze, the
// bridge auto-synthesizes CC + .bin + templates, and D2H drives replay
// back into the shadow buffers (see scripts/test_haze_integration.sh for
// transport orchestration).

#pragma once

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <niobium/compiler.h>
#include <niobium/fhetch_api.h>
#include <string>
#include <vector>

namespace haze::test {

// Combined integration setup: reset, ring dim, bridge CC init, align haze's
// ciphertext modulus with OpenFHE's picked prime, configure device. Returns
// the picked modulus so callers stay in lockstep with the .ct round-trip.
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

// Cross-check an MRP output by content (not auto-generated name) so op-order
// shifts don't break the test. SRP is the ground truth; this asserts the
// MRP read path returns the same per-residue values.
inline void check_mrp_against_per_residue(const std::vector<uint64_t> &base,
                                          const std::vector<std::vector<uint64_t>> &expected) {
    REQUIRE(expected.size() == base.size());
    namespace fs = std::filesystem;
    const auto probes_dir = niobium::compiler().get_program_directory() / "serialized_probes";
    INFO("scanning " << probes_dir);
    REQUIRE(fs::exists(probes_dir));

    auto residue_matches = [&](const niobium::fhetch::MRP &mrp) -> bool {
        if (mrp.num_residues() != base.size())
            return false;
        const auto &got_base = mrp.base();
        for (std::size_t i = 0; i < base.size(); ++i) {
            if (std::ranges::find(got_base, base[i]) == got_base.end())
                return false;
            if (mrp[base[i]].int_data() != expected[i])
                return false;
        }
        return true;
    };

    // At least one captured group should match; multiple is fine since
    // disk cleanup at hazeReplayBridgeReset prevents stale-file leaks.
    bool found = false;
    std::string matched_name;
    for (const auto &entry : fs::directory_iterator(probes_dir)) {
        if (!entry.is_regular_file())
            continue;
        const auto stem = entry.path().stem().string();
        if (!stem.starts_with("haze_mrp_out_"))
            continue;
        niobium::fhetch::MRP mrp;
        if (!niobium::fhetch::result(stem, mrp))
            continue;
        if (residue_matches(mrp)) {
            found = true;
            matched_name = stem;
            break;
        }
    }
    INFO("matched MRP group: " << (found ? matched_name : std::string{"<none>"}));
    REQUIRE(found);
}

} // namespace haze::test
