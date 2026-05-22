// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Compute API integration tests, parameterised over a per-shape Driver
// (SrpDriver = 1 residue, MrpDriver = 3 distinct primes for every-coefficient
// per-residue assertions). TEMPLATE_TEST_CASE fans out across both;
// SRP-only cases (modulus-error, hazeAutomorph, sync no-op) stay TEST_CASE.

#include "integration_helpers.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <niobium/compiler.h>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr uint64_t kRingDim = 4096;
constexpr std::size_t kBytes = kRingDim * sizeof(uint64_t);

// Three NTT-friendly primes (q ≡ 1 mod 2N) for N=4096; matches
// test_basis_convert.cpp::configure_three_moduli for suite consistency.
constexpr uint64_t kQ0 = 576460752303415297ULL;
constexpr uint64_t kQ1 = 576460752303439873ULL;
constexpr uint64_t kQ2 = 576460752303702017ULL;

// ---------------------------------------------------------------------------
// SrpDriver: single-residue path; vector args collapse to one element and
// dispatch the SRP entry points directly.
// ---------------------------------------------------------------------------

struct SrpDriver {
    static constexpr std::size_t kNumResidues = 1;
    static constexpr const char *kShape = "SRP";

    // Populated by setup(); test body uses base[i] as the per-residue modulus.
    std::vector<uint64_t> base;

    // Returns the picked prime so callers (e.g. negacyclic_conv_ref) get q
    // without re-deriving from `base`.
    uint64_t setup() {
        const uint64_t q =
            haze::test::setup_integration_compute_config(kRingDim, kQ0, /*mod_idx=*/0);
        base = {q};
        return q;
    }

    // SRP ops are static (read modulus from config via mod_idx); the MRP
    // counterparts read `base` and stay instance methods.
    static void add(const std::vector<void *> &dst, const std::vector<const void *> &s1,
                    const std::vector<const void *> &s2) {
        REQUIRE(hazeAdd(dst[0], s1[0], s2[0], 0, nullptr) == HAZE_SUCCESS);
    }
    static void sub(const std::vector<void *> &dst, const std::vector<const void *> &s1,
                    const std::vector<const void *> &s2) {
        REQUIRE(hazeSub(dst[0], s1[0], s2[0], 0, nullptr) == HAZE_SUCCESS);
    }
    static void mul(const std::vector<void *> &dst, const std::vector<const void *> &s1,
                    const std::vector<const void *> &s2) {
        REQUIRE(hazeMul(dst[0], s1[0], s2[0], 0, nullptr) == HAZE_SUCCESS);
    }
    static void add_scalar(const std::vector<void *> &dst, const std::vector<const void *> &src,
                           const std::vector<uint64_t> &scalars) {
        REQUIRE(hazeAddScalar(dst[0], src[0], scalars[0], 0, nullptr) == HAZE_SUCCESS);
    }
    static void sub_scalar(const std::vector<void *> &dst, const std::vector<const void *> &src,
                           const std::vector<uint64_t> &scalars) {
        REQUIRE(hazeSubScalar(dst[0], src[0], scalars[0], 0, nullptr) == HAZE_SUCCESS);
    }
    static void mul_scalar(const std::vector<void *> &dst, const std::vector<const void *> &src,
                           const std::vector<uint64_t> &scalars) {
        REQUIRE(hazeMulScalar(dst[0], src[0], scalars[0], 0, nullptr) == HAZE_SUCCESS);
    }
    static void ntt(const std::vector<void *> &dst, const std::vector<const void *> &src) {
        REQUIRE(hazeNTT(dst[0], src[0], 0, nullptr) == HAZE_SUCCESS);
    }
    static void intt(const std::vector<void *> &dst, const std::vector<const void *> &src) {
        REQUIRE(hazeINTT(dst[0], src[0], 0, nullptr) == HAZE_SUCCESS);
    }
    static void automorph(const std::vector<void *> &dst, const std::vector<const void *> &src,
                          uint64_t k) {
        REQUIRE(hazeAutomorph(dst[0], src[0], k, nullptr) == HAZE_SUCCESS);
    }
};

// ---------------------------------------------------------------------------
// MrpDriver: multi-residue path; three explicit primes feed the hazeXxxMrp
// entry points (the OpenFHE-picked prime is bridge plumbing — the simulator
// reads moduli from the trace's modulus_table).
// ---------------------------------------------------------------------------

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

    void add(const std::vector<void *> &dst, const std::vector<const void *> &s1,
             const std::vector<const void *> &s2) const {
        REQUIRE(hazeAddMrp(dst.data(), s1.data(), s2.data(), base.data(), base.size(), nullptr) ==
                HAZE_SUCCESS);
    }
    void sub(const std::vector<void *> &dst, const std::vector<const void *> &s1,
             const std::vector<const void *> &s2) const {
        REQUIRE(hazeSubMrp(dst.data(), s1.data(), s2.data(), base.data(), base.size(), nullptr) ==
                HAZE_SUCCESS);
    }
    void mul(const std::vector<void *> &dst, const std::vector<const void *> &s1,
             const std::vector<const void *> &s2) const {
        REQUIRE(hazeMulMrp(dst.data(), s1.data(), s2.data(), base.data(), base.size(), nullptr) ==
                HAZE_SUCCESS);
    }
    void add_scalar(const std::vector<void *> &dst, const std::vector<const void *> &src,
                    const std::vector<uint64_t> &scalars) const {
        REQUIRE(hazeAddScalarMrp(dst.data(), src.data(), scalars.data(), base.data(), base.size(),
                                 nullptr) == HAZE_SUCCESS);
    }
    void sub_scalar(const std::vector<void *> &dst, const std::vector<const void *> &src,
                    const std::vector<uint64_t> &scalars) const {
        REQUIRE(hazeSubScalarMrp(dst.data(), src.data(), scalars.data(), base.data(), base.size(),
                                 nullptr) == HAZE_SUCCESS);
    }
    void mul_scalar(const std::vector<void *> &dst, const std::vector<const void *> &src,
                    const std::vector<uint64_t> &scalars) const {
        REQUIRE(hazeMulScalarMrp(dst.data(), src.data(), scalars.data(), base.data(), base.size(),
                                 nullptr) == HAZE_SUCCESS);
    }
    void ntt(const std::vector<void *> &dst, const std::vector<const void *> &src) const {
        REQUIRE(hazeNTTMrp(dst.data(), src.data(), base.data(), base.size(), nullptr) ==
                HAZE_SUCCESS);
    }
    void intt(const std::vector<void *> &dst, const std::vector<const void *> &src) const {
        REQUIRE(hazeINTTMrp(dst.data(), src.data(), base.data(), base.size(), nullptr) ==
                HAZE_SUCCESS);
    }
    void automorph(const std::vector<void *> &dst, const std::vector<const void *> &src,
                   uint64_t k) const {
        REQUIRE(hazeAutomorphMrp(dst.data(), src.data(), k, base.data(), base.size(), nullptr) ==
                HAZE_SUCCESS);
    }
    void rot_automorph_coeff(const std::vector<void *> &dst, const std::vector<const void *> &src,
                             uint64_t offset) const {
        REQUIRE(hazeRotAutomorphCoeffMrp(dst.data(), src.data(), offset, base.data(), base.size(),
                                         nullptr) == HAZE_SUCCESS);
    }
};

// ---------------------------------------------------------------------------
// Per-residue oracles. Each takes the per-residue modulus and operates on
// vectors already reduced mod q, so the result is in [0, q) by induction.
// ---------------------------------------------------------------------------

inline uint64_t add_mod(uint64_t a, uint64_t b, uint64_t q) {
    const uint64_t s = a + b;
    return (s >= q) ? s - q : s;
}

inline uint64_t sub_mod(uint64_t a, uint64_t b, uint64_t q) {
    return (a >= b) ? a - b : a + (q - b);
}

inline uint64_t mul_mod(uint64_t a, uint64_t b, uint64_t q) {
    return static_cast<uint64_t>((static_cast<__uint128_t>(a) * static_cast<__uint128_t>(b)) % q);
}

// Verify all coefficients of all residues against the per-residue oracle.
// Sentinel-fills `got` so a missing D2H surfaces immediately, and tags
// every assertion with (residue, slot) for fast triage.
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

// SRP-vs-MRP cross-check; locates the matching group by content, skips for
// SRP drivers (degenerate 1-residue groups aren't registered).
template <typename Driver>
void check_against_per_residue_with_mrp(const Driver &d, const std::vector<void *> &dst,
                                        const std::vector<std::vector<uint64_t>> &expected) {
    check_against_per_residue(d, dst, expected);
    if constexpr (Driver::kNumResidues > 1) {
        haze::test::check_mrp_against_per_residue(d.base, expected);
    }
}

// Build a parallel pair of per-residue inputs (seed differs per residue)
// and the per-residue oracle for an op (a, b) → r mod q.
template <typename Op>
void make_two_residue_inputs(const std::vector<uint64_t> &base,
                             std::vector<std::vector<uint64_t>> &a_out,
                             std::vector<std::vector<uint64_t>> &b_out,
                             std::vector<std::vector<uint64_t>> &expected_out, uint64_t seed_a,
                             uint64_t seed_b, Op op) {
    const std::size_t k = base.size();
    a_out.assign(k, {});
    b_out.assign(k, {});
    expected_out.assign(k, {});
    for (std::size_t i = 0; i < k; ++i) {
        const uint64_t q = base[i];
        a_out[i] = haze::test::make_residue(q, seed_a + i, kRingDim);
        b_out[i] = haze::test::make_residue(q, seed_b + i, kRingDim);
        expected_out[i].resize(kRingDim);
        for (uint64_t s = 0; s < kRingDim; ++s) {
            expected_out[i][s] = op(a_out[i][s], b_out[i][s], q);
        }
    }
}

} // namespace

// ===========================================================================
// Pointwise operations (Add / Sub / Mul) — non-trivial inputs across every
// residue. Constants would be fully reproducible by aliasing the polymap
// across residues; make_residue makes each (residue, slot) pair distinct.
// ===========================================================================

TEMPLATE_TEST_CASE("hazeAdd: pointwise sum retrieved after D2H", "[integration]", SrpDriver,
                   MrpDriver) {
    TestType d;
    d.setup();

    std::vector<std::vector<uint64_t>> a;
    std::vector<std::vector<uint64_t>> b;
    std::vector<std::vector<uint64_t>> expected;
    make_two_residue_inputs(d.base, a, b, expected, /*seed_a=*/424242ULL,
                            /*seed_b=*/911223ULL, add_mod);

    auto da = haze::test::allocate_and_h2d_residues(a);
    auto db = haze::test::allocate_and_h2d_residues(b);
    auto dst = haze::test::allocate_dst_residues(TestType::kNumResidues, kBytes);

    d.add(dst, haze::test::to_const(da), haze::test::to_const(db));
    check_against_per_residue_with_mrp(d, dst, expected);

    haze::test::free_all_residues(da);
    haze::test::free_all_residues(db);
    haze::test::free_all_residues(dst);
}

TEMPLATE_TEST_CASE("hazeSub: pointwise difference retrieved after D2H", "[integration]", SrpDriver,
                   MrpDriver) {
    TestType d;
    d.setup();

    std::vector<std::vector<uint64_t>> a;
    std::vector<std::vector<uint64_t>> b;
    std::vector<std::vector<uint64_t>> expected;
    make_two_residue_inputs(d.base, a, b, expected, /*seed_a=*/123456ULL,
                            /*seed_b=*/789012ULL, sub_mod);

    auto da = haze::test::allocate_and_h2d_residues(a);
    auto db = haze::test::allocate_and_h2d_residues(b);
    auto dst = haze::test::allocate_dst_residues(TestType::kNumResidues, kBytes);

    d.sub(dst, haze::test::to_const(da), haze::test::to_const(db));
    check_against_per_residue_with_mrp(d, dst, expected);

    haze::test::free_all_residues(da);
    haze::test::free_all_residues(db);
    haze::test::free_all_residues(dst);
}

TEMPLATE_TEST_CASE("hazeMul: pointwise product retrieved after D2H", "[integration]", SrpDriver,
                   MrpDriver) {
    TestType d;
    d.setup();

    std::vector<std::vector<uint64_t>> a;
    std::vector<std::vector<uint64_t>> b;
    std::vector<std::vector<uint64_t>> expected;
    make_two_residue_inputs(d.base, a, b, expected, /*seed_a=*/0xC0FFEEULL,
                            /*seed_b=*/0xBEEFFEEDULL, mul_mod);

    auto da = haze::test::allocate_and_h2d_residues(a);
    auto db = haze::test::allocate_and_h2d_residues(b);
    auto dst = haze::test::allocate_dst_residues(TestType::kNumResidues, kBytes);

    d.mul(dst, haze::test::to_const(da), haze::test::to_const(db));
    check_against_per_residue_with_mrp(d, dst, expected);

    haze::test::free_all_residues(da);
    haze::test::free_all_residues(db);
    haze::test::free_all_residues(dst);
}

// ===========================================================================
// Scalar variants. Each residue gets a distinct scalar derived from the
// shared `kBaseScalar` so the oracle has to compute (a[i][k] op (s mod q[i])).
// MRP's `mr_*ps` ops take an MRS, so per-residue scalars are essential —
// asserting only one prime would mask wrong-base bugs.
// ===========================================================================

namespace {
// Per-residue scalar that fits in each prime; exercises reduction-on-input
// rather than a tiny constant that lives in [0, q) for every prime.
inline std::vector<uint64_t> derive_scalars(const std::vector<uint64_t> &base, uint64_t k_base) {
    std::vector<uint64_t> s(base.size());
    for (std::size_t i = 0; i < base.size(); ++i) {
        s[i] = (k_base + (i * 31ULL + 17ULL)) % base[i];
    }
    return s;
}
} // namespace

TEMPLATE_TEST_CASE("hazeAddScalar: pointwise scalar addition retrieved after D2H", "[integration]",
                   SrpDriver, MrpDriver) {
    TestType d;
    d.setup();

    const std::vector<uint64_t> scalars = derive_scalars(d.base, /*k_base=*/100003ULL);
    std::vector<std::vector<uint64_t>> a(TestType::kNumResidues);
    std::vector<std::vector<uint64_t>> expected(TestType::kNumResidues);
    for (std::size_t i = 0; i < TestType::kNumResidues; ++i) {
        a[i] = haze::test::make_residue(d.base[i], 0xA110CULL + i, kRingDim);
        expected[i].resize(kRingDim);
        for (uint64_t k = 0; k < kRingDim; ++k) {
            expected[i][k] = add_mod(a[i][k], scalars[i], d.base[i]);
        }
    }

    auto da = haze::test::allocate_and_h2d_residues(a);
    auto dst = haze::test::allocate_dst_residues(TestType::kNumResidues, kBytes);

    d.add_scalar(dst, haze::test::to_const(da), scalars);
    check_against_per_residue_with_mrp(d, dst, expected);

    haze::test::free_all_residues(da);
    haze::test::free_all_residues(dst);
}

TEMPLATE_TEST_CASE("hazeSubScalar: pointwise scalar subtraction retrieved after D2H",
                   "[integration]", SrpDriver, MrpDriver) {
    TestType d;
    d.setup();

    const std::vector<uint64_t> scalars = derive_scalars(d.base, /*k_base=*/200003ULL);
    std::vector<std::vector<uint64_t>> a(TestType::kNumResidues);
    std::vector<std::vector<uint64_t>> expected(TestType::kNumResidues);
    for (std::size_t i = 0; i < TestType::kNumResidues; ++i) {
        a[i] = haze::test::make_residue(d.base[i], 0xB055ULL + i, kRingDim);
        expected[i].resize(kRingDim);
        for (uint64_t k = 0; k < kRingDim; ++k) {
            expected[i][k] = sub_mod(a[i][k], scalars[i], d.base[i]);
        }
    }

    auto da = haze::test::allocate_and_h2d_residues(a);
    auto dst = haze::test::allocate_dst_residues(TestType::kNumResidues, kBytes);

    d.sub_scalar(dst, haze::test::to_const(da), scalars);
    check_against_per_residue_with_mrp(d, dst, expected);

    haze::test::free_all_residues(da);
    haze::test::free_all_residues(dst);
}

TEMPLATE_TEST_CASE("hazeMulScalar: pointwise scalar product retrieved after D2H", "[integration]",
                   SrpDriver, MrpDriver) {
    TestType d;
    d.setup();

    const std::vector<uint64_t> scalars = derive_scalars(d.base, /*k_base=*/300007ULL);
    std::vector<std::vector<uint64_t>> a(TestType::kNumResidues);
    std::vector<std::vector<uint64_t>> expected(TestType::kNumResidues);
    for (std::size_t i = 0; i < TestType::kNumResidues; ++i) {
        a[i] = haze::test::make_residue(d.base[i], 0xC0DEULL + i, kRingDim);
        expected[i].resize(kRingDim);
        for (uint64_t k = 0; k < kRingDim; ++k) {
            expected[i][k] = mul_mod(a[i][k], scalars[i], d.base[i]);
        }
    }

    auto da = haze::test::allocate_and_h2d_residues(a);
    auto dst = haze::test::allocate_dst_residues(TestType::kNumResidues, kBytes);

    d.mul_scalar(dst, haze::test::to_const(da), scalars);
    check_against_per_residue_with_mrp(d, dst, expected);

    haze::test::free_all_residues(da);
    haze::test::free_all_residues(dst);
}

// ===========================================================================
// NTT round-trip per residue: INTT(NTT(x)) must recover the input exactly.
// Each residue uses a deterministic non-constant input — constants are NTT
// eigenvectors and would mask twiddle/scaling bugs.
// ===========================================================================

TEMPLATE_TEST_CASE("NTT round-trip: INTT(NTT(x)) == x", "[integration]", SrpDriver, MrpDriver) {
    TestType d;
    d.setup();

    std::vector<std::vector<uint64_t>> src(TestType::kNumResidues);
    for (std::size_t i = 0; i < TestType::kNumResidues; ++i) {
        src[i].resize(kRingDim);
        const uint64_t q = d.base[i];
        for (uint64_t k = 0; k < kRingDim; ++k) {
            src[i][k] = (k * (1234567ULL + i) + 7ULL) % q;
        }
    }

    auto d_src = haze::test::allocate_and_h2d_residues(src);
    auto d_ntt = haze::test::allocate_dst_residues(TestType::kNumResidues, kBytes);
    auto d_intt = haze::test::allocate_dst_residues(TestType::kNumResidues, kBytes);

    d.ntt(d_ntt, haze::test::to_const(d_src));
    d.intt(d_intt, haze::test::to_const(d_ntt));

    check_against_per_residue(d, d_intt, src);
    if constexpr (TestType::kNumResidues > 1) {
        // Bulk-MRP read agrees with the SRP D2H above; content match means
        // a second MRP group in the same recording doesn't confuse it.
        haze::test::check_mrp_against_per_residue(d.base, src);
    }

    haze::test::free_all_residues(d_src);
    haze::test::free_all_residues(d_ntt);
    haze::test::free_all_residues(d_intt);
}

// ===========================================================================
// Galois automorphism (eval form) round-trip: automorph(k) ∘ automorph(k_inv)
// with k*k_inv ≡ 1 (mod 2N) is identity. Bracketed with NTT/INTT so the
// trace's modulus_table carries a real prime (sr_automorph_eval uses
// COPY_MODULUS).
// ===========================================================================

TEMPLATE_TEST_CASE("automorph impulse: X^1 -> X^k under automorph(_, k)", "[integration]",
                   SrpDriver, MrpDriver) {
    // Direction-sensitive check: ntt → automorph(_, k) → intt on impulse X^j
    // lands at X^(j*k mod 2N) (sign-flipped if ≥ N). With k=5, j=1, j*k=5<N,
    // expected is +1 at position 5; the round-trip test alone is symmetric
    // and wouldn't catch a substitution-direction bug.
    TestType d;
    d.setup();

    constexpr uint64_t k = 5;
    constexpr uint64_t j = 1;
    constexpr uint64_t out_pos = (j * k) % (2 * kRingDim);
    static_assert(out_pos < kRingDim,
                  "j*k must be < N for the impulse to land without a sign flip");

    std::vector<std::vector<uint64_t>> src(TestType::kNumResidues,
                                           std::vector<uint64_t>(kRingDim, 0));
    for (auto &row : src) {
        row[j] = 1;
    }

    std::vector<std::vector<uint64_t>> expected(TestType::kNumResidues,
                                                std::vector<uint64_t>(kRingDim, 0));
    for (auto &row : expected) {
        row[out_pos] = 1;
    }

    auto d_src = haze::test::allocate_and_h2d_residues(src);
    auto d_eval = haze::test::allocate_dst_residues(TestType::kNumResidues, kBytes);
    auto d_aut = haze::test::allocate_dst_residues(TestType::kNumResidues, kBytes);
    auto d_back = haze::test::allocate_dst_residues(TestType::kNumResidues, kBytes);

    d.ntt(d_eval, haze::test::to_const(d_src));
    d.automorph(d_aut, haze::test::to_const(d_eval), k);
    d.intt(d_back, haze::test::to_const(d_aut));

    check_against_per_residue(d, d_back, expected);
    if constexpr (TestType::kNumResidues > 1) {
        haze::test::check_mrp_against_per_residue(d.base, expected);
    }

    haze::test::free_all_residues(d_src);
    haze::test::free_all_residues(d_eval);
    haze::test::free_all_residues(d_aut);
    haze::test::free_all_residues(d_back);
}

TEMPLATE_TEST_CASE("automorph round-trip: automorph(k) then automorph(k_inv) == x", "[integration]",
                   SrpDriver, MrpDriver) {
    TestType d;
    d.setup();

    // k=5 is the standard CKKS rotation generator for power-of-2 N. For
    // N=4096 (so 2N=8192), 5 * 3277 = 16385 = 2*8192 + 1, so 3277 is k's
    // multiplicative inverse modulo 2N — applying automorph(5) then
    // automorph(3277) is X -> X^(5*3277) = X^1 = identity.
    constexpr uint64_t k = 5;
    constexpr uint64_t k_inv = 3277;
    static_assert((k * k_inv) % (2 * kRingDim) == 1,
                  "k * k_inv must be ≡ 1 mod 2N for the round-trip to be identity");

    std::vector<std::vector<uint64_t>> src(TestType::kNumResidues);
    for (std::size_t i = 0; i < TestType::kNumResidues; ++i) {
        src[i] = haze::test::make_residue(d.base[i], 0xA170ULL + i, kRingDim);
    }

    auto d_src = haze::test::allocate_and_h2d_residues(src);
    auto d_eval = haze::test::allocate_dst_residues(TestType::kNumResidues, kBytes);
    auto d_aut1 = haze::test::allocate_dst_residues(TestType::kNumResidues, kBytes);
    auto d_aut2 = haze::test::allocate_dst_residues(TestType::kNumResidues, kBytes);
    auto d_back = haze::test::allocate_dst_residues(TestType::kNumResidues, kBytes);

    d.ntt(d_eval, haze::test::to_const(d_src));
    d.automorph(d_aut1, haze::test::to_const(d_eval), k);
    d.automorph(d_aut2, haze::test::to_const(d_aut1), k_inv);
    d.intt(d_back, haze::test::to_const(d_aut2));

    check_against_per_residue(d, d_back, src);
    if constexpr (TestType::kNumResidues > 1) {
        haze::test::check_mrp_against_per_residue(d.base, src);
    }

    haze::test::free_all_residues(d_src);
    haze::test::free_all_residues(d_eval);
    haze::test::free_all_residues(d_aut1);
    haze::test::free_all_residues(d_aut2);
    haze::test::free_all_residues(d_back);
}

// ===========================================================================
// Negacyclic-coefficient rotation: rot(_, k) ∘ rot(_, N-k) = X^N = -1, so
// the result is -x per coefficient. Bracketed with NTT/INTT so the bridge
// captures the input addresses (rot alone doesn't register inputs);
// MRP-only (no SRP rot_automorph_coeff entry point exists).
// ===========================================================================

TEST_CASE("hazeRotAutomorphCoeffMrp: impulse lands at the spec-defined position", "[integration]") {
    // Direction-sensitive: feed a sparse input so the spec's landing
    // (output[i] = signs[i] * src[(i+offset) mod N]) is asymmetric. With
    // src=[1,2,0,...] and offset=1, output[0]=+src[1]=2 and output[N-1]=q-1.
    MrpDriver d;
    d.setup();

    constexpr uint64_t offset = 1;
    std::vector<std::vector<uint64_t>> src(MrpDriver::kNumResidues,
                                           std::vector<uint64_t>(kRingDim, 0));
    for (auto &row : src) {
        row[0] = 1;
        row[1] = 2;
    }

    std::vector<std::vector<uint64_t>> expected(MrpDriver::kNumResidues,
                                                std::vector<uint64_t>(kRingDim, 0));
    for (std::size_t r = 0; r < MrpDriver::kNumResidues; ++r) {
        const uint64_t q = d.base[r];
        for (uint64_t i = 0; i < kRingDim; ++i) {
            const uint64_t src_pos_unwrapped = i + offset;
            const uint64_t v = src[r][src_pos_unwrapped % kRingDim];
            const bool wraps = src_pos_unwrapped >= kRingDim;
            expected[r][i] = wraps ? (q - v) % q : v;
        }
    }
    // Sanity-check the oracle before pinning it on the device:
    REQUIRE(expected[0][0] == 2);
    REQUIRE(expected[0][kRingDim - 1] == d.base[0] - 1);
    REQUIRE(expected[0][1] == 0);
    REQUIRE(expected[0][kRingDim - 2] == 0);

    auto d_src = haze::test::allocate_and_h2d_residues(src);
    auto d_eval = haze::test::allocate_dst_residues(MrpDriver::kNumResidues, kBytes);
    auto d_coef = haze::test::allocate_dst_residues(MrpDriver::kNumResidues, kBytes);
    auto d_rot = haze::test::allocate_dst_residues(MrpDriver::kNumResidues, kBytes);

    d.ntt(d_eval, haze::test::to_const(d_src));
    d.intt(d_coef, haze::test::to_const(d_eval));
    d.rot_automorph_coeff(d_rot, haze::test::to_const(d_coef), offset);

    check_against_per_residue(d, d_rot, expected);

    haze::test::free_all_residues(d_src);
    haze::test::free_all_residues(d_eval);
    haze::test::free_all_residues(d_coef);
    haze::test::free_all_residues(d_rot);
}

TEST_CASE("hazeRotAutomorphCoeffMrp: rot(_, k) ∘ rot(_, N-k) == -x", "[integration]") {
    MrpDriver d;
    d.setup();

    constexpr uint64_t k = 1;
    constexpr uint64_t k_complement = kRingDim - 1;
    static_assert(k + k_complement == kRingDim,
                  "k + k_complement must equal N for the composition to land at X^N = -1");

    std::vector<std::vector<uint64_t>> src(MrpDriver::kNumResidues);
    std::vector<std::vector<uint64_t>> expected(MrpDriver::kNumResidues);
    for (std::size_t i = 0; i < MrpDriver::kNumResidues; ++i) {
        src[i] = haze::test::make_residue(d.base[i], 0xC0FFEEULL + i, kRingDim);
        expected[i].resize(kRingDim);
        for (uint64_t j = 0; j < kRingDim; ++j) {
            // -src[i][j] mod q; the (q - x) % q form handles x == 0
            // correctly (q % q = 0).
            expected[i][j] = (d.base[i] - src[i][j]) % d.base[i];
        }
    }

    auto d_src = haze::test::allocate_and_h2d_residues(src);
    auto d_eval = haze::test::allocate_dst_residues(MrpDriver::kNumResidues, kBytes);
    auto d_coef = haze::test::allocate_dst_residues(MrpDriver::kNumResidues, kBytes);
    auto d_aut1 = haze::test::allocate_dst_residues(MrpDriver::kNumResidues, kBytes);
    auto d_aut2 = haze::test::allocate_dst_residues(MrpDriver::kNumResidues, kBytes);

    d.ntt(d_eval, haze::test::to_const(d_src));
    d.intt(d_coef, haze::test::to_const(d_eval));
    d.rot_automorph_coeff(d_aut1, haze::test::to_const(d_coef), k);
    d.rot_automorph_coeff(d_aut2, haze::test::to_const(d_aut1), k_complement);

    check_against_per_residue(d, d_aut2, expected);

    haze::test::free_all_residues(d_src);
    haze::test::free_all_residues(d_eval);
    haze::test::free_all_residues(d_coef);
    haze::test::free_all_residues(d_aut1);
    haze::test::free_all_residues(d_aut2);
}

// ===========================================================================
// Polynomial product via NTT.Mul.INTT per residue: every coefficient must
// agree with negacyclic_conv_ref. Sub-cases cover monomial*monomial across
// the X^N=-1 boundary, linear*constant, small coefficients, and full-range.
// ===========================================================================

namespace {

// H2D, NTT, MUL, INTT each residue of (a, b); returns per-residue
// coefficient-form result via the driver dispatch.
template <typename Driver>
std::vector<std::vector<uint64_t>> run_ntt_mul_intt(const Driver &d,
                                                    const std::vector<std::vector<uint64_t>> &a,
                                                    const std::vector<std::vector<uint64_t>> &b) {
    REQUIRE(a.size() == Driver::kNumResidues);
    REQUIRE(b.size() == Driver::kNumResidues);

    auto d_a = haze::test::allocate_and_h2d_residues(a);
    auto d_b = haze::test::allocate_and_h2d_residues(b);
    auto d_a_eval = haze::test::allocate_dst_residues(Driver::kNumResidues, kBytes);
    auto d_b_eval = haze::test::allocate_dst_residues(Driver::kNumResidues, kBytes);
    auto d_c_eval = haze::test::allocate_dst_residues(Driver::kNumResidues, kBytes);
    auto d_c = haze::test::allocate_dst_residues(Driver::kNumResidues, kBytes);

    d.ntt(d_a_eval, haze::test::to_const(d_a));
    d.ntt(d_b_eval, haze::test::to_const(d_b));
    d.mul(d_c_eval, haze::test::to_const(d_a_eval), haze::test::to_const(d_b_eval));
    d.intt(d_c, haze::test::to_const(d_c_eval));

    std::vector<std::vector<uint64_t>> c(Driver::kNumResidues, std::vector<uint64_t>(kRingDim, 0));
    for (std::size_t i = 0; i < Driver::kNumResidues; ++i) {
        REQUIRE(hazeMemcpy(c[i].data(), d_c[i], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) ==
                HAZE_SUCCESS);
    }

    haze::test::free_all_residues(d_a);
    haze::test::free_all_residues(d_b);
    haze::test::free_all_residues(d_a_eval);
    haze::test::free_all_residues(d_b_eval);
    haze::test::free_all_residues(d_c_eval);
    haze::test::free_all_residues(d_c);
    return c;
}

} // namespace

TEMPLATE_TEST_CASE("polynomial product: monomial * monomial via NTT.Mul.INTT", "[integration]",
                   SrpDriver, MrpDriver) {
    TestType d;
    d.setup();

    // (i, j) pairs spanning the X^N = -1 wraparound boundary. The asymmetric
    // pair at i+j = N+1 catches off-by-one errors in the wrap threshold that
    // pure same-exponent squares miss.
    const std::pair<uint64_t, uint64_t> pairs[] = {{0, 0},
                                                   {kRingDim / 2, kRingDim / 2},
                                                   {kRingDim - 1, kRingDim - 1},
                                                   {(kRingDim / 2) + 1, kRingDim / 2}};

    for (const auto &[i, j] : pairs) {
        // Monomial inputs are identical across residues — the per-residue
        // oracle still encodes the wrap rule independently for every prime.
        std::vector<std::vector<uint64_t>> a(TestType::kNumResidues,
                                             std::vector<uint64_t>(kRingDim, 0));
        std::vector<std::vector<uint64_t>> b(TestType::kNumResidues,
                                             std::vector<uint64_t>(kRingDim, 0));
        for (auto &row : a) {
            row[i] = 1;
        }
        for (auto &row : b) {
            row[j] = 1;
        }

        const auto c = run_ntt_mul_intt(d, a, b);

        for (std::size_t r = 0; r < TestType::kNumResidues; ++r) {
            const uint64_t q = d.base[r];
            const auto expected = haze::test::negacyclic_conv_ref(a[r], b[r], q);
            for (uint64_t k = 0; k < kRingDim; ++k) {
                INFO(TestType::kShape << " residue " << r << " (mod " << q << ") slot " << k);
                REQUIRE(c[r][k] == expected[k]);
            }
            // Hand-check the wrap rule alongside the oracle.
            const uint64_t k = (i + j) % kRingDim;
            const bool wrap = (i + j) >= kRingDim;
            REQUIRE(c[r][k] == (wrap ? (q - 1) : 1));
        }
    }
}

TEMPLATE_TEST_CASE("polynomial product: linear * constant via NTT.Mul.INTT", "[integration]",
                   SrpDriver, MrpDriver) {
    TestType d;
    d.setup();

    // a(X) = 11 + 13*X (per residue, identical), b(X) = 17.
    std::vector<std::vector<uint64_t>> a(TestType::kNumResidues,
                                         std::vector<uint64_t>(kRingDim, 0));
    std::vector<std::vector<uint64_t>> b(TestType::kNumResidues,
                                         std::vector<uint64_t>(kRingDim, 0));
    for (auto &row : a) {
        row[0] = 11;
        row[1] = 13;
    }
    for (auto &row : b) {
        row[0] = 17;
    }

    const auto c = run_ntt_mul_intt(d, a, b);

    for (std::size_t r = 0; r < TestType::kNumResidues; ++r) {
        const uint64_t q = d.base[r];
        const auto expected = haze::test::negacyclic_conv_ref(a[r], b[r], q);
        for (uint64_t k = 0; k < kRingDim; ++k) {
            INFO(TestType::kShape << " residue " << r << " (mod " << q << ") slot " << k);
            REQUIRE(c[r][k] == expected[k]);
        }
        REQUIRE(c[r][0] == 11ULL * 17ULL);
        REQUIRE(c[r][1] == 13ULL * 17ULL);
        for (uint64_t k = 2; k < kRingDim; ++k) {
            REQUIRE(c[r][k] == 0);
        }
    }
}

TEMPLATE_TEST_CASE("polynomial product: random small coefficients via NTT.Mul.INTT",
                   "[integration]", SrpDriver, MrpDriver) {
    TestType d;
    d.setup();

    // Deterministic small coefficients (< 1024) with a per-residue pattern
    // so each residue runs a distinct convolution.
    std::vector<std::vector<uint64_t>> a(TestType::kNumResidues);
    std::vector<std::vector<uint64_t>> b(TestType::kNumResidues);
    for (std::size_t r = 0; r < TestType::kNumResidues; ++r) {
        a[r].resize(kRingDim);
        b[r].resize(kRingDim);
        for (uint64_t k = 0; k < kRingDim; ++k) {
            a[r][k] = ((k * 31ULL + 7ULL + r) ^ (k << 3)) & 0x3FFULL;
            b[r][k] = ((k * 17ULL + 5ULL + r) ^ (k << 2)) & 0x3FFULL;
        }
    }

    const auto c = run_ntt_mul_intt(d, a, b);

    for (std::size_t r = 0; r < TestType::kNumResidues; ++r) {
        const auto expected = haze::test::negacyclic_conv_ref(a[r], b[r], d.base[r]);
        for (uint64_t k = 0; k < kRingDim; ++k) {
            INFO(TestType::kShape << " residue " << r << " (mod " << d.base[r] << ") slot " << k);
            REQUIRE(c[r][k] == expected[k]);
        }
    }
}

TEMPLATE_TEST_CASE("polynomial product: random full-range coefficients via NTT.Mul.INTT",
                   "[integration]", SrpDriver, MrpDriver) {
    TestType d;
    d.setup();

    // Full-range coefficients in [0, q) to exercise modular reduction;
    // per-residue seeds mirror test_basis_convert.cpp's make_residue.
    std::vector<std::vector<uint64_t>> a(TestType::kNumResidues);
    std::vector<std::vector<uint64_t>> b(TestType::kNumResidues);
    for (std::size_t r = 0; r < TestType::kNumResidues; ++r) {
        const uint64_t q = d.base[r];
        a[r].resize(kRingDim);
        b[r].resize(kRingDim);
        for (uint64_t k = 0; k < kRingDim; ++k) {
            const __uint128_t va =
                (static_cast<__uint128_t>(0x9E3779B97F4A7C15ULL) * (k + 1) * (7 + r)) +
                (k & 0xFFFF) + 13;
            const __uint128_t vb =
                (static_cast<__uint128_t>(0xBB67AE8584CAA73BULL) * (k + 1) * (11 + r)) +
                (k & 0xFFFF) + 17;
            a[r][k] = static_cast<uint64_t>(va % q);
            b[r][k] = static_cast<uint64_t>(vb % q);
        }
    }

    const auto c = run_ntt_mul_intt(d, a, b);

    for (std::size_t r = 0; r < TestType::kNumResidues; ++r) {
        const auto expected = haze::test::negacyclic_conv_ref(a[r], b[r], d.base[r]);
        for (uint64_t k = 0; k < kRingDim; ++k) {
            INFO(TestType::kShape << " residue " << r << " (mod " << d.base[r] << ") slot " << k);
            REQUIRE(c[r][k] == expected[k]);
        }
    }
}

// ===========================================================================
// In-place ops: dst aliasing src1/src2/both must still produce the right
// answer per residue (lookup_or_create_locked's copy semantics make it safe).
// ===========================================================================

TEMPLATE_TEST_CASE("hazeAdd in-place (dst == src1) produces correct result", "[integration]",
                   SrpDriver, MrpDriver) {
    TestType d;
    d.setup();

    std::vector<std::vector<uint64_t>> a;
    std::vector<std::vector<uint64_t>> b;
    std::vector<std::vector<uint64_t>> expected;
    make_two_residue_inputs(d.base, a, b, expected, 0x1ULL, 0x2ULL, add_mod);

    auto da = haze::test::allocate_and_h2d_residues(a);
    auto db = haze::test::allocate_and_h2d_residues(b);

    // dst aliases src1 (in-place update of `da`).
    d.add(da, haze::test::to_const(da), haze::test::to_const(db));
    check_against_per_residue(d, da, expected);

    haze::test::free_all_residues(da);
    haze::test::free_all_residues(db);
}

TEMPLATE_TEST_CASE("hazeAdd in-place (dst == src2) produces correct result", "[integration]",
                   SrpDriver, MrpDriver) {
    TestType d;
    d.setup();

    std::vector<std::vector<uint64_t>> a;
    std::vector<std::vector<uint64_t>> b;
    std::vector<std::vector<uint64_t>> expected;
    make_two_residue_inputs(d.base, a, b, expected, 0x10ULL, 0x20ULL, add_mod);

    auto da = haze::test::allocate_and_h2d_residues(a);
    auto db = haze::test::allocate_and_h2d_residues(b);

    // dst aliases src2 (in-place update of `db`).
    d.add(db, haze::test::to_const(da), haze::test::to_const(db));
    check_against_per_residue(d, db, expected);

    haze::test::free_all_residues(da);
    haze::test::free_all_residues(db);
}

TEMPLATE_TEST_CASE("hazeAdd in-place squaring-style (dst == src1 == src2)", "[integration]",
                   SrpDriver, MrpDriver) {
    TestType d;
    d.setup();

    // Per-residue input; expected is x + x mod q.
    std::vector<std::vector<uint64_t>> a(TestType::kNumResidues);
    std::vector<std::vector<uint64_t>> expected(TestType::kNumResidues);
    for (std::size_t i = 0; i < TestType::kNumResidues; ++i) {
        a[i] = haze::test::make_residue(d.base[i], 0x5EEDULL + i, kRingDim);
        expected[i].resize(kRingDim);
        for (uint64_t k = 0; k < kRingDim; ++k) {
            expected[i][k] = add_mod(a[i][k], a[i][k], d.base[i]);
        }
    }

    auto da = haze::test::allocate_and_h2d_residues(a);
    d.add(da, haze::test::to_const(da), haze::test::to_const(da));
    check_against_per_residue(d, da, expected);
    haze::test::free_all_residues(da);
}

TEMPLATE_TEST_CASE("hazeMul in-place (dst == src1) produces correct result", "[integration]",
                   SrpDriver, MrpDriver) {
    TestType d;
    d.setup();

    std::vector<std::vector<uint64_t>> a;
    std::vector<std::vector<uint64_t>> b;
    std::vector<std::vector<uint64_t>> expected;
    make_two_residue_inputs(d.base, a, b, expected, 0xA1ULL, 0xB2ULL, mul_mod);

    auto da = haze::test::allocate_and_h2d_residues(a);
    auto db = haze::test::allocate_and_h2d_residues(b);

    d.mul(da, haze::test::to_const(da), haze::test::to_const(db));
    check_against_per_residue(d, da, expected);

    haze::test::free_all_residues(da);
    haze::test::free_all_residues(db);
}

TEMPLATE_TEST_CASE("hazeMul in-place (dst == src2) produces correct result", "[integration]",
                   SrpDriver, MrpDriver) {
    TestType d;
    d.setup();

    std::vector<std::vector<uint64_t>> a;
    std::vector<std::vector<uint64_t>> b;
    std::vector<std::vector<uint64_t>> expected;
    make_two_residue_inputs(d.base, a, b, expected, 0xA10ULL, 0xB20ULL, mul_mod);

    auto da = haze::test::allocate_and_h2d_residues(a);
    auto db = haze::test::allocate_and_h2d_residues(b);

    d.mul(db, haze::test::to_const(da), haze::test::to_const(db));
    check_against_per_residue(d, db, expected);

    haze::test::free_all_residues(da);
    haze::test::free_all_residues(db);
}

TEMPLATE_TEST_CASE("hazeMul in-place squaring-style (dst == src1 == src2)", "[integration]",
                   SrpDriver, MrpDriver) {
    TestType d;
    d.setup();

    std::vector<std::vector<uint64_t>> a(TestType::kNumResidues);
    std::vector<std::vector<uint64_t>> expected(TestType::kNumResidues);
    for (std::size_t i = 0; i < TestType::kNumResidues; ++i) {
        a[i] = haze::test::make_residue(d.base[i], 0xCAFEULL + i, kRingDim);
        expected[i].resize(kRingDim);
        for (uint64_t k = 0; k < kRingDim; ++k) {
            expected[i][k] = mul_mod(a[i][k], a[i][k], d.base[i]);
        }
    }

    auto da = haze::test::allocate_and_h2d_residues(a);
    d.mul(da, haze::test::to_const(da), haze::test::to_const(da));
    check_against_per_residue(d, da, expected);
    haze::test::free_all_residues(da);
}

// ===========================================================================
// Multi-op chains and lazy-recording semantics: EpochSession bundles
// multiple calls into one recording, and polymap invalidation (H2D / memset)
// works in the MRP fan-out as it does for SRP.
// ===========================================================================

TEMPLATE_TEST_CASE("multi-operation chain: add then mulscalar in one recording", "[integration]",
                   SrpDriver, MrpDriver) {
    TestType d;
    d.setup();

    std::vector<std::vector<uint64_t>> a(TestType::kNumResidues);
    std::vector<std::vector<uint64_t>> b(TestType::kNumResidues);
    const std::vector<uint64_t> scalars = derive_scalars(d.base, /*k_base=*/13ULL);
    std::vector<std::vector<uint64_t>> expected(TestType::kNumResidues);
    for (std::size_t i = 0; i < TestType::kNumResidues; ++i) {
        a[i] = haze::test::make_residue(d.base[i], 0xADD0ULL + i, kRingDim);
        b[i] = haze::test::make_residue(d.base[i], 0xADD1ULL + i, kRingDim);
        expected[i].resize(kRingDim);
        for (uint64_t k = 0; k < kRingDim; ++k) {
            const uint64_t s = add_mod(a[i][k], b[i][k], d.base[i]);
            expected[i][k] = mul_mod(s, scalars[i], d.base[i]);
        }
    }

    auto da = haze::test::allocate_and_h2d_residues(a);
    auto db = haze::test::allocate_and_h2d_residues(b);
    auto d_t = haze::test::allocate_dst_residues(TestType::kNumResidues, kBytes);
    auto d_dst = haze::test::allocate_dst_residues(TestType::kNumResidues, kBytes);

    d.add(d_t, haze::test::to_const(da), haze::test::to_const(db));
    d.mul_scalar(d_dst, haze::test::to_const(d_t), scalars);
    check_against_per_residue(d, d_dst, expected);

    haze::test::free_all_residues(da);
    haze::test::free_all_residues(db);
    haze::test::free_all_residues(d_t);
    haze::test::free_all_residues(d_dst);
}

TEMPLATE_TEST_CASE("multi-operation chain: mul then add in one recording", "[integration]",
                   SrpDriver, MrpDriver) {
    TestType d;
    d.setup();

    std::vector<std::vector<uint64_t>> a(TestType::kNumResidues);
    std::vector<std::vector<uint64_t>> b(TestType::kNumResidues);
    std::vector<std::vector<uint64_t>> expected(TestType::kNumResidues);
    for (std::size_t i = 0; i < TestType::kNumResidues; ++i) {
        a[i] = haze::test::make_residue(d.base[i], 0x10AULL + i, kRingDim);
        b[i] = haze::test::make_residue(d.base[i], 0x10BULL + i, kRingDim);
        expected[i].resize(kRingDim);
        for (uint64_t k = 0; k < kRingDim; ++k) {
            const uint64_t m = mul_mod(a[i][k], b[i][k], d.base[i]);
            expected[i][k] = add_mod(m, b[i][k], d.base[i]);
        }
    }

    auto da = haze::test::allocate_and_h2d_residues(a);
    auto db = haze::test::allocate_and_h2d_residues(b);
    auto d_t = haze::test::allocate_dst_residues(TestType::kNumResidues, kBytes);
    auto d_dst = haze::test::allocate_dst_residues(TestType::kNumResidues, kBytes);

    d.mul(d_t, haze::test::to_const(da), haze::test::to_const(db));
    d.add(d_dst, haze::test::to_const(d_t), haze::test::to_const(db));
    check_against_per_residue(d, d_dst, expected);

    haze::test::free_all_residues(da);
    haze::test::free_all_residues(db);
    haze::test::free_all_residues(d_t);
    haze::test::free_all_residues(d_dst);
}

TEMPLATE_TEST_CASE("multiple materializations: two independent D2H cycles", "[integration]",
                   SrpDriver, MrpDriver) {
    TestType d;
    d.setup();

    // First batch.
    std::vector<std::vector<uint64_t>> a1;
    std::vector<std::vector<uint64_t>> b1;
    std::vector<std::vector<uint64_t>> exp1;
    make_two_residue_inputs(d.base, a1, b1, exp1, 0x111ULL, 0x222ULL, add_mod);

    auto da = haze::test::allocate_and_h2d_residues(a1);
    auto db = haze::test::allocate_and_h2d_residues(b1);
    auto d_dst1 = haze::test::allocate_dst_residues(TestType::kNumResidues, kBytes);
    auto d_dst2 = haze::test::allocate_dst_residues(TestType::kNumResidues, kBytes);

    d.add(d_dst1, haze::test::to_const(da), haze::test::to_const(db));
    check_against_per_residue(d, d_dst1, exp1);

    // Second batch (after first materialisation).
    std::vector<std::vector<uint64_t>> a2(TestType::kNumResidues);
    std::vector<std::vector<uint64_t>> b2(TestType::kNumResidues);
    std::vector<std::vector<uint64_t>> exp2(TestType::kNumResidues);
    for (std::size_t i = 0; i < TestType::kNumResidues; ++i) {
        a2[i] = haze::test::make_residue(d.base[i], 0x333ULL + i, kRingDim);
        b2[i] = haze::test::make_residue(d.base[i], 0x444ULL + i, kRingDim);
        exp2[i].resize(kRingDim);
        for (uint64_t k = 0; k < kRingDim; ++k) {
            exp2[i][k] = add_mod(a2[i][k], b2[i][k], d.base[i]);
        }
        REQUIRE(hazeMemcpy(da[i], a2[i].data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) ==
                HAZE_SUCCESS);
        REQUIRE(hazeMemcpy(db[i], b2[i].data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) ==
                HAZE_SUCCESS);
    }
    d.add(d_dst2, haze::test::to_const(da), haze::test::to_const(db));
    check_against_per_residue(d, d_dst2, exp2);

    haze::test::free_all_residues(da);
    haze::test::free_all_residues(db);
    haze::test::free_all_residues(d_dst1);
    haze::test::free_all_residues(d_dst2);
}

TEMPLATE_TEST_CASE("H2D after compute invalidates the polymap binding", "[integration]", SrpDriver,
                   MrpDriver) {
    TestType d;
    d.setup();

    // First pass: dst1 = a1 + b.
    std::vector<std::vector<uint64_t>> a1;
    std::vector<std::vector<uint64_t>> b;
    std::vector<std::vector<uint64_t>> exp1;
    make_two_residue_inputs(d.base, a1, b, exp1, 0xAA1ULL, 0xBBBULL, add_mod);

    auto da = haze::test::allocate_and_h2d_residues(a1);
    auto db = haze::test::allocate_and_h2d_residues(b);
    auto d_dst1 = haze::test::allocate_dst_residues(TestType::kNumResidues, kBytes);
    auto d_dst2 = haze::test::allocate_dst_residues(TestType::kNumResidues, kBytes);

    d.add(d_dst1, haze::test::to_const(da), haze::test::to_const(db));

    // Overwrite a's shadow before flushing — recording is still active.
    // dst2 must reflect the *new* a, not the stale binding.
    std::vector<std::vector<uint64_t>> a2(TestType::kNumResidues);
    std::vector<std::vector<uint64_t>> exp2(TestType::kNumResidues);
    for (std::size_t i = 0; i < TestType::kNumResidues; ++i) {
        a2[i] = haze::test::make_residue(d.base[i], 0xAA2ULL + i, kRingDim);
        exp2[i].resize(kRingDim);
        for (uint64_t k = 0; k < kRingDim; ++k) {
            exp2[i][k] = add_mod(a2[i][k], b[i][k], d.base[i]);
        }
        REQUIRE(hazeMemcpy(da[i], a2[i].data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) ==
                HAZE_SUCCESS);
    }
    d.add(d_dst2, haze::test::to_const(da), haze::test::to_const(db));
    check_against_per_residue(d, d_dst2, exp2);

    haze::test::free_all_residues(da);
    haze::test::free_all_residues(db);
    haze::test::free_all_residues(d_dst1);
    haze::test::free_all_residues(d_dst2);
}

TEMPLATE_TEST_CASE("memset after compute invalidates the polymap binding", "[integration]",
                   SrpDriver, MrpDriver) {
    TestType d;
    d.setup();

    // First pass: dst1 = a + b.
    std::vector<std::vector<uint64_t>> a;
    std::vector<std::vector<uint64_t>> b;
    std::vector<std::vector<uint64_t>> exp1;
    make_two_residue_inputs(d.base, a, b, exp1, 0xAA1ULL, 0xBBBULL, add_mod);

    auto da = haze::test::allocate_and_h2d_residues(a);
    auto db = haze::test::allocate_and_h2d_residues(b);
    auto d_dst1 = haze::test::allocate_dst_residues(TestType::kNumResidues, kBytes);
    auto d_dst2 = haze::test::allocate_dst_residues(TestType::kNumResidues, kBytes);

    d.add(d_dst1, haze::test::to_const(da), haze::test::to_const(db));

    // memset zeros each residue's shadow; second add must read all zeros.
    for (std::size_t i = 0; i < TestType::kNumResidues; ++i) {
        REQUIRE(hazeMemset(da[i], 0, kBytes) == HAZE_SUCCESS);
    }
    d.add(d_dst2, haze::test::to_const(da), haze::test::to_const(db));

    // Expected: 0 + b[i][k] = b[i][k] (already in [0, q)).
    check_against_per_residue(d, d_dst2, b);

    haze::test::free_all_residues(da);
    haze::test::free_all_residues(db);
    haze::test::free_all_residues(d_dst1);
    haze::test::free_all_residues(d_dst2);
}

// ===========================================================================
// SRP-only cases below: things that don't have an MRP shape (modulus-index
// errors don't apply to MRP because the MRP shims take primes directly,
// not indices), or that test record-and-replay-orthogonal behaviour where
// SRP coverage is sufficient.
// ===========================================================================

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

    REQUIRE(hazeAdd(d_dst, d_a, d_b, 0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeDeviceSynchronize() == HAZE_SUCCESS);

    std::vector<uint64_t> result(kRingDim, 0);
    REQUIRE(hazeMemcpy(result.data(), d_dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    for (uint64_t i = 0; i < kRingDim; ++i) {
        REQUIRE(result[i] == 6);
    }

    REQUIRE(hazeFree(d_a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_b) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_dst) == HAZE_SUCCESS);
}

TEST_CASE("hazeAdd with unknown source address returns error", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(0, kQ0) == HAZE_SUCCESS);
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
    REQUIRE(hazeAdd(d_dst, fake1, fake2, 0, nullptr) == HAZE_ERROR_UNKNOWN_ADDRESS);
    hazeGetLastError();

    REQUIRE(hazeFree(d_dst) == HAZE_SUCCESS);
}

TEST_CASE("hazeAdd with invalid modulus index returns error", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(0, kQ0) == HAZE_SUCCESS);
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

    REQUIRE(hazeAdd(d_dst, d_a, d_b, -1, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    REQUIRE(hazeAdd(d_dst, d_a, d_b, 63, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();

    REQUIRE(hazeFree(d_a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_b) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_dst) == HAZE_SUCCESS);
}

TEST_CASE("hazeMul with unknown source address returns error", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(0, kQ0) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);

    void *d_dst = nullptr;
    REQUIRE(hazeMalloc(&d_dst, kBytes) == HAZE_SUCCESS);

    // NOLINTBEGIN(performance-no-int-to-ptr)
    void *fake1 = reinterpret_cast<void *>(uintptr_t{0x4000000000ULL} + 0x8000000ULL);
    void *fake2 = reinterpret_cast<void *>(uintptr_t{0x4000000000ULL} + 0x9000000ULL);
    // NOLINTEND(performance-no-int-to-ptr)
    REQUIRE(hazeMul(d_dst, fake1, fake2, 0, nullptr) == HAZE_ERROR_UNKNOWN_ADDRESS);
    hazeGetLastError();

    REQUIRE(hazeFree(d_dst) == HAZE_SUCCESS);
}

TEST_CASE("hazeMul with invalid modulus index returns error", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(0, kQ0) == HAZE_SUCCESS);
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

    REQUIRE(hazeMul(d_dst, d_a, d_b, -1, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    REQUIRE(hazeMul(d_dst, d_a, d_b, 63, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();

    REQUIRE(hazeFree(d_a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_b) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_dst) == HAZE_SUCCESS);
}

// ---------------------------------------------------------------------------
// MRP-shim argument validation. Mirrors the basis-convert null-pointer
// rejection pattern so a bad call surfaces before any EpochSession opens.
// ---------------------------------------------------------------------------

TEST_CASE("hazeAddMrp rejects null arrays / zero base length", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(0, kQ0) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);

    const uint64_t base[] = {kQ0};
    void *dst_polys[] = {nullptr};
    const void *src_polys[] = {nullptr};

    REQUIRE(hazeAddMrp(nullptr, src_polys, src_polys, base, 1, nullptr) ==
            HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeAddMrp(dst_polys, nullptr, src_polys, base, 1, nullptr) ==
            HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeAddMrp(dst_polys, src_polys, nullptr, base, 1, nullptr) ==
            HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeAddMrp(dst_polys, src_polys, src_polys, nullptr, 1, nullptr) ==
            HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeAddMrp(dst_polys, src_polys, src_polys, base, 0, nullptr) ==
            HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("hazeAddScalarMrp rejects null scalars array", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(0, kQ0) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);

    const uint64_t base[] = {kQ0};
    void *dst_polys[] = {nullptr};
    const void *src_polys[] = {nullptr};

    REQUIRE(hazeAddScalarMrp(dst_polys, src_polys, /*scalars=*/nullptr, base, 1, nullptr) ==
            HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("hazeNTTMrp rejects null arrays / zero base length", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(0, kQ0) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);

    const uint64_t base[] = {kQ0};
    void *dst_polys[] = {nullptr};
    const void *src_polys[] = {nullptr};

    REQUIRE(hazeNTTMrp(nullptr, src_polys, base, 1, nullptr) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeNTTMrp(dst_polys, nullptr, base, 1, nullptr) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeNTTMrp(dst_polys, src_polys, nullptr, 1, nullptr) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeNTTMrp(dst_polys, src_polys, base, 0, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("hazeAutomorphMrp rejects null arrays / zero base length", "[unit]") {
    // Distinct shape from hazeNTTMrp (carries a uint64_t index argument),
    // so the validation code lives in its own shim block — covered here
    // to ensure the same null-pointer guarantees apply.
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(0, kQ0) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);

    const uint64_t base[] = {kQ0};
    void *dst_polys[] = {nullptr};
    const void *src_polys[] = {nullptr};

    REQUIRE(hazeAutomorphMrp(nullptr, src_polys, 5, base, 1, nullptr) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeAutomorphMrp(dst_polys, nullptr, 5, base, 1, nullptr) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeAutomorphMrp(dst_polys, src_polys, 5, nullptr, 1, nullptr) ==
            HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeAutomorphMrp(dst_polys, src_polys, 5, base, 0, nullptr) ==
            HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

// ============================================================================
// MRP-shape output round-trip: dedicated coverage for the bulk MRP read path
// across an explicit 3-tower hazeMulMrp. The single-op cross-checks above
// (check_against_per_residue_with_mrp) already exercise the SRP-vs-MRP
// agreement path; this test is a focused regression locking in num_residues,
// base, and per-residue values for a known input pair.
// ============================================================================

TEST_CASE("MRP output round-trip: hazeMulMrp result via fhetch::result(name, MRP&)",
          "[integration]") {
    MrpDriver d;
    d.setup();

    std::vector<std::vector<uint64_t>> a;
    std::vector<std::vector<uint64_t>> b;
    std::vector<std::vector<uint64_t>> expected;
    make_two_residue_inputs(d.base, a, b, expected, /*seed_a=*/0xFACEFEEDULL,
                            /*seed_b=*/0xDEADC0DEULL, mul_mod);

    auto da = haze::test::allocate_and_h2d_residues(a);
    auto db = haze::test::allocate_and_h2d_residues(b);
    auto dst = haze::test::allocate_dst_residues(MrpDriver::kNumResidues, kBytes);

    d.mul(dst, haze::test::to_const(da), haze::test::to_const(db));

    // Trigger replay via the SRP path so the bridge writes the multi-tower
    // template and the simulator emits the corresponding
    // serialized_probes/haze_mrp_out_<addr-derived>.ct.
    check_against_per_residue(d, dst, expected);

    // Pull the MRP-shape view directly. Asserts num_residues, base, and
    // per-residue values; cross-checks that SRP ground truth and MRP view
    // agree exactly. Helper locates the group by content, no name needed.
    haze::test::check_mrp_against_per_residue(d.base, expected);

    haze::test::free_all_residues(da);
    haze::test::free_all_residues(db);
    haze::test::free_all_residues(dst);
}

// ============================================================================
// Regression: invalidate() must drop pending_mrp_groups_ entries for the freed
// addr so the stale group can't surface (or get silently rebound on recycle).
// ============================================================================

TEST_CASE("hazeFree mid-recording on MRP output addrs does not break replay", "[integration]") {
    MrpDriver d;
    d.setup();

    std::vector<std::vector<uint64_t>> a;
    std::vector<std::vector<uint64_t>> b;
    std::vector<std::vector<uint64_t>> expected;
    make_two_residue_inputs(d.base, a, b, expected, /*seed_a=*/0xCAFE1111ULL,
                            /*seed_b=*/0xBABE2222ULL, add_mod);

    auto d_a = haze::test::allocate_and_h2d_residues(a);
    auto d_b = haze::test::allocate_and_h2d_residues(b);
    // dst is pre-allocated so the post-free hazeAddMrp does not recycle
    // [intt_dst...] from pool_free_ before D2H triggers replay; otherwise
    // lookup_or_create_locked rebinds them via shadow and hides the bug.
    auto dst = haze::test::allocate_dst_residues(MrpDriver::kNumResidues, kBytes);
    auto intt_dst = haze::test::allocate_dst_residues(MrpDriver::kNumResidues, kBytes);
    d.intt(intt_dst, haze::test::to_const(d_a));

    // Pre-fix: leaks a stale pending_mrp_groups_ entry. At the D2H below,
    // replay_and_populate's group walk hits MissingPolyMapBinding ->
    // HAZE_ERROR_LAUNCH_FAILURE because poly_map_[intt_dst...] are gone.
    haze::test::free_all_residues(intt_dst);

    d.add(dst, haze::test::to_const(d_a), haze::test::to_const(d_b));

    check_against_per_residue_with_mrp(d, dst, expected);

    haze::test::free_all_residues(d_a);
    haze::test::free_all_residues(d_b);
    haze::test::free_all_residues(dst);
}

TEST_CASE("hazeFree mid-recording does not leak MRP group through allocator recycle",
          "[integration]") {
    MrpDriver d;
    d.setup();

    std::vector<std::vector<uint64_t>> src1(MrpDriver::kNumResidues);
    for (std::size_t i = 0; i < MrpDriver::kNumResidues; ++i)
        src1[i] = haze::test::make_residue(d.base[i], 0xA1B2C300ULL + i, kRingDim);

    std::vector<std::vector<uint64_t>> a;
    std::vector<std::vector<uint64_t>> b;
    std::vector<std::vector<uint64_t>> expected;
    make_two_residue_inputs(d.base, a, b, expected, /*seed_a=*/0xCAFEBABEULL,
                            /*seed_b=*/0xBADDCAFEULL, add_mod);

    // Leftover .ct files from prior binary invocations would inflate the
    // mrp-file count below; the bridge's own remove_all runs before the
    // bridge is initialised on the first test, so wipe explicitly.
    namespace fs = std::filesystem;
    const auto probes_dir = niobium::compiler().get_program_directory() / "serialized_probes";
    std::error_code ec;
    fs::remove_all(probes_dir, ec);

    auto d_src1 = haze::test::allocate_and_h2d_residues(src1);
    auto ntt_dst = haze::test::allocate_dst_residues(MrpDriver::kNumResidues, kBytes);
    d.ntt(ntt_dst, haze::test::to_const(d_src1));

    haze::test::free_all_residues(ntt_dst);

    auto d_a = haze::test::allocate_and_h2d_residues(a);
    auto d_b = haze::test::allocate_and_h2d_residues(b);
    auto recycled = haze::test::allocate_dst_residues(MrpDriver::kNumResidues, kBytes);
    d.add(recycled, haze::test::to_const(d_a), haze::test::to_const(d_b));

    check_against_per_residue(d, recycled, expected);

    // Pre-fix the leaked NTT group emits a second .ct under the original
    // leading addr with garbled residues; with the fix only the AddMrp file lands.
    REQUIRE(fs::exists(probes_dir));
    std::size_t mrp_out_files = 0;
    for (const auto &entry : fs::directory_iterator(probes_dir)) {
        if (entry.is_regular_file() && entry.path().stem().string().starts_with("haze_mrp_out_"))
            ++mrp_out_files;
    }
    REQUIRE(mrp_out_files == 1);

    haze::test::check_mrp_against_per_residue(d.base, expected);

    haze::test::free_all_residues(d_src1);
    haze::test::free_all_residues(d_a);
    haze::test::free_all_residues(d_b);
    haze::test::free_all_residues(recycled);
}
