// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Trace-modulus authority tests: hazeReplayBridgeInitCryptoContext picks its own
// scaffold prime, but the trace modulus is authoritative and synthesis rebases
// the tower onto it first. Only observable in the band between the two primes.

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

} // namespace

// Three distinct valid primes are in play: kQ0 (requested of the bridge),
// `picked` (what GenCryptoContext built cryptocontext.dat around), and the
// trace modulus we set (kQ1). The add wraps into [picked, kQ1) so mod-kQ1 (no
// wrap) and mod-picked (wrap) differ; the result must equal the kQ1 oracle,
// confirming the trace modulus — not the scaffold prime — drives replay.
TEST_CASE("trace modulus is authoritative over the bridge's scaffold prime",
          "[integration][modulus]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const uint64_t moduli[] = {kQ1};
    const hazeFheParams fhe = {.ring_dim = kRingDim, .moduli = moduli, .moduli_count = 1};
    REQUIRE(hazeConfigureDevice(&fhe, nullptr) == HAZE_SUCCESS);
    uint64_t picked = 0;
    REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, kQ0, &picked) == HAZE_SUCCESS);
    // The only prerequisite: the scaffold prime sits below the trace prime, so
    // the wrap window [picked, kQ1) used below is non-empty (and picked != kQ1).
    REQUIRE(picked < kQ1);

    // a = kQ1 - 7, b = 11  =>  a + b = kQ1 + 4.
    //   mod kQ1    = 4              (trace authoritative)
    //   mod picked = (kQ1-picked)+4 (scaffold prime authoritative)
    // a is a valid residue mod kQ1 but exceeds picked, so the two paths can't
    // coincide.
    std::vector<uint64_t> va(kRingDim, kQ1 - 7);
    std::vector<uint64_t> vb(kRingDim, 11);
    const uint64_t expect_kq1 = 4;
    const uint64_t expect_picked = (kQ1 - picked) + 4;
    REQUIRE(expect_kq1 != expect_picked);

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
        INFO("slot " << k << " picked=" << picked);
        REQUIRE(out[k] == expect_kq1);
    }
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

// Input residues in [scaffold_prime, trace_prime) must round-trip: the scaffold
// prime is smaller than the trace prime (picked < kQ0), so such a value would
// wrap if filled before the tower was rebased (synthesize rebases first).
// Random residues hit this ~3.4M-wide window with negligible probability, so
// this pins it explicitly.
TEST_CASE("input residues above the scaffold prime survive .bin synthesis",
          "[integration][modulus]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const uint64_t moduli[] = {kQ0};
    const hazeFheParams fhe = {.ring_dim = kRingDim, .moduli = moduli, .moduli_count = 1};
    REQUIRE(hazeConfigureDevice(&fhe, nullptr) == HAZE_SUCCESS);
    uint64_t picked = 0;
    REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, kQ0, &picked) == HAZE_SUCCESS);
    REQUIRE(picked < kQ0); // the wrap window [picked, kQ0) below relies on it

    // Seed the wrap window: values that exceed the scaffold prime but are
    // valid mod the trace prime. A scalar-mul by 1 is the identity, so each
    // output must equal its input exactly.
    std::vector<uint64_t> vals(kRingDim);
    for (uint64_t i = 0; i < kRingDim; ++i)
        vals[i] = (i % 5 == 0) ? (kQ0 - 1 - (i % 7)) // in [picked, kQ0)
                               : (i * 11 + 3) % picked;
    vals[1] = picked;     // exact lower edge of the window
    vals[2] = picked - 1; // just below — control

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
        INFO("slot " << k << " in=" << vals[k] << " picked=" << picked << " kQ0=" << kQ0);
        REQUIRE(out[k] == vals[k]);
    }
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}
