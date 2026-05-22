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
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>
#include <iostream>
#include <scheme/ckksrns/ckksrns-fhe.h>
#include <string>
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
        .ring_dim = 1u << 16, // 65536
    });
    ctx.cc->Enable(ADVANCEDSHE);
    ctx.cc->Enable(FHE);
    return ctx;
}

// Tiny-ring ctx for phase5 profiling. Drops security level so OpenFHE
// accepts the small N. Not for correctness — only for timing.
haze::test::ops::OpCtx make_bootstrap_ctx_tiny(std::uint32_t ring_dim) {
    using namespace lbcrypto;
    using namespace haze::test::ops;
    auto ctx = make_ctx({
        .mode = FIXEDAUTO,
        .mult_depth = 25,
        .scaling_mod_size = 50,
        .batch_size = 8,
        .with_relin_key = true,
        .rotate_indices = {},
        .ring_dim = ring_dim,
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

TEST_CASE("phase23 compute_cheby_tree on a conjugate-add input",
          "[integration][e2e]") {
    // Phase 15 validated compute_cheby_tree on a fresh ct. Phase 23 does
    // it on a conjugate-add ct (what eval_mod actually hands to Chebyshev).
    using namespace lbcrypto;
    namespace ops = haze::test::ops;

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx_tiny(1u << 11);
    constexpr std::uint32_t slots = 8;
    auto bk = ops::make_bootstrap_keys(ctx, ctx.cc, ctx.keys.secretKey, slots);

    const std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    const std::int32_t conj_idx = 2 * static_cast<std::int32_t>(ctx.ring_dim) - 1;
    auto conj_add = ctx.cc->EvalAdd(ct, ctx.cc->EvalAtIndex(ct, conj_idx));

    auto check_tree = [&](const std::string &label, const std::vector<double> &coeffs) {
        auto ref_tree = ctx.cc->EvalChebyPolys(conj_add, coeffs, -1.0, 1.0);
        REQUIRE(ref_tree);
        auto haze_ct = ops::h2d_ct(ctx, conj_add);
        auto my_tree = ops::compute_cheby_tree_for_test(ctx, haze_ct, coeffs);
        REQUIRE(my_tree.k == ref_tree->k);
        REQUIRE(my_tree.m == ref_tree->m);
        for (std::size_t i = 0; i < my_tree.T.size(); ++i)
            assert_rns_equal(ctx, my_tree.T[i], ref_tree->powersRe[i],
                             "phase23 " + label + " T[" + std::to_string(i) + "]");
        for (std::size_t i = 0; i < my_tree.T2.size(); ++i)
            assert_rns_equal(ctx, my_tree.T2[i], ref_tree->powers2Re[i],
                             "phase23 " + label + " T2[" + std::to_string(i) + "]");
        if (ref_tree->power2km1Re)
            assert_rns_equal(ctx, my_tree.T2km1, ref_tree->power2km1Re,
                             "phase23 " + label + " T2km1");
    };

    check_tree("degree=5", {0.5, 0.25, 0.125, 0.0625, 0.03125, 0.015625});
    {
        std::vector<double> c12(13, 0.0);
        for (std::size_t i = 0; i <= 12; ++i) c12[i] = 1.0 / static_cast<double>(i + 1);
        check_tree("degree=12", c12);
    }
    {
        std::vector<double> c84(85, 0.0);
        for (std::size_t i = 0; i <= 84; ++i) c84[i] = (i & 1) ? -0.001 : 0.05;
        check_tree("degree=84", c84);
    }
}

TEST_CASE("phase22 Chebyshev byte-parity on a conjugate-add input",
          "[integration][e2e]") {
    // Phase 14 validated eval_chebyshev_series on a fresh ct input.
    // Phase 22: same byte-parity but with the conjugate-add of fresh ct as
    // input — exactly what eval_mod feeds into the chebyshev step.
    using namespace lbcrypto;
    namespace ops = haze::test::ops;

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx_tiny(1u << 11);
    constexpr std::uint32_t slots = 8;
    auto bk = ops::make_bootstrap_keys(ctx, ctx.cc, ctx.keys.secretKey, slots);

    const std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    const std::int32_t conj_idx = 2 * static_cast<std::int32_t>(ctx.ring_dim) - 1;
    auto conj_add = ctx.cc->EvalAdd(ct, ctx.cc->EvalAtIndex(ct, conj_idx));

    // Match eval_mod's coefficient list.
    static const std::vector<double> coefficients{
        0.15421426400235561,    -0.0037671538417132409,  0.16032011744533031,
        -0.0034539657223742453, 0.17711481926851286,     -0.0027619720033372291,
        0.19949802549604084,    -0.0015928034845171929,  0.21756948616367638,
        0.00010729951647566607, 0.21600427371240055,     0.0022171399198851363,
        0.17647500259573556,    0.0042856217194480991,   0.086174491919472254,
        0.0054640252312780444,  -0.046667988130649173,   0.0047346914623733714,
        -0.17712686172280406,   0.0016205080004247200,   -0.22703114241338604,
        -0.0028145845916205865, -0.13123089730288540,    -0.0056345646688793190,
        0.078818395388692147,   -0.0037868875028868542,  0.23226434602675575,
        0.0021116338645426574,  0.13985510526186795,     0.0059365649669377071,
        -0.13918475289368595,   0.0018580676740836374,   -0.23254376365752788,
        -0.0054103844866927788, 0.056840618403875359,    -0.0035227192748552472,
        0.25667909012207590,    0.0055029673963982112,   -0.073334392714092062,
        0.0027810273357488265,  -0.24912792167850559,    -0.0069524866497120566,
        0.21288810409948347,    0.0017810057298691725,   0.088760951809475269,
        0.0055957188940032095,  -0.31937177676259115,    -0.0087539416335935556,
        0.34748800245527145,    0.0075378299617709235,   -0.25116537379803394,
        -0.0047285674679876204, 0.13970502851683486,     0.0023672533925155220,
        -0.063649401080083698,  -0.00098993213448982727, 0.024597838934816905,
        0.00035553235917057483, -0.0082485030307578155,  -0.00011176184313622549,
        0.0024390574829093264,  0.000031180384864488629, -0.00064373524734389861,
        -7.8036008952377965e-6, 0.00015310015145922058,  1.7670804180220134e-6,
        -0.000033066844379476900, -3.6460909134279425e-7, 6.5276969021754105e-6,
        6.8957843666189918e-8,  -1.1842811187642386e-6,  -1.2015133285307312e-8,
        1.9839339947648331e-7,  1.9372045971100854e-9,   -3.0815418032523593e-8,
        -2.9013806338735810e-10, 4.4540904298173700e-9,  4.0505136697916078e-11,
        -6.0104912807134771e-10, -5.2873323696828491e-12, 7.5943206779351725e-11,
        6.4679566322060472e-13, -9.0081200925539902e-12, -7.4396949275292252e-14,
        1.0057423059167244e-12, 8.1701187638005194e-15,  -1.0611736208855373e-13,
        -8.9597492970451533e-16, 1.1421575296031385e-14};

    auto check = [&](const std::string &label, const std::vector<double> &coeffs) {
        auto ref = ctx.cc->EvalChebyshevSeries(conj_add, coeffs, -1.0, 1.0);
        auto haze_ct = ops::h2d_ct(ctx, conj_add);
        auto out = ops::eval_chebyshev_series_for_test(ctx, haze_ct, coeffs);
        assert_rns_equal(ctx, out, ref, "phase22 " + label + " on conjugate-add");
    };

    check("degree=5", {0.5, 0.25, 0.125, 0.0625, 0.03125, 0.015625});
    {
        std::vector<double> c12(13, 0.0);
        for (std::size_t i = 0; i <= 12; ++i) c12[i] = 1.0 / static_cast<double>(i + 1);
        check("degree=12", c12);
    }
    check("degree=84 g_coefficientsUniform", coefficients);
}

TEST_CASE("phase21 rescale byte-parity vs cc->GetScheme()->ModReduceInternalInPlace",
          "[integration][e2e]") {
    // Validate ops::rescale against OpenFHE's ModReduceInternalInPlace.
    using namespace lbcrypto;
    namespace ops = haze::test::ops;

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx_tiny(1u << 11);

    auto fresh = [&]() {
        std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
        auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
        return ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    };
    auto bump_nsd2 = [&](Ciphertext<DCRTPoly> ct) { return ctx.cc->EvalMult(ct, 1.0); };

    auto check = [&](const std::string &label, Ciphertext<DCRTPoly> ct) {
        auto ref = ct->Clone();
        ctx.cc->GetScheme()->ModReduceInternalInPlace(ref, 1);

        auto haze_ct = ops::h2d_ct(ctx, ct);
        auto out = ops::rescale(ctx, haze_ct);
        assert_rns_equal(ctx, out, ref, "phase21 " + label);
    };

    check("NSD=1", fresh());
    check("NSD=2", bump_nsd2(fresh()));
}

TEST_CASE("phase20 conjugate-add byte-parity (the eval_mod head step)",
          "[integration][e2e]") {
    // Sparsely-packed eval_mod's first op is: ctxtEnc = EvalAdd(ct, Conjugate(ct)).
    // Phase 19 validates conjugate alone byte-exact. Phase 20 validates
    // the full conjugate-add composition.
    using namespace lbcrypto;
    namespace ops = haze::test::ops;

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx_tiny(1u << 11);
    constexpr std::uint32_t slots = 8;
    auto bk = ops::make_bootstrap_keys(ctx, ctx.cc, ctx.keys.secretKey, slots);

    const std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);

    const std::int32_t conj_idx = 2 * static_cast<std::int32_t>(ctx.ring_dim) - 1;
    auto ref = ctx.cc->EvalAdd(ct, ctx.cc->EvalAtIndex(ct, conj_idx));

    auto haze_ct = ops::h2d_ct(ctx, ct);
    auto conj = ops::conjugate(ctx, haze_ct, bk.conjugation_key);
    auto out = ops::add(ctx, haze_ct, conj);
    assert_rns_equal(ctx, out, ref, "phase20 conjugate-add");
}

TEST_CASE("phase19 conjugate + rotate byte-parity vs cc->EvalAtIndex",
          "[integration][e2e]") {
    // Validate ops::conjugate matches cc->EvalAtIndex(ct, 2N-1) byte-exact
    // (and rotate_with_key matches cc->EvalAtIndex(ct, idx) byte-exact for
    // a few common indices). If conjugate diverges, the eval_mod
    // conjugate-add will too; that's the next suspect after phase 14/17/16
    // are all green.
    using namespace lbcrypto;
    namespace ops = haze::test::ops;

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx_tiny(1u << 11);
    constexpr std::uint32_t slots = 8;
    auto bk = ops::make_bootstrap_keys(ctx, ctx.cc, ctx.keys.secretKey, slots);

    const std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);

    // Conjugate: index 2N-1 = M-1.
    const std::int32_t conj_idx = 2 * static_cast<std::int32_t>(ctx.ring_dim) - 1;
    auto ref_conj = ctx.cc->EvalAtIndex(ct, conj_idx);

    auto haze_ct = ops::h2d_ct(ctx, ct);
    auto haze_conj = ops::conjugate(ctx, haze_ct, bk.conjugation_key);
    assert_rns_equal(ctx, haze_conj, ref_conj, "phase19 conjugate idx=2N-1");
}

TEST_CASE("phase18 eval_mod byte-parity vs OpenFHE manual sparsely-packed replication",
          "[integration][e2e]") {
    // Compose OpenFHE's sparsely-packed eval_mod via the public cc-> API
    // (manual mirror of ckksrns-fhe.cpp:786-826) and compare byte-for-byte
    // to ops::eval_mod_for_test on the same input. Same coefficients,
    // same numIter, same scaling.
    using namespace lbcrypto;
    namespace ops = haze::test::ops;

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx_tiny(1u << 11);
    constexpr std::uint32_t slots = 8;
    auto bk = ops::make_bootstrap_keys(ctx, ctx.cc, ctx.keys.secretKey, slots);

    // Same g_coefficientsUniform list eval_mod uses internally.
    static const std::vector<double> coefficients{
        0.15421426400235561,    -0.0037671538417132409,  0.16032011744533031,
        -0.0034539657223742453, 0.17711481926851286,     -0.0027619720033372291,
        0.19949802549604084,    -0.0015928034845171929,  0.21756948616367638,
        0.00010729951647566607, 0.21600427371240055,     0.0022171399198851363,
        0.17647500259573556,    0.0042856217194480991,   0.086174491919472254,
        0.0054640252312780444,  -0.046667988130649173,   0.0047346914623733714,
        -0.17712686172280406,   0.0016205080004247200,   -0.22703114241338604,
        -0.0028145845916205865, -0.13123089730288540,    -0.0056345646688793190,
        0.078818395388692147,   -0.0037868875028868542,  0.23226434602675575,
        0.0021116338645426574,  0.13985510526186795,     0.0059365649669377071,
        -0.13918475289368595,   0.0018580676740836374,   -0.23254376365752788,
        -0.0054103844866927788, 0.056840618403875359,    -0.0035227192748552472,
        0.25667909012207590,    0.0055029673963982112,   -0.073334392714092062,
        0.0027810273357488265,  -0.24912792167850559,    -0.0069524866497120566,
        0.21288810409948347,    0.0017810057298691725,   0.088760951809475269,
        0.0055957188940032095,  -0.31937177676259115,    -0.0087539416335935556,
        0.34748800245527145,    0.0075378299617709235,   -0.25116537379803394,
        -0.0047285674679876204, 0.13970502851683486,     0.0023672533925155220,
        -0.063649401080083698,  -0.00098993213448982727, 0.024597838934816905,
        0.00035553235917057483, -0.0082485030307578155,  -0.00011176184313622549,
        0.0024390574829093264,  0.000031180384864488629, -0.00064373524734389861,
        -7.8036008952377965e-6, 0.00015310015145922058,  1.7670804180220134e-6,
        -0.000033066844379476900, -3.6460909134279425e-7, 6.5276969021754105e-6,
        6.8957843666189918e-8,  -1.1842811187642386e-6,  -1.2015133285307312e-8,
        1.9839339947648331e-7,  1.9372045971100854e-9,   -3.0815418032523593e-8,
        -2.9013806338735810e-10, 4.4540904298173700e-9,  4.0505136697916078e-11,
        -6.0104912807134771e-10, -5.2873323696828491e-12, 7.5943206779351725e-11,
        6.4679566322060472e-13, -9.0081200925539902e-12, -7.4396949275292252e-14,
        1.0057423059167244e-12, 8.1701187638005194e-15,  -1.0611736208855373e-13,
        -8.9597492970451533e-16, 1.1421575296031385e-14};

    const std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
    // Mirror the haze pipeline: we need ct AT THE SAME state as ops::eval_mod
    // expects (post-CtS). Easiest: synthesize ct at a state that matches
    // by running OpenFHE's sparsely-packed pre-eval_mod steps too.
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);

    // OpenFHE side: replicate sparsely-packed eval_mod body from line 789.
    // ct enters with whatever NSD/level. To mimic eval_mod's signature
    // (takes the post-CtS ct), we just hand cc->EvalAdd the conjugate
    // already and proceed.
    // Compute OpenFHE's bootstrap scalar (matches ckksrns-fhe.cpp:560-574).
    auto cp = std::dynamic_pointer_cast<CryptoParametersCKKSRNS>(ctx.cc->GetCryptoParameters());
    const double qDouble = cp->GetElementParams()->GetParams()[0]->GetModulus().ConvertToDouble();
    const double powP_ref = std::pow(2.0, cp->GetPlaintextModulus());
    const std::int32_t deg_ref =
        static_cast<std::int32_t>(std::round(std::log2(qDouble / powP_ref)));
    const std::uint64_t scalar_ref =
        static_cast<std::uint64_t>(std::llround(std::pow(2.0, deg_ref)));

    auto openfhe_eval_mod = [&](Ciphertext<DCRTPoly> ctxtEnc) {
        ctxtEnc = ctx.cc->EvalAdd(ctxtEnc, ctx.cc->EvalAtIndex(ctxtEnc, 2 * static_cast<std::int32_t>(ctx.ring_dim) - 1));
        if (ctxtEnc->GetNoiseScaleDeg() == 2)
            ctx.cc->GetScheme()->ModReduceInternalInPlace(ctxtEnc, 1);
        ctxtEnc = ctx.cc->EvalChebyshevSeries(ctxtEnc, coefficients, -1.0, 1.0);
        ctx.cc->GetScheme()->ModReduceInternalInPlace(ctxtEnc, 1);
        for (std::int32_t i = -2; i <= 0; ++i) {
            const double scalar = -std::pow(2.0 * M_PI, -std::pow(2.0, i));
            ctx.cc->EvalSquareInPlace(ctxtEnc);
            ctx.cc->EvalAddInPlace(ctxtEnc, ctx.cc->EvalAdd(ctxtEnc, scalar));
            ctx.cc->ModReduceInPlace(ctxtEnc); // FIXEDAUTO no-op
        }
        // OpenFHE line 825: algo->MultByIntegerInPlace(ctxtEnc, scalar).
        // For our setup scalar=1 so this is identity.
        ctx.cc->GetScheme()->MultByIntegerInPlace(ctxtEnc, scalar_ref);
        ctx.cc->GetScheme()->ModReduceInternalInPlace(ctxtEnc, 1);
        return ctxtEnc;
    };

    auto ref = openfhe_eval_mod(ct);
    auto haze_ct = ops::h2d_ct(ctx, ct);
    auto out = ops::eval_mod_for_test(ctx, bk, haze_ct);
    std::cerr << "  [phase18] haze.towers=" << out.towers()
              << " nsd=" << out.noise_scale_deg()
              << "; ref.towers=" << ref->GetElements()[0].GetNumOfElements()
              << " nsd=" << ref->GetNoiseScaleDeg() << "\n";
    assert_rns_equal(ctx, out, ref, "phase18");
}

TEST_CASE("phase17 add_const byte-parity vs cc->EvalAdd(ct, double)",
          "[integration][e2e]") {
    // Mirror phase 16 for the add side. add_const is used heavily in
    // compute_cheby_tree (add_const(_, -1.0)) and inner_eval_chebyshev_ps_nb
    // (add_const(qu, q_free/2), etc.). Phase 13 (apply_double_angle) covers
    // the specific scalars in the double-angle iter; phase 17 covers more.
    using namespace lbcrypto;
    namespace ops = haze::test::ops;

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx_tiny(1u << 11);

    auto fresh = [&]() {
        std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
        auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
        return ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    };
    auto bump_nsd2 = [&](Ciphertext<DCRTPoly> ct) { return ctx.cc->EvalMult(ct, 1.0); };

    auto check = [&](const std::string &label, Ciphertext<DCRTPoly> ct, double scalar) {
        auto ref = ctx.cc->EvalAdd(ct, scalar);
        auto haze_ct = ops::h2d_ct(ctx, ct);
        auto out = ops::add_const_for_test(ctx, haze_ct, scalar);
        assert_rns_equal(ctx, out, ref, "phase17 " + label);
    };

    check("NSD=1 scalar=0.0", fresh(), 0.0);
    check("NSD=1 scalar=-1.0", fresh(), -1.0);
    check("NSD=1 scalar=-0.001", fresh(), -0.001);
    check("NSD=1 scalar=0.5", fresh(), 0.5);
    check("NSD=1 scalar=-3.14", fresh(), -3.14);
    check("NSD=2 scalar=-1.0", bump_nsd2(fresh()), -1.0);
    check("NSD=2 scalar=-0.001", bump_nsd2(fresh()), -0.001);
    check("NSD=2 scalar=0.5", bump_nsd2(fresh()), 0.5);
}

TEST_CASE("phase16 eval_mult_scalar byte-parity vs cc->EvalMult(ct, double)",
          "[integration][e2e]") {
    // Test eval_mult_scalar (the auto-rescale-on-NSD=2 helper used in
    // accum_baby_step) against cc->EvalMult(ct, double) for various
    // scalars. If this passes, my scalar-mult building block is byte-
    // exact and the phase 14 divergence isn't in the per-coefficient
    // multiplication.
    using namespace lbcrypto;
    namespace ops = haze::test::ops;

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx_tiny(1u << 11);

    auto fresh = [&]() {
        std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
        auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
        return ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    };
    auto bump_nsd2 = [&](Ciphertext<DCRTPoly> ct) { return ctx.cc->EvalMult(ct, 1.0); };

    auto check = [&](const std::string &label, Ciphertext<DCRTPoly> ct, double scalar) {
        auto ref = ctx.cc->EvalMult(ct, scalar);
        auto haze_ct = ops::h2d_ct(ctx, ct);
        auto out = ops::eval_mult_scalar_for_test(ctx, haze_ct, scalar);
        assert_rns_equal(ctx, out, ref, "phase16 " + label);
    };

    // NSD=1 input, various scalars.
    check("NSD=1 scalar=1.0", fresh(), 1.0);
    check("NSD=1 scalar=0.5", fresh(), 0.5);
    check("NSD=1 scalar=-0.001", fresh(), -0.001);
    check("NSD=1 scalar=2.5", fresh(), 2.5);
    // NSD=2 input — auto-rescale path.
    check("NSD=2 scalar=1.0", bump_nsd2(fresh()), 1.0);
    check("NSD=2 scalar=0.25", bump_nsd2(fresh()), 0.25);
    check("NSD=2 scalar=-3.14", bump_nsd2(fresh()), -3.14);
    // Coverage for very small scalars (g_coefficientsUniform has values
    // near the IsNotEqualZero threshold of 0x1p-44 ≈ 5.68e-14).
    check("NSD=1 scalar=1e-6", fresh(), 1e-6);
    check("NSD=1 scalar=-1e-9", fresh(), -1e-9);
    check("NSD=1 scalar=1e-12", fresh(), 1e-12);
    check("NSD=1 scalar=-1e-13", fresh(), -1e-13);
    check("NSD=1 scalar=8.17e-15", fresh(), 8.17e-15);
    check("NSD=1 scalar=-8.96e-16", fresh(), -8.96e-16);
    check("NSD=1 scalar=1.14e-14", fresh(), 1.14e-14);
    check("NSD=1 scalar=large=1234.5", fresh(), 1234.5);
    check("NSD=1 scalar=large_neg=-99999.7", fresh(), -99999.7);
}

TEST_CASE("phase15 compute_cheby_tree byte-parity vs cc->EvalChebyPolys",
          "[integration][e2e]") {
    // Isolate whether the phase 14 chebyshev divergence is in the
    // T-tree build (compute_cheby_tree) or in the inner_eval recursion.
    // OpenFHE exposes cc->EvalChebyPolys returning a seriesPowers struct
    // (powersRe / powers2Re / power2km1Re). Compare T[0..k-1] / T2[0..m-1]
    // / T2km1 byte-for-byte.
    using namespace lbcrypto;
    namespace ops = haze::test::ops;

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx_tiny(1u << 11);

    auto fresh = [&]() {
        std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
        auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
        return ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    };

    auto check = [&](const std::string &label, const std::vector<double> &coeffs,
                     Ciphertext<DCRTPoly> ct) {
        auto ref_tree = ctx.cc->EvalChebyPolys(ct, coeffs, -1.0, 1.0);
        REQUIRE(ref_tree);
        auto haze_ct = ops::h2d_ct(ctx, ct);
        auto my_tree = ops::compute_cheby_tree_for_test(ctx, haze_ct, coeffs);
        REQUIRE(my_tree.k == ref_tree->k);
        REQUIRE(my_tree.m == ref_tree->m);
        REQUIRE(my_tree.T.size() == ref_tree->powersRe.size());
        REQUIRE(my_tree.T2.size() == ref_tree->powers2Re.size());
        for (std::size_t i = 0; i < my_tree.T.size(); ++i) {
            assert_rns_equal(ctx, my_tree.T[i], ref_tree->powersRe[i],
                             "phase15 " + label + " T[" + std::to_string(i) + "]");
        }
        for (std::size_t i = 0; i < my_tree.T2.size(); ++i) {
            assert_rns_equal(ctx, my_tree.T2[i], ref_tree->powers2Re[i],
                             "phase15 " + label + " T2[" + std::to_string(i) + "]");
        }
        if (ref_tree->power2km1Re)
            assert_rns_equal(ctx, my_tree.T2km1, ref_tree->power2km1Re,
                             "phase15 " + label + " T2km1");
    };

    std::vector<double> coeffs = {0.5, 0.25, 0.125, 0.0625, 0.03125, 0.015625};
    check("degree=5", coeffs, fresh());
}

TEST_CASE("phase14 eval_chebyshev_series byte-parity vs cc->EvalChebyshevSeries",
          "[integration][e2e]") {
    // Top rung: compare ops::eval_chebyshev_series to OpenFHE's
    // cc->EvalChebyshevSeries byte-for-byte. With phase 11 (adjust),
    // phase 12 (mult/add/sub), phase 13 (double-angle) all byte-exact,
    // this should pass — and if it doesn't, the gap is in
    // compute_cheby_tree or inner_eval_chebyshev_ps structure (not the
    // primitives).
    using namespace lbcrypto;
    namespace ops = haze::test::ops;

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx_tiny(1u << 11);

    auto fresh = [&]() {
        std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
        auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
        return ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    };

    auto check = [&](const std::string &label,
                     const std::vector<double> &coeffs,
                     Ciphertext<DCRTPoly> ct) {
        auto ref = ctx.cc->EvalChebyshevSeries(ct, coeffs, -1.0, 1.0);
        auto haze_ct = ops::h2d_ct(ctx, ct);
        auto out = ops::eval_chebyshev_series_for_test(ctx, haze_ct, coeffs);
        assert_rns_equal(ctx, out, ref, "phase14 " + label);
    };

    // Degree-5: ComputeDegreesPS → k=2, m=2. Smallest PS path.
    {
        std::vector<double> coeffs = {0.5, 0.25, 0.125, 0.0625, 0.03125, 0.015625};
        check("degree=5", coeffs, fresh());
    }
    // Degree-12: k=4, m=2.
    {
        std::vector<double> coeffs(13, 0.0);
        for (std::size_t i = 0; i <= 12; ++i) coeffs[i] = 1.0 / static_cast<double>(i + 1);
        check("degree=12", coeffs, fresh());
    }
    // Degree-84 (eval_mod-sized): k=6, m=4.
    {
        std::vector<double> coeffs(85, 0.0);
        for (std::size_t i = 0; i <= 84; ++i) coeffs[i] = (i & 1) ? -0.001 : 0.05;
        check("degree=84", coeffs, fresh());
    }
}

TEST_CASE("phase13 apply_double_angle_iterations byte-parity vs OpenFHE",
          "[integration][e2e]") {
    // Walk one more rung. OpenFHE's ApplyDoubleAngleIterations is on
    // FHECKKSRNS (private to the bootstrap path), so replicate its body
    // through the public cc-> API:
    //   for i in [1-numIter, 0]:
    //     scalar = -(2π)^(-2^i)
    //     EvalSquareInPlace(ct)
    //     EvalAddInPlace(ct, EvalAdd(ct, scalar))
    //     ModReduceInPlace(ct)    // no-op for FIXEDAUTO
    // and compare byte-for-byte against my apply_double_angle_iterations.
    using namespace lbcrypto;
    namespace ops = haze::test::ops;

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx_tiny(1u << 11);

    auto fresh = [&]() {
        std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
        auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
        return ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    };

    auto openfhe_double_angle = [&](Ciphertext<DCRTPoly> ct, std::uint32_t num_iter) {
        constexpr double twoPi = 2.0 * M_PI;
        for (std::int32_t i = 1 - static_cast<std::int32_t>(num_iter); i <= 0; ++i) {
            const double scalar = -std::pow(twoPi, -std::pow(2.0, i));
            ctx.cc->EvalSquareInPlace(ct);
            ctx.cc->EvalAddInPlace(ct, ctx.cc->EvalAdd(ct, scalar));
            ctx.cc->ModReduceInPlace(ct); // no-op in FIXEDAUTO
        }
        return ct;
    };

    auto check = [&](const std::string &label, Ciphertext<DCRTPoly> ct, std::uint32_t iters) {
        auto ref = openfhe_double_angle(ct->Clone(), iters);
        auto haze_ct = ops::h2d_ct(ctx, ct);
        ops::apply_double_angle_for_test(ctx, haze_ct, iters);
        assert_rns_equal(ctx, haze_ct, ref, "phase13 " + label);
    };

    // 1 iter from a fresh (NSD=1) input
    check("iters=1 fresh", fresh(), 1);
    // 2 iters from fresh
    check("iters=2 fresh", fresh(), 2);
    // 3 iters from fresh (this is what eval_mod actually does for UNIFORM_TERNARY)
    check("iters=3 fresh", fresh(), 3);
}

TEST_CASE("phase12 ops::mult / ops::add / ops::sub byte-parity vs cc->EvalMult / cc->EvalAdd",
          "[integration][e2e]") {
    // Next rung on the ladder above phase 11: with adjust_for_mult and
    // adjust_for_add now byte-exact, the compound ops::mult / ops::add /
    // ops::sub should also be byte-exact provided tensor + relin / add /
    // sub themselves track OpenFHE. Compare per-RNS-limb against
    // cc->EvalMult, cc->EvalAdd, cc->EvalSub across the same (L, NSD)
    // combinations.
    using namespace lbcrypto;
    namespace ops = haze::test::ops;

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx_tiny(1u << 11);

    auto fresh = [&]() {
        std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
        auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
        return ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    };
    auto bump_nsd2 = [&](Ciphertext<DCRTPoly> ct) { return ctx.cc->EvalMult(ct, 1.0); };
    auto drop_level = [&](Ciphertext<DCRTPoly> ct) {
        ctx.cc->GetScheme()->ModReduceInternalInPlace(ct, 1);
        return ct;
    };

    // ops::mult / add / sub assume aligned inputs; cc->EvalMult/Add/Sub
    // adjust internally. To compare apples-to-apples, wrap haze ops in
    // adjust_for_mult / adjust_for_add — matching how compute_cheby_tree
    // calls them in production.
    auto check_mult = [&](const std::string &label, Ciphertext<DCRTPoly> ct_a,
                          Ciphertext<DCRTPoly> ct_b) {
        auto ref = ctx.cc->EvalMult(ct_a, ct_b);
        auto haze_a = ops::h2d_ct(ctx, ct_a);
        auto haze_b = ops::h2d_ct(ctx, ct_b);
        auto pair = ops::adjust_for_mult_for_test(ctx, std::move(haze_a), std::move(haze_b));
        auto out = ops::mult(ctx, pair.a, pair.b);
        assert_rns_equal(ctx, out, ref, "phase12 mult " + label);
    };
    auto check_add = [&](const std::string &label, Ciphertext<DCRTPoly> ct_a,
                         Ciphertext<DCRTPoly> ct_b) {
        auto ref = ctx.cc->EvalAdd(ct_a, ct_b);
        auto haze_a = ops::h2d_ct(ctx, ct_a);
        auto haze_b = ops::h2d_ct(ctx, ct_b);
        auto pair = ops::adjust_for_add_for_test(ctx, std::move(haze_a), std::move(haze_b));
        auto out = ops::add(ctx, pair.a, pair.b);
        assert_rns_equal(ctx, out, ref, "phase12 add " + label);
    };
    auto check_sub = [&](const std::string &label, Ciphertext<DCRTPoly> ct_a,
                         Ciphertext<DCRTPoly> ct_b) {
        auto ref = ctx.cc->EvalSub(ct_a, ct_b);
        auto haze_a = ops::h2d_ct(ctx, ct_a);
        auto haze_b = ops::h2d_ct(ctx, ct_b);
        auto pair = ops::adjust_for_add_for_test(ctx, std::move(haze_a), std::move(haze_b));
        auto out = ops::sub(ctx, pair.a, pair.b);
        assert_rns_equal(ctx, out, ref, "phase12 sub " + label);
    };

    // mult: covers ops::mult as well as square_ct (a==b case).
    check_mult("L=0/N=1 vs L=0/N=1", fresh(), fresh());
    check_mult("square(L=0/N=1)", fresh(), fresh()); // same shape
    check_mult("L=0/N=2 vs L=0/N=2", bump_nsd2(fresh()), bump_nsd2(fresh()));
    check_mult("L=0/N=1 vs L=0/N=2", fresh(), bump_nsd2(fresh()));
    check_mult("L=0/N=2 vs L=1/N=1", bump_nsd2(fresh()), drop_level(bump_nsd2(fresh())));
    check_mult("L=0/N=1 vs L=1/N=1", fresh(), drop_level(bump_nsd2(fresh())));

    // add/sub same coverage.
    check_add("L=0/N=1 + L=0/N=1", fresh(), fresh());
    check_add("L=0/N=2 + L=0/N=2", bump_nsd2(fresh()), bump_nsd2(fresh()));
    check_add("L=0/N=1 + L=0/N=2", fresh(), bump_nsd2(fresh()));
    check_add("L=0/N=2 + L=1/N=1", bump_nsd2(fresh()), drop_level(bump_nsd2(fresh())));
    check_sub("L=0/N=1 - L=0/N=1", fresh(), fresh());
    check_sub("L=0/N=2 - L=0/N=2", bump_nsd2(fresh()), bump_nsd2(fresh()));
    check_sub("L=0/N=2 - L=1/N=1", bump_nsd2(fresh()), drop_level(bump_nsd2(fresh())));
}

TEST_CASE("phase11 adjust_for_mult / adjust_for_add byte-parity vs OpenFHE",
          "[integration][e2e]") {
    // Per-RNS-limb parity vs OpenFHE's
    // cc->GetScheme()->AdjustLevelsAndDepthToOneInPlace (mult variant)
    // and AdjustLevelsAndDepthInPlace (add variant), across all
    // (level, NSD) combinations that appear inside compute_cheby_tree
    // and inner_eval_chebyshev_ps.
    using namespace lbcrypto;
    namespace ops = haze::test::ops;

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx_tiny(1u << 11);

    auto fresh = [&]() {
        std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
        auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
        return ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    };
    // Build a ciphertext at NSD=2: one EvalMult by 1.0 (no auto-rescale
    // when input NSD=1, so output is NSD=2 at the same level).
    auto bump_nsd2 = [&](Ciphertext<DCRTPoly> ct) {
        return ctx.cc->EvalMult(ct, 1.0);
    };
    // ModReduce in place (FIXEDAUTO public ModReduceInPlace is a no-op;
    // go through the scheme to actually drop a level).
    auto drop_level = [&](Ciphertext<DCRTPoly> ct) {
        ctx.cc->GetScheme()->ModReduceInternalInPlace(ct, 1);
        return ct;
    };

    auto check_mult_pair = [&](const std::string &label, Ciphertext<DCRTPoly> ct_a,
                               Ciphertext<DCRTPoly> ct_b) {
        // OpenFHE reference BEFORE haze epoch opens so CPROBES doesn't
        // pollute the trace.
        auto ref_a = ct_a->Clone();
        auto ref_b = ct_b->Clone();
        ctx.cc->GetScheme()->AdjustLevelsAndDepthToOneInPlace(ref_a, ref_b);

        auto haze_a = ops::h2d_ct(ctx, ct_a);
        auto haze_b = ops::h2d_ct(ctx, ct_b);
        auto pair = ops::adjust_for_mult_for_test(ctx, std::move(haze_a), std::move(haze_b));

        assert_rns_equal(ctx, pair.a, ref_a, "phase11 mult " + label + " .a");
        assert_rns_equal(ctx, pair.b, ref_b, "phase11 mult " + label + " .b");
    };

    auto check_add_pair = [&](const std::string &label, Ciphertext<DCRTPoly> ct_a,
                              Ciphertext<DCRTPoly> ct_b) {
        auto ref_a = ct_a->Clone();
        auto ref_b = ct_b->Clone();
        ctx.cc->GetScheme()->AdjustLevelsAndDepthInPlace(ref_a, ref_b);

        auto haze_a = ops::h2d_ct(ctx, ct_a);
        auto haze_b = ops::h2d_ct(ctx, ct_b);
        auto pair = ops::adjust_for_add_for_test(ctx, std::move(haze_a), std::move(haze_b));

        assert_rns_equal(ctx, pair.a, ref_a, "phase11 add " + label + " .a");
        assert_rns_equal(ctx, pair.b, ref_b, "phase11 add " + label + " .b");
    };

    // Same level, varied NSD.
    check_mult_pair("L=0/N=1 vs L=0/N=1", fresh(), fresh());
    check_mult_pair("L=0/N=2 vs L=0/N=2", bump_nsd2(fresh()), bump_nsd2(fresh()));
    check_mult_pair("L=0/N=1 vs L=0/N=2", fresh(), bump_nsd2(fresh()));

    // Different levels, varied NSD. drop_level brings NSD 2→1 and L 0→1.
    check_mult_pair("L=0/N=2 vs L=1/N=2",
                    bump_nsd2(fresh()), bump_nsd2(drop_level(bump_nsd2(fresh()))));
    check_mult_pair("L=0/N=2 vs L=1/N=1",
                    bump_nsd2(fresh()), drop_level(bump_nsd2(fresh())));
    check_mult_pair("L=0/N=1 vs L=1/N=2",
                    fresh(), bump_nsd2(drop_level(bump_nsd2(fresh()))));
    check_mult_pair("L=0/N=1 vs L=1/N=1",
                    fresh(), drop_level(bump_nsd2(fresh())));

    // adjust_for_add: same coverage.
    check_add_pair("L=0/N=1 vs L=0/N=1", fresh(), fresh());
    check_add_pair("L=0/N=2 vs L=0/N=2", bump_nsd2(fresh()), bump_nsd2(fresh()));
    check_add_pair("L=0/N=1 vs L=0/N=2", fresh(), bump_nsd2(fresh()));
    check_add_pair("L=0/N=2 vs L=1/N=2",
                   bump_nsd2(fresh()), bump_nsd2(drop_level(bump_nsd2(fresh()))));
    check_add_pair("L=0/N=2 vs L=1/N=1",
                   bump_nsd2(fresh()), drop_level(bump_nsd2(fresh())));
    check_add_pair("L=0/N=1 vs L=1/N=2",
                   fresh(), bump_nsd2(drop_level(bump_nsd2(fresh()))));
    check_add_pair("L=0/N=1 vs L=1/N=1",
                   fresh(), drop_level(bump_nsd2(fresh())));
}

TEST_CASE("phase10 microbenchmark ops:: helpers at N=2048",
          "[integration][e2e]") {
    // Per-call wall time for each ops:: helper used by bootstrap. Tells us
    // which helpers run OpenFHE work inside the haze epoch.
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    using clk = std::chrono::steady_clock;
    auto measure = [](const char *label, std::size_t iters, auto &&fn) {
        auto t0 = clk::now();
        for (std::size_t i = 0; i < iters; ++i)
            fn();
        const double ms =
            std::chrono::duration<double, std::milli>(clk::now() - t0).count();
        const double iters_d = static_cast<double>(iters);
        std::cerr << "  [phase10] " << label << ": " << ms << " ms / " << iters
                  << " = " << (ms * 1000.0 / iters_d) << " us/call\n";
        std::cout << "  [phase10] " << label << ": "
                  << (ms * 1000.0 / iters_d) << " us/call\n";
    };

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx_tiny(1u << 11);
    constexpr std::uint32_t slots = 8;
    auto bk = ops::make_bootstrap_keys(ctx, ctx.cc, ctx.keys.secretKey, slots);

    const std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    auto haze_ct = ops::h2d_ct(ctx, ct); // opens epoch

    measure("ops::mult_by_const_for_test", 50,
            [&] { auto out = ops::mult_by_const_for_test(ctx, haze_ct, 0.5); (void)out; });
    measure("ops::add_const_for_test", 50,
            [&] { auto out = ops::add_const_for_test(ctx, haze_ct, 0.5); (void)out; });
    measure("ops::mult_int_scalar_for_test", 50,
            [&] { auto out = ops::mult_int_scalar_for_test(ctx, haze_ct, 7); (void)out; });
    measure("ops::add (ct+ct)", 100,
            [&] { auto out = ops::add(ctx, haze_ct, haze_ct); (void)out; });
    measure("ops::sub (ct-ct)", 100,
            [&] { auto out = ops::sub(ctx, haze_ct, haze_ct); (void)out; });
    measure("ops::rescale", 5,
            [&] { auto out = ops::rescale(ctx, haze_ct); (void)out; });
    measure("ops::square_ct_for_test", 5,
            [&] { auto out = ops::square_ct_for_test(ctx, haze_ct); (void)out; });
    measure("ops::clone_ct", 50,
            [&] { auto out = ops::clone_ct(ctx, haze_ct); (void)out; });
    measure("ops::mult_monomial_for_test", 5,
            [&] { auto out = ops::mult_monomial_for_test(ctx, haze_ct, 3); (void)out; });

    // Compound helpers: these are what bootstrap actually calls.
    measure("apply_double_angle (3 iters)", 1, [&] {
        auto y = ops::clone_ct(ctx, haze_ct);
        ops::apply_double_angle_for_test(ctx, y, 3);
        (void)y;
    });

    // Chebyshev at degree 5 (k=2, m=2) — fast PS path.
    measure("eval_chebyshev_series degree 5", 1, [&] {
        std::vector<double> coeffs = {0.5, 0.25, 0.125, 0.0625, 0.03125, 0.015625};
        auto out = ops::eval_chebyshev_series_for_test(ctx, haze_ct, coeffs);
        (void)out;
    });

    // Chebyshev at degree 12 — slightly bigger PS.
    measure("eval_chebyshev_series degree 12", 1, [&] {
        std::vector<double> coeffs(13, 0.0);
        for (std::size_t i = 0; i <= 12; ++i) coeffs[i] = 1.0 / static_cast<double>(i + 1);
        auto out = ops::eval_chebyshev_series_for_test(ctx, haze_ct, coeffs);
        (void)out;
    });

    // Chebyshev at degree 84 = what eval_mod actually uses.
    measure("eval_chebyshev_series degree 84 (eval_mod-sized)", 1, [&] {
        std::vector<double> coeffs(85, 0.0);
        for (std::size_t i = 0; i <= 84; ++i) coeffs[i] = (i & 1) ? -0.001 : 0.05;
        auto out = ops::eval_chebyshev_series_for_test(ctx, haze_ct, coeffs);
        (void)out;
    });

    // Repeat: does per-call cost stay flat as epoch state grows? Bootstrap
    // calls Chebyshev twice (real + imag) and then double-angle; if cost
    // grows, the full bootstrap explodes nonlinearly.
    measure("eval_chebyshev_series degree 12 (x50, look for drift)", 50, [&] {
        std::vector<double> coeffs(13, 0.0);
        for (std::size_t i = 0; i <= 12; ++i) coeffs[i] = 1.0 / static_cast<double>(i + 1);
        auto out = ops::eval_chebyshev_series_for_test(ctx, haze_ct, coeffs);
        (void)out;
    });

    // Same for mult_by_const after the heavy run above. If poly_map / shadow
    // / allocator have ballooned, per-call cost should drift up.
    measure("ops::mult_by_const_for_test (after heavy run)", 50,
            [&] { auto out = ops::mult_by_const_for_test(ctx, haze_ct, 0.5); (void)out; });
}

TEST_CASE("phase9 microbenchmark haze C-ABI primitives at N=2048",
          "[integration][e2e]") {
    // Tight-loop calls to individual haze C-ABI entry points to localize
    // the 18 ms/call observed in the full bootstrap. Each loop is sized
    // to give ~1 second of work at a realistic per-call cost (10K iters
    // at 100us = 1s); slower paths just take longer.
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    using clk = std::chrono::steady_clock;

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx_tiny(1u << 11);

    constexpr std::size_t kIters = 5000;
    constexpr std::size_t kTowers = 20;

    // (1) hazeMalloc rate. No epoch active yet. Pure allocator path.
    std::vector<void *> mptrs(kIters * kTowers, nullptr);
    auto t_malloc0 = clk::now();
    for (std::size_t i = 0; i < kIters * kTowers; ++i) {
        REQUIRE(hazeMalloc(&mptrs[i], ctx.poly_bytes) == HAZE_SUCCESS);
    }
    const double malloc_ms =
        std::chrono::duration<double, std::milli>(clk::now() - t_malloc0).count();
    const double per_malloc_us = malloc_ms * 1000.0 / (kIters * kTowers);

    // (2) Build a stable per-tower input ct: hazeMalloc + H2D.
    std::vector<void *> in_a(kTowers, nullptr);
    std::vector<void *> in_b(kTowers, nullptr);
    std::vector<void *> out_d(kTowers, nullptr);
    std::vector<uint64_t> host_a(ctx.ring_dim, 1);
    std::vector<uint64_t> host_b(ctx.ring_dim, 2);
    for (std::size_t t = 0; t < kTowers; ++t) {
        REQUIRE(hazeMalloc(&in_a[t], ctx.poly_bytes) == HAZE_SUCCESS);
        REQUIRE(hazeMalloc(&in_b[t], ctx.poly_bytes) == HAZE_SUCCESS);
        REQUIRE(hazeMalloc(&out_d[t], ctx.poly_bytes) == HAZE_SUCCESS);
        REQUIRE(hazeMemcpy(in_a[t], host_a.data(), ctx.poly_bytes,
                           HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
        REQUIRE(hazeMemcpy(in_b[t], host_b.data(), ctx.poly_bytes,
                           HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    }
    std::vector<uint64_t> base(ctx.q_base.begin(),
                               ctx.q_base.begin() + static_cast<std::ptrdiff_t>(kTowers));

    // (3) hazeAddMrp rate. Each call emits sr_addp per tower → fhetch emit
    // path is the hot loop here.
    auto t_add0 = clk::now();
    for (std::size_t i = 0; i < kIters; ++i) {
        REQUIRE(hazeAddMrp(out_d.data(),
                           const_cast<const void **>(in_a.data()),
                           const_cast<const void **>(in_b.data()),
                           base.data(), kTowers, nullptr) == HAZE_SUCCESS);
    }
    const double add_ms =
        std::chrono::duration<double, std::milli>(clk::now() - t_add0).count();
    const double per_add_us = add_ms * 1000.0 / kIters;

    // (4) hazeAddMrp with FRESH dst allocs each iter — simulates the
    // bootstrap pattern where every result Allocs is a new hazeMalloc.
    auto t_add_fresh0 = clk::now();
    std::vector<std::vector<void *>> fresh_dsts(kIters);
    for (std::size_t i = 0; i < kIters; ++i) {
        fresh_dsts[i].resize(kTowers, nullptr);
        for (std::size_t t = 0; t < kTowers; ++t)
            REQUIRE(hazeMalloc(&fresh_dsts[i][t], ctx.poly_bytes) == HAZE_SUCCESS);
        REQUIRE(hazeAddMrp(fresh_dsts[i].data(),
                           const_cast<const void **>(in_a.data()),
                           const_cast<const void **>(in_b.data()),
                           base.data(), kTowers, nullptr) == HAZE_SUCCESS);
    }
    const double add_fresh_ms =
        std::chrono::duration<double, std::milli>(clk::now() - t_add_fresh0).count();
    const double per_add_fresh_us = add_fresh_ms * 1000.0 / kIters;

    std::cerr << "  [phase9] iters=" << kIters << " towers=" << kTowers << " ring=2048\n";
    std::cerr << "  [phase9] hazeMalloc:                  " << malloc_ms
              << " ms total, " << per_malloc_us << " us/call\n";
    std::cerr << "  [phase9] hazeAddMrp (stable dst):     " << add_ms
              << " ms total, " << per_add_us << " us/call\n";
    std::cerr << "  [phase9] hazeAddMrp (fresh dst+alloc):" << add_fresh_ms
              << " ms total, " << per_add_fresh_us << " us/call\n";
    std::cout << "  [phase9] hazeMalloc:                  " << per_malloc_us << " us/call\n";
    std::cout << "  [phase9] hazeAddMrp (stable dst):     " << per_add_us << " us/call\n";
    std::cout << "  [phase9] hazeAddMrp (fresh dst+alloc):" << per_add_fresh_us << " us/call\n";
}

TEST_CASE("phase8 cprobes-only OpenFHE bootstrap (no ops::bootstrap) at N=2048",
          "[integration][e2e]") {
    // Open a haze recording, then call cc->EvalBootstrap. OpenFHE's
    // CPROBES-instrumented build emits IR for every poly-level op the
    // bootstrap performs — directly into the active fhetch trace. This
    // gives us a "what would the same trace look like if haze ops::bootstrap
    // were a pure passthrough of OpenFHE's bootstrap" baseline.
    //
    // emit_ms = how long OpenFHE+CPROBES take to record the bootstrap;
    // replay_ms = how long the local simulator takes to execute it.
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    using clk = std::chrono::steady_clock;

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx_tiny(1u << 11);
    constexpr std::uint32_t slots = 8;
    ctx.cc->EvalBootstrapSetup({1, 1}, {0, 0}, slots, 11);
    ctx.cc->EvalBootstrapKeyGen(ctx.keys.secretKey, slots);

    const std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);

    // Open the haze epoch with a single dummy compute op. Allocate two
    // polys, H2D into one, hazeAdd dummy+dummy → triggers start_recording.
    void *dummy_a = nullptr;
    void *dummy_dst = nullptr;
    REQUIRE(hazeMalloc(&dummy_a, ctx.poly_bytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&dummy_dst, ctx.poly_bytes) == HAZE_SUCCESS);
    std::vector<uint64_t> dummy_host(ctx.ring_dim, 1);
    REQUIRE(hazeMemcpy(dummy_a, dummy_host.data(), ctx.poly_bytes,
                       HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(dummy_dst, dummy_a, dummy_a, 0, nullptr) == HAZE_SUCCESS);

    // Now recording is active — CPROBES will emit IR for EvalBootstrap.
    auto t_emit0 = clk::now();
    auto ct_out = ctx.cc->EvalBootstrap(ct);
    const double emit_ms =
        std::chrono::duration<double, std::milli>(clk::now() - t_emit0).count();
    REQUIRE(ct_out);

    // Close epoch (D2H triggers replay).
    auto t_replay0 = clk::now();
    REQUIRE(hazeMemcpy(dummy_host.data(), dummy_dst, ctx.poly_bytes,
                       HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    const double replay_ms =
        std::chrono::duration<double, std::milli>(clk::now() - t_replay0).count();

    std::cerr << "  [phase8] N=2048 cprobes EvalBootstrap emit: " << emit_ms
              << " ms; replay: " << replay_ms << " ms\n";
    std::cout << "  [phase8] N=2048 cprobes EvalBootstrap emit: " << emit_ms
              << " ms; replay: " << replay_ms << " ms\n";
}

TEST_CASE("phase7 full haze ops::bootstrap end-to-end at N=2048",
          "[integration][e2e]") {
    // Time the full haze bootstrap at a tiny ring, print slot-level
    // diagnostics. Correctness is best-effort — N=2048 is below the noise
    // budget needed for the bootstrap parameters, so slots won't decrypt
    // cleanly. The goal is a wall-clock number, not slot accuracy.
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    using clk = std::chrono::steady_clock;

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto t_setup0 = clk::now();
    auto ctx = make_bootstrap_ctx_tiny(1u << 11);
    constexpr std::uint32_t slots = 8;
    auto bk = ops::make_bootstrap_keys(ctx, ctx.cc, ctx.keys.secretKey, slots);
    const double setup_ms =
        std::chrono::duration<double, std::milli>(clk::now() - t_setup0).count();

    const std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);

    // Reference (OpenFHE only) for slot-level comparison.
    auto t_ref0 = clk::now();
    auto ct_ref = ctx.cc->EvalBootstrap(ct);
    REQUIRE(ct_ref);
    const double ref_ms =
        std::chrono::duration<double, std::milli>(clk::now() - t_ref0).count();
    Plaintext pt_ref;
    ctx.cc->Decrypt(ctx.keys.secretKey, ct_ref, &pt_ref);
    pt_ref->SetLength(v.size());
    const auto slots_ref = pt_ref->GetRealPackedValue();

    // Full haze pipeline.
    auto t_haze0 = clk::now();
    auto haze_ct = ops::h2d_ct(ctx, ct);
    auto haze_refreshed = ops::bootstrap(ctx, bk, haze_ct, ops::BootstrapVariant::Standard);
    const auto haze_bytes = ops::d2h_ct(ctx, haze_refreshed);
    const double haze_ms =
        std::chrono::duration<double, std::milli>(clk::now() - t_haze0).count();

    auto ct_haze = ct_ref->Clone();
    ops::inject_ct(ctx, haze_bytes, ct_haze);
    Plaintext pt_haze;
    ctx.cc->Decrypt(ctx.keys.secretKey, ct_haze, &pt_haze);
    pt_haze->SetLength(v.size());
    const auto slots_haze = pt_haze->GetRealPackedValue();

    std::cerr << "  [phase7] N=2048 setup+keygen+bk: " << setup_ms
              << " ms; cc->EvalBootstrap ref: " << ref_ms
              << " ms; haze bootstrap (h2d+bootstrap+d2h): " << haze_ms << " ms\n";
    std::cout << "  [phase7] N=2048 setup+keygen+bk: " << setup_ms
              << " ms; cc->EvalBootstrap ref: " << ref_ms
              << " ms; haze bootstrap (h2d+bootstrap+d2h): " << haze_ms << " ms\n";
    for (std::size_t i = 0; i < v.size(); ++i) {
        std::cerr << "  [phase7] slot[" << i << "] in=" << v[i]
                  << " ref=" << slots_ref[i] << " haze=" << slots_haze[i]
                  << " diff=" << (slots_haze[i] - slots_ref[i]) << "\n";
    }
}

TEST_CASE("phase6 openfhe-only bootstrap reference benchmark at 2^16",
          "[integration][e2e]") {
    // Time JUST cc->EvalBootstrap at full ring_dim, with the
    // CPROBES-instrumented OpenFHE we link against. No haze epoch active,
    // so CPROBES is inert — this is the baseline "what should this take?"
    // number the haze recording should track within a small constant factor.
    using namespace lbcrypto;
    using clk = std::chrono::steady_clock;

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx(); // ring_dim = 1<<16, depth=25
    constexpr std::uint32_t slots = 8;

    auto t_setup0 = clk::now();
    ctx.cc->EvalBootstrapSetup({1, 1}, {0, 0}, slots, 11);
    ctx.cc->EvalBootstrapKeyGen(ctx.keys.secretKey, slots);
    const double setup_ms =
        std::chrono::duration<double, std::milli>(clk::now() - t_setup0).count();

    const std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);

    auto t_boot0 = clk::now();
    auto ct_out = ctx.cc->EvalBootstrap(ct);
    const double boot_ms =
        std::chrono::duration<double, std::milli>(clk::now() - t_boot0).count();
    REQUIRE(ct_out);

    std::cerr << "  [phase6] ring_dim=65536 setup+keygen: " << setup_ms
              << " ms; cc->EvalBootstrap: " << boot_ms << " ms\n";
    std::cout << "  [phase6] ring_dim=65536 setup+keygen: " << setup_ms
              << " ms; cc->EvalBootstrap: " << boot_ms << " ms\n";
}

TEST_CASE("phase5 profile each bootstrap phase", "[integration][e2e]") {
    // Time each bootstrap phase independently to localize slowness.
    // Uses ring_dim=2^12 (set by make_bootstrap_ctx) for fast turnaround.
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    using clk = std::chrono::steady_clock;
    auto tick = [](const std::string &label, clk::time_point &t0) {
        auto t1 = clk::now();
        const double ms =
            std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::cerr << "  [phase5] " << label << ": " << ms << " ms\n";
        t0 = t1;
    };

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto t0 = clk::now();
    // Tiny ring (2048) so each phase completes quickly; this is profiling,
    // not correctness — output won't decrypt cleanly at this size.
    auto ctx = make_bootstrap_ctx_tiny(1u << 11);
    tick("make_bootstrap_ctx_tiny(2048) (build cc + relin key)", t0);

    constexpr std::uint32_t slots = 8;
    auto bk = ops::make_bootstrap_keys(ctx, ctx.cc, ctx.keys.secretKey, slots);
    tick("make_bootstrap_keys (EvalBootstrapSetup+KeyGen + extract)", t0);

    const std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    tick("Encrypt", t0);

    auto ct_ref = ctx.cc->EvalBootstrap(ct);
    REQUIRE(ct_ref);
    tick("cc->EvalBootstrap (OpenFHE reference, with CPROBES)", t0);

    // Haze pipeline, phase-by-phase. h2d + ModReduce-to-1 + mod_raise + rescale.
    auto haze_ct_full = ops::h2d_ct(ctx, ct);
    tick("h2d_ct", t0);
    if (haze_ct_full.towers() > 1)
        haze_ct_full = ops::level_reduce(ctx, std::move(haze_ct_full),
                                          haze_ct_full.towers() - 1);
    tick("haze level_reduce-to-1-tower", t0);
    auto raised = ops::mod_raise(ctx, bk, haze_ct_full);
    tick("ops::mod_raise", t0);
    raised = ops::rescale(ctx, raised);
    tick("rescale post-mod_raise", t0);

    std::cerr << "  [phase5] cts_matrices.size=" << bk.cts_matrices.size()
              << " cts[0].size=" << bk.cts_matrices.front().size()
              << " stc_matrices.size=" << bk.stc_matrices.size()
              << " stc[0].size=" << bk.stc_matrices.front().size()
              << " q_base.size=" << ctx.q_base.size()
              << " p_base.size=" << ctx.p_base.size() << "\n";

    auto in_slots = ops::linear_transform(ctx, bk, bk.cts_matrices, raised);
    tick("linear_transform (CtS)", t0);
    std::cerr << "  [phase5] in_slots.towers=" << in_slots.towers()
              << " nsd=" << in_slots.noise_scale_deg() << "\n";

    auto modded = ops::eval_mod(ctx, bk, in_slots);
    tick("eval_mod (full Chebyshev x2 + double-angle x2)", t0);
    std::cerr << "  [phase5] modded.towers=" << modded.towers()
              << " nsd=" << modded.noise_scale_deg() << "\n";

    // Align modded.towers to the StC matrices' Q-tower count. OpenFHE's
    // adjust-and-mult does this implicitly via per-mult ModReduces; we
    // collapse it into a single level_reduce here so the StC mult lines
    // up. This is metadata-only — does not match OpenFHE's polynomial
    // values, just lets us measure StC wall time.
    const std::size_t stc_q_towers = bk.stc_matrices.front().size() - ctx.p_base.size();
    if (modded.towers() > stc_q_towers)
        modded = ops::level_reduce(ctx, std::move(modded), modded.towers() - stc_q_towers);
    std::cerr << "  [phase5] post-align modded.towers=" << modded.towers() << "\n";
    auto out = ops::linear_transform(ctx, bk, bk.stc_matrices, modded);
    tick("linear_transform (StC)", t0);

    const auto haze_bytes = ops::d2h_ct(ctx, out);
    tick("d2h_ct (flush + replay)", t0);
    std::cerr << "  [phase5] out.towers=" << out.towers()
              << " (skipping inject_ct — towers mismatch vs ref ct shell)\n";
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

TEST_CASE("ckks bootstrap haze ops::bootstrap slot parity vs EvalBootstrap (N=2048)",
          "[integration][e2e]") {
    // Round-trip ct through haze, run ops::bootstrap, decrypt and compare
    // slot-wise to cc->EvalBootstrap on the same input. N=2048 keeps the
    // simulator memory footprint manageable; the 2^16 variant OOMs.
    using namespace lbcrypto;
    namespace ops = haze::test::ops;

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx_tiny(1u << 11);

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
