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
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <vector>

static constexpr uint64_t kRingDim = 4096;
static constexpr size_t kBytes = kRingDim * sizeof(uint64_t);

// Three NTT-friendly primes (q ≡ 1 mod 2N) for N=4096.
static constexpr uint64_t kQ0 = 576460752303415297ULL;
static constexpr uint64_t kQ1 = 576460752303439873ULL;
static constexpr uint64_t kQ2 = 576460752303702017ULL;

// Reset HAZE state up front so test ordering does not leak epoch /
// allocator state between cases under --order rand.
static void configure_three_moduli() {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(0, kQ0) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(1, kQ1) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(2, kQ2) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);
}

// Parameter validation.

TEST_CASE("hazeBasisConvert rejects null params") {
    configure_three_moduli();
    REQUIRE(hazeBasisConvert(nullptr, nullptr, nullptr, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("hazeModDown rejects null params") {
    configure_three_moduli();
    REQUIRE(hazeModDown(nullptr, nullptr, nullptr, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("hazeModUp rejects null params") {
    configure_three_moduli();
    REQUIRE(hazeModUp(nullptr, nullptr, nullptr, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("hazeBasisConvert rejects empty source base") {
    configure_three_moduli();
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);

    const uint64_t src_base[] = {kQ0};
    const uint64_t dst_base[] = {kQ1};
    const void *src_polys[] = {nullptr};
    void *dst_polys[] = {dst};

    hazeBasisConvertParams p{};
    p.src_base = src_base;
    p.src_base_len = 0; // invalid
    p.dst_base = dst_base;
    p.dst_base_len = 1;
    REQUIRE(hazeBasisConvert(dst_polys, src_polys, &p, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();

    REQUIRE(hazeFree(dst) == HAZE_SUCCESS);
}

TEST_CASE("hazeModDown rejects foreign modulus in rescale_base") {
    // rescale_base contains a prime not present in src_base. The HAZE
    // layer must reject this BEFORE opening an EpochSession — otherwise
    // the next D2H would replay a dirty recording and crash.
    configure_three_moduli();
    void *d = nullptr;
    REQUIRE(hazeMalloc(&d, kBytes) == HAZE_SUCCESS);

    const uint64_t src_base[] = {kQ0, kQ1};
    const uint64_t rescale_base[] = {kQ2}; // not a member of src_base
    const void *src_polys[] = {nullptr, nullptr};
    void *dst_polys[] = {d};

    hazeModDownParams p{};
    p.src_base = src_base;
    p.src_base_len = 2;
    p.rescale_base = rescale_base;
    p.rescale_base_len = 1;
    REQUIRE(hazeModDown(dst_polys, src_polys, &p, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();

    // Subsequent compute + D2H must still work (recording state is clean).
    void *a = nullptr;
    void *b = nullptr;
    void *c = nullptr;
    REQUIRE(hazeMalloc(&a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&c, kBytes) == HAZE_SUCCESS);
    std::vector<uint64_t> va(kRingDim, 1);
    std::vector<uint64_t> vb(kRingDim, 2);
    REQUIRE(hazeMemcpy(a, va.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(b, vb.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(c, a, b, 0, nullptr) == HAZE_SUCCESS);
    std::vector<uint64_t> out(kRingDim, 0);
    REQUIRE(hazeMemcpy(out.data(), c, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    REQUIRE(out[0] == 3);

    REQUIRE(hazeFree(a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(b) == HAZE_SUCCESS);
    REQUIRE(hazeFree(c) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d) == HAZE_SUCCESS);
}

TEST_CASE("hazeModDown rejects rescale_base_len >= src_base_len") {
    // Equal-length rescale would leave dst empty (FhetchApi.cpp:1660
    // asserts rescale must be a *proper* subset). Strictly-greater is
    // outright invalid. Both must surface as INVALID_VALUE before any
    // backend call.
    configure_three_moduli();
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);

    const uint64_t src_base[] = {kQ0};
    const uint64_t rescale_base_eq[] = {kQ0};
    const uint64_t rescale_base_gt[] = {kQ0, kQ1};
    const void *src_polys[] = {nullptr};
    void *dst_polys[] = {dst};

    hazeModDownParams p{};
    p.src_base = src_base;
    p.src_base_len = 1;
    p.rescale_base = rescale_base_eq;
    p.rescale_base_len = 1; // equal to src_base_len — would yield empty dst
    REQUIRE(hazeModDown(dst_polys, src_polys, &p, nullptr) == HAZE_ERROR_INVALID_VALUE);

    p.rescale_base = rescale_base_gt;
    p.rescale_base_len = 2; // strictly greater than src_base_len
    REQUIRE(hazeModDown(dst_polys, src_polys, &p, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();

    REQUIRE(hazeFree(dst) == HAZE_SUCCESS);
}

TEST_CASE("hazeModUp rejects mismatched digit_bases_total_len") {
    // digit_bases is flat; sum of digit_base_lens must equal
    // digit_bases_total_len, else slicing reads out of bounds.
    configure_three_moduli();

    void *src = nullptr;
    REQUIRE(hazeMalloc(&src, kBytes) == HAZE_SUCCESS);
    const void *src_polys[] = {src};

    const uint64_t src_base[] = {kQ0};
    const uint64_t digit_bases[] = {kQ0};
    const size_t digit_base_lens[] = {1};
    const uint64_t p_base[] = {kQ2};

    void *dst = nullptr;
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);
    void *dst_polys[] = {dst};

    hazeModUpParams p{};
    p.src_base = src_base;
    p.src_base_len = 1;
    p.digit_bases = digit_bases;
    p.digit_bases_total_len = 7; // does not match sum(digit_base_lens) == 1
    p.digit_base_lens = digit_base_lens;
    p.digit_count = 1;
    p.p_base = p_base;
    p.p_base_len = 1;
    REQUIRE(hazeModUp(dst_polys, src_polys, &p, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();

    REQUIRE(hazeFree(src) == HAZE_SUCCESS);
    REQUIRE(hazeFree(dst) == HAZE_SUCCESS);
}

// End-to-end: basis-convert and retrieve results.
//
// Tests use inputs whose expected outputs are exactly knowable from the
// CRT semantics — no FBC arithmetic is performed in the test itself.
// Two patterns:
//   1. Shared moduli — primes that appear in both src_base and dst_base
//      pass through as same-modulus copies (FhetchApi.cpp:1585-1588).
//   2. Zero inputs — every CRT operation on zero polynomials yields
//      zero polynomials.
// Cross-checking non-trivial FBC values against an OpenFHE reference
// is deferred to integration tests (FIDESlib examples).

TEST_CASE("hazeBasisConvert: shared-modulus copies produce input values") {
    // src_base = {q0, q1}, dst_base = {q0, q1, q2}. Every prime in
    // src_base is also in dst_base, so the first two dst residues are
    // same-modulus copies of the corresponding src residues. The third
    // (q2) is FBC-derived; not asserted on value but D2H must succeed
    // and the materialization path must persist its shadow alongside
    // d0/d1 so subsequent D2Hs in this epoch see fresh data.
    configure_three_moduli();

    void *s0 = nullptr;
    void *s1 = nullptr;
    void *d0 = nullptr;
    void *d1 = nullptr;
    void *d2 = nullptr;
    REQUIRE(hazeMalloc(&s0, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&s1, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d0, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d1, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d2, kBytes) == HAZE_SUCCESS);

    constexpr uint64_t kInput0 = 17;
    constexpr uint64_t kInput1 = 42;
    std::vector<uint64_t> poly_a(kRingDim, kInput0);
    std::vector<uint64_t> poly_b(kRingDim, kInput1);
    REQUIRE(hazeMemcpy(s0, poly_a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(s1, poly_b.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    const uint64_t src_base[] = {kQ0, kQ1};
    const uint64_t dst_base[] = {kQ0, kQ1, kQ2};
    const void *src_polys[] = {s0, s1};
    void *dst_polys[] = {d0, d1, d2};

    hazeBasisConvertParams p{};
    p.src_base = src_base;
    p.src_base_len = 2;
    p.dst_base = dst_base;
    p.dst_base_len = 3;
    REQUIRE(hazeBasisConvert(dst_polys, src_polys, &p, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> out0(kRingDim, 0);
    std::vector<uint64_t> out1(kRingDim, 0);
    std::vector<uint64_t> out2(kRingDim, 0);
    REQUIRE(hazeMemcpy(out0.data(), d0, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(out1.data(), d1, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(out2.data(), d2, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    // Same-modulus copies: dst residue == src residue, every coefficient.
    for (uint64_t i = 0; i < kRingDim; ++i) {
        REQUIRE(out0[i] == kInput0);
        REQUIRE(out1[i] == kInput1);
    }
}

TEST_CASE("hazeBasisConvert: zero input produces zero output") {
    // FBC of zero polynomials is zero on every target modulus, so we
    // can verify the non-trivial dst residues without computing FBC.
    configure_three_moduli();

    void *s0 = nullptr;
    void *s1 = nullptr;
    void *d0 = nullptr;
    void *d1 = nullptr;
    REQUIRE(hazeMalloc(&s0, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&s1, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d0, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d1, kBytes) == HAZE_SUCCESS);

    std::vector<uint64_t> zeros(kRingDim, 0);
    REQUIRE(hazeMemcpy(s0, zeros.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(s1, zeros.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    const uint64_t src_base[] = {kQ0, kQ1};
    const uint64_t dst_base[] = {kQ0, kQ2};
    const void *src_polys[] = {s0, s1};
    void *dst_polys[] = {d0, d1};

    hazeBasisConvertParams p{};
    p.src_base = src_base;
    p.src_base_len = 2;
    p.dst_base = dst_base;
    p.dst_base_len = 2;
    REQUIRE(hazeBasisConvert(dst_polys, src_polys, &p, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> out0(kRingDim, 0xDEADBEEF);
    std::vector<uint64_t> out1(kRingDim, 0xDEADBEEF);
    REQUIRE(hazeMemcpy(out0.data(), d0, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(out1.data(), d1, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    // Catches both (a) a backend that writes garbage and (b) the
    // multi-D2H staleness regression where the second D2H would read
    // uninitialised (post-allocator-reset) shadow rather than the
    // computed zero.
    for (uint64_t i = 0; i < kRingDim; ++i) {
        REQUIRE(out0[i] == 0);
        REQUIRE(out1[i] == 0);
    }
}

TEST_CASE("hazeBasisConvert: src/dst aliasing is safe (in-place 1->1)") {
    // Aliasing src[i] == dst[j] for any (i,j) is documented as safe in
    // core/basis_convert.cpp because all reads complete before any
    // store. Test the simplest case: same-modulus 1->1 convert with
    // dst[0] == src[0]. dst should be unchanged from src.
    configure_three_moduli();

    void *p0 = nullptr;
    REQUIRE(hazeMalloc(&p0, kBytes) == HAZE_SUCCESS);

    std::vector<uint64_t> input(kRingDim, 42);
    REQUIRE(hazeMemcpy(p0, input.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    const uint64_t base[] = {kQ0};
    const void *src_polys[] = {p0};
    void *dst_polys[] = {p0}; // alias

    hazeBasisConvertParams p{};
    p.src_base = base;
    p.src_base_len = 1;
    p.dst_base = base;
    p.dst_base_len = 1;
    REQUIRE(hazeBasisConvert(dst_polys, src_polys, &p, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> out(kRingDim, 0);
    REQUIRE(hazeMemcpy(out.data(), p0, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    REQUIRE(out[0] == 42);

    REQUIRE(hazeFree(p0) == HAZE_SUCCESS);
}

TEST_CASE("hazeModDown: zero input rescales to zero output") {
    // ApproxModDown is rescale_fbc(x, rescale_base): CRT-divide x by
    // P=prod(rescale_base) with rounding. For x identically zero on
    // every residue, every output is also zero — we can assert exact
    // values without doing any FBC arithmetic.
    //
    // Multi-output verification: with the EpochState::flush_for_d2h
    // fix, both d0 and d1 shadow buffers are persisted on the first
    // D2H, so reading both back returns the materialized result, not
    // stale post-allocator-reset memory.
    configure_three_moduli();

    void *s0 = nullptr;
    void *s1 = nullptr;
    void *s2 = nullptr;
    void *d0 = nullptr;
    void *d1 = nullptr;
    REQUIRE(hazeMalloc(&s0, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&s1, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&s2, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d0, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d1, kBytes) == HAZE_SUCCESS);

    std::vector<uint64_t> zeros(kRingDim, 0);
    REQUIRE(hazeMemcpy(s0, zeros.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(s1, zeros.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(s2, zeros.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    const uint64_t src_base[] = {kQ0, kQ1, kQ2};
    const uint64_t rescale_base[] = {kQ2};
    const void *src_polys[] = {s0, s1, s2};
    void *dst_polys[] = {d0, d1};

    hazeModDownParams p{};
    p.src_base = src_base;
    p.src_base_len = 3;
    p.rescale_base = rescale_base;
    p.rescale_base_len = 1;
    REQUIRE(hazeModDown(dst_polys, src_polys, &p, nullptr) == HAZE_SUCCESS);

    // Initialise to non-zero so a no-op D2H would surface as a failure.
    std::vector<uint64_t> out0(kRingDim, 0xDEADBEEF);
    std::vector<uint64_t> out1(kRingDim, 0xDEADBEEF);
    REQUIRE(hazeMemcpy(out0.data(), d0, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(out1.data(), d1, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    for (uint64_t i = 0; i < kRingDim; ++i) {
        REQUIRE(out0[i] == 0);
        REQUIRE(out1[i] == 0);
    }

    REQUIRE(hazeFree(s0) == HAZE_SUCCESS);
    REQUIRE(hazeFree(s1) == HAZE_SUCCESS);
    REQUIRE(hazeFree(s2) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d0) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d1) == HAZE_SUCCESS);
}

TEST_CASE("hazeModUp: zero input produces zero output across both digits") {
    // dig_decomp(x, digit_bases, p_base): for each digit i, take the
    // x|_{digit_bases[i]} subset and FBC-extend it onto src_base ∪
    // p_base. For x identically zero, every digit's every output is
    // zero, so we can assert exact values across all 6 dst slots.
    // This exercises the digit-major flatten arithmetic at d=0 and d=1
    // (catching off-by-one on the per_digit stride) and the multi-D2H
    // materialization fix (every poly_map_ entry persisted on the first
    // flush).
    configure_three_moduli();

    void *s0 = nullptr;
    void *s1 = nullptr;
    REQUIRE(hazeMalloc(&s0, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&s1, kBytes) == HAZE_SUCCESS);

    void *dst_storage[6] = {nullptr};
    for (auto &slot : dst_storage) {
        REQUIRE(hazeMalloc(&slot, kBytes) == HAZE_SUCCESS);
    }

    std::vector<uint64_t> zeros(kRingDim, 0);
    REQUIRE(hazeMemcpy(s0, zeros.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(s1, zeros.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    const uint64_t src_base[] = {kQ0, kQ1};
    const uint64_t digit_bases_flat[] = {kQ0, kQ1};
    const size_t digit_base_lens[] = {1, 1};
    const uint64_t p_base[] = {kQ2};
    const void *src_polys[] = {s0, s1};
    void *dst_polys[] = {dst_storage[0], dst_storage[1], dst_storage[2],
                         dst_storage[3], dst_storage[4], dst_storage[5]};

    hazeModUpParams p{};
    p.src_base = src_base;
    p.src_base_len = 2;
    p.digit_bases = digit_bases_flat;
    p.digit_bases_total_len = 2;
    p.digit_base_lens = digit_base_lens;
    p.digit_count = 2;
    p.p_base = p_base;
    p.p_base_len = 1;
    REQUIRE(hazeModUp(dst_polys, src_polys, &p, nullptr) == HAZE_SUCCESS);

    // D2H every output and verify exact zero. Initialise the host
    // buffer to a sentinel so a skipped D2H is detectable.
    for (size_t slot = 0; slot < 6; ++slot) {
        std::vector<uint64_t> out(kRingDim, 0xDEADBEEF);
        REQUIRE(hazeMemcpy(out.data(), dst_storage[slot], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) ==
                HAZE_SUCCESS);
        for (uint64_t i = 0; i < kRingDim; ++i) {
            REQUIRE(out[i] == 0);
        }
    }
    REQUIRE(hazeFree(s0) == HAZE_SUCCESS);
    REQUIRE(hazeFree(s1) == HAZE_SUCCESS);
    for (auto *slot : dst_storage) {
        REQUIRE(hazeFree(slot) == HAZE_SUCCESS);
    }
}
