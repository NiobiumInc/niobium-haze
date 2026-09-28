// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
// The contents of this file and all related materials provided herein (the
// "Product") may not be used except pursuant to a separate written
// agreement signed by a duly authorized officer of Niobium Microsystems,
// Inc. (a "License Agreement").
// Without limiting the foregoing, you may not, at any time or for any
// reason, directly or indirectly, in whole or in part: (i) copy, modify,
// or create derivative works of the Product; (ii) rent, lease, lend, sell,
// sublicense, assign, distribute, publish, transfer, or otherwise make
// available the Product; (iii) reverse engineer, disassemble, decompile,
// decode, or adapt the Product; or (iv) remove any proprietary notices
// from the Product.
//
// Exact CRT base conversion (core/exact_basis.hpp): the raw-lift gadget is
// recorded white-box through haze_objs, and every result is checked against
// direct integer arithmetic.

#include "common/handle.hpp"
#include "common/mod_arith.hpp"
#include "core/epoch.hpp"
#include "core/exact_basis.hpp"
#include "integration_helpers.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <random>
#include <vector>

namespace {

constexpr uint64_t kRingDim = 2048;
constexpr std::size_t kBytes = kRingDim * sizeof(uint64_t);
// Prime sizes of a key-switching shape: ciphertext-chain primes just below
// 2^40, special primes just below 2^60.
constexpr unsigned kChainPrimeBits = 40;
constexpr unsigned kSpecialPrimeBits = 60;
constexpr uint64_t kSeed = 20260928;

// The `count` largest primes below 2^bits that are 1 mod 2N, NTT-friendly for
// kRingDim, in descending order.
std::vector<uint64_t> ntt_primes_below(unsigned bits, std::size_t count) {
    constexpr uint64_t kStep = 2 * kRingDim;
    std::vector<uint64_t> primes;
    for (uint64_t c = (uint64_t{1} << bits) - kStep + 1; primes.size() < count; c -= kStep) {
        if (haze::is_prime_u64(c))
            primes.push_back(c);
    }
    return primes;
}

// Reset, then configure every prime the case records under.
void configure(const std::vector<uint64_t> &moduli, int reduced_noise) {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const hazeFheParams fhe = {
        .ring_dim = kRingDim, .moduli = moduli.data(), .moduli_count = moduli.size()};
    const hazeReplayConfig replay = {.target = haze::test::target_from_env(),
                                     .reduced_noise = reduced_noise};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_SUCCESS);
    REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, moduli.front()) == HAZE_SUCCESS);
}

// Residues mod q: the boundary values 0, 1, (q-1)/2, (q+1)/2 and q-1, then
// uniform ones.
std::vector<uint64_t> boundary_then_uniform(uint64_t q, std::mt19937_64 &rng) {
    std::vector<uint64_t> v = {0, 1, (q - 1) / 2, (q + 1) / 2, q - 1};
    std::uniform_int_distribution<uint64_t> uniform(0, q - 1);
    while (v.size() < kRingDim)
        v.push_back(uniform(rng));
    return v;
}

// D2H `dst` and compare it with `want` slot by slot.
void require_residues(void *dst, const std::vector<uint64_t> &want, uint64_t p) {
    std::vector<uint64_t> got(kRingDim);
    REQUIRE(hazeMemcpy(got.data(), dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    for (std::size_t k = 0; k < kRingDim; ++k) {
        INFO("mod " << p << " slot " << k);
        REQUIRE(got[k] == want[k]);
    }
}

// Records raw_lift of the residue at src (mod q) into dst[i] (mod dst_base[i]).
void record_raw_lift(void *src, uint64_t q, const std::vector<uint64_t> &dst_base,
                     const std::vector<void *> &dst) {
    haze::EpochSession session;
    REQUIRE(haze::epoch().require_recording_locked().has_value());
    auto v = haze::epoch().lookup_or_create_locked(haze::to_dev_addr(src));
    REQUIRE(v.has_value());
    const auto lifted = haze::raw_lift(*v, q, dst_base);
    REQUIRE(lifted.size() == dst_base.size());
    for (std::size_t i = 0; i < dst.size(); ++i) {
        haze::epoch().store_compute_result_locked(haze::to_dev_addr(dst[i]), lifted[i],
                                                  dst_base[i]);
    }
}

} // namespace

TEST_CASE("raw_lift reproduces the integer at every destination prime", "[integration]") {
    const auto chain = ntt_primes_below(kChainPrimeBits, 3);
    const auto special = ntt_primes_below(kSpecialPrimeBits, 3);
    std::vector<uint64_t> moduli = chain;
    moduli.insert(moduli.end(), special.begin(), special.end());
    // Each source lifts into much smaller, slightly smaller and larger primes.
    struct Lift {
        uint64_t q;
        std::vector<uint64_t> dst_base;
    };
    const std::vector<Lift> lifts = {
        {.q = special[1], .dst_base = {chain[0], chain[1], chain[2], special[0], special[2]}},
        {.q = chain[1], .dst_base = {chain[0], chain[2], special[0], special[1], special[2]}},
    };

    // The lift must not depend on the FBC variant the config selects.
    for (const int reduced_noise : {0, 1}) {
        INFO("reduced_noise " << reduced_noise);
        configure(moduli, reduced_noise);
        std::mt19937_64 rng(kSeed);
        std::vector<std::vector<uint64_t>> inputs;
        std::vector<void *> sources;
        std::vector<std::vector<void *>> outputs;
        for (const auto &lift : lifts) {
            inputs.push_back(boundary_then_uniform(lift.q, rng));
            sources.push_back(
                haze::test::allocate_and_h2d_residues({inputs.back()}, {lift.q}).front());
            outputs.push_back(haze::test::allocate_dst_residues(lift.dst_base.size(), kBytes));
            record_raw_lift(sources.back(), lift.q, lift.dst_base, outputs.back());
        }
        for (const auto &out : outputs) {
            for (void *p : out)
                REQUIRE(hazeTagOutput(p) == HAZE_SUCCESS);
        }
        REQUIRE(hazeFlush() == HAZE_SUCCESS);

        for (std::size_t l = 0; l < lifts.size(); ++l) {
            INFO("source mod " << lifts[l].q);
            for (std::size_t i = 0; i < lifts[l].dst_base.size(); ++i) {
                const uint64_t p = lifts[l].dst_base[i];
                std::vector<uint64_t> want(kRingDim);
                for (std::size_t k = 0; k < kRingDim; ++k)
                    want[k] = inputs[l][k] % p;
                require_residues(outputs[l][i], want, p);
            }
            haze::test::free_all_residues(outputs[l]);
        }
        haze::test::free_all_residues(sources);
    }
}
