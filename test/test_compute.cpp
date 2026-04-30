// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
#include "integration_helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <vector>

static constexpr uint64_t kRingDim = 4096;
static constexpr size_t kBytes = kRingDim * sizeof(uint64_t);
static constexpr uint64_t kModulus = 576460752303415297ULL;
static constexpr int kModIdx = 0;

TEST_CASE("hazeAdd: pointwise sum retrieved after D2H", "[integration]") {
    haze::test::setup_integration_compute_config();

    void *d_a = nullptr, *d_b = nullptr, *d_dst = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst, kBytes) == HAZE_SUCCESS);

    // Constant polynomials: a[i] = 1, b[i] = 2 → expected sum = 3
    std::vector<uint64_t> a(kRingDim, 1), b(kRingDim, 2);
    REQUIRE(hazeMemcpy(d_a, a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_b, b.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    REQUIRE(hazeAdd(d_dst, d_a, d_b, kModIdx, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> result(kRingDim, 0);
    REQUIRE(hazeReplay() == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(result.data(), d_dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    for (uint64_t i = 0; i < kRingDim; ++i) {
        REQUIRE(result[i] == 3);
    }

    REQUIRE(hazeFree(d_a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_b) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_dst) == HAZE_SUCCESS);
}

TEST_CASE("hazeSub: pointwise difference retrieved after D2H", "[integration]") {
    haze::test::setup_integration_compute_config();

    void *d_a = nullptr, *d_b = nullptr, *d_dst = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst, kBytes) == HAZE_SUCCESS);

    // a[i] = 10, b[i] = 3 → expected difference = 7
    std::vector<uint64_t> a(kRingDim, 10), b(kRingDim, 3);
    REQUIRE(hazeMemcpy(d_a, a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_b, b.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    REQUIRE(hazeSub(d_dst, d_a, d_b, kModIdx, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> result(kRingDim, 0);
    REQUIRE(hazeReplay() == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(result.data(), d_dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    for (uint64_t i = 0; i < kRingDim; ++i) {
        REQUIRE(result[i] == 7);
    }

    REQUIRE(hazeFree(d_a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_b) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_dst) == HAZE_SUCCESS);
}

TEST_CASE("hazeMulScalar: pointwise scalar product retrieved after D2H", "[integration]") {
    haze::test::setup_integration_compute_config();

    void *d_a = nullptr, *d_dst = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst, kBytes) == HAZE_SUCCESS);

    // a[i] = 2, scalar = 4 → expected product = 8
    std::vector<uint64_t> a(kRingDim, 2);
    REQUIRE(hazeMemcpy(d_a, a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    REQUIRE(hazeMulScalar(d_dst, d_a, 4, kModIdx, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> result(kRingDim, 0);
    REQUIRE(hazeReplay() == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(result.data(), d_dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    for (uint64_t i = 0; i < kRingDim; ++i) {
        REQUIRE(result[i] == 8);
    }

    REQUIRE(hazeFree(d_a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_dst) == HAZE_SUCCESS);
}

TEST_CASE("hazeAddScalar: pointwise scalar addition retrieved after D2H", "[integration]") {
    haze::test::setup_integration_compute_config();

    void *d_a = nullptr, *d_dst = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst, kBytes) == HAZE_SUCCESS);

    // a[i] = 5, scalar = 3 → expected = 8
    std::vector<uint64_t> a(kRingDim, 5);
    REQUIRE(hazeMemcpy(d_a, a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    REQUIRE(hazeAddScalar(d_dst, d_a, 3, kModIdx, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> result(kRingDim, 0);
    REQUIRE(hazeReplay() == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(result.data(), d_dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    for (uint64_t i = 0; i < kRingDim; ++i) {
        REQUIRE(result[i] == 8);
    }

    REQUIRE(hazeFree(d_a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_dst) == HAZE_SUCCESS);
}

// ---------------------------------------------------------------------------
// NTT / INTT round-trip
// ---------------------------------------------------------------------------

TEST_CASE("NTT round-trip: INTT(NTT(x)) == x", "[integration]") {
    haze::test::setup_integration_compute_config();

    void *d_src = nullptr, *d_ntt = nullptr, *d_intt = nullptr;
    REQUIRE(hazeMalloc(&d_src, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_ntt, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_intt, kBytes) == HAZE_SUCCESS);

    // Constant polynomial; constant polys are eigenvectors of NTT-based ops
    std::vector<uint64_t> src(kRingDim, 7);
    REQUIRE(hazeMemcpy(d_src, src.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    REQUIRE(hazeNTT(d_ntt, d_src, kModIdx, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeINTT(d_intt, d_ntt, kModIdx, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> result(kRingDim, 0);
    REQUIRE(hazeReplay() == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(result.data(), d_intt, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    for (uint64_t i = 0; i < kRingDim; ++i) {
        REQUIRE(result[i] == 7);
    }

    REQUIRE(hazeFree(d_src) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_ntt) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_intt) == HAZE_SUCCESS);
}

// ---------------------------------------------------------------------------
// In-place operations
// ---------------------------------------------------------------------------

TEST_CASE("hazeAdd in-place (dst == src1) produces correct result", "[integration]") {
    haze::test::setup_integration_compute_config();

    void *d_a = nullptr, *d_b = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_b, kBytes) == HAZE_SUCCESS);

    std::vector<uint64_t> a(kRingDim, 4), b(kRingDim, 5);
    REQUIRE(hazeMemcpy(d_a, a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_b, b.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    // dst == src1 (in-place update of d_a)
    REQUIRE(hazeAdd(d_a, d_a, d_b, kModIdx, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> result(kRingDim, 0);
    REQUIRE(hazeReplay() == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(result.data(), d_a, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    for (uint64_t i = 0; i < kRingDim; ++i) {
        REQUIRE(result[i] == 9);
    }

    REQUIRE(hazeFree(d_a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_b) == HAZE_SUCCESS);
}

TEST_CASE("hazeAdd in-place (dst == src2) produces correct result", "[integration]") {
    haze::test::setup_integration_compute_config();

    void *d_a = nullptr, *d_b = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_b, kBytes) == HAZE_SUCCESS);

    std::vector<uint64_t> a(kRingDim, 6), b(kRingDim, 7);
    REQUIRE(hazeMemcpy(d_a, a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_b, b.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    // dst == src2 (in-place update of d_b)
    REQUIRE(hazeAdd(d_b, d_a, d_b, kModIdx, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> result(kRingDim, 0);
    REQUIRE(hazeReplay() == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(result.data(), d_b, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    for (uint64_t i = 0; i < kRingDim; ++i) {
        REQUIRE(result[i] == 13);
    }

    REQUIRE(hazeFree(d_a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_b) == HAZE_SUCCESS);
}

TEST_CASE("hazeAdd in-place squaring-style (dst == src1 == src2)", "[integration]") {
    haze::test::setup_integration_compute_config();

    void *d_a = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);

    std::vector<uint64_t> a(kRingDim, 3);
    REQUIRE(hazeMemcpy(d_a, a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    // Both sources and destination alias the same allocation.
    REQUIRE(hazeAdd(d_a, d_a, d_a, kModIdx, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> result(kRingDim, 0);
    REQUIRE(hazeReplay() == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(result.data(), d_a, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    for (uint64_t i = 0; i < kRingDim; ++i) {
        REQUIRE(result[i] == 6);
    }

    REQUIRE(hazeFree(d_a) == HAZE_SUCCESS);
}

// ---------------------------------------------------------------------------
// Multi-operation chains
// ---------------------------------------------------------------------------

TEST_CASE("multi-operation chain: add then mulscalar in one recording", "[integration]") {
    haze::test::setup_integration_compute_config();

    void *d_a = nullptr, *d_b = nullptr, *d_t = nullptr, *d_dst = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_t, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst, kBytes) == HAZE_SUCCESS);

    // a=2, b=3 → t = a+b = 5 → dst = t*2 = 10
    std::vector<uint64_t> a(kRingDim, 2), b(kRingDim, 3);
    REQUIRE(hazeMemcpy(d_a, a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_b, b.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    REQUIRE(hazeAdd(d_t, d_a, d_b, kModIdx, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeMulScalar(d_dst, d_t, 2, kModIdx, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> result(kRingDim, 0);
    REQUIRE(hazeReplay() == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(result.data(), d_dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    for (uint64_t i = 0; i < kRingDim; ++i) {
        REQUIRE(result[i] == 10);
    }

    REQUIRE(hazeFree(d_a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_b) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_t) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_dst) == HAZE_SUCCESS);
}

// ---------------------------------------------------------------------------
// Multiple materializations
// ---------------------------------------------------------------------------

TEST_CASE("multiple materializations: two independent D2H cycles", "[integration]") {
    haze::test::setup_integration_compute_config();

    void *d_a = nullptr, *d_b = nullptr, *d_dst1 = nullptr, *d_dst2 = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst1, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst2, kBytes) == HAZE_SUCCESS);

    // First batch: a=1, b=2 → sum=3
    std::vector<uint64_t> a(kRingDim, 1), b(kRingDim, 2);
    REQUIRE(hazeMemcpy(d_a, a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_b, b.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(d_dst1, d_a, d_b, kModIdx, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> r1(kRingDim, 0);
    REQUIRE(hazeReplay() == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(r1.data(), d_dst1, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    for (uint64_t i = 0; i < kRingDim; ++i) {
        REQUIRE(r1[i] == 3);
    }

    // Second batch (after first materialization): a=5, b=6 → sum=11
    std::fill(a.begin(), a.end(), 5);
    std::fill(b.begin(), b.end(), 6);
    REQUIRE(hazeMemcpy(d_a, a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_b, b.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(d_dst2, d_a, d_b, kModIdx, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> r2(kRingDim, 0);
    REQUIRE(hazeReplay() == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(r2.data(), d_dst2, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    for (uint64_t i = 0; i < kRingDim; ++i) {
        REQUIRE(r2[i] == 11);
    }

    REQUIRE(hazeFree(d_a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_b) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_dst1) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_dst2) == HAZE_SUCCESS);
}

// ---------------------------------------------------------------------------
// hazeDeviceSynchronize is a no-op (does not trigger materialization)
// ---------------------------------------------------------------------------

TEST_CASE("hazeDeviceSynchronize does not trigger materialization", "[integration]") {
    haze::test::setup_integration_compute_config();

    void *d_a = nullptr, *d_b = nullptr, *d_dst = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst, kBytes) == HAZE_SUCCESS);

    std::vector<uint64_t> a(kRingDim, 3), b(kRingDim, 3);
    REQUIRE(hazeMemcpy(d_a, a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_b, b.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    REQUIRE(hazeAdd(d_dst, d_a, d_b, kModIdx, nullptr) == HAZE_SUCCESS);
    // Sync between compute and D2H must not flush the recording
    REQUIRE(hazeDeviceSynchronize() == HAZE_SUCCESS);

    std::vector<uint64_t> result(kRingDim, 0);
    REQUIRE(hazeReplay() == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(result.data(), d_dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    // If sync had flushed state, result would be stale/zero. Correct: 6.
    for (uint64_t i = 0; i < kRingDim; ++i) {
        REQUIRE(result[i] == 6);
    }

    REQUIRE(hazeFree(d_a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_b) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_dst) == HAZE_SUCCESS);
}

// ---------------------------------------------------------------------------
// Error cases
// ---------------------------------------------------------------------------

TEST_CASE("hazeAdd with unknown source address returns error", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(kModIdx, kModulus) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);

    void *d_dst = nullptr;
    REQUIRE(hazeMalloc(&d_dst, kBytes) == HAZE_SUCCESS);

    // src pointers that were never hazeMemcpy'd (no shadow data)
    void *fake1 = reinterpret_cast<void *>(uintptr_t{0x4000000000ULL} + 0x8000000ULL);
    void *fake2 = reinterpret_cast<void *>(uintptr_t{0x4000000000ULL} + 0x9000000ULL);
    REQUIRE(hazeAdd(d_dst, fake1, fake2, kModIdx, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();

    REQUIRE(hazeFree(d_dst) == HAZE_SUCCESS);
}

TEST_CASE("hazeAdd with invalid modulus index returns error", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(kModIdx, kModulus) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);

    void *d_a = nullptr, *d_b = nullptr, *d_dst = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst, kBytes) == HAZE_SUCCESS);

    std::vector<uint64_t> a(kRingDim, 1), b(kRingDim, 2);
    REQUIRE(hazeMemcpy(d_a, a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_b, b.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    // Negative index maps to zero modulus, which is rejected.
    REQUIRE(hazeAdd(d_dst, d_a, d_b, -1, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();

    // Index past the end of the moduli table (also zero modulus).
    REQUIRE(hazeAdd(d_dst, d_a, d_b, 63, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();

    REQUIRE(hazeFree(d_a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_b) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_dst) == HAZE_SUCCESS);
}

// ---------------------------------------------------------------------------
// H2D / memset polymap-invalidation regression tests
// ---------------------------------------------------------------------------
// H2D and memset must drop any polymap binding for the target address.
// Without invalidation, a sequence of (H2D → compute → H2D → compute) within
// one recording would replay the FIRST H2D's data on the second compute,
// because lookup_or_create finds the still-bound polynomial from the first
// pass and skips the fresh shadow data. Same shape applies to memset.

TEST_CASE("H2D after compute invalidates the polymap binding", "[integration]") {
    haze::test::setup_integration_compute_config();

    void *d_a = nullptr, *d_b = nullptr, *d_dst1 = nullptr, *d_dst2 = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst1, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst2, kBytes) == HAZE_SUCCESS);

    // First pass: a=2, b=3 → dst1 = a + b = 5
    std::vector<uint64_t> a1(kRingDim, 2), b1(kRingDim, 3);
    REQUIRE(hazeMemcpy(d_a, a1.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_b, b1.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(d_dst1, d_a, d_b, kModIdx, nullptr) == HAZE_SUCCESS);

    // Overwrite a's shadow before flushing — recording is still active.
    // dst2 must reflect a=20, b=3 (sum=23), not the stale a=2.
    std::vector<uint64_t> a2(kRingDim, 20);
    REQUIRE(hazeMemcpy(d_a, a2.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(d_dst2, d_a, d_b, kModIdx, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> r2(kRingDim, 0);
    REQUIRE(hazeReplay() == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(r2.data(), d_dst2, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    for (uint64_t i = 0; i < kRingDim; ++i) {
        REQUIRE(r2[i] == 23);
    }

    REQUIRE(hazeFree(d_a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_b) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_dst1) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_dst2) == HAZE_SUCCESS);
}

TEST_CASE("memset after compute invalidates the polymap binding", "[integration]") {
    haze::test::setup_integration_compute_config();

    void *d_a = nullptr, *d_b = nullptr, *d_dst1 = nullptr, *d_dst2 = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst1, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst2, kBytes) == HAZE_SUCCESS);

    // First pass: a=4, b=5 → dst1 = 9.
    std::vector<uint64_t> a1(kRingDim, 4), b1(kRingDim, 5);
    REQUIRE(hazeMemcpy(d_a, a1.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_b, b1.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(d_dst1, d_a, d_b, kModIdx, nullptr) == HAZE_SUCCESS);

    // memset zeros the shadow; second add must read all zeros for d_a.
    REQUIRE(hazeMemset(d_a, 0, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(d_dst2, d_a, d_b, kModIdx, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> r2(kRingDim, 0);
    REQUIRE(hazeReplay() == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(r2.data(), d_dst2, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    for (uint64_t i = 0; i < kRingDim; ++i) {
        REQUIRE(r2[i] == 5);
    }

    REQUIRE(hazeFree(d_a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_b) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_dst1) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_dst2) == HAZE_SUCCESS);
}
