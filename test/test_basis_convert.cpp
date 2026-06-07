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
#include <algorithm>
#include <array>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <math/math-hal.h>
#include <utility>
#include <vector>

static constexpr uint64_t kRingDim = 4096;
static constexpr size_t kBytes = kRingDim * sizeof(uint64_t);

// Three NTT-friendly primes (q ≡ 1 mod 2N) for N=4096.
static constexpr uint64_t kQ0 = 576460752303415297ULL;
static constexpr uint64_t kQ1 = 576460752303439873ULL;
static constexpr uint64_t kQ2 = 576460752303702017ULL;

// Reset HAZE state up front so test ordering does not leak epoch /
// allocator state between cases under --order rand.
//
// The bridge init is required to satisfy the simulator's "ring dimension
// must be set" precondition (capture_crypto_context populates it) and to
// register the post-recording hook that writes per-input bins / per-output
// templates for downstream replay. The picked modulus is intentionally NOT
// fed back into haze's table — the simulator pulls primes from the FHETCH
// trace's modulus_table (built from src_base / dst_base / etc. directly),
// and the multi-residue tests need to keep their explicit kQ0 / kQ1 / kQ2
// values rather than be clobbered by the bridge's single picked prime.
static void configure_three_moduli() {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
    uint64_t picked = 0;
    REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, kQ0, &picked) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(0, kQ0) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(1, kQ1) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(2, kQ2) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);
}

// Parameter validation.

TEST_CASE("hazeBasisConvert rejects null params", "[unit]") {
    configure_three_moduli();
    REQUIRE(hazeBasisConvert(nullptr, nullptr, nullptr, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("hazeModDown rejects null params", "[unit]") {
    configure_three_moduli();
    REQUIRE(hazeModDown(nullptr, nullptr, nullptr, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("hazeModUp rejects null params", "[unit]") {
    configure_three_moduli();
    REQUIRE(hazeModUp(nullptr, nullptr, nullptr, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("hazeBasisConvert rejects empty source base", "[unit]") {
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

TEST_CASE("hazeModDown rejects foreign modulus in rescale_base", "[integration]") {
    // rescale_base contains a prime not present in src_base. The HAZE
    // layer must reject this BEFORE opening an EpochSession — otherwise
    // the next flush would replay a dirty recording and crash.
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
    REQUIRE(hazeTagOutput(c) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);
    std::vector<uint64_t> out(kRingDim, 0);
    REQUIRE(hazeMemcpy(out.data(), c, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    REQUIRE(out[0] == 3);

    REQUIRE(hazeFree(a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(b) == HAZE_SUCCESS);
    REQUIRE(hazeFree(c) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d) == HAZE_SUCCESS);
}

TEST_CASE("hazeModDown rejects rescale_base_len >= src_base_len", "[unit]") {
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

TEST_CASE("hazeModUp rejects mismatched digit_bases_total_len", "[unit]") {
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

TEST_CASE("hazeBasisConvert: shared-modulus copies produce input values", "[integration]") {
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

    REQUIRE(hazeTagOutput(d0) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(d1) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(d2) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

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

TEST_CASE("hazeBasisConvert: zero input produces zero output", "[integration]") {
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

    REQUIRE(hazeTagOutput(d0) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(d1) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

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

TEST_CASE("hazeBasisConvert: src/dst aliasing is safe (in-place 1->1)", "[integration]") {
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

    REQUIRE(hazeTagOutput(p0) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);
    std::vector<uint64_t> out(kRingDim, 0);
    REQUIRE(hazeMemcpy(out.data(), p0, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    REQUIRE(out[0] == 42);

    REQUIRE(hazeFree(p0) == HAZE_SUCCESS);
}

TEST_CASE("hazeModDown: zero input rescales to zero output", "[integration]") {
    // ApproxModDown is rescale_fbc(x, rescale_base): CRT-divide x by
    // P=prod(rescale_base) with rounding. For x identically zero on
    // every residue, every output is also zero — we can assert exact
    // values without doing any FBC arithmetic.
    //
    // Multi-output verification: hazeFlush materializes every tagged output to
    // its shadow buffer in one pass, so reading both d0 and d1 back via D2H
    // returns the materialized result, not stale post-allocator-reset memory.
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

    REQUIRE(hazeTagOutput(d0) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(d1) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

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

TEST_CASE("hazeModUp: zero input produces zero output across both digits", "[integration]") {
    // dig_decomp(x, digit_bases, p_base): for each digit i, take the
    // x|_{digit_bases[i]} subset and FBC-extend it onto src_base ∪
    // p_base. For x identically zero, every digit's every output is
    // zero, so we can assert exact values across all 6 dst slots.
    // This exercises the digit-major flatten arithmetic at d=0 and d=1
    // (catching off-by-one on the per_digit stride) and the multi-output
    // materialization path (every tagged output persisted on a single flush).
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

    for (auto *slot : dst_storage)
        REQUIRE(hazeTagOutput(slot) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    // D2H every output and verify exact zero. Initialise the host
    // buffer to a sentinel so a skipped D2H is detectable.
    for (auto &slot : dst_storage) {
        std::vector<uint64_t> out(kRingDim, 0xDEADBEEF);
        REQUIRE(hazeMemcpy(out.data(), slot, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
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

// ---------------------------------------------------------------------------
// Real-sized basis-conversion tests (12-limb source basis).
//
// HAZE's basis-conversion wrappers pass through to fhetch primitives that,
// in turn, run inside libnbcc's OpenFHE-backed replay subprocess. To
// validate that what HAZE records and replays matches the documented
// math, each test below runs the operation through HAZE end-to-end and
// compares every output residue against an independent host-side
// reference implementation of the same formula.
//
// Reference choice: the oracle is built from OpenFHE BigInteger
// arithmetic (math/hal/bigintbackend.h), independent of the gadget's
// __uint128_t path inside libnbcc. This is the same oracle pattern
// niobium-compiler/examples/fhetch/fast_base_convert_test.cpp:209-284
// uses to validate fhetch itself. We do NOT call OpenFHE's
// DCRTPoly::ApproxSwitchCRTBasis directly — the vendored OpenFHE in
// niobium-compiler is built with WITH_REDUCED_NOISE, which selects a
// SwitchModulus-based variant that is not bit-equal to upstream
// OpenFHE's plain-mod variant. The CMake option HAZE_FBC_REDUCED_NOISE
// (default ON) tells the oracle which variant to compute; flip it if
// you ever pair HAZE with a Standard-variant OpenFHE.
// ---------------------------------------------------------------------------

namespace {

// Twelve NTT-friendly primes (q ≡ 1 mod 2N, N=4096) for the source basis,
// plus four more for the extension P-basis used by ModUp. All primes are
// ~60 bits so __uint128_t arithmetic suffices through the FBC formula.
// Generated by enumerating primes ≡ 1 mod 8192 starting from kQ0; the
// first three values match the existing kQ0/kQ1/kQ2 used elsewhere in
// this file.
constexpr size_t kSrcLimbs = 12;
constexpr size_t kPLimbs = 4;
constexpr uint64_t kBigBase[kSrcLimbs + kPLimbs] = {
    576460752303415297ULL,
    576460752303439873ULL,
    576460752303702017ULL,
    576460752304439297ULL,
    576460752304545793ULL,
    576460752304619521ULL,
    576460752304832513ULL,
    576460752305111041ULL,
    576460752305348609ULL,
    576460752305569793ULL,
    576460752305799169ULL,
    576460752305872897ULL,
    // Extension primes (used only by ModUp's p_base).
    576460752305889281ULL,
    576460752305922049ULL,
    576460752306339841ULL,
    576460752306364417ULL,
};

// Configure HAZE with the full 16-prime moduli table once so the same
// state covers basis_convert (uses 12 src + 1 dst extension), ModDown
// (12 src), and ModUp (12 src + 4 p) tests.
//
// Same bridge-init rationale as configure_three_moduli: needed for the
// simulator's ring-dim precondition + post-recording hook registration.
// The picked modulus is intentionally not realigned; the trace's
// modulus_table carries the real kBigBase[i] values from the recorded
// sr_* ops.
void configure_sixteen_moduli() {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
    uint64_t picked = 0;
    REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, kBigBase[0], &picked) == HAZE_SUCCESS);
    for (int i = 0; std::cmp_less(i, kSrcLimbs + kPLimbs); ++i) {
        REQUIRE(hazeSetCiphertextModulus(i, kBigBase[static_cast<size_t>(i)]) == HAZE_SUCCESS);
    }
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);
}

// Deterministic non-trivial residues. Avoids zeros (which trigger
// trivial paths in the optimizer) and all-equal values across primes
// (which would mask q_hat / q_star bugs).
std::vector<uint64_t> make_residue(uint64_t prime, uint64_t seed) {
    std::vector<uint64_t> r(kRingDim);
    for (uint64_t k = 0; k < kRingDim; ++k) {
        const __uint128_t v = (static_cast<__uint128_t>(seed) * (k + 1) * 7) + (k & 0xFFFF) + 13;
        r[k] = static_cast<uint64_t>(v % prime);
    }
    return r;
}

// ----- Reference oracle: BigInteger arithmetic from OpenFHE -----
//
// Adapted from niobium-compiler/examples/fhetch/fast_base_convert_test.cpp's
// oracle_bigint_fbc. Same formula as the gadget on the recording side
// (niobium-compiler/src/FhetchApi.cpp:1566), independent arithmetic
// backend.
//
// HAZE always calls the 2-arg `fhetch::fast_base_convert` / `rescale_fbc`,
// which default to the variant that matches the niobium-instrumented
// OpenFHE backend. The build-time HAZE_FBC_REDUCED_NOISE define (set by
// CMakeLists.txt) tells this oracle which variant to compute. Default is
// 1 (ReducedNoise) — matches the openfhe submodule's WITH_REDUCED_NOISE=ON
// build. Flip the CMake option if you ever pair HAZE with a Standard-
// variant openfhe.

namespace ref {

#if HAZE_FBC_REDUCED_NOISE
constexpr bool kReducedNoise = true;
#else
constexpr bool kReducedNoise = false;
#endif

using lbcrypto::BigInteger;

// Reference fast-base-convert oracle. residues[i] holds the residue
// vector at src_base[i]; result[j] holds the residue vector at
// dst_base[j], length kRingDim. Pass-through is applied at every prime
// in `src_base ∩ dst_base` to match the gadget's identity-at-digit-primes
// invariant.
std::vector<std::vector<uint64_t>>
fast_base_convert(const std::vector<std::vector<uint64_t>> &residues,
                  const std::vector<uint64_t> &src_base, const std::vector<uint64_t> &dst_base) {
    const size_t k = src_base.size();

    BigInteger Q(1);
    for (uint64_t q : src_base) {
        Q = Q * BigInteger(q);
    }
    std::vector<uint64_t> q_hat(k);
    std::vector<std::vector<uint64_t>> q_star(k, std::vector<uint64_t>(dst_base.size()));
    for (size_t i = 0; i < k; ++i) {
        const BigInteger qi(src_base[i]);
        const BigInteger Qhat_i = Q / qi;
        q_hat[i] = Qhat_i.Mod(qi).ModInverse(qi).template ConvertToInt<uint64_t>();
        for (size_t j = 0; j < dst_base.size(); ++j) {
            q_star[i][j] = Qhat_i.Mod(BigInteger(dst_base[j])).template ConvertToInt<uint64_t>();
        }
    }

    std::vector<uint64_t> halfQ(k);
    for (size_t i = 0; i < k; ++i) {
        halfQ[i] = (src_base[i] - 1) / 2;
    }

    std::vector<std::vector<uint64_t>> out(dst_base.size(), std::vector<uint64_t>(kRingDim, 0));
    for (uint64_t s = 0; s < kRingDim; ++s) {
        std::vector<uint64_t> scaled(k);
        for (size_t i = 0; i < k; ++i) {
            const BigInteger v(residues[i][s]);
            scaled[i] = (v * BigInteger(q_hat[i]))
                            .Mod(BigInteger(src_base[i]))
                            .template ConvertToInt<uint64_t>();
        }
        for (size_t j = 0; j < dst_base.size(); ++j) {
            const uint64_t p = dst_base[j];
            const BigInteger pp(p);
            // Pass-through for src ∩ dst.
            auto src_it = std::ranges::find(src_base, p);
            if (src_it != src_base.end()) {
                const auto src_idx = static_cast<size_t>(src_it - src_base.begin());
                out[j][s] = residues[src_idx][s];
                continue;
            }
            BigInteger acc(0);
            for (size_t i = 0; i < k; ++i) {
                BigInteger scaled_at_p;
                if (kReducedNoise && scaled[i] > halfQ[i]) {
                    // Signed rebase: scaled[i] - q_i (negative) reduced mod p,
                    // expressed positively as (scaled mod p + p - q_i mod p) mod p.
                    const BigInteger qi_mod_p = BigInteger(src_base[i]).Mod(pp);
                    const BigInteger scaled_mod_p = BigInteger(scaled[i]).Mod(pp);
                    scaled_at_p = (scaled_mod_p + pp - qi_mod_p).Mod(pp);
                } else {
                    scaled_at_p = BigInteger(scaled[i]).Mod(pp);
                }
                acc = acc + scaled_at_p * BigInteger(q_star[i][j]);
            }
            out[j][s] = acc.Mod(pp).template ConvertToInt<uint64_t>();
        }
    }
    return out;
}

// Reference rescale_fbc. Output basis is `src_base \ rescale_base` in
// src_base's original order, matching HAZE's mod_down dst layout
// (src/core/basis_convert.cpp:160-168). Variant follows the same compile-
// time gate as fast_base_convert.
std::vector<std::vector<uint64_t>> rescale_fbc(const std::vector<std::vector<uint64_t>> &residues,
                                               const std::vector<uint64_t> &src_base,
                                               const std::vector<uint64_t> &rescale_base) {
    std::vector<uint64_t> target_base;
    std::vector<size_t> target_src_idx;
    std::vector<size_t> rescale_src_idx;
    for (size_t i = 0; i < src_base.size(); ++i) {
        const bool in_rescale = std::ranges::find(rescale_base, src_base[i]) != rescale_base.end();
        if (!in_rescale) {
            target_base.push_back(src_base[i]);
            target_src_idx.push_back(i);
        }
    }
    for (uint64_t q : rescale_base) {
        auto it = std::ranges::find(src_base, q);
        rescale_src_idx.push_back(static_cast<size_t>(it - src_base.begin()));
    }

    std::vector<std::vector<uint64_t>> rescale_residues(rescale_base.size());
    for (size_t i = 0; i < rescale_base.size(); ++i) {
        rescale_residues[i] = residues[rescale_src_idx[i]];
    }
    auto lifted = fast_base_convert(rescale_residues, rescale_base, target_base);

    std::vector<std::vector<uint64_t>> out(target_base.size(), std::vector<uint64_t>(kRingDim, 0));
    for (size_t j = 0; j < target_base.size(); ++j) {
        const uint64_t q = target_base[j];
        const BigInteger qq(q);
        BigInteger P_mod_q(1);
        for (uint64_t r : rescale_base) {
            P_mod_q = (P_mod_q * BigInteger(r)).Mod(qq);
        }
        const uint64_t P_inv = P_mod_q.ModInverse(qq).template ConvertToInt<uint64_t>();
        const BigInteger P_inv_big(P_inv);
        for (uint64_t s = 0; s < kRingDim; ++s) {
            const uint64_t a = residues[target_src_idx[j]][s];
            const uint64_t b = lifted[j][s];
            const BigInteger diff = (BigInteger(a) + qq - BigInteger(b)).Mod(qq);
            out[j][s] = (diff * P_inv_big).Mod(qq).template ConvertToInt<uint64_t>();
        }
    }
    return out;
}

// Reference dig_decomp. For each digit d, applies fast_base_convert from
// digit_bases[d] to (src_base ∪ p_base), matching HAZE's mod_up output
// layout (src/core/basis_convert.cpp:202-208).
//
// Note: fhetch's `dig_decomp` does not currently expose an FbcVariant
// parameter — it always calls the 2-arg fast_base_convert internally
// (niobium-compiler/src/FhetchApi.cpp:1711), so the variant here is
// pinned to whatever HAZE_FBC_REDUCED_NOISE selects. If a future fhetch
// adds a 4-arg overload, this function can be extended to thread the
// variant explicitly.
std::vector<std::vector<std::vector<uint64_t>>> dig_decomp(
    const std::vector<std::vector<uint64_t>> &residues, const std::vector<uint64_t> &src_base,
    const std::vector<std::vector<uint64_t>> &digit_bases, const std::vector<uint64_t> &p_base) {
    std::vector<uint64_t> target_base = src_base;
    target_base.insert(target_base.end(), p_base.begin(), p_base.end());

    std::vector<std::vector<std::vector<uint64_t>>> out(digit_bases.size());
    for (size_t d = 0; d < digit_bases.size(); ++d) {
        std::vector<std::vector<uint64_t>> d_residues(digit_bases[d].size());
        for (size_t i = 0; i < digit_bases[d].size(); ++i) {
            auto it = std::ranges::find(src_base, digit_bases[d][i]);
            REQUIRE(it != src_base.end());
            const auto src_idx = static_cast<size_t>(it - src_base.begin());
            d_residues[i] = residues[src_idx];
        }
        out[d] = fast_base_convert(d_residues, digit_bases[d], target_base);
    }
    return out;
}

} // namespace ref

// ----- HAZE-side helpers: allocate / fill / free a basis worth of polys. -----

// Allocate `count` polynomial slots and H2D the supplied residues. The
// returned vector is a parallel index of dev pointers; ownership stays
// with the test (free with hazeFree per slot). Aborts the test on any
// HAZE error to keep the body of the test linear.
std::vector<void *> allocate_and_h2d(const std::vector<std::vector<uint64_t>> &residues) {
    std::vector<void *> ptrs(residues.size(), nullptr);
    for (size_t i = 0; i < residues.size(); ++i) {
        REQUIRE(hazeMalloc(&ptrs[i], kBytes) == HAZE_SUCCESS);
        REQUIRE(hazeMemcpy(ptrs[i], residues[i].data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) ==
                HAZE_SUCCESS);
    }
    return ptrs;
}

// Allocate `count` empty polynomial slots for a basis-conversion output.
std::vector<void *> allocate_dst(size_t count) {
    std::vector<void *> ptrs(count, nullptr);
    for (size_t i = 0; i < count; ++i) {
        REQUIRE(hazeMalloc(&ptrs[i], kBytes) == HAZE_SUCCESS);
    }
    return ptrs;
}

void free_all(const std::vector<void *> &ptrs) {
    for (void *p : ptrs) {
        REQUIRE(hazeFree(p) == HAZE_SUCCESS);
    }
}

// D2H every dst slot and verify against the reference, residue-by-residue.
// Stops at the first mismatch with diagnostic info — Catch2's REQUIRE
// already truncates large failures, but we still want a useful message.
void check_against_reference(const std::vector<void *> &dst,
                             const std::vector<std::vector<uint64_t>> &expected,
                             const std::vector<uint64_t> &dst_base) {
    REQUIRE(dst.size() == expected.size());
    REQUIRE(dst.size() == dst_base.size());
    for (size_t j = 0; j < dst.size(); ++j) {
        std::vector<uint64_t> got(kRingDim, 0xDEADBEEFULL);
        REQUIRE(hazeMemcpy(got.data(), dst[j], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
        for (uint64_t s = 0; s < kRingDim; ++s) {
            INFO("dst index " << j << " (mod " << dst_base[j] << ") slot " << s);
            REQUIRE(got[s] == expected[j][s]);
        }
    }
}

} // namespace

TEST_CASE("hazeBasisConvert: 12-limb fast base convert matches reference", "[integration]") {
    configure_sixteen_moduli();

    const std::vector<uint64_t> src_base(kBigBase, kBigBase + kSrcLimbs);
    // dst_base is the four extension primes — none of them overlap
    // src_base, so the test exercises the full FBC accumulator path
    // (no pass-through shortcut).
    const std::vector<uint64_t> dst_base(kBigBase + kSrcLimbs, kBigBase + kSrcLimbs + kPLimbs);

    std::vector<std::vector<uint64_t>> residues(src_base.size());
    for (size_t i = 0; i < src_base.size(); ++i) {
        residues[i] = make_residue(src_base[i], /*seed=*/424242ULL + i);
    }

    auto src_ptrs = allocate_and_h2d(residues);
    auto dst_ptrs = allocate_dst(dst_base.size());

    std::vector<const void *> src_const_ptrs(src_ptrs.begin(), src_ptrs.end());

    hazeBasisConvertParams params{};
    params.src_base = src_base.data();
    params.src_base_len = src_base.size();
    params.dst_base = dst_base.data();
    params.dst_base_len = dst_base.size();
    REQUIRE(hazeBasisConvert(dst_ptrs.data(), src_const_ptrs.data(), &params, nullptr) ==
            HAZE_SUCCESS);

    for (void *p : dst_ptrs)
        REQUIRE(hazeTagOutput(p) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    auto expected = ref::fast_base_convert(residues, src_base, dst_base);
    check_against_reference(dst_ptrs, expected, dst_base);

    free_all(src_ptrs);
    free_all(dst_ptrs);
}

TEST_CASE("hazeModDown: 12-limb rescale matches reference", "[integration]") {
    configure_sixteen_moduli();

    const std::vector<uint64_t> src_base(kBigBase, kBigBase + kSrcLimbs);
    // Drop the last two src primes — typical CKKS rescale pattern (two
    // "special" primes at the tail). Output is the first 10 src primes.
    const std::vector<uint64_t> rescale_base(kBigBase + kSrcLimbs - 2, kBigBase + kSrcLimbs);

    std::vector<std::vector<uint64_t>> residues(src_base.size());
    for (size_t i = 0; i < src_base.size(); ++i) {
        residues[i] = make_residue(src_base[i], /*seed=*/911223ULL + i);
    }

    auto src_ptrs = allocate_and_h2d(residues);
    const size_t dst_count = src_base.size() - rescale_base.size();
    auto dst_ptrs = allocate_dst(dst_count);

    std::vector<const void *> src_const_ptrs(src_ptrs.begin(), src_ptrs.end());

    hazeModDownParams params{};
    params.src_base = src_base.data();
    params.src_base_len = src_base.size();
    params.rescale_base = rescale_base.data();
    params.rescale_base_len = rescale_base.size();
    REQUIRE(hazeModDown(dst_ptrs.data(), src_const_ptrs.data(), &params, nullptr) == HAZE_SUCCESS);

    auto expected = ref::rescale_fbc(residues, src_base, rescale_base);
    // Output basis = src_base \ rescale_base = the first 10 src primes,
    // in src_base's original order.
    const std::vector<uint64_t> dst_base(src_base.begin(),
                                         src_base.begin() + static_cast<long>(dst_count));
    for (void *p : dst_ptrs)
        REQUIRE(hazeTagOutput(p) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);
    check_against_reference(dst_ptrs, expected, dst_base);

    free_all(src_ptrs);
    free_all(dst_ptrs);
}

TEST_CASE("hazeModUp: 12-limb digit-decomp matches reference", "[integration]") {
    configure_sixteen_moduli();

    const std::vector<uint64_t> src_base(kBigBase, kBigBase + kSrcLimbs);
    const std::vector<uint64_t> p_base(kBigBase + kSrcLimbs, kBigBase + kSrcLimbs + kPLimbs);

    // Three digits of four primes each — partition src_base.
    constexpr size_t kDigitCount = 3;
    constexpr size_t kDigitLen = 4;
    std::vector<std::vector<uint64_t>> digit_bases(kDigitCount);
    std::vector<uint64_t> digit_bases_flat;
    std::array<size_t, kDigitCount> digit_base_lens{};
    for (size_t d = 0; d < kDigitCount; ++d) {
        digit_bases[d] = std::vector<uint64_t>(
            src_base.begin() + static_cast<std::ptrdiff_t>(d * kDigitLen),
            src_base.begin() + static_cast<std::ptrdiff_t>((d + 1) * kDigitLen));
        digit_bases_flat.insert(digit_bases_flat.end(), digit_bases[d].begin(),
                                digit_bases[d].end());
        digit_base_lens[d] = kDigitLen;
    }

    std::vector<std::vector<uint64_t>> residues(src_base.size());
    for (size_t i = 0; i < src_base.size(); ++i) {
        residues[i] = make_residue(src_base[i], /*seed=*/777111ULL + i);
    }

    auto src_ptrs = allocate_and_h2d(residues);
    // Output layout: digit_count × (src_base_len + p_base_len) polys,
    // digit-major order matching src/core/basis_convert.cpp:202-208.
    const size_t per_digit = src_base.size() + p_base.size();
    const size_t dst_count = kDigitCount * per_digit;
    auto dst_ptrs = allocate_dst(dst_count);

    std::vector<const void *> src_const_ptrs(src_ptrs.begin(), src_ptrs.end());

    hazeModUpParams params{};
    params.src_base = src_base.data();
    params.src_base_len = src_base.size();
    params.digit_bases = digit_bases_flat.data();
    params.digit_bases_total_len = digit_bases_flat.size();
    params.digit_base_lens = digit_base_lens.data();
    params.digit_count = kDigitCount;
    params.p_base = p_base.data();
    params.p_base_len = p_base.size();
    REQUIRE(hazeModUp(dst_ptrs.data(), src_const_ptrs.data(), &params, nullptr) == HAZE_SUCCESS);

    auto expected = ref::dig_decomp(residues, src_base, digit_bases, p_base);
    REQUIRE(expected.size() == kDigitCount);

    // The dst-base for each digit is src_base concatenated with p_base
    // (matches HAZE's wrapper at src/core/basis_convert.cpp:206-207).
    std::vector<uint64_t> per_digit_dst_base = src_base;
    per_digit_dst_base.insert(per_digit_dst_base.end(), p_base.begin(), p_base.end());
    REQUIRE(per_digit_dst_base.size() == per_digit);

    for (void *p : dst_ptrs)
        REQUIRE(hazeTagOutput(p) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    for (size_t d = 0; d < kDigitCount; ++d) {
        std::vector<void *> digit_dst(dst_ptrs.begin() + static_cast<long>(d * per_digit),
                                      dst_ptrs.begin() + static_cast<long>((d + 1) * per_digit));
        check_against_reference(digit_dst, expected[d], per_digit_dst_base);
    }

    free_all(src_ptrs);
    free_all(dst_ptrs);
}
