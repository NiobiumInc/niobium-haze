// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
#include "integration_helpers.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <utility>
#include <vector>

static constexpr uint64_t kRingDim = 4096;
static constexpr size_t kBytes = kRingDim * sizeof(uint64_t);
static constexpr uint64_t kModulus = 576460752303415297ULL;
static constexpr int kModIdx = 0;

TEST_CASE("hazeAdd: pointwise sum retrieved after D2H", "[integration]") {
    haze::test::setup_integration_compute_config();

    void *d_a = nullptr;
    void *d_b = nullptr;
    void *d_dst = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst, kBytes) == HAZE_SUCCESS);

    // Constant polynomials: a[i] = 1, b[i] = 2 → expected sum = 3
    std::vector<uint64_t> a(kRingDim, 1);
    std::vector<uint64_t> b(kRingDim, 2);
    REQUIRE(hazeMemcpy(d_a, a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_b, b.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    REQUIRE(hazeAdd(d_dst, d_a, d_b, kModIdx, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> result(kRingDim, 0);
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

    void *d_a = nullptr;
    void *d_b = nullptr;
    void *d_dst = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst, kBytes) == HAZE_SUCCESS);

    // a[i] = 10, b[i] = 3 → expected difference = 7
    std::vector<uint64_t> a(kRingDim, 10);
    std::vector<uint64_t> b(kRingDim, 3);
    REQUIRE(hazeMemcpy(d_a, a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_b, b.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    REQUIRE(hazeSub(d_dst, d_a, d_b, kModIdx, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> result(kRingDim, 0);
    REQUIRE(hazeMemcpy(result.data(), d_dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    for (uint64_t i = 0; i < kRingDim; ++i) {
        REQUIRE(result[i] == 7);
    }

    REQUIRE(hazeFree(d_a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_b) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_dst) == HAZE_SUCCESS);
}

TEST_CASE("hazeMul: pointwise product retrieved after D2H", "[integration]") {
    haze::test::setup_integration_compute_config();

    void *d_a = nullptr, *d_b = nullptr, *d_dst = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst, kBytes) == HAZE_SUCCESS);

    // Constant polynomials: a[i] = 3, b[i] = 5 -> expected product = 15
    std::vector<uint64_t> a(kRingDim, 3), b(kRingDim, 5);
    REQUIRE(hazeMemcpy(d_a, a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_b, b.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    REQUIRE(hazeMul(d_dst, d_a, d_b, kModIdx, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> result(kRingDim, 0);
    REQUIRE(hazeMemcpy(result.data(), d_dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    for (uint64_t i = 0; i < kRingDim; ++i) {
        REQUIRE(result[i] == 15);
    }

    REQUIRE(hazeFree(d_a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_b) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_dst) == HAZE_SUCCESS);
}

TEST_CASE("hazeMulScalar: pointwise scalar product retrieved after D2H", "[integration]") {
    haze::test::setup_integration_compute_config();

    void *d_a = nullptr;
    void *d_dst = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst, kBytes) == HAZE_SUCCESS);

    // a[i] = 2, scalar = 4 → expected product = 8
    std::vector<uint64_t> a(kRingDim, 2);
    REQUIRE(hazeMemcpy(d_a, a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    REQUIRE(hazeMulScalar(d_dst, d_a, 4, kModIdx, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> result(kRingDim, 0);
    REQUIRE(hazeMemcpy(result.data(), d_dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    for (uint64_t i = 0; i < kRingDim; ++i) {
        REQUIRE(result[i] == 8);
    }

    REQUIRE(hazeFree(d_a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_dst) == HAZE_SUCCESS);
}

TEST_CASE("hazeAddScalar: pointwise scalar addition retrieved after D2H", "[integration]") {
    haze::test::setup_integration_compute_config();

    void *d_a = nullptr;
    void *d_dst = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst, kBytes) == HAZE_SUCCESS);

    // a[i] = 5, scalar = 3 → expected = 8
    std::vector<uint64_t> a(kRingDim, 5);
    REQUIRE(hazeMemcpy(d_a, a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    REQUIRE(hazeAddScalar(d_dst, d_a, 3, kModIdx, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> result(kRingDim, 0);
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
    const uint64_t q = haze::test::setup_integration_compute_config();

    void *d_src = nullptr, *d_ntt = nullptr, *d_intt = nullptr;
    REQUIRE(hazeMalloc(&d_src, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_ntt, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_intt, kBytes) == HAZE_SUCCESS);

    // Non-constant deterministic input. Constants are eigenvectors of the
    // negacyclic NTT, so a constant-input round-trip would still pass under
    // swapped forward/inverse twiddles or a wrong ring-dim scaling — this
    // input forces every butterfly stage to actually run.
    std::vector<uint64_t> src(kRingDim);
    for (uint64_t i = 0; i < kRingDim; ++i) {
        src[i] = (i * 1234567ULL + 7ULL) % q;
    }
    REQUIRE(hazeMemcpy(d_src, src.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    REQUIRE(hazeNTT(d_ntt, d_src, kModIdx, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeINTT(d_intt, d_ntt, kModIdx, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> result(kRingDim, 0);
    REQUIRE(hazeMemcpy(result.data(), d_intt, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    for (uint64_t i = 0; i < kRingDim; ++i) {
        REQUIRE(result[i] == src[i]);
    }

    REQUIRE(hazeFree(d_src) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_ntt) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_intt) == HAZE_SUCCESS);
}

TEST_CASE("NTT round-trip on constant input (smoke test)", "[integration]") {
    haze::test::setup_integration_compute_config();

    void *d_src = nullptr;
    void *d_ntt = nullptr;
    void *d_intt = nullptr;
    REQUIRE(hazeMalloc(&d_src, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_ntt, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_intt, kBytes) == HAZE_SUCCESS);

    // Constant polys are eigenvectors of NTT and survive even pathological
    // twiddle / scaling bugs. Kept as a fast smoke check; the non-constant
    // case above is the real correctness assertion.
    std::vector<uint64_t> src(kRingDim, 7);
    REQUIRE(hazeMemcpy(d_src, src.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    REQUIRE(hazeNTT(d_ntt, d_src, kModIdx, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeINTT(d_intt, d_ntt, kModIdx, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> result(kRingDim, 0);
    REQUIRE(hazeMemcpy(result.data(), d_intt, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    for (uint64_t i = 0; i < kRingDim; ++i) {
        REQUIRE(result[i] == 7);
    }

    REQUIRE(hazeFree(d_src) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_ntt) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_intt) == HAZE_SUCCESS);
}

// ---------------------------------------------------------------------------
// Polynomial product via NTT -> hazeMul -> INTT
// ---------------------------------------------------------------------------
// hazeMul is component-wise on the residue. To use it as a ring multiply
// a(X)*b(X) mod (X^N+1, q), inputs are pushed into evaluation form via NTT
// first; the pointwise product there equals negacyclic convolution in
// coefficient form after INTT. The host oracle is
// haze::test::negacyclic_conv_ref.

namespace {

// Drives one ring-multiply test case end-to-end. Records H2D(a), NTT(a),
// H2D(b), NTT(b), Mul, INTT into one recording, then D2H to flush. The
// returned coefficient-form result is compared against the host oracle by
// the caller.
inline std::vector<uint64_t> run_ntt_mul_intt(const std::vector<uint64_t> &a,
                                              const std::vector<uint64_t> &b) {
    REQUIRE(a.size() == kRingDim);
    REQUIRE(b.size() == kRingDim);

    void *d_a = nullptr, *d_b = nullptr;
    void *d_a_eval = nullptr, *d_b_eval = nullptr;
    void *d_c_eval = nullptr, *d_c = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_a_eval, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_b_eval, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_c_eval, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_c, kBytes) == HAZE_SUCCESS);

    REQUIRE(hazeMemcpy(d_a, a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_b, b.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeNTT(d_a_eval, d_a, kModIdx, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeNTT(d_b_eval, d_b, kModIdx, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeMul(d_c_eval, d_a_eval, d_b_eval, kModIdx, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeINTT(d_c, d_c_eval, kModIdx, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> c(kRingDim, 0);
    REQUIRE(hazeMemcpy(c.data(), d_c, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    REQUIRE(hazeFree(d_a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_b) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_a_eval) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_b_eval) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_c_eval) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_c) == HAZE_SUCCESS);

    return c;
}

} // namespace

TEST_CASE("polynomial product: monomial * monomial via NTT.Mul.INTT", "[integration]") {
    const uint64_t q = haze::test::setup_integration_compute_config();

    // (i, j) pairs spanning the X^N = -1 wraparound boundary. Includes
    // an asymmetric pair at i+j = N+1 to catch off-by-one errors in the
    // wrap threshold that pure same-exponent squares miss.
    const std::pair<uint64_t, uint64_t> pairs[] = {{0, 0},
                                                   {kRingDim / 2, kRingDim / 2},
                                                   {kRingDim - 1, kRingDim - 1},
                                                   {kRingDim / 2 + 1, kRingDim / 2}};

    for (const auto &[i, j] : pairs) {
        std::vector<uint64_t> a(kRingDim, 0), b(kRingDim, 0);
        a[i] = 1;
        b[j] = 1;

        const auto c = run_ntt_mul_intt(a, b);
        const auto expected = haze::test::negacyclic_conv_ref(a, b, q);

        for (uint64_t k = 0; k < kRingDim; ++k) {
            REQUIRE(c[k] == expected[k]);
        }
        // Hand-check the wrap rule alongside the oracle.
        const uint64_t k = (i + j) % kRingDim;
        const bool wrap = (i + j) >= kRingDim;
        REQUIRE(c[k] == (wrap ? (q - 1) : 1));
    }
}

TEST_CASE("polynomial product: linear * constant via NTT.Mul.INTT", "[integration]") {
    const uint64_t q = haze::test::setup_integration_compute_config();

    // a(X) = 11 + 13*X, b(X) = 17.
    std::vector<uint64_t> a(kRingDim, 0), b(kRingDim, 0);
    a[0] = 11;
    a[1] = 13;
    b[0] = 17;

    const auto c = run_ntt_mul_intt(a, b);
    const auto expected = haze::test::negacyclic_conv_ref(a, b, q);

    for (uint64_t k = 0; k < kRingDim; ++k) {
        REQUIRE(c[k] == expected[k]);
    }
    REQUIRE(c[0] == 11ULL * 17ULL);
    REQUIRE(c[1] == 13ULL * 17ULL);
    for (uint64_t k = 2; k < kRingDim; ++k) {
        REQUIRE(c[k] == 0);
    }
}

TEST_CASE("polynomial product: random small coefficients via NTT.Mul.INTT", "[integration]") {
    const uint64_t q = haze::test::setup_integration_compute_config();

    // Deterministic small coefficients (< 1024). Catches obvious convolution
    // bugs without 128-bit overflow concerns in the oracle.
    std::vector<uint64_t> a(kRingDim), b(kRingDim);
    for (uint64_t k = 0; k < kRingDim; ++k) {
        a[k] = ((k * 31ULL + 7ULL) ^ (k << 3)) & 0x3FFULL;
        b[k] = ((k * 17ULL + 5ULL) ^ (k << 2)) & 0x3FFULL;
    }

    const auto c = run_ntt_mul_intt(a, b);
    const auto expected = haze::test::negacyclic_conv_ref(a, b, q);

    for (uint64_t k = 0; k < kRingDim; ++k) {
        REQUIRE(c[k] == expected[k]);
    }
}

TEST_CASE("polynomial product: random full-range coefficients via NTT.Mul.INTT", "[integration]") {
    const uint64_t q = haze::test::setup_integration_compute_config();

    // Full-range coefficients uniform over [0, q). Exercises modular
    // reduction in the convolution. Pattern mirrors
    // test_basis_convert.cpp:make_residue.
    std::vector<uint64_t> a(kRingDim), b(kRingDim);
    for (uint64_t k = 0; k < kRingDim; ++k) {
        const __uint128_t va =
            (static_cast<__uint128_t>(0x9E3779B97F4A7C15ULL) * (k + 1) * 7) + (k & 0xFFFF) + 13;
        const __uint128_t vb =
            (static_cast<__uint128_t>(0xBB67AE8584CAA73BULL) * (k + 1) * 11) + (k & 0xFFFF) + 17;
        a[k] = static_cast<uint64_t>(va % q);
        b[k] = static_cast<uint64_t>(vb % q);
    }

    const auto c = run_ntt_mul_intt(a, b);
    const auto expected = haze::test::negacyclic_conv_ref(a, b, q);

    for (uint64_t k = 0; k < kRingDim; ++k) {
        REQUIRE(c[k] == expected[k]);
    }
}

// ---------------------------------------------------------------------------
// In-place operations
// ---------------------------------------------------------------------------

TEST_CASE("hazeAdd in-place (dst == src1) produces correct result", "[integration]") {
    haze::test::setup_integration_compute_config();

    void *d_a = nullptr;
    void *d_b = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_b, kBytes) == HAZE_SUCCESS);

    std::vector<uint64_t> a(kRingDim, 4);
    std::vector<uint64_t> b(kRingDim, 5);
    REQUIRE(hazeMemcpy(d_a, a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_b, b.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    // dst == src1 (in-place update of d_a)
    REQUIRE(hazeAdd(d_a, d_a, d_b, kModIdx, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> result(kRingDim, 0);
    REQUIRE(hazeMemcpy(result.data(), d_a, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    for (uint64_t i = 0; i < kRingDim; ++i) {
        REQUIRE(result[i] == 9);
    }

    REQUIRE(hazeFree(d_a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_b) == HAZE_SUCCESS);
}

TEST_CASE("hazeAdd in-place (dst == src2) produces correct result", "[integration]") {
    haze::test::setup_integration_compute_config();

    void *d_a = nullptr;
    void *d_b = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_b, kBytes) == HAZE_SUCCESS);

    std::vector<uint64_t> a(kRingDim, 6);
    std::vector<uint64_t> b(kRingDim, 7);
    REQUIRE(hazeMemcpy(d_a, a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_b, b.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    // dst == src2 (in-place update of d_b)
    REQUIRE(hazeAdd(d_b, d_a, d_b, kModIdx, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> result(kRingDim, 0);
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
    REQUIRE(hazeMemcpy(result.data(), d_a, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    for (uint64_t i = 0; i < kRingDim; ++i) {
        REQUIRE(result[i] == 6);
    }

    REQUIRE(hazeFree(d_a) == HAZE_SUCCESS);
}

TEST_CASE("hazeMul in-place (dst == src1) produces correct result", "[integration]") {
    haze::test::setup_integration_compute_config();

    void *d_a = nullptr, *d_b = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_b, kBytes) == HAZE_SUCCESS);

    std::vector<uint64_t> a(kRingDim, 4), b(kRingDim, 5);
    REQUIRE(hazeMemcpy(d_a, a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_b, b.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    // dst == src1 (in-place update of d_a)
    REQUIRE(hazeMul(d_a, d_a, d_b, kModIdx, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> result(kRingDim, 0);
    REQUIRE(hazeMemcpy(result.data(), d_a, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    for (uint64_t i = 0; i < kRingDim; ++i) {
        REQUIRE(result[i] == 20);
    }

    REQUIRE(hazeFree(d_a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_b) == HAZE_SUCCESS);
}

TEST_CASE("hazeMul in-place (dst == src2) produces correct result", "[integration]") {
    haze::test::setup_integration_compute_config();

    void *d_a = nullptr, *d_b = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_b, kBytes) == HAZE_SUCCESS);

    std::vector<uint64_t> a(kRingDim, 6), b(kRingDim, 7);
    REQUIRE(hazeMemcpy(d_a, a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_b, b.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    // dst == src2 (in-place update of d_b)
    REQUIRE(hazeMul(d_b, d_a, d_b, kModIdx, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> result(kRingDim, 0);
    REQUIRE(hazeMemcpy(result.data(), d_b, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    for (uint64_t i = 0; i < kRingDim; ++i) {
        REQUIRE(result[i] == 42);
    }

    REQUIRE(hazeFree(d_a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_b) == HAZE_SUCCESS);
}

TEST_CASE("hazeMul in-place squaring-style (dst == src1 == src2)", "[integration]") {
    haze::test::setup_integration_compute_config();

    void *d_a = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);

    std::vector<uint64_t> a(kRingDim, 4);
    REQUIRE(hazeMemcpy(d_a, a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    // Both sources and destination alias the same allocation.
    REQUIRE(hazeMul(d_a, d_a, d_a, kModIdx, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> result(kRingDim, 0);
    REQUIRE(hazeMemcpy(result.data(), d_a, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    for (uint64_t i = 0; i < kRingDim; ++i) {
        REQUIRE(result[i] == 16);
    }

    REQUIRE(hazeFree(d_a) == HAZE_SUCCESS);
}

// ---------------------------------------------------------------------------
// Multi-operation chains
// ---------------------------------------------------------------------------

TEST_CASE("multi-operation chain: add then mulscalar in one recording", "[integration]") {
    haze::test::setup_integration_compute_config();

    void *d_a = nullptr;
    void *d_b = nullptr;
    void *d_t = nullptr;
    void *d_dst = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_t, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst, kBytes) == HAZE_SUCCESS);

    // a=2, b=3 → t = a+b = 5 → dst = t*2 = 10
    std::vector<uint64_t> a(kRingDim, 2);
    std::vector<uint64_t> b(kRingDim, 3);
    REQUIRE(hazeMemcpy(d_a, a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_b, b.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    REQUIRE(hazeAdd(d_t, d_a, d_b, kModIdx, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeMulScalar(d_dst, d_t, 2, kModIdx, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> result(kRingDim, 0);
    REQUIRE(hazeMemcpy(result.data(), d_dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    for (uint64_t i = 0; i < kRingDim; ++i) {
        REQUIRE(result[i] == 10);
    }

    REQUIRE(hazeFree(d_a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_b) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_t) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_dst) == HAZE_SUCCESS);
}

TEST_CASE("multi-operation chain: mul then add in one recording", "[integration]") {
    haze::test::setup_integration_compute_config();

    void *d_a = nullptr, *d_b = nullptr, *d_t = nullptr, *d_dst = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_t, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst, kBytes) == HAZE_SUCCESS);

    // a=2, b=3 -> t = a*b = 6 -> dst = t + b = 9
    std::vector<uint64_t> a(kRingDim, 2), b(kRingDim, 3);
    REQUIRE(hazeMemcpy(d_a, a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_b, b.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    REQUIRE(hazeMul(d_t, d_a, d_b, kModIdx, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(d_dst, d_t, d_b, kModIdx, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> result(kRingDim, 0);
    REQUIRE(hazeMemcpy(result.data(), d_dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    for (uint64_t i = 0; i < kRingDim; ++i) {
        REQUIRE(result[i] == 9);
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

    void *d_a = nullptr;
    void *d_b = nullptr;
    void *d_dst1 = nullptr;
    void *d_dst2 = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst1, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst2, kBytes) == HAZE_SUCCESS);

    // First batch: a=1, b=2 → sum=3
    std::vector<uint64_t> a(kRingDim, 1);
    std::vector<uint64_t> b(kRingDim, 2);
    REQUIRE(hazeMemcpy(d_a, a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_b, b.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(d_dst1, d_a, d_b, kModIdx, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> r1(kRingDim, 0);
    REQUIRE(hazeMemcpy(r1.data(), d_dst1, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    for (uint64_t i = 0; i < kRingDim; ++i) {
        REQUIRE(r1[i] == 3);
    }

    // Second batch (after first materialization): a=5, b=6 → sum=11
    std::ranges::fill(a, 5);
    std::ranges::fill(b, 6);
    REQUIRE(hazeMemcpy(d_a, a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_b, b.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(d_dst2, d_a, d_b, kModIdx, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> r2(kRingDim, 0);
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

    void *d_a = nullptr;
    void *d_b = nullptr;
    void *d_dst = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst, kBytes) == HAZE_SUCCESS);

    std::vector<uint64_t> a(kRingDim, 3);
    std::vector<uint64_t> b(kRingDim, 3);
    REQUIRE(hazeMemcpy(d_a, a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_b, b.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    REQUIRE(hazeAdd(d_dst, d_a, d_b, kModIdx, nullptr) == HAZE_SUCCESS);
    // Sync between compute and D2H must not flush the recording
    REQUIRE(hazeDeviceSynchronize() == HAZE_SUCCESS);

    std::vector<uint64_t> result(kRingDim, 0);
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

    // src pointers that were never hazeMemcpy'd (no shadow data). The
    // ints-to-pointers are deliberate: the test asserts the allocator
    // classifies these synthetic device addresses as unmapped, which
    // requires the addresses themselves, not real allocations.
    // NOLINTBEGIN(performance-no-int-to-ptr)
    void *fake1 = reinterpret_cast<void *>(uintptr_t{0x4000000000ULL} + 0x8000000ULL);
    void *fake2 = reinterpret_cast<void *>(uintptr_t{0x4000000000ULL} + 0x9000000ULL);
    // NOLINTEND(performance-no-int-to-ptr)
    REQUIRE(hazeAdd(d_dst, fake1, fake2, kModIdx, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();

    REQUIRE(hazeFree(d_dst) == HAZE_SUCCESS);
}

TEST_CASE("hazeAdd with invalid modulus index returns error", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(kModIdx, kModulus) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);

    void *d_a = nullptr;
    void *d_b = nullptr;
    void *d_dst = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst, kBytes) == HAZE_SUCCESS);

    std::vector<uint64_t> a(kRingDim, 1);
    std::vector<uint64_t> b(kRingDim, 2);
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

TEST_CASE("hazeMul with unknown source address returns error", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(kModIdx, kModulus) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);

    void *d_dst = nullptr;
    REQUIRE(hazeMalloc(&d_dst, kBytes) == HAZE_SUCCESS);

    // src pointers that were never hazeMemcpy'd (no shadow data)
    void *fake1 = reinterpret_cast<void *>(uintptr_t{0x4000000000ULL} + 0x8000000ULL);
    void *fake2 = reinterpret_cast<void *>(uintptr_t{0x4000000000ULL} + 0x9000000ULL);
    REQUIRE(hazeMul(d_dst, fake1, fake2, kModIdx, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();

    REQUIRE(hazeFree(d_dst) == HAZE_SUCCESS);
}

TEST_CASE("hazeMul with invalid modulus index returns error", "[unit]") {
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
    REQUIRE(hazeMul(d_dst, d_a, d_b, -1, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();

    // Index past the end of the moduli table (also zero modulus).
    REQUIRE(hazeMul(d_dst, d_a, d_b, 63, nullptr) == HAZE_ERROR_INVALID_VALUE);
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

    void *d_a = nullptr;
    void *d_b = nullptr;
    void *d_dst1 = nullptr;
    void *d_dst2 = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst1, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst2, kBytes) == HAZE_SUCCESS);

    // First pass: a=2, b=3 → dst1 = a + b = 5
    std::vector<uint64_t> a1(kRingDim, 2);
    std::vector<uint64_t> b1(kRingDim, 3);
    REQUIRE(hazeMemcpy(d_a, a1.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_b, b1.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(d_dst1, d_a, d_b, kModIdx, nullptr) == HAZE_SUCCESS);

    // Overwrite a's shadow before flushing — recording is still active.
    // dst2 must reflect a=20, b=3 (sum=23), not the stale a=2.
    std::vector<uint64_t> a2(kRingDim, 20);
    REQUIRE(hazeMemcpy(d_a, a2.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(d_dst2, d_a, d_b, kModIdx, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> r2(kRingDim, 0);
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

    void *d_a = nullptr;
    void *d_b = nullptr;
    void *d_dst1 = nullptr;
    void *d_dst2 = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst1, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst2, kBytes) == HAZE_SUCCESS);

    // First pass: a=4, b=5 → dst1 = 9.
    std::vector<uint64_t> a1(kRingDim, 4);
    std::vector<uint64_t> b1(kRingDim, 5);
    REQUIRE(hazeMemcpy(d_a, a1.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_b, b1.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(d_dst1, d_a, d_b, kModIdx, nullptr) == HAZE_SUCCESS);

    // memset zeros the shadow; second add must read all zeros for d_a.
    REQUIRE(hazeMemset(d_a, 0, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(d_dst2, d_a, d_b, kModIdx, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> r2(kRingDim, 0);
    REQUIRE(hazeMemcpy(r2.data(), d_dst2, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    for (uint64_t i = 0; i < kRingDim; ++i) {
        REQUIRE(r2[i] == 5);
    }

    REQUIRE(hazeFree(d_a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_b) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_dst1) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_dst2) == HAZE_SUCCESS);
}
