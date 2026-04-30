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
// Positive tests for ring_dim handling on the HAZE/niobium boundary.
// Companion to docs/lazy_shadow_flake.md, which tracks an intermittent
// abort signature `m=962` from OpenFHE's RootOfUnity inside niobium's
// stop_epoch NTT-table generation. The corruption is downstream of
// HAZE's tag_input handoff (we instrumented HAZE to log any drift in
// the Polynomial it registers and observed zero drift events across
// hundreds of runs). These tests therefore exercise the public-API
// paths that *would* exercise a HAZE-side ring_dim leak if one
// existed — round-trip at canonical N, multi-op chaining within one
// epoch, repeated reset/configure cycles, basis-convert identity
// math, and reset-then-reconfigure-at-same-N — all expected to pass.
// They serve as positive regression coverage; they will not catch the
// niobium-side leak that remains under investigation.

#include "integration_helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>       // IWYU pragma: keep
#include <haze/haze_types.h> // IWYU pragma: keep
#include <vector>

namespace {

constexpr uint64_t kQ = 576460752303415297ULL; // standard CKKS test prime
constexpr uint64_t kN = 4096;
constexpr size_t kBytes = kN * sizeof(uint64_t);

// Setup with the canonical test ring dim. Resets HAZE state first so
// each test sees a clean Config / EpochState / DeviceAllocator. The
// reset is deliberate — without it Config / poly_map_ state from a
// prior test could leak through.
//
// Routes through haze_replay_bridge so the test runs end-to-end via
// the FHETCH transport: bridge picks the actual tower modulus near
// 2^bits(kQ), aligns haze's ciphertext modulus, sets target=FUNC_SIM
// so hazeReplay() dispatches to nbcc_fhetch_replay rather than no-op'ing.
void setup_4096() {
    haze::test::setup_integration_compute_config(kN, kQ, 0);
}

inline void d2h(std::vector<uint64_t> &dst, const void *dev) {
    REQUIRE(hazeReplay() == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(dst.data(), dev, dst.size() * sizeof(uint64_t),
                       HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
}

inline void h2d(void *dev, const std::vector<uint64_t> &src) {
    REQUIRE(hazeMemcpy(dev, src.data(), src.size() * sizeof(uint64_t),
                       HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
}

} // namespace

// (1) Construction-time ring_dim is preserved end-to-end at the canonical N.
//
// The pertinent property is not the arithmetic but that stop_epoch()
// doesn't fire RootOfUnity with a bogus 2*N. The bisect in
// docs/lazy_shadow_flake.md traced the failure signature ("m=962")
// back to a Polynomial reaching tag_input with ring_dim ≠ 4096 — this
// test is the simplest end-to-end exercise that would surface the
// HAZE-side variant of that bug.
TEST_CASE("ring_dim consistency: H2D->Add->D2H round-trip at N=4096", "[integration]") {
    setup_4096();

    void *d_a = nullptr, *d_b = nullptr, *d_dst = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst, kBytes) == HAZE_SUCCESS);

    std::vector<uint64_t> a(kN, 7), b(kN, 11);
    h2d(d_a, a);
    h2d(d_b, b);
    REQUIRE(hazeAdd(d_dst, d_a, d_b, 0, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> out(kN, 0);
    d2h(out, d_dst);
    for (uint64_t i = 0; i < kN; ++i)
        REQUIRE(out[i] == 18);

    REQUIRE(hazeFree(d_a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_b) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_dst) == HAZE_SUCCESS);
}

// (2) Multiple compute ops within a single recording window share
// poly_map_'s cached entries — the cached-Polynomial branch of
// lookup_or_create_locked is the hot path here. The second
// operation reuses the first op's output (compute_results_ entry)
// as input. If poly_map_ ever served a Polynomial with a stale
// ring_dim, this is the path that would expose it.
TEST_CASE("ring_dim consistency: chained ops within one epoch reuse poly_map_ entries",
          "[integration]") {
    setup_4096();

    void *d_a = nullptr, *d_b = nullptr, *d_c = nullptr, *d_t = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_c, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_t, kBytes) == HAZE_SUCCESS);

    std::vector<uint64_t> a(kN, 1), b(kN, 2), c(kN, 3);
    h2d(d_a, a);
    h2d(d_b, b);
    h2d(d_c, c);

    // (a + b) + c, two chained adds in a single recording window.
    // The second add reuses d_t (a compute_results_ entry produced
    // by the first add) on the cached path through
    // lookup_or_create_locked.
    REQUIRE(hazeAdd(d_t, d_a, d_b, 0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(d_t, d_t, d_c, 0, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> out(kN, 0);
    d2h(out, d_t);
    for (uint64_t i = 0; i < kN; ++i)
        REQUIRE(out[i] == 6);

    REQUIRE(hazeFree(d_a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_b) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_c) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_t) == HAZE_SUCCESS);
}

// (3) Stress: repeated reset → configure → compute cycles at the
// same ring_dim. Exercises the hazeDeviceReset → reconfigure →
// compute → D2H → stop_epoch loop heavily within one process.
// Each cycle goes through clear_state_locked at end-of-epoch and
// a fresh ensure_recording_locked at the start of the next, so any
// state leak across epoch boundaries that depends on accumulated
// heap pressure would surface here.
TEST_CASE("ring_dim consistency: 50 reset/configure/compute cycles", "[integration]") {
    for (int iter = 0; iter < 50; ++iter) {
        setup_4096();

        void *d_a = nullptr, *d_dst = nullptr;
        REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
        REQUIRE(hazeMalloc(&d_dst, kBytes) == HAZE_SUCCESS);

        std::vector<uint64_t> a(kN, static_cast<uint64_t>(iter));
        h2d(d_a, a);
        REQUIRE(hazeAddScalar(d_dst, d_a, 1, 0, nullptr) == HAZE_SUCCESS);

        std::vector<uint64_t> out(kN, 0);
        d2h(out, d_dst);
        for (uint64_t i = 0; i < kN; ++i) {
            REQUIRE(out[i] == static_cast<uint64_t>(iter + 1));
        }

        REQUIRE(hazeFree(d_a) == HAZE_SUCCESS);
        REQUIRE(hazeFree(d_dst) == HAZE_SUCCESS);
    }
}

// (4) Basis-convert end-to-end: the multi-output materialization path
// is the dominant trigger for the lazy_shadow_flake.md flake — every
// basis-convert call tags multiple input residues as fhetch inputs
// and stores multiple output residues, multiplying the chances of a
// stale-ring_dim Polynomial slipping through. Verify the math
// actually computes correctly, not just that the call returns
// SUCCESS. A "shared modulus identity convert" (src_base == dst_base,
// single residue) is the simplest non-trivial case: dst should
// equal src byte-for-byte.
TEST_CASE("ring_dim consistency: hazeBasisConvert preserves data on identity convert",
          "[integration]") {
    setup_4096();

    void *src = nullptr, *dst = nullptr;
    REQUIRE(hazeMalloc(&src, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);

    std::vector<uint64_t> src_data(kN);
    for (uint64_t i = 0; i < kN; ++i)
        src_data[i] = (i * 7919 + 13) % kQ;
    h2d(src, src_data);

    const uint64_t base[] = {kQ};
    const void *src_polys[] = {src};
    void *dst_polys[] = {dst};

    hazeBasisConvertParams p{};
    p.src_base = base;
    p.src_base_len = 1;
    p.dst_base = base;
    p.dst_base_len = 1;
    REQUIRE(hazeBasisConvert(dst_polys, src_polys, &p, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> out(kN, 0);
    d2h(out, dst);
    for (uint64_t i = 0; i < kN; ++i) {
        REQUIRE(out[i] == src_data[i]);
    }

    REQUIRE(hazeFree(src) == HAZE_SUCCESS);
    REQUIRE(hazeFree(dst) == HAZE_SUCCESS);
}

// (5) Cross-test isolation: two complete record→D2H epochs at the
// same ring_dim, separated by an explicit hazeDeviceReset.
//
// We deliberately keep ring_dim the same across the two epochs.
// Changing it across epochs in one process exposes a separate
// caching bug on the niobium-compiler side (the replay subprocess
// holds onto the ring_dim from the first init across subsequent
// epochs; observed as "[NBCC ERROR] Ring dimension mismatch: memory
// has X expected Y"). That bug is filed separately.
TEST_CASE("ring_dim consistency: hazeDeviceReset between two epochs at same N", "[integration]") {
    // Two epochs, both at N=4096, separated by an explicit reset.
    setup_4096();
    {
        void *d_a = nullptr, *d_dst = nullptr;
        REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
        REQUIRE(hazeMalloc(&d_dst, kBytes) == HAZE_SUCCESS);
        std::vector<uint64_t> a(kN, 5);
        h2d(d_a, a);
        REQUIRE(hazeAddScalar(d_dst, d_a, 2, 0, nullptr) == HAZE_SUCCESS);
        std::vector<uint64_t> out(kN, 0);
        d2h(out, d_dst);
        for (uint64_t i = 0; i < kN; ++i)
            REQUIRE(out[i] == 7);
        REQUIRE(hazeFree(d_a) == HAZE_SUCCESS);
        REQUIRE(hazeFree(d_dst) == HAZE_SUCCESS);
    }

    setup_4096(); // reset + reconfigure at the same N
    {
        void *d_a = nullptr, *d_dst = nullptr;
        REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
        REQUIRE(hazeMalloc(&d_dst, kBytes) == HAZE_SUCCESS);
        std::vector<uint64_t> a(kN, 9);
        h2d(d_a, a);
        REQUIRE(hazeAddScalar(d_dst, d_a, 4, 0, nullptr) == HAZE_SUCCESS);
        std::vector<uint64_t> out(kN, 0);
        d2h(out, d_dst);
        for (uint64_t i = 0; i < kN; ++i)
            REQUIRE(out[i] == 13);
        REQUIRE(hazeFree(d_a) == HAZE_SUCCESS);
        REQUIRE(hazeFree(d_dst) == HAZE_SUCCESS);
    }
}
