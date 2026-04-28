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

static void configure_three_moduli() {
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(0, kQ0) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(1, kQ1) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(2, kQ2) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);
}

// ---------------------------------------------------------------------------
// Parameter validation
// ---------------------------------------------------------------------------

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
    p.src_polys = src_polys;
    p.src_base = src_base;
    p.src_base_len = 0; // invalid
    p.dst_polys = dst_polys;
    p.dst_base = dst_base;
    p.dst_base_len = 1;
    p.ring_dim = kRingDim;
    REQUIRE(hazeBasisConvert(nullptr, nullptr, &p, nullptr) == HAZE_ERROR_INVALID_VALUE);
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
    p.src_polys = src_polys;
    p.src_base = src_base;
    p.src_base_len = 2;
    p.dst_polys = dst_polys;
    p.rescale_base = rescale_base;
    p.rescale_base_len = 1;
    p.ring_dim = kRingDim;
    REQUIRE(hazeModDown(nullptr, nullptr, &p, nullptr) == HAZE_ERROR_INVALID_VALUE);
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

TEST_CASE("hazeModDown rejects rescale base longer than source") {
    configure_three_moduli();
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);

    const uint64_t src_base[] = {kQ0};
    const uint64_t rescale_base[] = {kQ0, kQ1};
    const void *src_polys[] = {nullptr};
    void *dst_polys[] = {dst};

    hazeModDownParams p{};
    p.src_polys = src_polys;
    p.src_base = src_base;
    p.src_base_len = 1;
    p.dst_polys = dst_polys;
    p.rescale_base = rescale_base;
    p.rescale_base_len = 2; // larger than src
    p.ring_dim = kRingDim;
    REQUIRE(hazeModDown(nullptr, nullptr, &p, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();

    REQUIRE(hazeFree(dst) == HAZE_SUCCESS);
}

// ---------------------------------------------------------------------------
// End-to-end: basis convert and retrieve results
// ---------------------------------------------------------------------------

TEST_CASE("hazeBasisConvert: 2->2 base converts and produces retrievable output") {
    configure_three_moduli();

    // Source MRP: 2 residues at q0, q1
    void *s0 = nullptr;
    void *s1 = nullptr;
    void *d0 = nullptr;
    void *d1 = nullptr;
    REQUIRE(hazeMalloc(&s0, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&s1, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d0, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d1, kBytes) == HAZE_SUCCESS);

    std::vector<uint64_t> poly_a(kRingDim, 1);
    std::vector<uint64_t> poly_b(kRingDim, 2);
    REQUIRE(hazeMemcpy(s0, poly_a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(s1, poly_b.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    const uint64_t src_base[] = {kQ0, kQ1};
    const uint64_t dst_base[] = {kQ0, kQ2};
    const void *src_polys[] = {s0, s1};
    void *dst_polys[] = {d0, d1};

    hazeBasisConvertParams p{};
    p.src_polys = src_polys;
    p.src_base = src_base;
    p.src_base_len = 2;
    p.dst_polys = dst_polys;
    p.dst_base = dst_base;
    p.dst_base_len = 2;
    p.ring_dim = kRingDim;
    REQUIRE(hazeBasisConvert(nullptr, nullptr, &p, nullptr) == HAZE_SUCCESS);

    // Retrieve results — values are deterministic from fast_base_convert;
    // the test verifies the pipeline runs end-to-end, not specific values.
    std::vector<uint64_t> out0(kRingDim, 0);
    std::vector<uint64_t> out1(kRingDim, 0);
    REQUIRE(hazeMemcpy(out0.data(), d0, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(out1.data(), d1, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    REQUIRE(hazeFree(s0) == HAZE_SUCCESS);
    REQUIRE(hazeFree(s1) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d0) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d1) == HAZE_SUCCESS);
}

TEST_CASE("hazeModDown: 3->2 emits instructions and registers outputs") {
    // The original task-04 test tolerated wrong numeric output because of
    // a placeholder rescale_fbc gadget in the niobium compiler. PR #1206
    // (1f78da4b) corrects fast_base_convert's CRT constants, so this test
    // can now assert that the full pipeline (ModDown + D2H) succeeds.
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

    std::vector<uint64_t> vals(kRingDim, 7);
    REQUIRE(hazeMemcpy(s0, vals.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(s1, vals.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(s2, vals.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    const uint64_t src_base[] = {kQ0, kQ1, kQ2};
    const uint64_t rescale_base[] = {kQ2};
    const void *src_polys[] = {s0, s1, s2};
    void *dst_polys[] = {d0, d1};

    hazeModDownParams p{};
    p.src_polys = src_polys;
    p.src_base = src_base;
    p.src_base_len = 3;
    p.dst_polys = dst_polys;
    p.rescale_base = rescale_base;
    p.rescale_base_len = 1;
    p.ring_dim = kRingDim;
    REQUIRE(hazeModDown(nullptr, nullptr, &p, nullptr) == HAZE_SUCCESS);

    // Flush the recording. With FBC fix in place, D2H should succeed
    // without HAZE_ERROR_LAUNCH_FAILURE.
    std::vector<uint64_t> out(kRingDim, 0);
    REQUIRE(hazeMemcpy(out.data(), d0, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    REQUIRE(hazeFree(s0) == HAZE_SUCCESS);
    REQUIRE(hazeFree(s1) == HAZE_SUCCESS);
    REQUIRE(hazeFree(s2) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d0) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d1) == HAZE_SUCCESS);
}

TEST_CASE("hazeModUp: 2 digits over Q={q0,q1} extended by P={q2}") {
    // Smoke test for the dig_decomp dispatch: 2-residue src in Q, two
    // digits each covering one Q residue, extended by a single-residue
    // P base. Output is digit_count * (src_base_len + p_base_len) =
    // 2 * (2 + 1) = 6 polynomials, written in src_base-then-p_base
    // order per digit.
    configure_three_moduli();

    void *s0 = nullptr;
    void *s1 = nullptr;
    REQUIRE(hazeMalloc(&s0, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&s1, kBytes) == HAZE_SUCCESS);

    void *dst_storage[6] = {nullptr};
    for (auto &slot : dst_storage) {
        REQUIRE(hazeMalloc(&slot, kBytes) == HAZE_SUCCESS);
    }

    std::vector<uint64_t> vals(kRingDim, 5);
    REQUIRE(hazeMemcpy(s0, vals.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(s1, vals.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    const uint64_t src_base[] = {kQ0, kQ1};
    const uint64_t digit_bases_flat[] = {kQ0, kQ1};
    const size_t digit_base_lens[] = {1, 1};
    const uint64_t p_base[] = {kQ2};
    const void *src_polys[] = {s0, s1};
    void *dst_polys[] = {dst_storage[0], dst_storage[1], dst_storage[2],
                         dst_storage[3], dst_storage[4], dst_storage[5]};

    hazeModUpParams p{};
    p.src_polys = src_polys;
    p.src_base = src_base;
    p.src_base_len = 2;
    p.digit_bases = digit_bases_flat;
    p.digit_base_lens = digit_base_lens;
    p.digit_count = 2;
    p.p_base = p_base;
    p.p_base_len = 1;
    p.dst_polys = dst_polys;
    p.ring_dim = kRingDim;
    REQUIRE(hazeModUp(nullptr, nullptr, &p, nullptr) == HAZE_SUCCESS);

    // Flush the recording via D2H of the first dst poly. With FBC fix
    // in place, this exercises the dig_decomp gadget end-to-end through
    // the materialization pipeline.
    std::vector<uint64_t> out(kRingDim, 0);
    REQUIRE(hazeMemcpy(out.data(), dst_storage[0], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) ==
            HAZE_SUCCESS);

    REQUIRE(hazeFree(s0) == HAZE_SUCCESS);
    REQUIRE(hazeFree(s1) == HAZE_SUCCESS);
    for (auto *slot : dst_storage) {
        REQUIRE(hazeFree(slot) == HAZE_SUCCESS);
    }
}
