// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Trace-modulus authority tests: hazeReplayBridgeInitCryptoContext builds its
// own scaffold CryptoContext around `desired_modulus`, but the trace modulus
// (hazeFheParams::moduli) is authoritative and synthesis rebases the tower
// onto it before replay. Neither test reads the scaffold prime back (the API
// does not expose it); each instead picks `desired_modulus` so the scaffold's
// bit width is provably far from the trace modulus's, and shows replay
// behaves as if the scaffold prime were never in play.

#include <bit>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <vector>

namespace {

constexpr uint64_t kRingDim = 4096;
constexpr size_t kBytes = kRingDim * sizeof(uint64_t);

// Same NTT-friendly primes (q ≡ 1 mod 2N, N=4096) as test_basis_convert.cpp.
constexpr uint64_t kQ0 = 576460752303415297ULL;
constexpr uint64_t kQ1 = 576460752303439873ULL;

// openfhe_template.cpp's build_context sizes its scaffold's first-tower prime
// from bit_width(desired_modulus) alone (SetFirstModSize(max_bits)), and
// OpenFHE's NTT-friendly prime search for FirstModSize=k lands in
// [2^(k-1), 2^k). A desired_modulus far smaller (in bit width) than the real
// trace modulus therefore bounds the scaffold prime far below it, without
// this test ever reading the scaffold prime back. Wide on purpose (40 bits,
// not a narrow one): a suitable q ≡ 1 mod 2N NTT-friendly prime is far more
// likely to exist (and to be found quickly) in a wide window than in a
// narrow one, so this does not depend on one existing in some specific
// 21-bit band.
constexpr uint64_t kSmallDesiredModulus = 1ULL << 39; // FirstModSize = 40
static_assert(std::bit_width(kSmallDesiredModulus) == 40);

} // namespace

// Two distinct moduli are in play: kSmallDesiredModulus (only a sizing hint
// for the bridge's scaffold prime, itself never used as a modulus) and the
// trace modulus kQ1 (set via hazeFheParams::moduli). The scaffold prime S is
// bounded to [2^39, 2^40) by construction (see kSmallDesiredModulus above) —
// far below kQ1 (~2^59) — so S != kQ1.
TEST_CASE("trace modulus is authoritative over the bridge's scaffold prime",
          "[integration][modulus]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const uint64_t moduli[] = {kQ1};
    const hazeFheParams fhe = {.ring_dim = kRingDim, .moduli = moduli, .moduli_count = 1};
    REQUIRE(hazeConfigureDevice(&fhe, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, kSmallDesiredModulus) == HAZE_SUCCESS);

    // a = kQ1 - 7, b = 11  =>  a + b = kQ1 + 4, which overflows kQ1 by exactly 4.
    // Wrapping the sum at any modulus S != kQ1 instead gives (kQ1 + 4) mod S,
    // and since 4 < S, that equals 4 exactly iff kQ1 mod S == 0, i.e. S | kQ1.
    // kQ1 is prime, so its only divisors are 1 and itself; the scaffold prime
    // is a different prime (bounded to [2^39, 2^40), far below kQ1), so it
    // cannot divide kQ1. Getting 4 below is therefore exact proof the sum
    // wrapped at kQ1 and not at the scaffold prime — not merely likely.
    std::vector<uint64_t> va(kRingDim, kQ1 - 7);
    std::vector<uint64_t> vb(kRingDim, 11);
    constexpr uint64_t kExpect = 4;

    void *pa = nullptr;
    void *pb = nullptr;
    void *sum = nullptr;
    REQUIRE(hazeMalloc(&pa, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&pb, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&sum, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(pa, va.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(pb, vb.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(sum, pa, pb, 0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(sum) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    std::vector<uint64_t> out(kRingDim, 0);
    REQUIRE(hazeMemcpy(out.data(), sum, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    for (uint64_t k = 0; k < kRingDim; ++k) {
        INFO("slot " << k);
        REQUIRE(out[k] == kExpect);
    }
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

// Input residues in [2^40, kQ0) must round-trip: the scaffold prime built from
// kSmallDesiredModulus sits in [2^39, 2^40) (see kSmallDesiredModulus above),
// strictly below kQ0 (~2^59), so a value in that band would wrap if filled
// before the tower was rebased onto kQ0 (synthesize rebases first). At this
// bit-width gap the wrap window [2^40, kQ0) covers nearly the entire
// representable range, so a *random* residue would almost certainly land in
// it (the opposite of rare) — the two boundary values below (vals[1],
// vals[2]) are pinned explicitly instead, to nail down the exact threshold
// rather than lean on that near-certainty.
TEST_CASE("input residues above the scaffold prime survive .bin synthesis",
          "[integration][modulus]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const uint64_t moduli[] = {kQ0};
    const hazeFheParams fhe = {.ring_dim = kRingDim, .moduli = moduli, .moduli_count = 1};
    REQUIRE(hazeConfigureDevice(&fhe, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, kSmallDesiredModulus) == HAZE_SUCCESS);

    // Seed the wrap window: values that provably exceed the scaffold prime
    // (>= 2^40) but are valid mod the trace prime (< kQ0). A scalar-mul by 1
    // is the identity, so each output must equal its input exactly.
    std::vector<uint64_t> vals(kRingDim);
    for (uint64_t i = 0; i < kRingDim; ++i)
        vals[i] = (i % 5 == 0) ? (kQ0 - 1 - (i % 7))          // provably in [2^40, kQ0)
                               : (i * 11 + 3) % (1ULL << 39); // provably below the scaffold prime
    vals[1] = 1ULL << 40;       // lower edge of the guaranteed wrap window
    vals[2] = (1ULL << 39) - 1; // just below the scaffold prime's own range — control

    void *src = nullptr;
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&src, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(src, vals.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMulScalar(dst, src, 1, 0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(dst) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    std::vector<uint64_t> out(kRingDim, 0);
    REQUIRE(hazeMemcpy(out.data(), dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    for (uint64_t k = 0; k < kRingDim; ++k) {
        INFO("slot " << k << " in=" << vals[k]);
        REQUIRE(out[k] == vals[k]);
    }
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}
