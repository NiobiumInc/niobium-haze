// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Coverage for hazeMul followed by two hazeAutomorph calls sharing the
// same product `c` in a single epoch. Host reference: pointwise mul +
// σ_k slot permutation.

#include "integration_helpers.hpp"
#include "integration_introspect.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <vector>

namespace {

constexpr uint64_t kRingDim = 2048;
constexpr std::size_t kBytes = kRingDim * sizeof(uint64_t);
constexpr unsigned kLogRingDim = 11;
static_assert((1ULL << kLogRingDim) == kRingDim, "kLogRingDim must match log2(kRingDim)");

constexpr uint64_t kQ0 = 576460752303415297ULL;
constexpr uint64_t kQ1 = 576460752303439873ULL;
constexpr uint64_t kQ2 = 576460752303702017ULL;

constexpr uint64_t kIdx1 = 5ULL;
constexpr uint64_t kIdx2 = (kIdx1 * kIdx1) % (2ULL * kRingDim);
static_assert(kIdx1 % 2 == 1, "kIdx1 must be odd");
static_assert(kIdx2 % 2 == 1, "kIdx2 must be odd");
static_assert(kIdx1 != 1, "kIdx1 must not be the identity automorphism");
static_assert(kIdx2 != 1, "kIdx2 must not be the identity automorphism");
static_assert(kIdx1 != kIdx2, "kIdx1 and kIdx2 must be distinct");
static_assert((kIdx1 * kIdx2) % (2ULL * kRingDim) != 1,
              "kIdx2 must not be the multiplicative inverse of kIdx1 mod 2N");

struct SrpDriver {
    static constexpr std::size_t kNumResidues = 1;
    static constexpr const char *kShape = "SRP";

    std::vector<uint64_t> base;

    uint64_t setup() {
        const uint64_t q =
            haze::test::setup_integration_compute_config(kRingDim, kQ0, /*mod_idx=*/0);
        base = {q};
        return q;
    }

    static void mul(const std::vector<void *> &dst, const std::vector<const void *> &s1,
                    const std::vector<const void *> &s2) {
        REQUIRE(hazeMul(dst[0], s1[0], s2[0], 0, nullptr) == HAZE_SUCCESS);
    }
    static void automorph(const std::vector<void *> &dst, const std::vector<const void *> &src,
                          uint64_t k) {
        REQUIRE(hazeAutomorph(dst[0], src[0], k, nullptr) == HAZE_SUCCESS);
    }
};

struct MrpDriver {
    static constexpr std::size_t kNumResidues = 3;
    static constexpr const char *kShape = "MRP";

    std::vector<uint64_t> base;

    uint64_t setup() {
        REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
        REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
        uint64_t picked = 0;
        REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, kQ0, &picked) == HAZE_SUCCESS);
        REQUIRE(picked != 0);
        REQUIRE(hazeSetCiphertextModulus(0, kQ0) == HAZE_SUCCESS);
        REQUIRE(hazeSetCiphertextModulus(1, kQ1) == HAZE_SUCCESS);
        REQUIRE(hazeSetCiphertextModulus(2, kQ2) == HAZE_SUCCESS);
        REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);
        base = {kQ0, kQ1, kQ2};
        return kQ0;
    }

    void mul(const std::vector<void *> &dst, const std::vector<const void *> &s1,
             const std::vector<const void *> &s2) const {
        REQUIRE(hazeMulMrp(dst.data(), s1.data(), s2.data(), base.data(), base.size(), nullptr) ==
                HAZE_SUCCESS);
    }
    void automorph(const std::vector<void *> &dst, const std::vector<const void *> &src,
                   uint64_t k) const {
        REQUIRE(hazeAutomorphMrp(dst.data(), src.data(), k, base.data(), base.size(), nullptr) ==
                HAZE_SUCCESS);
    }
};

inline uint64_t mul_mod(uint64_t a, uint64_t b, uint64_t q) {
    return static_cast<uint64_t>((static_cast<__uint128_t>(a) * static_cast<__uint128_t>(b)) % q);
}

inline uint64_t bit_rev(uint64_t x, unsigned bits) {
    uint64_t r = 0;
    for (unsigned i = 0; i < bits; ++i) {
        r |= ((x >> i) & 1ULL) << (bits - 1 - i);
    }
    return r;
}

// hazeAutomorph spec is in slot space, but the FHETCH simulator stores
// eval-form values bit-reversed; bit_rev both indices to translate (one-way
// translation is the docstring trap).
inline std::vector<uint64_t> sigma(const std::vector<uint64_t> &in_storage, uint64_t k) {
    const uint64_t two_n = 2ULL * kRingDim;
    std::vector<uint64_t> out_storage(kRingDim);
    for (uint64_t a = 0; a < kRingDim; ++a) {
        const uint64_t i = bit_rev(a, kLogRingDim);
        const uint64_t j = ((k * (2ULL * i + 1ULL) - 1ULL) % two_n) / 2ULL;
        const uint64_t b = bit_rev(j, kLogRingDim);
        out_storage[a] = in_storage[b];
    }
    return out_storage;
}

template <typename Driver>
void check_against_per_residue(const Driver &d, const std::vector<void *> &dst,
                               const std::vector<std::vector<uint64_t>> &expected) {
    REQUIRE(dst.size() == Driver::kNumResidues);
    REQUIRE(expected.size() == Driver::kNumResidues);
    for (std::size_t i = 0; i < Driver::kNumResidues; ++i) {
        std::vector<uint64_t> got(kRingDim, 0xDEADBEEFULL);
        REQUIRE(hazeMemcpy(got.data(), dst[i], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
        for (uint64_t k = 0; k < kRingDim; ++k) {
            INFO(Driver::kShape << " residue " << i << " (mod " << d.base[i] << ") slot " << k);
            REQUIRE(got[k] == expected[i][k]);
        }
    }
}

} // namespace

TEMPLATE_TEST_CASE("mul→automorph→automorph: two rotations sharing one product", "[integration]",
                   SrpDriver, MrpDriver) {
    TestType d;
    d.setup();

    std::vector<std::vector<uint64_t>> a(TestType::kNumResidues);
    std::vector<std::vector<uint64_t>> b(TestType::kNumResidues);
    for (std::size_t i = 0; i < TestType::kNumResidues; ++i) {
        a[i] = haze::test::make_residue(d.base[i], 0xA110CULL + i, kRingDim);
        b[i] = haze::test::make_residue(d.base[i], 0xB055ULL + i, kRingDim);
    }

    std::vector<std::vector<uint64_t>> r1_expected(TestType::kNumResidues);
    std::vector<std::vector<uint64_t>> r2_expected(TestType::kNumResidues);
    for (std::size_t i = 0; i < TestType::kNumResidues; ++i) {
        const uint64_t q = d.base[i];
        std::vector<uint64_t> c_i(kRingDim);
        for (uint64_t s = 0; s < kRingDim; ++s) {
            c_i[s] = mul_mod(a[i][s], b[i][s], q);
        }
        r1_expected[i] = sigma(c_i, kIdx1);
        r2_expected[i] = sigma(c_i, kIdx2);
    }

    auto d_a = haze::test::allocate_and_h2d_residues(a);
    auto d_b = haze::test::allocate_and_h2d_residues(b);
    auto d_c = haze::test::allocate_dst_residues(TestType::kNumResidues, kBytes);
    auto d_r1 = haze::test::allocate_dst_residues(TestType::kNumResidues, kBytes);
    auto d_r2 = haze::test::allocate_dst_residues(TestType::kNumResidues, kBytes);

    // Single epoch: no D2H between automorph calls.
    d.mul(d_c, haze::test::to_const(d_a), haze::test::to_const(d_b));
    d.automorph(d_r1, haze::test::to_const(d_c), kIdx1);
    d.automorph(d_r2, haze::test::to_const(d_c), kIdx2);

    for (std::size_t i = 0; i < TestType::kNumResidues; ++i) {
        REQUIRE(hazeTagOutput(d_r1[i]) == HAZE_SUCCESS);
        REQUIRE(hazeTagOutput(d_r2[i]) == HAZE_SUCCESS);
    }
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    check_against_per_residue(d, d_r1, r1_expected);
    check_against_per_residue(d, d_r2, r2_expected);

    // Cross-check MRP path against SRP ground truth (both end up reading
    // the same simulator output); helper matches by content, not name.
    if constexpr (TestType::kNumResidues > 1) {
        haze::test::check_mrp_against_per_residue(d.base, r1_expected);
        haze::test::check_mrp_against_per_residue(d.base, r2_expected);
    }

    haze::test::free_all_residues(d_a);
    haze::test::free_all_residues(d_b);
    haze::test::free_all_residues(d_c);
    haze::test::free_all_residues(d_r1);
    haze::test::free_all_residues(d_r2);
}
