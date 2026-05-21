// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Phase-by-phase parity tests for the haze CKKS bootstrap helpers,
// against OpenFHE's reference implementation. Tolerance is slot-level
// (1e-2), matching FIDESlib's CKKS_BOOTSTRAP_PRECISION.

#include "bootstrap.hpp"
#include "openfhe.h"
#include "ops.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>
#include <scheme/ckksrns/ckksrns-fhe.h>
#include <vector>

namespace {

constexpr double kBootstrapTolerance = 1e-2;

haze::test::ops::OpCtx make_bootstrap_ctx() {
    using namespace lbcrypto;
    using namespace haze::test::ops;

    // Match FIDESlib's CKKSBootstrapTest fixture: depth=25, slots=8,
    // FIXEDAUTO, UNIFORM_TERNARY, scalingMod=50.
    auto ctx = make_ctx({
        .mode = FIXEDAUTO,
        .mult_depth = 25,
        .scaling_mod_size = 50,
        .batch_size = 8,
        .with_relin_key = true,
        .rotate_indices = {},
        .ring_dim = 1u << 16, // 65536 — halves polynomial size vs auto 2^17
    });
    ctx.cc->Enable(ADVANCEDSHE);
    ctx.cc->Enable(FHE);
    return ctx;
}

// Compare a haze Ct's per-RNS-limb data against an OpenFHE reference.
// Asserts towers, NSD, and every uint64 coefficient.
void assert_rns_equal(const haze::test::ops::OpCtx &ctx, const haze::test::ops::Ct &haze,
                      const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &ref,
                      const std::string &label) {
    REQUIRE(haze.towers() == ref->GetElements()[0].GetNumOfElements());
    const auto haze_bytes = haze::test::ops::d2h_ct(ctx, haze);
    for (std::size_t elem = 0; elem < 2; ++elem) {
        const auto &ref_dcrt = ref->GetElements()[elem];
        const auto &haze_chain = (elem == 0) ? haze_bytes.c0 : haze_bytes.c1;
        for (std::size_t t = 0; t < haze.towers(); ++t) {
            const auto &ref_np = ref_dcrt.GetElementAtIndex(static_cast<usint>(t));
            const auto &ref_vals = ref_np.GetValues();
            REQUIRE(haze_chain[t].size() == ref_vals.GetLength());
            std::size_t mism = 0;
            for (std::size_t i = 0; i < haze_chain[t].size(); ++i) {
                if (haze_chain[t][i] != ref_vals[i].template ConvertToInt<std::uint64_t>())
                    ++mism;
            }
            INFO(label << " elem=" << elem << " tower=" << t << " mismatches=" << mism);
            REQUIRE(mism == 0);
        }
    }
}

} // namespace

TEST_CASE("phase1 add_const RNS parity vs cc->EvalAdd(ct, scalar)", "[integration][e2e]") {
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx();

    auto pt = ctx.cc->MakeCKKSPackedPlaintext(
        std::vector<double>{0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8}, 1, 1);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    auto ref = ctx.cc->EvalAdd(ct, 0.5);
    auto haze_ct = ops::h2d_ct(ctx, ct);
    auto haze_out = ops::add_const_for_test(ctx, haze_ct, 0.5);
    assert_rns_equal(ctx, haze_out, ref, "add_const");
}

TEST_CASE("phase1 mult_by_const RNS parity vs cc->EvalMult(ct, scalar) + rescale",
          "[integration][e2e]") {
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx();

    auto pt = ctx.cc->MakeCKKSPackedPlaintext(
        std::vector<double>{0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8}, 1, 1);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    auto ref = ctx.cc->EvalMult(ct, 0.5);
    auto haze_ct = ops::h2d_ct(ctx, ct);
    auto haze_out = ops::mult_by_const_for_test(ctx, haze_ct, 0.5);
    assert_rns_equal(ctx, haze_out, ref, "mult_by_const");
}

TEST_CASE("phase1 mult_monomial RNS parity vs cc->GetScheme()->MultByMonomialInPlace",
          "[integration][e2e]") {
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx();

    auto pt = ctx.cc->MakeCKKSPackedPlaintext(
        std::vector<double>{0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8}, 1, 1);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    auto ref = ct->Clone();
    ctx.cc->GetScheme()->MultByMonomialInPlace(ref, 24);
    auto haze_ct = ops::h2d_ct(ctx, ct);
    auto haze_out = ops::mult_monomial_for_test(ctx, haze_ct, 24);
    assert_rns_equal(ctx, haze_out, ref, "mult_monomial");
}

TEST_CASE("phase1b add_const RNS parity when ct has NSD=2 (post-square)", "[integration][e2e]") {
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx();

    auto pt = ctx.cc->MakeCKKSPackedPlaintext(
        std::vector<double>{0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8}, 1, 1);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    // Square once to get a ct at NSD=2.
    ctx.cc->EvalSquareInPlace(ct);
    auto ref = ctx.cc->EvalAdd(ct, 0.5);
    std::cerr << "phase1b post-square NSD=" << ct->GetNoiseScaleDeg() << " ref NSD="
              << ref->GetNoiseScaleDeg() << " ref towers="
              << ref->GetElements()[0].GetNumOfElements() << "\n";
    auto haze_ct = ops::h2d_ct(ctx, ct);
    auto haze_out = ops::add_const_for_test(ctx, haze_ct, 0.5);
    assert_rns_equal(ctx, haze_out, ref, "add_const_NSD2");
}

TEST_CASE("phase1c add(ct,ct) RNS parity at NSD=2 (doubling post-square)", "[integration][e2e]") {
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx();

    auto pt = ctx.cc->MakeCKKSPackedPlaintext(
        std::vector<double>{0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8}, 1, 1);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    ctx.cc->EvalSquareInPlace(ct);
    auto ref = ctx.cc->EvalAdd(ct, ct);  // doubling
    auto haze_ct = ops::h2d_ct(ctx, ct);
    auto haze_out = ops::add(ctx, haze_ct, haze_ct);  // self-add
    std::cerr << "phase1c haze NSD=" << haze_out.noise_scale_deg() << " ref NSD="
              << ref->GetNoiseScaleDeg() << "\n";
    assert_rns_equal(ctx, haze_out, ref, "add_self_NSD2");
}

TEST_CASE("phase2 apply_double_angle_iterations RNS parity vs OpenFHE", "[integration][e2e]") {
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx();

    auto pt = ctx.cc->MakeCKKSPackedPlaintext(
        std::vector<double>{0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8}, 1, 1);
    auto ct_in = ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    constexpr std::uint32_t numIter = 3;
    constexpr double twoPi = 2.0 * M_PI;
    auto ref = ct_in->Clone();
    for (std::int32_t i = 1 - static_cast<std::int32_t>(numIter); i <= 0; ++i) {
        const double scalar = -std::pow(twoPi, -std::pow(2.0, i));
        ctx.cc->EvalSquareInPlace(ref);
        ctx.cc->EvalAddInPlace(ref, ctx.cc->EvalAdd(ref, scalar));
        ctx.cc->ModReduceInPlace(ref);
    }
    auto haze_ct = ops::h2d_ct(ctx, ct_in);
    ops::apply_double_angle_for_test(ctx, haze_ct, numIter);
    assert_rns_equal(ctx, haze_ct, ref, "apply_double_angle_3iter");
}

TEST_CASE("phase1 square_ct RNS parity vs cc->EvalSquare(ct)", "[integration][e2e]") {
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx();

    auto pt = ctx.cc->MakeCKKSPackedPlaintext(
        std::vector<double>{0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8}, 1, 1);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    auto ref = ctx.cc->EvalSquare(ct);
    auto haze_ct = ops::h2d_ct(ctx, ct);
    auto haze_out = ops::square_ct_for_test(ctx, haze_ct);
    assert_rns_equal(ctx, haze_out, ref, "square_ct");
}

TEST_CASE("phase2a clone_ct of trace-input preserves polynomial bytes", "[integration][e2e]") {
    // Sanity: clone_ct of an h2d'd ct (trace input) should produce a Ct
    // with identical polynomial values. This is the easy case.
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx();

    auto pt = ctx.cc->MakeCKKSPackedPlaintext(
        std::vector<double>{0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8}, 1, 1);
    auto ct_in = ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    auto haze_ct = ops::h2d_ct(ctx, ct_in);
    auto haze_clone = ops::clone_ct(ctx, haze_ct);
    assert_rns_equal(ctx, haze_clone, ct_in, "clone_of_input");
}

TEST_CASE("phase2b clone_ct of trace-output preserves polynomial bytes",
          "[integration][e2e]") {
    // The failing case: clone_ct of a Ct produced by a haze compute op.
    // If this fails, D2D on trace-output Allocs doesn't replay correctly.
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx();

    auto pt = ctx.cc->MakeCKKSPackedPlaintext(
        std::vector<double>{0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8}, 1, 1);
    auto ct_in = ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    auto ref = ctx.cc->EvalSquare(ct_in);  // OpenFHE reference for ct_in²

    auto haze_ct = ops::h2d_ct(ctx, ct_in);
    auto haze_sq = ops::square_ct_for_test(ctx, haze_ct);  // trace-OUTPUT
    auto haze_clone = ops::clone_ct(ctx, haze_sq);          // clone of trace-output
    assert_rns_equal(ctx, haze_clone, ref, "clone_of_trace_output");
}

TEST_CASE("phase4 eval_chebyshev_series slot-level at degree 12 (k=3,m=2)",
          "[integration][e2e]") {
    // Larger Chebyshev test exercising more recursion depth before
    // attempting the full bootstrap's degree-80 Chebyshev.
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx();

    const std::vector<double> x_vals = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(x_vals, 1, 1);
    auto ct_in = ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    std::vector<double> coeffs(13);
    for (std::size_t i = 0; i < coeffs.size(); ++i)
        coeffs[i] = 1.0 / (1 << (i + 1));
    auto ref = ctx.cc->EvalChebyshevSeries(ct_in, coeffs, -1.0, 1.0);
    REQUIRE(ref);
    auto haze_ct = ops::h2d_ct(ctx, ct_in);
    auto haze_out = ops::eval_chebyshev_series_for_test(ctx, haze_ct, coeffs);
    REQUIRE(haze_out.towers() == ref->GetElements()[0].GetNumOfElements());
    const auto haze_bytes = ops::d2h_ct(ctx, haze_out);
    auto ct_haze = ref->Clone();
    ops::inject_ct(ctx, haze_bytes, ct_haze);
    Plaintext pt_haze, pt_ref;
    ctx.cc->Decrypt(ctx.keys.secretKey, ct_haze, &pt_haze);
    ctx.cc->Decrypt(ctx.keys.secretKey, ref, &pt_ref);
    pt_haze->SetLength(x_vals.size());
    pt_ref->SetLength(x_vals.size());
    const auto haze_slots = pt_haze->GetRealPackedValue();
    const auto ref_slots = pt_ref->GetRealPackedValue();
    for (std::size_t i = 0; i < x_vals.size(); ++i) {
        INFO("slot " << i << " haze=" << haze_slots[i] << " ref=" << ref_slots[i]);
        REQUIRE_THAT(haze_slots[i], Catch::Matchers::WithinAbs(ref_slots[i], 1e-2));
    }
}

TEST_CASE("phase3 eval_chebyshev_series slot-level vs cc->EvalChebyshevSeries",
          "[integration][e2e]") {
    // Slot-level near-equivalence (not bit-exact). FIDESlib doesn't
    // achieve bit-exact vs OpenFHE either — they target precision 1e-2.
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx();

    const std::vector<double> x_vals = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(x_vals, 1, 1);
    auto ct_in = ctx.cc->Encrypt(ctx.keys.publicKey, pt);

    // ComputeDegreesPS(5) → k=2, m=2. Minimal PS path.
    std::vector<double> coeffs = {0.5, 0.25, 0.125, 0.0625, 0.03125, 0.015625};
    auto ref = ctx.cc->EvalChebyshevSeries(ct_in, coeffs, -1.0, 1.0);
    REQUIRE(ref);

    auto haze_ct = ops::h2d_ct(ctx, ct_in);
    auto haze_out = ops::eval_chebyshev_series_for_test(ctx, haze_ct, coeffs);
    REQUIRE(haze_out.towers() == ref->GetElements()[0].GetNumOfElements());
    const auto haze_bytes = ops::d2h_ct(ctx, haze_out);

    // Inject haze result into a Ciphertext shell built from ref's
    // metadata, decrypt, and compare slot-wise.
    auto ct_haze = ref->Clone();
    ops::inject_ct(ctx, haze_bytes, ct_haze);
    Plaintext pt_haze, pt_ref;
    ctx.cc->Decrypt(ctx.keys.secretKey, ct_haze, &pt_haze);
    ctx.cc->Decrypt(ctx.keys.secretKey, ref, &pt_ref);
    pt_haze->SetLength(x_vals.size());
    pt_ref->SetLength(x_vals.size());
    const auto haze_slots = pt_haze->GetRealPackedValue();
    const auto ref_slots = pt_ref->GetRealPackedValue();
    for (std::size_t i = 0; i < x_vals.size(); ++i) {
        INFO("slot " << i << " haze=" << haze_slots[i] << " ref=" << ref_slots[i]);
        REQUIRE_THAT(haze_slots[i], Catch::Matchers::WithinAbs(ref_slots[i], 1e-2));
    }
}

TEST_CASE("ckks bootstrap make_bootstrap_keys extracts keys + plaintexts", "[integration][e2e]") {
    using namespace lbcrypto;
    namespace ops = haze::test::ops;

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx();

    constexpr std::uint32_t slots = 8;
    auto bk = ops::make_bootstrap_keys(ctx, ctx.cc, ctx.keys.secretKey, slots);

    REQUIRE(bk.params.slots == slots);
    REQUIRE(bk.cts_matrices.size() == slots);
    REQUIRE(bk.stc_matrices.size() == slots);
    REQUIRE(!bk.relin_key.a_limbs.empty());
    REQUIRE(!bk.conjugation_key.a_limbs.empty());
    REQUIRE(!bk.rotation_keys.empty());
    for (const auto &row : bk.cts_matrices)
        REQUIRE(!row.empty());
    for (const auto &row : bk.stc_matrices)
        REQUIRE(!row.empty());
}

TEST_CASE("ckks bootstrap reference (OpenFHE EvalBootstrap) decrypts", "[integration][e2e]") {
    // Sanity check the OpenFHE reference path itself works at the chosen
    // parameters. If this fails the bootstrap parameters are wrong; if it
    // passes, our haze parity tests have a valid target.
    using namespace lbcrypto;
    namespace ops = haze::test::ops;

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx();

    constexpr std::uint32_t slots = 8;
    ctx.cc->EvalBootstrapSetup({1, 1}, {0, 0}, slots, 11);
    ctx.cc->EvalBootstrapKeyGen(ctx.keys.secretKey, slots);

    const std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    auto ct_refreshed = ctx.cc->EvalBootstrap(ct);
    REQUIRE(ct_refreshed);

    Plaintext out;
    ctx.cc->Decrypt(ctx.keys.secretKey, ct_refreshed, &out);
    out->SetLength(v.size());
    const auto got = out->GetRealPackedValue();
    for (std::size_t i = 0; i < v.size(); ++i) {
        INFO("slot " << i << " got=" << got[i] << " ref=" << v[i]);
        REQUIRE_THAT(got[i], Catch::Matchers::WithinAbs(v[i], kBootstrapTolerance));
    }
}

TEST_CASE("ckks bootstrap linear_transform slot parity vs EvalLinearTransform",
          "[integration][e2e]") {
    // Hoisted linear_transform compared to OpenFHE's EvalLinearTransform.
    using namespace lbcrypto;
    namespace ops = haze::test::ops;

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx();

    constexpr std::uint32_t slots = 8;
    auto bk = ops::make_bootstrap_keys(ctx, ctx.cc, ctx.keys.secretKey, slots);

    // Encrypt at level=1 so ct's tower count matches the CtS plaintexts'
    // Q-portion (the plaintexts encoded at lEnc = L0 - 2).
    const std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v, 1, 1);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);

    auto fhe_base = ctx.cc->GetScheme()->GetFHE();
    auto fhe_ckks = std::dynamic_pointer_cast<FHECKKSRNS>(fhe_base);
    REQUIRE(fhe_ckks);
    const auto &precom_map = fhe_ckks->GetBootPrecomMap();
    const auto &precom = *precom_map.at(slots);
    auto ct_ref = fhe_ckks->EvalLinearTransform(precom.m_U0hatTPre, ct);
    REQUIRE(ct_ref);

    // CtS output is intermediate-state (slot-encoded coefficients) and
    // does not decrypt to the original v. Compare RNS limbs directly.
    auto haze_ct = ops::h2d_ct(ctx, ct);
    auto haze_out = ops::linear_transform(ctx, bk, bk.cts_matrices, haze_ct);
    const auto haze_bytes = ops::d2h_ct(ctx, haze_out);

    const std::size_t ref_q_towers = ct_ref->GetElements()[0].GetNumOfElements();
    REQUIRE(haze_out.towers() == ref_q_towers);
    for (std::size_t elem = 0; elem < 2; ++elem) {
        const auto &ref_dcrt = ct_ref->GetElements()[elem];
        const auto &haze_chain = (elem == 0) ? haze_bytes.c0 : haze_bytes.c1;
        for (std::size_t t = 0; t < ref_q_towers; ++t) {
            const auto &ref_np = ref_dcrt.GetElementAtIndex(static_cast<usint>(t));
            const auto &ref_vals = ref_np.GetValues();
            const auto &haze_limb = haze_chain[t];
            REQUIRE(haze_limb.size() == ref_vals.GetLength());
            std::size_t mismatches = 0;
            for (std::size_t i = 0; i < haze_limb.size(); ++i) {
                if (haze_limb[i] != ref_vals[i].template ConvertToInt<std::uint64_t>())
                    ++mismatches;
            }
            INFO("elem=" << elem << " tower=" << t << " mismatches=" << mismatches
                         << "/" << haze_limb.size());
            REQUIRE(mismatches == 0);
        }
    }
}

TEST_CASE("ckks bootstrap mod_raise RNS parity vs OpenFHE basis extension",
          "[integration][e2e]") {
    // Take a fresh ciphertext, drop to a single-tower state matching what
    // bootstrap does after ModReduceInternalInPlace, then raise it via
    // both paths and compare RNS limbs.
    using namespace lbcrypto;
    namespace ops = haze::test::ops;

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx();

    constexpr std::uint32_t slots = 8;
    auto bk = ops::make_bootstrap_keys(ctx, ctx.cc, ctx.keys.secretKey, slots);

    const std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
    auto ct_full = ctx.cc->Encrypt(ctx.keys.publicKey, pt);

    // Construct ct_q0: take only tower 0 of c0 / c1 from ct_full (in
    // COEFFICIENT form), then build an OpenFHE Ciphertext at a fresh
    // single-tower elementParams.
    auto cryptoParams =
        std::dynamic_pointer_cast<CryptoParametersCKKSRNS>(ctx.cc->GetCryptoParameters());
    auto fullParams = cryptoParams->GetElementParams();
    std::vector<NativeInteger> q0_moduli = {fullParams->GetParams()[0]->GetModulus()};
    std::vector<NativeInteger> q0_roots = {fullParams->GetParams()[0]->GetRootOfUnity()};
    auto q0_params =
        std::make_shared<ILDCRTParams<DCRTPoly::Integer>>(ctx.cc->GetCyclotomicOrder(), q0_moduli,
                                                          q0_roots);

    // Build the haze single-tower ct (in EVAL form, from ct_full's tower 0).
    std::vector<std::vector<std::uint64_t>> c0_q0(1, std::vector<std::uint64_t>(ctx.ring_dim));
    std::vector<std::vector<std::uint64_t>> c1_q0(1, std::vector<std::uint64_t>(ctx.ring_dim));
    {
        const auto &c0 = ct_full->GetElements()[0].GetElementAtIndex(0);
        const auto &c1 = ct_full->GetElements()[1].GetElementAtIndex(0);
        REQUIRE(c0.GetFormat() == Format::EVALUATION);
        for (std::size_t i = 0; i < ctx.ring_dim; ++i) {
            c0_q0[0][i] = c0.GetValues()[i].template ConvertToInt<std::uint64_t>();
            c1_q0[0][i] = c1.GetValues()[i].template ConvertToInt<std::uint64_t>();
        }
    }
    ops::Allocs haze_c0(c0_q0);
    ops::Allocs haze_c1(c1_q0);
    ops::Ct haze_ct{std::move(haze_c0), std::move(haze_c1), 1,
                    static_cast<std::uint32_t>(ct_full->GetNoiseScaleDeg())};

    auto haze_raised = ops::mod_raise(ctx, bk, haze_ct);
    const auto haze_bytes = ops::d2h_ct(ctx, haze_raised);

    // OpenFHE reference: take tower 0 of ct_full, extend to full Q chain.
    auto ct_q0 = ct_full->Clone();
    {
        auto elements = ct_q0->GetElements();
        for (auto &dcrt : elements) {
            dcrt.SetFormat(COEFFICIENT);
            DCRTPoly tmp(dcrt.GetElementAtIndex(0), q0_params);
            tmp.SetFormat(EVALUATION);
            dcrt = std::move(tmp);
        }
        ct_q0->SetElements(std::move(elements));
    }
    auto ct_raised_ref = ct_q0->Clone();
    {
        auto elements = ct_raised_ref->GetElements();
        for (auto &dcrt : elements) {
            dcrt.SetFormat(COEFFICIENT);
            DCRTPoly tmp(dcrt.GetElementAtIndex(0), fullParams);
            tmp.SetFormat(EVALUATION);
            dcrt = std::move(tmp);
        }
        ct_raised_ref->SetElements(std::move(elements));
    }

    REQUIRE(haze_raised.towers() == ct_raised_ref->GetElements()[0].GetNumOfElements());
    for (std::size_t elem = 0; elem < 2; ++elem) {
        const auto &ref_dcrt = ct_raised_ref->GetElements()[elem];
        const auto &haze_chain = (elem == 0) ? haze_bytes.c0 : haze_bytes.c1;
        for (std::size_t t = 0; t < haze_raised.towers(); ++t) {
            const auto &ref_np = ref_dcrt.GetElementAtIndex(static_cast<usint>(t));
            const auto &ref_vals = ref_np.GetValues();
            std::size_t mismatches = 0;
            for (std::size_t i = 0; i < ctx.ring_dim; ++i) {
                if (haze_chain[t][i] != ref_vals[i].template ConvertToInt<std::uint64_t>())
                    ++mismatches;
            }
            INFO("elem=" << elem << " tower=" << t << " mismatches=" << mismatches);
            REQUIRE(mismatches == 0);
        }
    }
}

TEST_CASE("ckks bootstrap haze ops::bootstrap slot parity vs EvalBootstrap",
          "[integration][e2e]") {
    // Round-trip ct through haze, run ops::bootstrap, decrypt and compare
    // slot-wise to cc->EvalBootstrap on the same input.
    using namespace lbcrypto;
    namespace ops = haze::test::ops;

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx();

    constexpr std::uint32_t slots = 8;
    auto bk = ops::make_bootstrap_keys(ctx, ctx.cc, ctx.keys.secretKey, slots);

    const std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);

    // OpenFHE reference.
    auto ct_ref = ctx.cc->EvalBootstrap(ct);
    REQUIRE(ct_ref);
    Plaintext pt_ref;
    ctx.cc->Decrypt(ctx.keys.secretKey, ct_ref, &pt_ref);
    pt_ref->SetLength(v.size());
    const auto slots_ref = pt_ref->GetRealPackedValue();

    auto haze_ct = ops::h2d_ct(ctx, ct);
    auto haze_refreshed = ops::bootstrap(ctx, bk, haze_ct, ops::BootstrapVariant::Standard);
    const auto haze_bytes = ops::d2h_ct(ctx, haze_refreshed);

    // Inject the haze result into a shell and decrypt.
    auto ct_haze = ct_ref->Clone();
    ops::inject_ct(ctx, haze_bytes, ct_haze);
    Plaintext pt_haze;
    ctx.cc->Decrypt(ctx.keys.secretKey, ct_haze, &pt_haze);
    pt_haze->SetLength(v.size());
    const auto slots_haze = pt_haze->GetRealPackedValue();

    for (std::size_t i = 0; i < v.size(); ++i) {
        INFO("slot " << i << " haze=" << slots_haze[i] << " ref=" << slots_ref[i]);
        REQUIRE_THAT(slots_haze[i],
                     Catch::Matchers::WithinAbs(slots_ref[i], kBootstrapTolerance));
    }
}
