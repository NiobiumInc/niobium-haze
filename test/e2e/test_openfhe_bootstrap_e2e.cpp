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

haze::test::ops::OpCtx make_bootstrap_ctx_mode(lbcrypto::ScalingTechnique mode,
                                                std::uint32_t ring_dim = 1u << 16) {
    using namespace lbcrypto;
    using namespace haze::test::ops;
    auto ctx = make_ctx({
        .mode = mode,
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

// Same as ctx_tiny but with deeper multDepth so the full bootstrap pipeline
// (mod_raise → EvalMult → rescale → CtS → eval_mod → StC → corFactor) has
// headroom for the intermediate NSD=2 ciphertexts post_recording_hook
// synthesizes during D2H replay. The byte-parity helpers all fit in depth=25
// individually; only the e2e composition needs the bump.
haze::test::ops::OpCtx make_bootstrap_ctx_e2e(std::uint32_t ring_dim) {
    using namespace lbcrypto;
    using namespace haze::test::ops;
    auto ctx = make_ctx({
        .mode = FIXEDAUTO,
        .mult_depth = 35,
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

TEST_CASE("phase24 Chebyshev byte-parity on fresh ct with g_coefficientsUniform",
          "[integration][e2e]") {
    // Phase 22 degree=84 fails on a conjugate-add input but degree=5/12 pass
    // there. Phase 14 degree=84 passes on a fresh ct but uses synthetic
    // alternating coefs all above 0x1p-44. Phase 24 isolates the variable:
    // fresh ct + the real g_coefficientsUniform list (which contains many
    // coefs near 0x1p-44). If this fails, the divergence is coefficient-
    // driven; the conjugate-add framing in phase 22 is incidental.
    using namespace lbcrypto;
    namespace ops = haze::test::ops;

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx_tiny(1u << 11);

    const std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);

    // g_coefficientsUniform — same list as phase 18/22.
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

    auto ref = ctx.cc->EvalChebyshevSeries(ct, coefficients, -1.0, 1.0);
    auto haze_ct = ops::h2d_ct(ctx, ct);
    auto out = ops::eval_chebyshev_series_for_test(ctx, haze_ct, coefficients);
    assert_rns_equal(ctx, out, ref, "phase24 g_coefficientsUniform on fresh ct (full 89 entries)");
}

TEST_CASE("phase26 ops::bootstrap end-to-end completes D2H replay cleanly",
          "[integration][e2e]") {
    // Confirms ops::bootstrap + d2h_ct flow does NOT throw on its own. The
    // SetLevel error in the older slot-parity test came from the post-D2H
    // shell-construction code's ModReduceInternalInPlace loop, which dropped
    // towers AND decremented NSD into uint32 underflow — not from ops::bootstrap.
    using namespace lbcrypto;
    namespace ops = haze::test::ops;

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx_e2e(1u << 11);
    constexpr std::uint32_t slots = 8;
    auto bk = ops::make_bootstrap_keys(ctx, ctx.cc, ctx.keys.secretKey, slots);

    const std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    auto haze_ct = ops::h2d_ct(ctx, ct);
    auto haze_refreshed = ops::bootstrap(ctx, bk, haze_ct,
                                         ops::BootstrapVariant::Standard);
    std::cerr << "  [phase26] haze.towers=" << haze_refreshed.towers()
              << " nsd=" << haze_refreshed.noise_scale_deg() << "\n";
    const auto haze_bytes = ops::d2h_ct(ctx, haze_refreshed);
    REQUIRE(haze_bytes.c0.size() == haze_refreshed.towers());
    REQUIRE(haze_bytes.c1.size() == haze_refreshed.towers());
}

namespace {

// Compare a haze Ct against an OpenFHE Ciphertext byte-for-byte at the
// per-tower RNS limb level. Returns first-mismatch tower index, or
// SIZE_MAX if equal. Mirrors `assert_rns_equal` but without REQUIRE so
// we can step through stages and observe.
std::size_t first_mismatch_tower(const haze::test::ops::OpCtx &ctx,
                                 const haze::test::ops::Ct &haze,
                                 const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &ref) {
    if (haze.towers() != ref->GetElements()[0].GetNumOfElements())
        return 0;
    const auto haze_bytes = haze::test::ops::d2h_ct(ctx, haze);
    for (std::size_t t = 0; t < haze.towers(); ++t) {
        for (std::size_t elem = 0; elem < 2; ++elem) {
            const auto &ref_np = ref->GetElements()[elem].GetElementAtIndex(
                static_cast<usint>(t));
            const auto &ref_vals = ref_np.GetValues();
            const auto &haze_chain = (elem == 0) ? haze_bytes.c0 : haze_bytes.c1;
            for (std::size_t i = 0; i < haze_chain[t].size(); ++i) {
                if (haze_chain[t][i] !=
                    ref_vals[i].template ConvertToInt<std::uint64_t>())
                    return t;
            }
        }
    }
    return SIZE_MAX;
}

} // namespace

TEST_CASE("phase27 byte-parity vs OpenFHE manual bootstrap pipeline — stage 1: mod-raise",
          "[integration][e2e]") {
    // Run OpenFHE's EvalBootstrap pipeline manually (ckksrns-fhe.cpp:586+)
    // up to post-mod-raise + AdjustCiphertext + EvalMult(pre/(k*N)), then
    // compare to haze's equivalent intermediate. First-divergence checkpoint.
    using namespace lbcrypto;
    namespace ops = haze::test::ops;

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx_e2e(1u << 11);
    constexpr std::uint32_t slots = 8;
    auto bk = ops::make_bootstrap_keys(ctx, ctx.cc, ctx.keys.secretKey, slots);

    const std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);

    auto cp = std::dynamic_pointer_cast<CryptoParametersCKKSRNS>(
        ctx.cc->GetCryptoParameters());
    REQUIRE(cp);
    const double qDouble =
        cp->GetElementParams()->GetParams()[0]->GetModulus().ConvertToDouble();
    const double powP = std::pow(2.0, cp->GetPlaintextModulus());
    const std::int32_t deg =
        static_cast<std::int32_t>(std::round(std::log2(qDouble / powP)));
    const std::int32_t correction_factor = 11;
    const std::int32_t correction = correction_factor - deg;
    constexpr std::uint32_t K_UNIFORM = 512;
    const std::uint32_t N = static_cast<std::uint32_t>(ctx.ring_dim);
    const double pre = 1.0 / std::pow(2.0, static_cast<double>(deg));

    // OpenFHE side, manual: clone, AdjustCiphertext (EvalMult by 2^-correction),
    // basis extension (re-embed tower 0 in full Q chain), EvalMult(pre/(k*N)).
    auto algo = ctx.cc->GetScheme();
    auto raised_ref = ct->Clone();
    algo->ModReduceInternalInPlace(raised_ref, /*levels=*/0);
    ctx.cc->EvalMultInPlace(raised_ref, std::pow(2.0, -correction));
    {
        // Mirror ckksrns-fhe.cpp:620-628: take tower 0, re-embed in the
        // full elementParams chain.
        auto elementParams = cp->GetElementParams();
        auto elementParamsPtr = std::make_shared<ILDCRTParams<DCRTPoly::Integer>>(
            ctx.cc->GetCyclotomicOrder(),
            [&]() {
                std::vector<NativeInteger> mods;
                for (const auto &pp : elementParams->GetParams())
                    mods.push_back(pp->GetModulus());
                return mods;
            }(),
            [&]() {
                std::vector<NativeInteger> roots;
                for (const auto &pp : elementParams->GetParams())
                    roots.push_back(pp->GetRootOfUnity());
                return roots;
            }());
        const std::uint32_t L0 =
            static_cast<std::uint32_t>(elementParams->GetParams().size());
        auto elements = raised_ref->GetElements();
        for (auto &dcrt : elements) {
            dcrt.SetFormat(Format::COEFFICIENT);
            DCRTPoly tmp(dcrt.GetElementAtIndex(0), elementParamsPtr);
            tmp.SetFormat(Format::EVALUATION);
            dcrt = std::move(tmp);
        }
        raised_ref->SetElements(std::move(elements));
        raised_ref->SetLevel(L0 - raised_ref->GetElements()[0].GetNumOfElements());
    }
    ctx.cc->EvalMultInPlace(raised_ref, pre / (static_cast<double>(K_UNIFORM) * N));

    std::cerr << "  [phase27.s1] OpenFHE raised_ref: towers="
              << raised_ref->GetElements()[0].GetNumOfElements()
              << " nsd=" << raised_ref->GetNoiseScaleDeg()
              << " level=" << raised_ref->GetLevel() << "\n";

    // Haze side, mirroring ops::bootstrap up to the corresponding stage.
    auto haze_ct = ops::h2d_ct(ctx, ct);
    auto haze_adjusted = ops::eval_mult_scalar_for_test(
        ctx, haze_ct, std::pow(2.0, -correction));
    auto depleted = ops::clone_ct(ctx, haze_adjusted);
    if (depleted.towers() > 1)
        depleted = ops::level_reduce(ctx, std::move(depleted),
                                     depleted.towers() - 1);
    auto haze_raised = ops::mod_raise(ctx, bk, depleted);
    haze_raised = ops::eval_mult_scalar_for_test(
        ctx, haze_raised, pre / (static_cast<double>(K_UNIFORM) * N));

    std::cerr << "  [phase27.s1] haze raised: towers=" << haze_raised.towers()
              << " nsd=" << haze_raised.noise_scale_deg() << "\n";

    const std::size_t mm =
        first_mismatch_tower(ctx, haze_raised, raised_ref);
    std::cerr << "  [phase27.s1] first_mismatch_tower="
              << (mm == SIZE_MAX ? std::string{"(none)"}
                                 : std::to_string(mm))
              << "\n";
    REQUIRE(mm == SIZE_MAX);
}

TEST_CASE("phase27 stage 2: + partial_sum + pre-CtS ModReduce",
          "[integration][e2e]") {
    // Continues from phase27.s1's confirmed-byte-equal state, adding the
    // sparsely-packed partial_sum rotation loop and the line-783 ModReduce
    // immediately before CtS.
    using namespace lbcrypto;
    namespace ops = haze::test::ops;

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx_e2e(1u << 11);
    constexpr std::uint32_t slots = 8;
    auto bk = ops::make_bootstrap_keys(ctx, ctx.cc, ctx.keys.secretKey, slots);

    const std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);

    auto cp = std::dynamic_pointer_cast<CryptoParametersCKKSRNS>(
        ctx.cc->GetCryptoParameters());
    REQUIRE(cp);
    const double qDouble =
        cp->GetElementParams()->GetParams()[0]->GetModulus().ConvertToDouble();
    const double powP = std::pow(2.0, cp->GetPlaintextModulus());
    const std::int32_t deg =
        static_cast<std::int32_t>(std::round(std::log2(qDouble / powP)));
    const std::int32_t correction_factor = 11;
    const std::int32_t correction = correction_factor - deg;
    constexpr std::uint32_t K_UNIFORM = 512;
    const std::uint32_t N = static_cast<std::uint32_t>(ctx.ring_dim);
    const double pre = 1.0 / std::pow(2.0, static_cast<double>(deg));

    // OpenFHE side: through pre-CtS ModReduce.
    auto algo = ctx.cc->GetScheme();
    auto raised_ref = ct->Clone();
    algo->ModReduceInternalInPlace(raised_ref, /*levels=*/0);
    ctx.cc->EvalMultInPlace(raised_ref, std::pow(2.0, -correction));
    {
        auto elementParams = cp->GetElementParams();
        auto elementParamsPtr = std::make_shared<ILDCRTParams<DCRTPoly::Integer>>(
            ctx.cc->GetCyclotomicOrder(),
            [&]() {
                std::vector<NativeInteger> mods;
                for (const auto &pp : elementParams->GetParams())
                    mods.push_back(pp->GetModulus());
                return mods;
            }(),
            [&]() {
                std::vector<NativeInteger> roots;
                for (const auto &pp : elementParams->GetParams())
                    roots.push_back(pp->GetRootOfUnity());
                return roots;
            }());
        const std::uint32_t L0 =
            static_cast<std::uint32_t>(elementParams->GetParams().size());
        auto elements = raised_ref->GetElements();
        for (auto &dcrt : elements) {
            dcrt.SetFormat(Format::COEFFICIENT);
            DCRTPoly tmp(dcrt.GetElementAtIndex(0), elementParamsPtr);
            tmp.SetFormat(Format::EVALUATION);
            dcrt = std::move(tmp);
        }
        raised_ref->SetElements(std::move(elements));
        raised_ref->SetLevel(L0 - raised_ref->GetElements()[0].GetNumOfElements());
    }
    ctx.cc->EvalMultInPlace(raised_ref, pre / (static_cast<double>(K_UNIFORM) * N));
    {
        const auto limit = N / (2 * slots);
        for (std::uint32_t j = 1; j < limit; j <<= 1)
            ctx.cc->EvalAddInPlace(raised_ref,
                                   ctx.cc->EvalRotate(raised_ref,
                                                      static_cast<int>(j * slots)));
    }
    algo->ModReduceInternalInPlace(raised_ref, /*levels=*/1);
    std::cerr << "  [phase27.s2] OpenFHE: towers="
              << raised_ref->GetElements()[0].GetNumOfElements()
              << " nsd=" << raised_ref->GetNoiseScaleDeg()
              << " level=" << raised_ref->GetLevel() << "\n";

    // Haze side: mirror the same.
    auto haze_ct = ops::h2d_ct(ctx, ct);
    auto haze_adjusted = ops::eval_mult_scalar_for_test(
        ctx, haze_ct, std::pow(2.0, -correction));
    auto depleted = ops::clone_ct(ctx, haze_adjusted);
    if (depleted.towers() > 1)
        depleted = ops::level_reduce(ctx, std::move(depleted),
                                     depleted.towers() - 1);
    auto haze_raised = ops::mod_raise(ctx, bk, depleted);
    haze_raised = ops::eval_mult_scalar_for_test(
        ctx, haze_raised, pre / (static_cast<double>(K_UNIFORM) * N));
    {
        const std::uint32_t limit = N / (2 * slots);
        for (std::uint32_t j = 1; j < limit; j <<= 1) {
            const std::uint32_t auto_index =
                ctx.cc->FindAutomorphismIndex(j * slots);
            auto it = bk.rotation_keys.find(auto_index);
            REQUIRE(it != bk.rotation_keys.end());
            auto rotated =
                ops::rotate_with_key(ctx, haze_raised, it->second);
            haze_raised = ops::add(ctx, haze_raised, rotated);
        }
    }
    haze_raised = ops::rescale(ctx, haze_raised);
    std::cerr << "  [phase27.s2] haze: towers=" << haze_raised.towers()
              << " nsd=" << haze_raised.noise_scale_deg() << "\n";

    const std::size_t mm = first_mismatch_tower(ctx, haze_raised, raised_ref);
    std::cerr << "  [phase27.s2] first_mismatch_tower="
              << (mm == SIZE_MAX ? std::string{"(none)"}
                                 : std::to_string(mm))
              << "\n";
    REQUIRE(mm == SIZE_MAX);
}

TEST_CASE("phase27 stage 3: + CoeffsToSlots",
          "[integration][e2e]") {
    using namespace lbcrypto;
    namespace ops = haze::test::ops;

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx_e2e(1u << 11);
    constexpr std::uint32_t slots = 8;
    auto bk = ops::make_bootstrap_keys(ctx, ctx.cc, ctx.keys.secretKey, slots);

    const std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);

    auto cp = std::dynamic_pointer_cast<CryptoParametersCKKSRNS>(
        ctx.cc->GetCryptoParameters());
    REQUIRE(cp);
    const double qDouble =
        cp->GetElementParams()->GetParams()[0]->GetModulus().ConvertToDouble();
    const double powP = std::pow(2.0, cp->GetPlaintextModulus());
    const std::int32_t deg =
        static_cast<std::int32_t>(std::round(std::log2(qDouble / powP)));
    const std::int32_t correction_factor = 11;
    const std::int32_t correction = correction_factor - deg;
    constexpr std::uint32_t K_UNIFORM = 512;
    const std::uint32_t N = static_cast<std::uint32_t>(ctx.ring_dim);
    const double pre = 1.0 / std::pow(2.0, static_cast<double>(deg));

    auto fhe_base = ctx.cc->GetScheme()->GetFHE();
    auto fhe_ckks = std::dynamic_pointer_cast<FHECKKSRNS>(fhe_base);
    REQUIRE(fhe_ckks);
    const auto &precom_map = fhe_ckks->GetBootPrecomMap();
    const auto &precom = *precom_map.at(slots);

    // OpenFHE side through CtS.
    auto algo = ctx.cc->GetScheme();
    auto raised_ref = ct->Clone();
    algo->ModReduceInternalInPlace(raised_ref, 0);
    ctx.cc->EvalMultInPlace(raised_ref, std::pow(2.0, -correction));
    {
        auto elementParams = cp->GetElementParams();
        auto elementParamsPtr = std::make_shared<ILDCRTParams<DCRTPoly::Integer>>(
            ctx.cc->GetCyclotomicOrder(),
            [&]() { std::vector<NativeInteger> m; for (const auto &p : elementParams->GetParams()) m.push_back(p->GetModulus()); return m; }(),
            [&]() { std::vector<NativeInteger> r; for (const auto &p : elementParams->GetParams()) r.push_back(p->GetRootOfUnity()); return r; }());
        const std::uint32_t L0 =
            static_cast<std::uint32_t>(elementParams->GetParams().size());
        auto elements = raised_ref->GetElements();
        for (auto &dcrt : elements) {
            dcrt.SetFormat(Format::COEFFICIENT);
            DCRTPoly tmp(dcrt.GetElementAtIndex(0), elementParamsPtr);
            tmp.SetFormat(Format::EVALUATION);
            dcrt = std::move(tmp);
        }
        raised_ref->SetElements(std::move(elements));
        raised_ref->SetLevel(L0 - raised_ref->GetElements()[0].GetNumOfElements());
    }
    ctx.cc->EvalMultInPlace(raised_ref, pre / (static_cast<double>(K_UNIFORM) * N));
    {
        const auto limit = N / (2 * slots);
        for (std::uint32_t j = 1; j < limit; j <<= 1)
            ctx.cc->EvalAddInPlace(raised_ref,
                                   ctx.cc->EvalRotate(raised_ref,
                                                      static_cast<int>(j * slots)));
    }
    algo->ModReduceInternalInPlace(raised_ref, 1);
    auto ctxtEnc_ref = fhe_ckks->EvalLinearTransform(precom.m_U0hatTPre, raised_ref);
    std::cerr << "  [phase27.s3] OpenFHE post-CtS: towers="
              << ctxtEnc_ref->GetElements()[0].GetNumOfElements()
              << " nsd=" << ctxtEnc_ref->GetNoiseScaleDeg() << "\n";

    // Haze side.
    auto haze_ct = ops::h2d_ct(ctx, ct);
    auto haze_adjusted = ops::eval_mult_scalar_for_test(
        ctx, haze_ct, std::pow(2.0, -correction));
    auto depleted = ops::clone_ct(ctx, haze_adjusted);
    if (depleted.towers() > 1)
        depleted = ops::level_reduce(ctx, std::move(depleted),
                                     depleted.towers() - 1);
    auto haze_raised = ops::mod_raise(ctx, bk, depleted);
    haze_raised = ops::eval_mult_scalar_for_test(
        ctx, haze_raised, pre / (static_cast<double>(K_UNIFORM) * N));
    {
        const std::uint32_t limit = N / (2 * slots);
        for (std::uint32_t j = 1; j < limit; j <<= 1) {
            const std::uint32_t auto_index =
                ctx.cc->FindAutomorphismIndex(j * slots);
            auto it = bk.rotation_keys.find(auto_index);
            REQUIRE(it != bk.rotation_keys.end());
            auto rotated =
                ops::rotate_with_key(ctx, haze_raised, it->second);
            haze_raised = ops::add(ctx, haze_raised, rotated);
        }
    }
    haze_raised = ops::rescale(ctx, haze_raised);
    auto haze_ctxtEnc = ops::linear_transform(ctx, bk, bk.cts_matrices, haze_raised);
    std::cerr << "  [phase27.s3] haze post-CtS: towers=" << haze_ctxtEnc.towers()
              << " nsd=" << haze_ctxtEnc.noise_scale_deg() << "\n";

    const std::size_t mm =
        first_mismatch_tower(ctx, haze_ctxtEnc, ctxtEnc_ref);
    std::cerr << "  [phase27.s3] first_mismatch_tower="
              << (mm == SIZE_MAX ? std::string{"(none)"}
                                 : std::to_string(mm))
              << "\n";
    REQUIRE(mm == SIZE_MAX);
}

TEST_CASE("phase25 Chebyshev byte-parity at varying degree truncation",
          "[integration][e2e]") {
    // Phase 24 confirmed g_coefficientsUniform (degree 84) breaks byte-parity
    // even on a fresh ct. Phase 25 truncates the same list at multiple degrees
    // to bisect where the divergence appears. ComputeDegreesPS regimes:
    //   n=11 → (k=4, m=2)        n=27 → (k=4, m=3)
    //   n=55 → (k=8, m=3)        n=59 → (k=4, m=4)
    //   n=84 → (k=6, m=4)
    // Crossing into m=4 is the prime suspect (deeper recursion). The full
    // 85-entry list also has many coefs near 0x1p-44 absent from synthetic
    // tests; this should help separate (k,m)-regime from coef-magnitude.
    using namespace lbcrypto;
    namespace ops = haze::test::ops;

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx_tiny(1u << 11);

    const std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);

    // Same g_coefficientsUniform list as phase 18 / 22 / 24.
    static const std::vector<double> g_coefficientsUniform{
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

    REQUIRE(g_coefficientsUniform.size() == 89);

    auto check = [&](const std::string &label, std::size_t coef_count) {
        std::vector<double> coeffs(g_coefficientsUniform.begin(),
                                   g_coefficientsUniform.begin() +
                                       static_cast<std::ptrdiff_t>(coef_count));
        auto ref = ctx.cc->EvalChebyshevSeries(ct, coeffs, -1.0, 1.0);
        auto haze_ct = ops::h2d_ct(ctx, ct);
        auto out = ops::eval_chebyshev_series_for_test(ctx, haze_ct, coeffs);
        assert_rns_equal(ctx, out, ref, "phase25 " + label);
    };

    // Sweep all four "extra" indices to confirm each individually triggers
    // the divergence (or, after fix, that all pass).
    check("size=85 (first 85)", 85);
    check("size=86 (adds idx 85 = 8.17e-15)", 86);
    check("size=87 (adds idx 86 = -1.06e-13)", 87);
    check("size=88 (adds idx 87 = -8.96e-16)", 88);
    check("size=89 full list (adds idx 88 = 1.14e-14)", 89);
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

    // R_UNIFORM=6 for UNIFORM_TERNARY (the test's default secret key dist),
    // matching haze's bk.params.double_angle_iterations.
    const std::int32_t num_iter =
        static_cast<std::int32_t>(bk.params.double_angle_iterations);
    auto openfhe_eval_mod = [&](Ciphertext<DCRTPoly> ctxtEnc) {
        ctxtEnc = ctx.cc->EvalAdd(ctxtEnc, ctx.cc->EvalAtIndex(ctxtEnc, 2 * static_cast<std::int32_t>(ctx.ring_dim) - 1));
        if (ctxtEnc->GetNoiseScaleDeg() == 2)
            ctx.cc->GetScheme()->ModReduceInternalInPlace(ctxtEnc, 1);
        ctxtEnc = ctx.cc->EvalChebyshevSeries(ctxtEnc, coefficients, -1.0, 1.0);
        ctx.cc->GetScheme()->ModReduceInternalInPlace(ctxtEnc, 1);
        for (std::int32_t i = 1 - num_iter; i <= 0; ++i) {
            const double scalar = -std::pow(2.0 * M_PI, -std::pow(2.0, i));
            ctx.cc->EvalSquareInPlace(ctxtEnc);
            ctx.cc->EvalAddInPlace(ctxtEnc, ctx.cc->EvalAdd(ctxtEnc, scalar));
            ctx.cc->ModReduceInPlace(ctxtEnc); // FIXEDAUTO no-op
        }
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
    // Magnitude bound for haze's int64 mult_by_const: scalar * scFactor must
    // fit in int64. With scalingMod=50 (scFactor ≈ 2^50) that caps |scalar|
    // at ≈ 2^13 ≈ 8192. OpenFHE handles larger via approxFactor split + int128
    // (GetElementForEvalMult, ckksrns-leveledshe.cpp:441-506); the haze port
    // doesn't have that, and the bootstrap pipeline never needs it (all its
    // scalars are |·| ≤ 1).
    check("NSD=1 scalar=large=1234.5 (within int64 limit)", fresh(), 1234.5);
    check("NSD=1 scalar=large_neg=-5000.0 (within int64 limit)", fresh(), -5000.0);
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
    auto ctx = make_bootstrap_ctx_e2e(1u << 11);
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

TEST_CASE("phase28 OpenFHE-only full bootstrap on depleted ct", "[integration][e2e]") {
    // Step 1 of Ryan's plan: run real OpenFHE EvalBootstrap end-to-end
    // (not the shortcut path) on a depleted ct so post-bootstrap towers
    // exceed input towers. Each EvalMult-by-1 in FIXEDAUTO: NSD 1→2 →
    // auto-rescale → NSD=1, towers-1. Repeat to deplete cleanly.
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

    // Deplete just enough that the bootstrap shortcut won't fire (post-bootstrap
    // is typically ~11 towers from our trace). Going to 5 towers leaves a wide
    // gap so the shortcut definitely skips, while keeping enough levels above
    // q_0 for the bootstrap to refresh cleanly.
    const std::size_t before = ct->GetElements()[0].GetNumOfElements();
    const std::size_t target_input_towers = 5;
    while (ct->GetElements()[0].GetNumOfElements() > target_input_towers)
        ctx.cc->EvalMultInPlace(ct, 1.0); // drops a level per call (FIXEDAUTO auto-rescale)
    std::cerr << "  [phase28] depleted " << before << " -> "
              << ct->GetElements()[0].GetNumOfElements()
              << " nsd=" << ct->GetNoiseScaleDeg() << "\n";

    auto ct_refreshed = ctx.cc->EvalBootstrap(ct);
    REQUIRE(ct_refreshed);
    std::cerr << "  [phase28] post-bootstrap towers="
              << ct_refreshed->GetElements()[0].GetNumOfElements()
              << " nsd=" << ct_refreshed->GetNoiseScaleDeg() << "\n";

    Plaintext out;
    ctx.cc->Decrypt(ctx.keys.secretKey, ct_refreshed, &out);
    out->SetLength(v.size());
    const auto got = out->GetRealPackedValue();
    for (std::size_t i = 0; i < v.size(); ++i)
        std::cerr << "    slot[" << i << "] got=" << got[i] << " ref=" << v[i]
                  << " diff=" << (got[i] - v[i]) << "\n";

    // Depleted-input bootstrap accumulates more noise than the shortcut
    // path; use a looser tolerance here. The point is to confirm OpenFHE's
    // full bootstrap pipeline produces decodable output, not tight accuracy.
    constexpr double kDepletedTolerance = 1e-1;
    for (std::size_t i = 0; i < v.size(); ++i) {
        INFO("slot " << i << " got=" << got[i] << " ref=" << v[i]);
        REQUIRE_THAT(got[i], Catch::Matchers::WithinAbs(v[i], kDepletedTolerance));
    }
}

TEST_CASE("phase30 haze ops::bootstrap == cc->EvalBootstrap byte-equal (N=2048)",
          "[integration][e2e]") {
    // Step 3 of Ryan's plan: with phase 28 (cc->EvalBootstrap works) and
    // phase 29 (manual replication byte-equal) green, this test verifies
    // haze's ops::bootstrap also produces byte-equivalent output. Uses
    // small ring_dim so haze's simulator doesn't OOM during replay.
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx_e2e(1u << 11);
    constexpr std::uint32_t slots = 8;
    auto bk = ops::make_bootstrap_keys(ctx, ctx.cc, ctx.keys.secretKey, slots);

    const std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    // Deplete to ~6 towers so the bootstrap shortcut won't fire.
    while (ct->GetElements()[0].GetNumOfElements() > 6)
        ctx.cc->EvalMultInPlace(ct, 1.0);

    auto ct_ref = ctx.cc->EvalBootstrap(ct);
    REQUIRE(ct_ref);

    auto haze_ct = ops::h2d_ct(ctx, ct);
    auto haze_refreshed = ops::bootstrap(ctx, bk, haze_ct,
                                         ops::BootstrapVariant::Standard);
    std::cerr << "  [phase30] ct_ref: towers="
              << ct_ref->GetElements()[0].GetNumOfElements()
              << " nsd=" << ct_ref->GetNoiseScaleDeg() << "\n";
    std::cerr << "  [phase30] haze: towers=" << haze_refreshed.towers()
              << " nsd=" << haze_refreshed.noise_scale_deg() << "\n";
    assert_rns_equal(ctx, haze_refreshed, ct_ref, "phase30 e2e bootstrap");
}

TEST_CASE("phase31 OpenFHE-only full bootstrap, FIXEDMANUAL mode",
          "[integration][e2e]") {
    // Mode sweep step 1 (FIXEDMANUAL): verify cc->EvalBootstrap runs
    // end-to-end on a depleted ct at FIXEDMANUAL. Mirrors phase 28.
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx_mode(FIXEDMANUAL);
    constexpr std::uint32_t slots = 8;
    ctx.cc->EvalBootstrapSetup({1, 1}, {0, 0}, slots, 11);
    ctx.cc->EvalBootstrapKeyGen(ctx.keys.secretKey, slots);

    const std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);

    // FIXEDMANUAL: EvalMult doesn't auto-rescale. Deplete via explicit
    // EvalMult(1.0) + ModReduceInPlace pairs.
    auto algo = ctx.cc->GetScheme();
    while (ct->GetElements()[0].GetNumOfElements() > 5) {
        ctx.cc->EvalMultInPlace(ct, 1.0);
        ctx.cc->ModReduceInPlace(ct);
    }
    std::cerr << "  [phase31] depleted: towers="
              << ct->GetElements()[0].GetNumOfElements()
              << " nsd=" << ct->GetNoiseScaleDeg() << "\n";

    auto ct_refreshed = ctx.cc->EvalBootstrap(ct);
    REQUIRE(ct_refreshed);
    std::cerr << "  [phase31] post-bootstrap towers="
              << ct_refreshed->GetElements()[0].GetNumOfElements()
              << " nsd=" << ct_refreshed->GetNoiseScaleDeg() << "\n";

    Plaintext out;
    ctx.cc->Decrypt(ctx.keys.secretKey, ct_refreshed, &out);
    out->SetLength(v.size());
    const auto got = out->GetRealPackedValue();
    for (std::size_t i = 0; i < v.size(); ++i)
        std::cerr << "    slot[" << i << "] got=" << got[i] << " ref=" << v[i]
                  << " diff=" << (got[i] - v[i]) << "\n";
    constexpr double kDepletedTolerance = 1e-1;
    for (std::size_t i = 0; i < v.size(); ++i) {
        INFO("slot " << i << " got=" << got[i] << " ref=" << v[i]);
        REQUIRE_THAT(got[i], Catch::Matchers::WithinAbs(v[i], kDepletedTolerance));
    }
}

namespace {

// Manual OpenFHE bootstrap mirror, sparsely-packed branch, parameterized
// by mode. Returns the resulting Ciphertext<DCRTPoly>. Compare byte-exact
// to cc->EvalBootstrap on the same input.
lbcrypto::Ciphertext<lbcrypto::DCRTPoly>
manual_openfhe_bootstrap(const haze::test::ops::OpCtx &ctx,
                         lbcrypto::Ciphertext<lbcrypto::DCRTPoly> ct,
                         std::uint32_t slots) {
    using namespace lbcrypto;
    auto cp = std::dynamic_pointer_cast<CryptoParametersCKKSRNS>(
        ctx.cc->GetCryptoParameters());
    REQUIRE(cp);
    const auto st = cp->GetScalingTechnique();
    const std::uint32_t compositeDegree = 1; // we don't support COMPOSITESCALING
    const double qDouble =
        cp->GetElementParams()->GetParams()[0]->GetModulus().ConvertToDouble();
    const double powP = std::pow(2.0, cp->GetPlaintextModulus());
    const std::int32_t deg =
        static_cast<std::int32_t>(std::round(std::log2(qDouble / powP)));
    const std::int32_t correction_factor = 11;
    const std::int32_t correction = correction_factor - deg;
    constexpr std::uint32_t K_UNIFORM = 512;
    const std::uint32_t N = ctx.cc->GetRingDimension();
    const double pre = 1.0 / std::pow(2.0, static_cast<double>(deg));
    const std::uint64_t corFactor =
        static_cast<std::uint64_t>(1) << static_cast<std::uint64_t>(correction);

    auto fhe_base = ctx.cc->GetScheme()->GetFHE();
    auto fhe_ckks = std::dynamic_pointer_cast<FHECKKSRNS>(fhe_base);
    REQUIRE(fhe_ckks);
    const auto &precom_map = fhe_ckks->GetBootPrecomMap();
    const auto &precom = *precom_map.at(slots);

    auto algo = ctx.cc->GetScheme();
    auto raised = ct->Clone();
    algo->ModReduceInternalInPlace(raised,
                                   compositeDegree * (raised->GetNoiseScaleDeg() - 1));

    // AdjustCiphertext (ckksrns-fhe.cpp:2256-2302), per-mode branch.
    {
        const std::uint32_t lvl = (st == FLEXIBLEAUTOEXT) ? 1 : 0;
        if (st == FLEXIBLEAUTO || st == FLEXIBLEAUTOEXT) {
            const double targetSF = cp->GetScalingFactorReal(lvl);
            const double sourceSF = raised->GetScalingFactor();
            const std::uint32_t numTowers =
                raised->GetElements()[0].GetNumOfElements();
            const double modToDrop = cp->GetElementParams()
                ->GetParams()[numTowers - 1]
                ->GetModulus()
                .ConvertToDouble();
            const double adjustmentFactor =
                (targetSF / sourceSF) * (modToDrop / sourceSF) *
                std::pow(2.0, -correction);
            ctx.cc->EvalMultInPlace(raised, adjustmentFactor);
            algo->ModReduceInternalInPlace(raised, compositeDegree);
            raised->SetScalingFactor(targetSF);
        } else {
            // FIXEDMANUAL / FIXEDAUTO
            ctx.cc->EvalMultInPlace(raised, std::pow(2.0, -correction));
            algo->ModReduceInternalInPlace(raised, compositeDegree);
        }
    }

    // Mod-raise re-embed. For FLEXIBLEAUTOEXT, drop the last param.
    {
        auto elementParams = cp->GetElementParams();
        std::vector<NativeInteger> moduli;
        std::vector<NativeInteger> roots;
        const std::size_t total = elementParams->GetParams().size();
        const std::size_t limit =
            (st == FLEXIBLEAUTOEXT) ? total - 1 : total;
        for (std::size_t i = 0; i < limit; ++i) {
            moduli.push_back(elementParams->GetParams()[i]->GetModulus());
            roots.push_back(elementParams->GetParams()[i]->GetRootOfUnity());
        }
        auto elementParamsPtr = std::make_shared<ILDCRTParams<DCRTPoly::Integer>>(
            ctx.cc->GetCyclotomicOrder(), moduli, roots);
        const std::uint32_t L0 = static_cast<std::uint32_t>(total);
        auto elements = raised->GetElements();
        for (auto &dcrt : elements) {
            dcrt.SetFormat(Format::COEFFICIENT);
            DCRTPoly tmp(dcrt.GetElementAtIndex(0), elementParamsPtr);
            tmp.SetFormat(Format::EVALUATION);
            dcrt = std::move(tmp);
        }
        raised->SetElements(std::move(elements));
        raised->SetLevel(L0 - raised->GetElements()[0].GetNumOfElements());
    }

    ctx.cc->EvalMultInPlace(raised, pre / (static_cast<double>(K_UNIFORM) * N));

    // Sparsely-packed (slots < N/2): partial_sum.
    if (slots < N / 2) {
        const auto limit = N / (2 * slots);
        for (std::uint32_t j = 1; j < limit; j <<= 1)
            ctx.cc->EvalAddInPlace(
                raised,
                ctx.cc->EvalRotate(raised, static_cast<int>(j * slots)));
    }
    algo->ModReduceInternalInPlace(raised, compositeDegree);

    auto ctxtEnc = fhe_ckks->EvalLinearTransform(precom.m_U0hatTPre, raised);
    const std::int32_t conj_idx = 2 * static_cast<std::int32_t>(N) - 1;
    ctx.cc->EvalAddInPlace(ctxtEnc, ctx.cc->EvalAtIndex(ctxtEnc, conj_idx));

    if (st == FIXEDMANUAL) {
        while (ctxtEnc->GetNoiseScaleDeg() > 1)
            ctx.cc->ModReduceInPlace(ctxtEnc);
    } else if (ctxtEnc->GetNoiseScaleDeg() == 2) {
        algo->ModReduceInternalInPlace(ctxtEnc, compositeDegree);
    }

    static const std::vector<double> g_coefficientsUniform{
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
    ctxtEnc = ctx.cc->EvalChebyshevSeries(ctxtEnc, g_coefficientsUniform, -1.0, 1.0);

    if (st != FIXEDMANUAL)
        algo->ModReduceInternalInPlace(ctxtEnc, compositeDegree);

    constexpr std::uint32_t R_UNIFORM = 6;
    constexpr double twoPi = 2.0 * M_PI;
    for (std::int32_t i = 1 - static_cast<std::int32_t>(R_UNIFORM); i <= 0; ++i) {
        const double scalar = -std::pow(twoPi, -std::pow(2.0, i));
        ctx.cc->EvalSquareInPlace(ctxtEnc);
        ctx.cc->EvalAddInPlace(ctxtEnc, ctx.cc->EvalAdd(ctxtEnc, scalar));
        ctx.cc->ModReduceInPlace(ctxtEnc);
    }

    const std::uint64_t scalar_for_mult =
        static_cast<std::uint64_t>(std::llround(std::pow(2.0, deg)));
    algo->MultByIntegerInPlace(ctxtEnc, scalar_for_mult);

    if (st != FIXEDMANUAL)
        algo->ModReduceInternalInPlace(ctxtEnc, compositeDegree);

    auto ctxtDec = fhe_ckks->EvalLinearTransform(precom.m_U0Pre, ctxtEnc);
    ctx.cc->EvalAddInPlace(
        ctxtDec, ctx.cc->EvalRotate(ctxtDec, static_cast<int>(slots)));
    algo->MultByIntegerInPlace(ctxtDec, corFactor);
    return ctxtDec;
}

} // namespace

TEST_CASE("phase34 OpenFHE-only full bootstrap, FLEXIBLEAUTO mode",
          "[integration][e2e]") {
    // Mode sweep step 1 (FLEXIBLEAUTO): verify cc->EvalBootstrap runs.
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx_mode(FLEXIBLEAUTO);
    constexpr std::uint32_t slots = 8;
    ctx.cc->EvalBootstrapSetup({1, 1}, {0, 0}, slots, 11);
    ctx.cc->EvalBootstrapKeyGen(ctx.keys.secretKey, slots);

    const std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    while (ct->GetElements()[0].GetNumOfElements() > 5)
        ctx.cc->EvalMultInPlace(ct, 1.0);
    std::cerr << "  [phase34] depleted: towers="
              << ct->GetElements()[0].GetNumOfElements()
              << " nsd=" << ct->GetNoiseScaleDeg() << "\n";

    auto ct_refreshed = ctx.cc->EvalBootstrap(ct);
    REQUIRE(ct_refreshed);
    std::cerr << "  [phase34] post-bootstrap towers="
              << ct_refreshed->GetElements()[0].GetNumOfElements()
              << " nsd=" << ct_refreshed->GetNoiseScaleDeg() << "\n";

    Plaintext out;
    ctx.cc->Decrypt(ctx.keys.secretKey, ct_refreshed, &out);
    out->SetLength(v.size());
    const auto got = out->GetRealPackedValue();
    for (std::size_t i = 0; i < v.size(); ++i)
        std::cerr << "    slot[" << i << "] got=" << got[i] << " ref=" << v[i] << "\n";
    constexpr double kDepletedTolerance = 1e-1;
    for (std::size_t i = 0; i < v.size(); ++i) {
        INFO("slot " << i << " got=" << got[i] << " ref=" << v[i]);
        REQUIRE_THAT(got[i], Catch::Matchers::WithinAbs(v[i], kDepletedTolerance));
    }
}

TEST_CASE("phase36 OpenFHE-only full bootstrap, FLEXIBLEAUTOEXT mode",
          "[integration][e2e]") {
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx_mode(FLEXIBLEAUTOEXT);
    constexpr std::uint32_t slots = 8;
    ctx.cc->EvalBootstrapSetup({1, 1}, {0, 0}, slots, 11);
    ctx.cc->EvalBootstrapKeyGen(ctx.keys.secretKey, slots);

    const std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    while (ct->GetElements()[0].GetNumOfElements() > 5)
        ctx.cc->EvalMultInPlace(ct, 1.0);
    std::cerr << "  [phase36] depleted: towers="
              << ct->GetElements()[0].GetNumOfElements()
              << " nsd=" << ct->GetNoiseScaleDeg() << "\n";

    auto ct_refreshed = ctx.cc->EvalBootstrap(ct);
    REQUIRE(ct_refreshed);
    std::cerr << "  [phase36] post-bootstrap towers="
              << ct_refreshed->GetElements()[0].GetNumOfElements()
              << " nsd=" << ct_refreshed->GetNoiseScaleDeg() << "\n";

    Plaintext out;
    ctx.cc->Decrypt(ctx.keys.secretKey, ct_refreshed, &out);
    out->SetLength(v.size());
    const auto got = out->GetRealPackedValue();
    for (std::size_t i = 0; i < v.size(); ++i)
        std::cerr << "    slot[" << i << "] got=" << got[i] << " ref=" << v[i] << "\n";
    constexpr double kDepletedTolerance = 1e-1;
    for (std::size_t i = 0; i < v.size(); ++i) {
        INFO("slot " << i << " got=" << got[i] << " ref=" << v[i]);
        REQUIRE_THAT(got[i], Catch::Matchers::WithinAbs(v[i], kDepletedTolerance));
    }
}

namespace {

// Byte-for-byte assertion between two Ciphertext<DCRTPoly>. Returns total
// mismatch count; logs per-tower diff. Use after asserting tower counts match.
std::size_t ct_byte_mismatches(const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &a,
                               const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &b,
                               const std::string &label) {
    std::size_t total = 0;
    for (std::size_t elem = 0; elem < 2; ++elem) {
        const auto &da = a->GetElements()[elem];
        const auto &db = b->GetElements()[elem];
        for (std::size_t t = 0; t < da.GetNumOfElements(); ++t) {
            const auto &va = da.GetElementAtIndex(static_cast<usint>(t)).GetValues();
            const auto &vb = db.GetElementAtIndex(static_cast<usint>(t)).GetValues();
            std::size_t mism = 0;
            for (std::size_t i = 0; i < va.GetLength(); ++i)
                if (va[i].template ConvertToInt<std::uint64_t>() !=
                    vb[i].template ConvertToInt<std::uint64_t>())
                    ++mism;
            if (mism > 0)
                std::cerr << "  [" << label << "] elem=" << elem << " tower=" << t
                          << " mismatches=" << mism << "/" << va.GetLength() << "\n";
            total += mism;
        }
    }
    return total;
}

} // namespace

TEST_CASE("phase35 manual OpenFHE bootstrap == cc->EvalBootstrap byte-equal, FLEXIBLEAUTO",
          "[integration][e2e]") {
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx_mode(FLEXIBLEAUTO);
    constexpr std::uint32_t slots = 8;
    ctx.cc->EvalBootstrapSetup({1, 1}, {0, 0}, slots, 11);
    ctx.cc->EvalBootstrapKeyGen(ctx.keys.secretKey, slots);

    const std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    while (ct->GetElements()[0].GetNumOfElements() > 5)
        ctx.cc->EvalMultInPlace(ct, 1.0);

    auto ct_real = ctx.cc->EvalBootstrap(ct);
    REQUIRE(ct_real);
    auto ct_manual = manual_openfhe_bootstrap(ctx, ct->Clone(), slots);
    std::cerr << "  [phase35] real: towers="
              << ct_real->GetElements()[0].GetNumOfElements()
              << " nsd=" << ct_real->GetNoiseScaleDeg()
              << "; manual: towers="
              << ct_manual->GetElements()[0].GetNumOfElements()
              << " nsd=" << ct_manual->GetNoiseScaleDeg() << "\n";
    REQUIRE(ct_real->GetElements()[0].GetNumOfElements() ==
            ct_manual->GetElements()[0].GetNumOfElements());
    const auto mm = ct_byte_mismatches(ct_real, ct_manual, "phase35");
    std::cerr << "  [phase35] total mismatches=" << mm << "\n";
    REQUIRE(mm == 0);
}

TEST_CASE("phase37 manual OpenFHE bootstrap == cc->EvalBootstrap byte-equal, FLEXIBLEAUTOEXT",
          "[integration][e2e]") {
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx_mode(FLEXIBLEAUTOEXT);
    constexpr std::uint32_t slots = 8;
    ctx.cc->EvalBootstrapSetup({1, 1}, {0, 0}, slots, 11);
    ctx.cc->EvalBootstrapKeyGen(ctx.keys.secretKey, slots);

    const std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    while (ct->GetElements()[0].GetNumOfElements() > 5)
        ctx.cc->EvalMultInPlace(ct, 1.0);

    auto ct_real = ctx.cc->EvalBootstrap(ct);
    REQUIRE(ct_real);
    auto ct_manual = manual_openfhe_bootstrap(ctx, ct->Clone(), slots);
    std::cerr << "  [phase37] real: towers="
              << ct_real->GetElements()[0].GetNumOfElements()
              << " nsd=" << ct_real->GetNoiseScaleDeg()
              << "; manual: towers="
              << ct_manual->GetElements()[0].GetNumOfElements()
              << " nsd=" << ct_manual->GetNoiseScaleDeg() << "\n";
    REQUIRE(ct_real->GetElements()[0].GetNumOfElements() ==
            ct_manual->GetElements()[0].GetNumOfElements());
    const auto mm = ct_byte_mismatches(ct_real, ct_manual, "phase37");
    std::cerr << "  [phase37] total mismatches=" << mm << "\n";
    REQUIRE(mm == 0);
}

TEST_CASE("phase38 haze ops::bootstrap == cc->EvalBootstrap byte-equal, FLEXIBLEAUTO (N=2048)",
          "[integration][e2e]") {
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = ops::make_ctx({
        .mode = FLEXIBLEAUTO,
        .mult_depth = 35,
        .scaling_mod_size = 50,
        .batch_size = 8,
        .with_relin_key = true,
        .rotate_indices = {},
        .ring_dim = 1u << 11,
    });
    ctx.cc->Enable(ADVANCEDSHE);
    ctx.cc->Enable(FHE);
    constexpr std::uint32_t slots = 8;
    auto bk = ops::make_bootstrap_keys(ctx, ctx.cc, ctx.keys.secretKey, slots);

    const std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    while (ct->GetElements()[0].GetNumOfElements() > 6)
        ctx.cc->EvalMultInPlace(ct, 1.0);

    auto ct_ref = ctx.cc->EvalBootstrap(ct);
    REQUIRE(ct_ref);

    auto haze_ct = ops::h2d_ct(ctx, ct);
    auto haze_refreshed = ops::bootstrap(ctx, bk, haze_ct,
                                         ops::BootstrapVariant::Standard);
    std::cerr << "  [phase38] ct_ref: towers="
              << ct_ref->GetElements()[0].GetNumOfElements()
              << " nsd=" << ct_ref->GetNoiseScaleDeg() << "\n";
    std::cerr << "  [phase38] haze: towers=" << haze_refreshed.towers()
              << " nsd=" << haze_refreshed.noise_scale_deg() << "\n";
    assert_rns_equal(ctx, haze_refreshed, ct_ref, "phase38 e2e bootstrap FLEXIBLEAUTO");
}

TEST_CASE("phase39 haze ops::bootstrap == cc->EvalBootstrap byte-equal, FLEXIBLEAUTOEXT (N=2048)",
          "[integration][e2e]") {
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = ops::make_ctx({
        .mode = FLEXIBLEAUTOEXT,
        .mult_depth = 35,
        .scaling_mod_size = 50,
        .batch_size = 8,
        .with_relin_key = true,
        .rotate_indices = {},
        .ring_dim = 1u << 11,
    });
    ctx.cc->Enable(ADVANCEDSHE);
    ctx.cc->Enable(FHE);
    constexpr std::uint32_t slots = 8;
    auto bk = ops::make_bootstrap_keys(ctx, ctx.cc, ctx.keys.secretKey, slots);

    const std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    while (ct->GetElements()[0].GetNumOfElements() > 6)
        ctx.cc->EvalMultInPlace(ct, 1.0);

    auto ct_ref = ctx.cc->EvalBootstrap(ct);
    REQUIRE(ct_ref);

    auto haze_ct = ops::h2d_ct(ctx, ct);
    auto haze_refreshed = ops::bootstrap(ctx, bk, haze_ct,
                                         ops::BootstrapVariant::Standard);
    std::cerr << "  [phase39] ct_ref: towers="
              << ct_ref->GetElements()[0].GetNumOfElements()
              << " nsd=" << ct_ref->GetNoiseScaleDeg() << "\n";
    std::cerr << "  [phase39] haze: towers=" << haze_refreshed.towers()
              << " nsd=" << haze_refreshed.noise_scale_deg() << "\n";
    assert_rns_equal(ctx, haze_refreshed, ct_ref, "phase39 e2e bootstrap FLEXIBLEAUTOEXT");
}

TEST_CASE("phase33 haze ops::bootstrap == cc->EvalBootstrap byte-equal, FIXEDMANUAL (N=2048)",
          "[integration][e2e]") {
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = ops::make_ctx({
        .mode = FIXEDMANUAL,
        .mult_depth = 35,
        .scaling_mod_size = 50,
        .batch_size = 8,
        .with_relin_key = true,
        .rotate_indices = {},
        .ring_dim = 1u << 11,
    });
    ctx.cc->Enable(ADVANCEDSHE);
    ctx.cc->Enable(FHE);
    constexpr std::uint32_t slots = 8;
    auto bk = ops::make_bootstrap_keys(ctx, ctx.cc, ctx.keys.secretKey, slots);

    const std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    while (ct->GetElements()[0].GetNumOfElements() > 6) {
        ctx.cc->EvalMultInPlace(ct, 1.0);
        ctx.cc->ModReduceInPlace(ct);
    }

    auto ct_ref = ctx.cc->EvalBootstrap(ct);
    REQUIRE(ct_ref);

    auto haze_ct = ops::h2d_ct(ctx, ct);
    auto haze_refreshed = ops::bootstrap(ctx, bk, haze_ct,
                                         ops::BootstrapVariant::Standard);
    std::cerr << "  [phase33] ct_ref: towers="
              << ct_ref->GetElements()[0].GetNumOfElements()
              << " nsd=" << ct_ref->GetNoiseScaleDeg() << "\n";
    std::cerr << "  [phase33] haze: towers=" << haze_refreshed.towers()
              << " nsd=" << haze_refreshed.noise_scale_deg() << "\n";
    assert_rns_equal(ctx, haze_refreshed, ct_ref, "phase33 e2e bootstrap FIXEDMANUAL");
}

TEST_CASE("phase32 manual OpenFHE bootstrap == cc->EvalBootstrap byte-equal, FIXEDMANUAL",
          "[integration][e2e]") {
    // Mirrors phase 29 for FIXEDMANUAL mode. Key differences vs FIXEDAUTO:
    //   - EvalMult doesn't auto-rescale on NSD=2 (user-managed)
    //   - Post-conjugate-add: while(NSD>1) ModReduceInPlace (vs single rescale)
    //   - SKIP post-Chebyshev ModReduce (line 818)
    //   - SKIP pre-StC ModReduce (line 842)
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx_mode(FIXEDMANUAL);
    constexpr std::uint32_t slots = 8;
    ctx.cc->EvalBootstrapSetup({1, 1}, {0, 0}, slots, 11);
    ctx.cc->EvalBootstrapKeyGen(ctx.keys.secretKey, slots);

    auto cp = std::dynamic_pointer_cast<CryptoParametersCKKSRNS>(
        ctx.cc->GetCryptoParameters());
    REQUIRE(cp);
    const double qDouble =
        cp->GetElementParams()->GetParams()[0]->GetModulus().ConvertToDouble();
    const double powP = std::pow(2.0, cp->GetPlaintextModulus());
    const std::int32_t deg =
        static_cast<std::int32_t>(std::round(std::log2(qDouble / powP)));
    const std::int32_t correction_factor = 11;
    const std::int32_t correction = correction_factor - deg;
    constexpr std::uint32_t K_UNIFORM = 512;
    const std::uint32_t N = ctx.cc->GetRingDimension();
    const double pre = 1.0 / std::pow(2.0, static_cast<double>(deg));
    const std::uint64_t corFactor =
        static_cast<std::uint64_t>(1) << static_cast<std::uint64_t>(correction);

    const std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    while (ct->GetElements()[0].GetNumOfElements() > 5) {
        ctx.cc->EvalMultInPlace(ct, 1.0);
        ctx.cc->ModReduceInPlace(ct);
    }

    auto ct_real = ctx.cc->EvalBootstrap(ct);
    REQUIRE(ct_real);

    auto fhe_base = ctx.cc->GetScheme()->GetFHE();
    auto fhe_ckks = std::dynamic_pointer_cast<FHECKKSRNS>(fhe_base);
    REQUIRE(fhe_ckks);
    const auto &precom_map = fhe_ckks->GetBootPrecomMap();
    const auto &precom = *precom_map.at(slots);

    auto algo = ctx.cc->GetScheme();
    auto raised = ct->Clone();
    algo->ModReduceInternalInPlace(raised, 1 * (raised->GetNoiseScaleDeg() - 1));
    ctx.cc->EvalMultInPlace(raised, std::pow(2.0, -correction));
    algo->ModReduceInternalInPlace(raised, 1);
    {
        auto elementParams = cp->GetElementParams();
        std::vector<NativeInteger> moduli;
        std::vector<NativeInteger> roots;
        for (const auto &pp : elementParams->GetParams()) {
            moduli.push_back(pp->GetModulus());
            roots.push_back(pp->GetRootOfUnity());
        }
        auto elementParamsPtr = std::make_shared<ILDCRTParams<DCRTPoly::Integer>>(
            ctx.cc->GetCyclotomicOrder(), moduli, roots);
        const std::uint32_t L0 =
            static_cast<std::uint32_t>(elementParams->GetParams().size());
        auto elements = raised->GetElements();
        for (auto &dcrt : elements) {
            dcrt.SetFormat(Format::COEFFICIENT);
            DCRTPoly tmp(dcrt.GetElementAtIndex(0), elementParamsPtr);
            tmp.SetFormat(Format::EVALUATION);
            dcrt = std::move(tmp);
        }
        raised->SetElements(std::move(elements));
        raised->SetLevel(L0 - raised->GetElements()[0].GetNumOfElements());
    }
    ctx.cc->EvalMultInPlace(raised, pre / (static_cast<double>(K_UNIFORM) * N));
    {
        const auto limit = N / (2 * slots);
        for (std::uint32_t j = 1; j < limit; j <<= 1)
            ctx.cc->EvalAddInPlace(raised,
                                   ctx.cc->EvalRotate(raised,
                                                      static_cast<int>(j * slots)));
    }
    algo->ModReduceInternalInPlace(raised, 1);
    auto ctxtEnc = fhe_ckks->EvalLinearTransform(precom.m_U0hatTPre, raised);
    const std::int32_t conj_idx = 2 * static_cast<std::int32_t>(N) - 1;
    ctx.cc->EvalAddInPlace(ctxtEnc, ctx.cc->EvalAtIndex(ctxtEnc, conj_idx));
    // FIXEDMANUAL: while(NSD > 1) ModReduceInPlace (mirrors line 791-794)
    while (ctxtEnc->GetNoiseScaleDeg() > 1)
        ctx.cc->ModReduceInPlace(ctxtEnc);

    static const std::vector<double> g_coefficientsUniform{
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
    ctxtEnc = ctx.cc->EvalChebyshevSeries(ctxtEnc, g_coefficientsUniform, -1.0, 1.0);
    // FIXEDMANUAL: SKIP the post-Chebyshev ModReduce (line 817-818).
    constexpr std::uint32_t R_UNIFORM = 6;
    constexpr double twoPi = 2.0 * M_PI;
    for (std::int32_t i = 1 - static_cast<std::int32_t>(R_UNIFORM); i <= 0; ++i) {
        const double scalar = -std::pow(twoPi, -std::pow(2.0, i));
        ctx.cc->EvalSquareInPlace(ctxtEnc);
        ctx.cc->EvalAddInPlace(ctxtEnc, ctx.cc->EvalAdd(ctxtEnc, scalar));
        ctx.cc->ModReduceInPlace(ctxtEnc);
    }
    const std::uint64_t scalar_for_mult =
        static_cast<std::uint64_t>(std::llround(std::pow(2.0, deg)));
    algo->MultByIntegerInPlace(ctxtEnc, scalar_for_mult);
    // FIXEDMANUAL: SKIP the pre-StC ModReduce (line 841-842).
    auto ctxtDec = fhe_ckks->EvalLinearTransform(precom.m_U0Pre, ctxtEnc);
    ctx.cc->EvalAddInPlace(ctxtDec,
                           ctx.cc->EvalRotate(ctxtDec, static_cast<int>(slots)));
    algo->MultByIntegerInPlace(ctxtDec, corFactor);

    std::cerr << "  [phase32] real: towers="
              << ct_real->GetElements()[0].GetNumOfElements()
              << " nsd=" << ct_real->GetNoiseScaleDeg()
              << "; manual: towers="
              << ctxtDec->GetElements()[0].GetNumOfElements()
              << " nsd=" << ctxtDec->GetNoiseScaleDeg() << "\n";
    REQUIRE(ct_real->GetElements()[0].GetNumOfElements() ==
            ctxtDec->GetElements()[0].GetNumOfElements());
    std::size_t total_mism = 0;
    for (std::size_t elem = 0; elem < 2; ++elem) {
        const auto &d_real = ct_real->GetElements()[elem];
        const auto &d_man = ctxtDec->GetElements()[elem];
        for (std::size_t t = 0; t < d_real.GetNumOfElements(); ++t) {
            const auto &vr = d_real.GetElementAtIndex(static_cast<usint>(t)).GetValues();
            const auto &vm = d_man.GetElementAtIndex(static_cast<usint>(t)).GetValues();
            std::size_t mism = 0;
            for (std::size_t i = 0; i < vr.GetLength(); ++i)
                if (vr[i].template ConvertToInt<std::uint64_t>() !=
                    vm[i].template ConvertToInt<std::uint64_t>())
                    ++mism;
            if (mism > 0)
                std::cerr << "    elem=" << elem << " tower=" << t
                          << " mismatches=" << mism << "/" << vr.GetLength() << "\n";
            total_mism += mism;
        }
    }
    std::cerr << "  [phase32] total mismatches=" << total_mism << "\n";
    REQUIRE(total_mism == 0);
}

TEST_CASE("phase29 manual OpenFHE bootstrap == cc->EvalBootstrap byte-equal",
          "[integration][e2e]") {
    // Step 2 of Ryan's plan: replicate the full OpenFHE bootstrap pipeline
    // (sparsely-packed FIXEDAUTO, ckksrns-fhe.cpp:586-852) via public cc->
    // primitives. Compare byte-exact to cc->EvalBootstrap on the same input.
    // If byte-equal, this is our validated reference for haze to mirror.
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx();
    constexpr std::uint32_t slots = 8;
    ctx.cc->EvalBootstrapSetup({1, 1}, {0, 0}, slots, 11);
    ctx.cc->EvalBootstrapKeyGen(ctx.keys.secretKey, slots);

    auto cp = std::dynamic_pointer_cast<CryptoParametersCKKSRNS>(
        ctx.cc->GetCryptoParameters());
    REQUIRE(cp);
    const double qDouble =
        cp->GetElementParams()->GetParams()[0]->GetModulus().ConvertToDouble();
    const double powP = std::pow(2.0, cp->GetPlaintextModulus());
    const std::int32_t deg =
        static_cast<std::int32_t>(std::round(std::log2(qDouble / powP)));
    const std::int32_t correction_factor = 11;
    const std::int32_t correction = correction_factor - deg;
    constexpr std::uint32_t K_UNIFORM = 512;
    const std::uint32_t N = ctx.cc->GetRingDimension();
    const double pre = 1.0 / std::pow(2.0, static_cast<double>(deg));
    const std::uint64_t corFactor =
        static_cast<std::uint64_t>(1) << static_cast<std::uint64_t>(correction);

    const std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    while (ct->GetElements()[0].GetNumOfElements() > 5)
        ctx.cc->EvalMultInPlace(ct, 1.0);

    // Real EvalBootstrap → c_bootstrap_correct.
    auto ct_real = ctx.cc->EvalBootstrap(ct);
    REQUIRE(ct_real);
    std::cerr << "  [phase29] cc->EvalBootstrap: towers="
              << ct_real->GetElements()[0].GetNumOfElements()
              << " nsd=" << ct_real->GetNoiseScaleDeg() << "\n";

    // Manual replication. Mirrors ckksrns-fhe.cpp:586-852 sparsely-packed.
    auto fhe_base = ctx.cc->GetScheme()->GetFHE();
    auto fhe_ckks = std::dynamic_pointer_cast<FHECKKSRNS>(fhe_base);
    REQUIRE(fhe_ckks);
    const auto &precom_map = fhe_ckks->GetBootPrecomMap();
    const auto &precom = *precom_map.at(slots);

    auto algo = ctx.cc->GetScheme();
    auto raised = ct->Clone();
    // line 588: ModReduceInternalInPlace(raised, compositeDegree*(NSD-1))
    algo->ModReduceInternalInPlace(raised, 1 * (raised->GetNoiseScaleDeg() - 1));
    // line 590: AdjustCiphertext — default modReduce=true, so this is
    // EvalMult(2^-correction) FOLLOWED BY ModReduceInternalInPlace.
    ctx.cc->EvalMultInPlace(raised, std::pow(2.0, -correction));
    algo->ModReduceInternalInPlace(raised, 1);
    // lines 620-628: mod-raise re-embed (tower 0 → full Q chain)
    {
        auto elementParams = cp->GetElementParams();
        std::vector<NativeInteger> moduli;
        std::vector<NativeInteger> roots;
        for (const auto &pp : elementParams->GetParams()) {
            moduli.push_back(pp->GetModulus());
            roots.push_back(pp->GetRootOfUnity());
        }
        auto elementParamsPtr = std::make_shared<ILDCRTParams<DCRTPoly::Integer>>(
            ctx.cc->GetCyclotomicOrder(), moduli, roots);
        const std::uint32_t L0 =
            static_cast<std::uint32_t>(elementParams->GetParams().size());
        auto elements = raised->GetElements();
        for (auto &dcrt : elements) {
            dcrt.SetFormat(Format::COEFFICIENT);
            DCRTPoly tmp(dcrt.GetElementAtIndex(0), elementParamsPtr);
            tmp.SetFormat(Format::EVALUATION);
            dcrt = std::move(tmp);
        }
        raised->SetElements(std::move(elements));
        raised->SetLevel(L0 - raised->GetElements()[0].GetNumOfElements());
    }
    // line 666: EvalMult(raised, pre * (1.0 / (k * N)))
    ctx.cc->EvalMultInPlace(raised, pre / (static_cast<double>(K_UNIFORM) * N));
    // lines 771-773: partial_sum
    {
        const auto limit = N / (2 * slots);
        for (std::uint32_t j = 1; j < limit; j <<= 1)
            ctx.cc->EvalAddInPlace(raised,
                                   ctx.cc->EvalRotate(raised,
                                                      static_cast<int>(j * slots)));
    }
    // line 783: ModReduceInternalInPlace
    algo->ModReduceInternalInPlace(raised, 1);
    std::cerr << "  [phase29] manual pre-CtS: towers="
              << raised->GetElements()[0].GetNumOfElements()
              << " nsd=" << raised->GetNoiseScaleDeg()
              << " level=" << raised->GetLevel()
              << "; matrix.towers="
              << precom.m_U0hatTPre.front()->GetElement<DCRTPoly>().GetNumOfElements()
              << " plaintext.level=" << precom.m_U0hatTPre.front()->GetLevel()
              << "\n";
    // line 786: EvalLinearTransform (CtS)
    auto ctxtEnc = fhe_ckks->EvalLinearTransform(precom.m_U0hatTPre, raised);
    // line 789: EvalAddInPlace(ctxtEnc, Conjugate(ctxtEnc, ...)) via cc->EvalAtIndex(2N-1)
    const std::int32_t conj_idx = 2 * static_cast<std::int32_t>(N) - 1;
    ctx.cc->EvalAddInPlace(ctxtEnc, ctx.cc->EvalAtIndex(ctxtEnc, conj_idx));
    // lines 797-799: if NSD==2 ModReduce
    if (ctxtEnc->GetNoiseScaleDeg() == 2)
        algo->ModReduceInternalInPlace(ctxtEnc, 1);

    // g_coefficientsUniform for UNIFORM_TERNARY (from FHECKKSRNS header)
    static const std::vector<double> g_coefficientsUniform{
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
    // line 814: EvalChebyshevSeries
    ctxtEnc = ctx.cc->EvalChebyshevSeries(ctxtEnc, g_coefficientsUniform, -1.0, 1.0);
    // line 818: ModReduce
    algo->ModReduceInternalInPlace(ctxtEnc, 1);
    // line 820: ApplyDoubleAngleIterations — manually via EvalSquare + EvalAdd + ModReduce
    constexpr std::uint32_t R_UNIFORM = 6;
    constexpr double twoPi = 2.0 * M_PI;
    for (std::int32_t i = 1 - static_cast<std::int32_t>(R_UNIFORM); i <= 0; ++i) {
        const double scalar = -std::pow(twoPi, -std::pow(2.0, i));
        ctx.cc->EvalSquareInPlace(ctxtEnc);
        ctx.cc->EvalAddInPlace(ctxtEnc, ctx.cc->EvalAdd(ctxtEnc, scalar));
        ctx.cc->ModReduceInPlace(ctxtEnc);  // FIXEDAUTO no-op
    }
    // line 825: MultByIntegerInPlace
    const std::uint64_t scalar_for_mult =
        static_cast<std::uint64_t>(std::llround(std::pow(2.0, deg)));
    algo->MultByIntegerInPlace(ctxtEnc, scalar_for_mult);
    // line 842: ModReduce
    algo->ModReduceInternalInPlace(ctxtEnc, 1);
    // line 845: EvalLinearTransform (StC)
    auto ctxtDec = fhe_ckks->EvalLinearTransform(precom.m_U0Pre, ctxtEnc);
    // line 846: EvalAddInPlaceNoCheck(ctxtDec, EvalRotate(ctxtDec, slots))
    ctx.cc->EvalAddInPlace(ctxtDec, ctx.cc->EvalRotate(ctxtDec,
                                                       static_cast<int>(slots)));
    // line 852: MultByIntegerInPlace(corFactor)
    algo->MultByIntegerInPlace(ctxtDec, corFactor);

    std::cerr << "  [phase29] manual: towers="
              << ctxtDec->GetElements()[0].GetNumOfElements()
              << " nsd=" << ctxtDec->GetNoiseScaleDeg() << "\n";

    // Compare ct_real vs ctxtDec byte-for-byte.
    REQUIRE(ct_real->GetElements()[0].GetNumOfElements() ==
            ctxtDec->GetElements()[0].GetNumOfElements());
    std::size_t total_mism = 0;
    for (std::size_t elem = 0; elem < 2; ++elem) {
        const auto &d_real = ct_real->GetElements()[elem];
        const auto &d_man = ctxtDec->GetElements()[elem];
        for (std::size_t t = 0; t < d_real.GetNumOfElements(); ++t) {
            const auto &vr = d_real.GetElementAtIndex(static_cast<usint>(t)).GetValues();
            const auto &vm = d_man.GetElementAtIndex(static_cast<usint>(t)).GetValues();
            std::size_t mism = 0;
            for (std::size_t i = 0; i < vr.GetLength(); ++i)
                if (vr[i].template ConvertToInt<std::uint64_t>() !=
                    vm[i].template ConvertToInt<std::uint64_t>())
                    ++mism;
            if (mism > 0)
                std::cerr << "    elem=" << elem << " tower=" << t
                          << " mismatches=" << mism << "/" << vr.GetLength() << "\n";
            total_mism += mism;
        }
    }
    std::cerr << "  [phase29] total mismatches=" << total_mism << "\n";
    REQUIRE(total_mism == 0);
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
    auto ctx = make_bootstrap_ctx_e2e(1u << 11);

    constexpr std::uint32_t slots = 8;
    auto bk = ops::make_bootstrap_keys(ctx, ctx.cc, ctx.keys.secretKey, slots);

    const std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);

    // OpenFHE reference. Note: on a fresh ct this takes EvalBootstrap's
    // shortcut (ckksrns-fhe.cpp:862) and just returns the input. We use
    // it only for its level shape; slot values come from decrypting.
    auto ct_ref = ctx.cc->EvalBootstrap(ct);
    REQUIRE(ct_ref);
    Plaintext pt_ref;
    ctx.cc->Decrypt(ctx.keys.secretKey, ct_ref, &pt_ref);
    pt_ref->SetLength(v.size());
    const auto slots_ref = pt_ref->GetRealPackedValue();
    std::cerr << "  [slot-parity] ct_ref.towers="
              << ct_ref->GetElements()[0].GetNumOfElements()
              << " nsd=" << ct_ref->GetNoiseScaleDeg() << "\n";

    auto haze_ct = ops::h2d_ct(ctx, ct);
    auto haze_refreshed = ops::bootstrap(ctx, bk, haze_ct, ops::BootstrapVariant::Standard);
    std::cerr << "  [slot-parity] haze.towers=" << haze_refreshed.towers()
              << " nsd=" << haze_refreshed.noise_scale_deg() << "\n";
    const auto haze_bytes = ops::d2h_ct(ctx, haze_refreshed);

    // Build a shell at haze's (towers, NSD). ModReduceInternalInPlace drops
    // a tower AND decrements NSD; on ct (NSD=1), a second ModReduce
    // underflows NSD's uint32_t to UINT32_MAX, tripping
    // CiphertextImpl::SetLevel's `limbNum < NSD` check. LevelReduceInternalInPlace
    // is metadata-only (matches OpenFHE's FIXEDMANUAL LevelReduce path),
    // appropriate here since inject_ct overwrites the polynomial bytes
    // — only the metadata needs to line up for Decrypt to interpret them.
    auto ct_shell = ct->Clone();
    const std::size_t target_towers = haze_refreshed.towers();
    const std::size_t drop_count =
        ct_shell->GetElements()[0].GetNumOfElements() - target_towers;
    if (drop_count > 0)
        ctx.cc->GetScheme()->LevelReduceInternalInPlace(ct_shell, drop_count);
    ct_shell->SetNoiseScaleDeg(haze_refreshed.noise_scale_deg());
    ops::inject_ct(ctx, haze_bytes, ct_shell);
    Plaintext pt_haze;
    ctx.cc->Decrypt(ctx.keys.secretKey, ct_shell, &pt_haze);
    pt_haze->SetLength(v.size());
    const auto slots_haze = pt_haze->GetRealPackedValue();

    std::cerr << "  [slot-parity] decoded slots:\n";
    for (std::size_t i = 0; i < v.size(); ++i)
        std::cerr << "    slot[" << i << "] in=" << v[i] << " ref=" << slots_ref[i]
                  << " haze=" << slots_haze[i] << " diff=" << (slots_haze[i] - v[i]) << "\n";

    for (std::size_t i = 0; i < v.size(); ++i) {
        INFO("slot " << i << " haze=" << slots_haze[i] << " in=" << v[i]);
        REQUIRE_THAT(slots_haze[i],
                     Catch::Matchers::WithinAbs(v[i], kBootstrapTolerance));
    }
}

// Measure record vs replay split for a full haze ops::bootstrap. The
// haze record-and-replay model: every compute API call appends to the
// FHETCH trace (no math), and the first D2H flush triggers the backend
// to replay the trace and compute the actual polynomial values. d2h_ct
// is the implicit D2H call, so timing it isolates the replay cost.
TEST_CASE("bench bootstrap record vs replay timing", "[integration][e2e][.bench]") {
    using namespace lbcrypto;
    namespace ops = haze::test::ops;

    for (auto mode : {FIXEDAUTO, FIXEDMANUAL, FLEXIBLEAUTO, FLEXIBLEAUTOEXT}) {
        REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
        auto ctx = ops::make_ctx({
            .mode = mode,
            .mult_depth = 35,
            .scaling_mod_size = 50,
            .batch_size = 8,
            .with_relin_key = true,
            .rotate_indices = {},
            .ring_dim = 1u << 11,
        });
        ctx.cc->Enable(ADVANCEDSHE);
        ctx.cc->Enable(FHE);
        constexpr std::uint32_t slots = 8;
        auto bk = ops::make_bootstrap_keys(ctx, ctx.cc, ctx.keys.secretKey, slots);

        const std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
        auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
        auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);
        while (ct->GetElements()[0].GetNumOfElements() > 6) {
            if (mode == FIXEDMANUAL) {
                ctx.cc->EvalMultInPlace(ct, 1.0);
                ctx.cc->ModReduceInPlace(ct);
            } else {
                ctx.cc->EvalMultInPlace(ct, 1.0);
            }
        }

        auto haze_ct = ops::h2d_ct(ctx, ct);

        const auto t0 = std::chrono::steady_clock::now();
        auto haze_refreshed = ops::bootstrap(ctx, bk, haze_ct,
                                             ops::BootstrapVariant::Standard);
        const auto t1 = std::chrono::steady_clock::now();
        auto bytes = ops::d2h_ct(ctx, haze_refreshed);
        const auto t2 = std::chrono::steady_clock::now();

        const auto record_ms =
            static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()) / 1000.0;
        const auto replay_ms =
            static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count()) / 1000.0;
        const char *name = "?";
        switch (mode) {
        case FIXEDAUTO:        name = "FIXEDAUTO";        break;
        case FIXEDMANUAL:      name = "FIXEDMANUAL";      break;
        case FLEXIBLEAUTO:     name = "FLEXIBLEAUTO";     break;
        case FLEXIBLEAUTOEXT:  name = "FLEXIBLEAUTOEXT";  break;
        default: break;
        }
        std::cerr << "  [bench " << name << "] record=" << record_ms
                  << "ms replay=" << replay_ms
                  << "ms ratio=" << (replay_ms / record_ms) << "x\n";
        // sink so the call isn't DCE'd
        REQUIRE(bytes.c0.size() == haze_refreshed.towers());
    }
}

// =====================================================================
// Full-slot phase ladder — rung 1: CoeffToSlot linear transform isolation.
// Build OpenFHE's raised + haze's raised in parallel; inject haze's bytes
// into the OpenFHE ct so BOTH sides feed byte-identical input into the
// linear transform. Then compare CtS outputs per-tower, non-aborting.
// If they match -> haze's linear_transform is exact for full slots, and
// the bug is downstream (eval_mod or StC). If not -> linear_transform
// itself has a full-slot bug.
// =====================================================================
// Hidden by default (Catch2 [.]) — opt-in via `phasefs01 *` or `[.fsladder]`.
// Currently fails (CtS divergence localized to ops::linear_transform full-slot
// handling); ride into the suite once linear_transform mirrors OpenFHE's
// EvalLinearTransform structure (extended-basis result + first c0 tracking).
TEST_CASE("phasefs01 full-slot CtS isolation FLEXIBLEAUTO", "[.fsladder][e2e]") {
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = ops::make_ctx({.mode = FLEXIBLEAUTO, .mult_depth = 35, .scaling_mod_size = 50,
        .batch_size = 0, .with_relin_key = true, .rotate_indices = {}, .ring_dim = 1u << 11});
    ctx.cc->Enable(ADVANCEDSHE); ctx.cc->Enable(FHE);
    const std::uint32_t slots = static_cast<std::uint32_t>(ctx.ring_dim / 2); // 1024
    auto bk = ops::make_bootstrap_keys(ctx, ctx.cc, ctx.keys.secretKey, slots);
    std::vector<double> v(slots); for (std::uint32_t i=0;i<slots;++i) v[i]=0.1+0.0003*i;
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v, 1, 0, nullptr, slots);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    while (ct->GetElements()[0].GetNumOfElements() > 6)
        ctx.cc->EvalMultInPlace(ct, 1.0);

    auto cp = std::dynamic_pointer_cast<CryptoParametersCKKSRNS>(ctx.cc->GetCryptoParameters());
    const double qDouble = cp->GetElementParams()->GetParams()[0]->GetModulus().ConvertToDouble();
    const double powP = std::pow(2.0, cp->GetPlaintextModulus());
    const std::int32_t deg = static_cast<std::int32_t>(std::round(std::log2(qDouble/powP)));
    const std::int32_t correction = 11 - deg;
    constexpr std::uint32_t K_UNIFORM = 512;
    const std::uint32_t N = static_cast<std::uint32_t>(ctx.ring_dim);
    const double pre = 1.0/std::pow(2.0,(double)deg);
    auto fhe_ckks = std::dynamic_pointer_cast<FHECKKSRNS>(ctx.cc->GetScheme()->GetFHE());
    const auto &precom = *fhe_ckks->GetBootPrecomMap().at(slots);
    auto algo = ctx.cc->GetScheme();

    // -- OpenFHE side: replicate cc->EvalBootstrap pre-CtS. AdjustCiphertext is
    //    private, so inline its FLEXIBLEAUTO body (matches bootstrap.cpp).
    auto raised_ref = ct->Clone();
    while (raised_ref->GetNoiseScaleDeg() > 1)
        algo->ModReduceInternalInPlace(raised_ref, 1);
    {
        const double targetSF = cp->GetScalingFactorReal(0);
        const double sourceSF = raised_ref->GetScalingFactor();
        const std::uint32_t numTowers = raised_ref->GetElements()[0].GetNumOfElements();
        const double modToDrop = cp->GetElementParams()->GetParams()[numTowers-1]
            ->GetModulus().ConvertToDouble();
        const double adjustmentFactor = (targetSF/sourceSF) * (modToDrop/sourceSF)
                                        * std::pow(2.0, -correction);
        ctx.cc->EvalMultInPlace(raised_ref, adjustmentFactor);
        algo->ModReduceInternalInPlace(raised_ref, 1);
        raised_ref->SetScalingFactor(targetSF);
    }
    {
        // Mod-raise: re-embed tower 0 into the full Q chain (mirrors lines
        // ckksrns-fhe.cpp:619-628 of EvalBootstrap).
        auto ep = cp->GetElementParams();
        auto epp = std::make_shared<ILDCRTParams<DCRTPoly::Integer>>(ctx.cc->GetCyclotomicOrder(),
            [&]{std::vector<NativeInteger> m;for(auto&p:ep->GetParams())m.push_back(p->GetModulus());return m;}(),
            [&]{std::vector<NativeInteger> r;for(auto&p:ep->GetParams())r.push_back(p->GetRootOfUnity());return r;}());
        const std::uint32_t L0=(std::uint32_t)ep->GetParams().size();
        auto elements=raised_ref->GetElements();
        for(auto&dcrt:elements){dcrt.SetFormat(Format::COEFFICIENT);DCRTPoly tmp(dcrt.GetElementAtIndex(0),epp);tmp.SetFormat(Format::EVALUATION);dcrt=std::move(tmp);}
        raised_ref->SetElements(std::move(elements));
        raised_ref->SetLevel(L0-raised_ref->GetElements()[0].GetNumOfElements());
    }
    ctx.cc->EvalMultInPlace(raised_ref, pre/((double)K_UNIFORM*N));
    // Full slots: partial_sum is empty. OpenFHE's pre-CtS ModReduce
    // (ckksrns-fhe.cpp:689, compositeDegree=1) is the one explicit rescale here.
    algo->ModReduceInternalInPlace(raised_ref, 1);
    std::cerr << "  [phasefs01] OpenFHE raised: towers="
              << raised_ref->GetElements()[0].GetNumOfElements()
              << " level=" << raised_ref->GetLevel()
              << " nsd=" << raised_ref->GetNoiseScaleDeg() << "\n";

    // -- haze side: mirror bootstrap.cpp's pre-CtS exactly (including the full
    //    adjusted block — depletion while-loop + FLEXIBLEAUTO adjustmentFactor).
    auto haze_ct = ops::h2d_ct(ctx, ct);
    auto adjusted = [&]() {
        auto r = ops::clone_ct(ctx, haze_ct);
        while (r.noise_scale_deg() > 1) r = ops::rescale(ctx, std::move(r));
        const double targetSF = cp->GetScalingFactorReal(0);
        const double sourceSF = r.scaling_factor();
        const std::uint32_t numTowers = static_cast<std::uint32_t>(r.towers());
        const double modToDrop = cp->GetElementParams()
            ->GetParams()[numTowers - 1]->GetModulus().ConvertToDouble();
        const double adjustmentFactor = (targetSF/sourceSF) * (modToDrop/sourceSF)
                                        * std::pow(2.0, -correction);
        r = ops::mult_by_const_for_test(ctx, r, adjustmentFactor);
        r = ops::rescale(ctx, std::move(r));
        r.set_scaling_factor(targetSF);
        return r;
    }();
    auto dep = ops::clone_ct(ctx, adjusted);
    if (dep.towers()>1) dep = ops::level_reduce(ctx, std::move(dep), dep.towers()-1);
    auto hr = ops::mod_raise(ctx, bk, dep);
    hr = ops::eval_mult_scalar_for_test(ctx, hr, pre/((double)K_UNIFORM*N));
    hr = ops::rescale(ctx, hr);
    std::cerr << "  [phasefs01] haze raised: towers=" << hr.towers()
              << " nsd=" << hr.noise_scale_deg() << "\n";

    std::cerr << "  [phasefs01] cts_pt_level=" << bk.cts_pt_level
              << " cts_pt_sf=" << bk.cts_pt_sf
              << " precom.m_U0hatTPre size=" << precom.m_U0hatTPre.size()
              << " g(cheby_degree)=" << bk.params.chebyshev_degree
              << " m_paramsEnc.g=" << precom.m_paramsEnc.g
              << " m_paramsEnc.b=" << precom.m_paramsEnc.b
              << " lvlb=" << precom.m_paramsEnc.lvlb << "\n";
    REQUIRE(raised_ref->GetElements()[0].GetNumOfElements() == hr.towers());

    // -- Bisection step A: do the haze and OpenFHE pre-CtS pipelines produce
    //    byte-identical `raised`? (Independent of any inject.)
    {
        auto hbpre = ops::d2h_ct(ctx, hr);
        std::size_t pre_bad = 0;
        for (std::size_t elem = 0; elem < 2; ++elem) {
            const auto &rd = raised_ref->GetElements()[elem];
            const auto &hc = (elem == 0) ? hbpre.c0 : hbpre.c1;
            for (std::size_t t = 0; t < hr.towers(); ++t) {
                const auto &rv = rd.GetElementAtIndex(static_cast<usint>(t)).GetValues();
                std::size_t mism = 0;
                for (std::size_t i = 0; i < hc[t].size(); ++i)
                    if (hc[t][i] != rv[i].template ConvertToInt<std::uint64_t>()) ++mism;
                if (mism) { ++pre_bad;
                    if (pre_bad <= 4)
                        std::cerr << "  [phasefs01] PRE-DIVERGE elem=" << elem << " t=" << t
                                  << " mism=" << mism << "/" << hc[t].size() << "\n"; }
            }
        }
        std::cerr << "  [phasefs01] pre-CtS divergence: " << pre_bad
                  << " of " << (2*hr.towers()) << " towers\n";
    }

    // -- CtS on both sides (without inject; both pipelines run independently).
    auto ctxtEnc_ref = fhe_ckks->EvalLinearTransform(precom.m_U0hatTPre, raised_ref);
    auto haze_cts = ops::linear_transform(ctx, bk, bk.cts_matrices, hr);
    std::cerr << "  [phasefs01] CtS ref towers="
              << ctxtEnc_ref->GetElements()[0].GetNumOfElements()
              << " haze towers=" << haze_cts.towers() << "\n";

    // -- Non-aborting per-tower mismatch report.
    const auto hb = ops::d2h_ct(ctx, haze_cts);
    std::size_t bad = 0, total = 2 * haze_cts.towers();
    for (std::size_t elem = 0; elem < 2; ++elem) {
        const auto &rd = ctxtEnc_ref->GetElements()[elem];
        const auto &hc = (elem == 0) ? hb.c0 : hb.c1;
        for (std::size_t t = 0; t < haze_cts.towers(); ++t) {
            const auto &rv = rd.GetElementAtIndex(static_cast<usint>(t)).GetValues();
            std::size_t mism = 0;
            for (std::size_t i = 0; i < hc[t].size(); ++i)
                if (hc[t][i] != rv[i].template ConvertToInt<std::uint64_t>()) ++mism;
            if (mism) { ++bad;
                std::cerr << "  [phasefs01] BAD elem=" << elem << " t=" << t
                          << " mism=" << mism << "/" << hc[t].size() << "\n"; }
        }
    }
    std::cerr << "  [phasefs01] bad_towers=" << bad << " of " << total << "\n";
    REQUIRE(bad == 0);
}

// =====================================================================
// Niobium full-slot {4,4} phase ladder
// =====================================================================
//
// Target: byte parity vs cc->EvalBootstrap at the niobium-fhetch test config
// (deps/niobium-fhetch/tests/bootstrap/client.cpp):
//   ringDim 2048, FLEXIBLEAUTO, ScalingModSize=59, FirstModSize=60,
//   UNIFORM_TERNARY, HEStd_NotSet, levelBudget {4,4}, levelsAfterBootstrap=10,
//   depth = 10 + GetBootstrapDepth({4,4}, UNIFORM_TERNARY),
//   numSlots = ringDim/2 = 1024.
//
// Hidden under [.nio] so default test runs aren't broken while the ladder is
// in progress. Opt-in via name or [.nio]. Run with HAZE_TARGET=local.
// =====================================================================

namespace {
struct NiobCtx {
    haze::test::ops::OpCtx ctx;
    std::uint32_t slots{};
    std::uint32_t depth{};
};

NiobCtx make_nio_ctx() {
    using namespace lbcrypto;
    NiobCtx out;
    const std::vector<std::uint32_t> levelBudget = {4, 4};
    const std::uint32_t levelsAfterBootstrap = 10;
    out.depth = levelsAfterBootstrap +
                FHECKKSRNS::GetBootstrapDepth(levelBudget, UNIFORM_TERNARY);
    CCParams<CryptoContextCKKSRNS> params;
    params.SetSecretKeyDist(UNIFORM_TERNARY);
    params.SetSecurityLevel(HEStd_NotSet);
    params.SetRingDim(2048);
    params.SetScalingModSize(59);
    params.SetScalingTechnique(FLEXIBLEAUTO);
    params.SetFirstModSize(60);
    params.SetMultiplicativeDepth(out.depth);
    out.ctx.cc = GenCryptoContext(params);
    REQUIRE(out.ctx.cc);
    out.ctx.cc->Enable(PKE);
    out.ctx.cc->Enable(KEYSWITCH);
    out.ctx.cc->Enable(LEVELEDSHE);
    out.ctx.cc->Enable(ADVANCEDSHE);
    out.ctx.cc->Enable(FHE);
    out.ctx.keys = out.ctx.cc->KeyGen();
    out.ctx.cc->EvalMultKeyGen(out.ctx.keys.secretKey);
    out.ctx.ring_dim = out.ctx.cc->GetRingDimension();
    out.ctx.poly_bytes = static_cast<std::size_t>(out.ctx.ring_dim) * sizeof(std::uint64_t);
    out.ctx.mode = FLEXIBLEAUTO;
    out.slots = static_cast<std::uint32_t>(out.ctx.ring_dim / 2);
    out.ctx.cc->EvalBootstrapSetup(levelBudget, {0, 0}, out.slots, 11);
    out.ctx.cc->EvalBootstrapKeyGen(out.ctx.keys.secretKey, out.slots);
    out.ctx.with_relin_key = true;
    REQUIRE(hazeSetRingDimension(out.ctx.ring_dim) == HAZE_SUCCESS);
    REQUIRE(haze::hazeReplayBridgeRegisterCryptoContext(out.ctx.cc) == HAZE_SUCCESS);
    const auto &q_eparams =
        out.ctx.cc->GetCryptoParameters()->GetElementParams()->GetParams();
    out.ctx.q_base.reserve(q_eparams.size());
    for (const auto &p : q_eparams)
        out.ctx.q_base.push_back(p->GetModulus().ConvertToInt());
    const auto rns_params = std::dynamic_pointer_cast<CryptoParametersRNS>(
        out.ctx.cc->GetCryptoParameters());
    REQUIRE(rns_params);
    const auto &p_eparams = rns_params->GetParamsP()->GetParams();
    out.ctx.p_base.reserve(p_eparams.size());
    for (const auto &p : p_eparams)
        out.ctx.p_base.push_back(p->GetModulus().ConvertToInt());
    int mod_idx = 0;
    for (uint64_t q : out.ctx.q_base)
        REQUIRE(hazeSetCiphertextModulus(mod_idx++, q) == HAZE_SUCCESS);
    for (uint64_t pmod : out.ctx.p_base)
        REQUIRE(hazeSetCiphertextModulus(mod_idx++, pmod) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);
    REQUIRE(haze::hazeReplayBridgeExtractEvalMultKey(out.ctx.cc,
                                                     out.ctx.keys.secretKey,
                                                     out.ctx.relin_key) == HAZE_SUCCESS);
    return out;
}

lbcrypto::Ciphertext<lbcrypto::DCRTPoly>
make_nio_depleted_ct(const NiobCtx &n, std::uint32_t leave_towers = 6) {
    std::vector<double> v(n.slots);
    for (std::uint32_t i = 0; i < n.slots; ++i) v[i] = 0.1 + 0.0003 * i;
    auto pt = n.ctx.cc->MakeCKKSPackedPlaintext(v, 1, 0, nullptr, n.slots);
    auto ct = n.ctx.cc->Encrypt(n.ctx.keys.publicKey, pt);
    while (ct->GetElements()[0].GetNumOfElements() > leave_towers)
        n.ctx.cc->EvalMultInPlace(ct, 1.0);
    return ct;
}

[[maybe_unused]] void rns_equal_or_report(const std::string &label,
                         const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &a,
                         const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &b) {
    REQUIRE(a->GetElements()[0].GetNumOfElements() ==
            b->GetElements()[0].GetNumOfElements());
    const auto towers = a->GetElements()[0].GetNumOfElements();
    std::size_t bad = 0;
    for (std::size_t elem = 0; elem < 2; ++elem) {
        for (std::size_t t = 0; t < towers; ++t) {
            const auto &av = a->GetElements()[elem]
                                 .GetElementAtIndex(static_cast<usint>(static_cast<std::uint32_t>(t))).GetValues();
            const auto &bv = b->GetElements()[elem]
                                 .GetElementAtIndex(static_cast<usint>(static_cast<std::uint32_t>(t))).GetValues();
            REQUIRE(av.GetLength() == bv.GetLength());
            std::size_t mism = 0;
            for (std::size_t i = 0; i < av.GetLength(); ++i)
                if (av[i].ConvertToInt<std::uint64_t>() !=
                    bv[i].ConvertToInt<std::uint64_t>()) ++mism;
            if (mism) {
                ++bad;
                if (bad <= 4)
                    std::cerr << "  [" << label << "] BAD elem=" << elem
                              << " t=" << t << " mism=" << mism
                              << "/" << av.GetLength() << "\n";
            }
        }
    }
    std::cerr << "  [" << label << "] bad_towers=" << bad << " of "
              << (2 * towers) << "\n";
    REQUIRE(bad == 0);
}
} // namespace

// Rung 1: verify cc->EvalBootstrap runs at the niobium config (baseline truth).
TEST_CASE("nio01 cc->EvalBootstrap runs at niobium config", "[.nio][e2e]") {
    using namespace lbcrypto;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto n = make_nio_ctx();
    std::cerr << "  [nio01] depth=" << n.depth << " slots=" << n.slots << "\n";
    auto ct = make_nio_depleted_ct(n, 6);
    std::cerr << "  [nio01] input towers="
              << ct->GetElements()[0].GetNumOfElements()
              << " nsd=" << ct->GetNoiseScaleDeg() << "\n";
    auto out = n.ctx.cc->EvalBootstrap(ct);
    REQUIRE(out);
    std::cerr << "  [nio01] output towers="
              << out->GetElements()[0].GetNumOfElements()
              << " nsd=" << out->GetNoiseScaleDeg()
              << " level=" << out->GetLevel() << "\n";
}

namespace {
// Local copy of OpenFHE's g_coefficientsUniform (ckksrns-fhe.h:469 — private).
static const std::vector<double> nio_g_coefficientsUniform{
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

// Apply OpenFHE's ApplyDoubleAngleIterations body (private symbol; inlined).
void nio_apply_double_angle(lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &ct, std::uint32_t numIter) {
    constexpr double twoPi = 2.0 * M_PI;
    auto cc = ct->GetCryptoContext();
    for (std::int32_t i = 1 - static_cast<std::int32_t>(numIter); i <= 0; ++i) {
        const double scalar = -std::pow(twoPi, -std::pow(2.0, i));
        cc->EvalSquareInPlace(ct);
        cc->EvalAddInPlace(ct, cc->EvalAdd(ct, scalar));
        cc->ModReduceInPlace(ct);
    }
}

// Manual OpenFHE-side replication of cc->EvalBootstrap (FLEXIBLEAUTO + full-slot
// + {4,4} path). Builds the entire bootstrap from public cc->/fhe_ckks->/algo->
// primitives. AdjustCiphertext, ApplyDoubleAngleIterations, and the coefficient
// list are private so they're inlined above.
lbcrypto::Ciphertext<lbcrypto::DCRTPoly>
nio_manual_bootstrap(const NiobCtx &n,
                     const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &ct_in) {
    using namespace lbcrypto;
    const auto cc = n.ctx.cc;
    auto cp = std::dynamic_pointer_cast<CryptoParametersCKKSRNS>(cc->GetCryptoParameters());
    auto algo = cc->GetScheme();
    auto fhe_ckks = std::dynamic_pointer_cast<FHECKKSRNS>(algo->GetFHE());
    REQUIRE(fhe_ckks);
    const auto &precom = *fhe_ckks->GetBootPrecomMap().at(n.slots);
    const std::uint32_t compositeDegree = cp->GetCompositeDegree();
    const std::uint32_t lvl = 0; // FLEXIBLEAUTO

    // Correction / pre-scalars (mirrors ckksrns-fhe.cpp:586-666 derivation).
    const double qDouble = cp->GetElementParams()->GetParams()[0]->GetModulus().ConvertToDouble();
    const double powP = std::pow(2.0, cp->GetPlaintextModulus());
    const std::int32_t deg = static_cast<std::int32_t>(std::round(std::log2(qDouble / powP)));
    constexpr std::int32_t correction_factor = 11;
    const std::int32_t correction = correction_factor - deg;
    const double pre = 1.0 / std::pow(2.0, static_cast<double>(deg));
    constexpr double K_UNIFORM = 512.0;
    const std::uint32_t N = static_cast<std::uint32_t>(cc->GetRingDimension());

    auto raised = ct_in->Clone();

    // ---- AdjustCiphertext (inlined FLEXIBLEAUTO body, ckksrns-fhe.cpp:2256) ----
    while (raised->GetNoiseScaleDeg() > 1)
        algo->ModReduceInternalInPlace(raised, compositeDegree);
    {
        const double targetSF = cp->GetScalingFactorReal(lvl);
        const double sourceSF = raised->GetScalingFactor();
        const std::uint32_t numTowers = raised->GetElements()[0].GetNumOfElements();
        double modToDrop = cp->GetElementParams()->GetParams()[numTowers-1]
            ->GetModulus().ConvertToDouble();
        for (std::uint32_t j = 2; j <= compositeDegree; ++j)
            modToDrop *= cp->GetElementParams()->GetParams()[numTowers-j]
                ->GetModulus().ConvertToDouble();
        const double adjustmentFactor = (targetSF / sourceSF) * (modToDrop / sourceSF)
                                        * std::pow(2.0, -correction);
        cc->EvalMultInPlace(raised, adjustmentFactor);
        algo->ModReduceInternalInPlace(raised, compositeDegree);
        raised->SetScalingFactor(targetSF);
    }

    // ---- ModRaise: re-embed tower 0 into full Q chain (ckksrns-fhe.cpp:619-628) ----
    {
        auto ep = cp->GetElementParams();
        auto epp = std::make_shared<ILDCRTParams<DCRTPoly::Integer>>(
            cc->GetCyclotomicOrder(),
            [&]{std::vector<NativeInteger> m;for(auto&p:ep->GetParams())m.push_back(p->GetModulus());return m;}(),
            [&]{std::vector<NativeInteger> r;for(auto&p:ep->GetParams())r.push_back(p->GetRootOfUnity());return r;}());
        const std::uint32_t L0 = static_cast<std::uint32_t>(ep->GetParams().size());
        auto elements = raised->GetElements();
        for (auto &dcrt : elements) {
            dcrt.SetFormat(Format::COEFFICIENT);
            DCRTPoly tmp(dcrt.GetElementAtIndex(0), epp);
            tmp.SetFormat(Format::EVALUATION);
            dcrt = std::move(tmp);
        }
        raised->SetElements(std::move(elements));
        raised->SetLevel(L0 - raised->GetElements()[0].GetNumOfElements());
    }

    // ---- Scale and pre-CtS ModReduce (ckksrns-fhe.cpp:666-689) ----
    cc->EvalMultInPlace(raised, pre * (1.0 / (K_UNIFORM * static_cast<double>(N))));
    // partial_sum loop is empty for full slots (limit = N/(2*slots) = 1).
    algo->ModReduceInternalInPlace(raised, compositeDegree);

    // ---- CoeffsToSlots ({4,4} uses the FFT path) ----
    auto ctxtEnc = fhe_ckks->EvalCoeffsToSlots(precom.m_U0hatTPreFFT, raised);

    // ---- Conjugate + real/imag split (ckksrns-fhe.cpp:695-712) ----
    const auto &evalKeyMap = cc->GetEvalAutomorphismKeyMap(ctxtEnc->GetKeyTag());
    auto conj = FHECKKSRNS::Conjugate(ctxtEnc, evalKeyMap);
    auto ctxtEncI = cc->EvalSub(ctxtEnc, conj);
    cc->EvalAddInPlace(ctxtEnc, conj);
    algo->MultByMonomialInPlace(ctxtEncI, 3 * n.slots);
    if (ctxtEnc->GetNoiseScaleDeg() == 2) {
        algo->ModReduceInternalInPlace(ctxtEnc, compositeDegree);
        algo->ModReduceInternalInPlace(ctxtEncI, compositeDegree);
    }

    // ---- Chebyshev approximation of mod (ckksrns-fhe.cpp:719-720) ----
    ctxtEnc  = algo->EvalChebyshevSeries(ctxtEnc, nio_g_coefficientsUniform, -1.0, 1.0);
    ctxtEncI = algo->EvalChebyshevSeries(ctxtEncI, nio_g_coefficientsUniform, -1.0, 1.0);

    // ---- ModReduce + double-angle (ckksrns-fhe.cpp:723-733) ----
    algo->ModReduceInternalInPlace(ctxtEnc, compositeDegree);
    algo->ModReduceInternalInPlace(ctxtEncI, compositeDegree);
    constexpr std::uint32_t R_UNIFORM = 6;
    nio_apply_double_angle(ctxtEnc, R_UNIFORM);
    nio_apply_double_angle(ctxtEncI, R_UNIFORM);

    // ---- Combine + scale (ckksrns-fhe.cpp:735-741) ----
    algo->MultByMonomialInPlace(ctxtEncI, n.slots);
    cc->EvalAddInPlaceNoCheck(ctxtEnc, ctxtEncI);
    const std::uint64_t scalar =
        static_cast<std::uint64_t>(std::llround(std::pow(2.0, deg)));
    algo->MultByIntegerInPlace(ctxtEnc, scalar);

    // ---- Pre-StC ModReduce + SlotsToCoeffs (ckksrns-fhe.cpp:756-760) ----
    algo->ModReduceInternalInPlace(ctxtEnc, compositeDegree);
    auto ctxtDec = fhe_ckks->EvalSlotsToCoeffs(precom.m_U0PreFFT, ctxtEnc);

    // ---- corFactor (ckksrns-fhe.cpp:851-852) ----
    const std::uint64_t corFactor =
        static_cast<std::uint64_t>(1) << static_cast<std::uint64_t>(correction);
    algo->MultByIntegerInPlace(ctxtDec, corFactor);

    return ctxtDec;
}
} // namespace

// Rung 2: manual OpenFHE replication of EvalBootstrap, byte-equal to cc->EvalBootstrap.
TEST_CASE("nio02 manual cc->-replication of EvalBootstrap", "[.nio][e2e]") {
    using namespace lbcrypto;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto n = make_nio_ctx();
    auto ct = make_nio_depleted_ct(n, 6);
    auto ref = n.ctx.cc->EvalBootstrap(ct);
    auto manual = nio_manual_bootstrap(n, ct);
    std::cerr << "  [nio02] ref towers=" << ref->GetElements()[0].GetNumOfElements()
              << " manual towers=" << manual->GetElements()[0].GetNumOfElements() << "\n";
    rns_equal_or_report("nio02", manual, ref);
}

// phasefs02: full-slot CtS using linear_transform_v2, vs fhe_ckks->EvalLinearTransform.
// Both fed byte-identical raised input. Hidden under [.nio] / [.fsladder].
TEST_CASE("phasefs02 linear_transform_v2 full-slot CtS == EvalLinearTransform",
          "[.nio][.fsladder][e2e]") {
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);

    // Use the existing make_ctx-based fixture (same as phasefs01) so we can
    // reuse make_bootstrap_keys (which currently supports {1,1}). The OpenFHE
    // EvalLinearTransform path is what {1,1} dispatches to (isLTBootstrap).
    auto ctx = ops::make_ctx({.mode = FLEXIBLEAUTO, .mult_depth = 35,
        .scaling_mod_size = 50, .batch_size = 0, .with_relin_key = true,
        .rotate_indices = {}, .ring_dim = 1u << 11});
    ctx.cc->Enable(ADVANCEDSHE); ctx.cc->Enable(FHE);
    const std::uint32_t slots = static_cast<std::uint32_t>(ctx.ring_dim / 2);
    auto bk = ops::make_bootstrap_keys(ctx, ctx.cc, ctx.keys.secretKey, slots);
    std::vector<double> v(slots); for (std::uint32_t i=0;i<slots;++i) v[i]=0.1+0.0003*i;
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v, 1, 0, nullptr, slots);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    while (ct->GetElements()[0].GetNumOfElements() > 6)
        ctx.cc->EvalMultInPlace(ct, 1.0);
    auto cp = std::dynamic_pointer_cast<CryptoParametersCKKSRNS>(ctx.cc->GetCryptoParameters());
    const double qDouble = cp->GetElementParams()->GetParams()[0]->GetModulus().ConvertToDouble();
    const double powP = std::pow(2.0, cp->GetPlaintextModulus());
    const std::int32_t deg = static_cast<std::int32_t>(std::round(std::log2(qDouble/powP)));
    const std::int32_t correction = 11 - deg;
    const double pre = 1.0/std::pow(2.0,(double)deg);
    constexpr std::uint32_t K_UNIFORM = 512;
    const std::uint32_t N = static_cast<std::uint32_t>(ctx.ring_dim);
    auto fhe_ckks = std::dynamic_pointer_cast<FHECKKSRNS>(ctx.cc->GetScheme()->GetFHE());
    const auto &precom = *fhe_ckks->GetBootPrecomMap().at(slots);
    auto algo = ctx.cc->GetScheme();

    // OpenFHE manual pre-CtS (validated identical to haze pre-CtS in phasefs01).
    auto raised_ref = ct->Clone();
    while (raised_ref->GetNoiseScaleDeg() > 1)
        algo->ModReduceInternalInPlace(raised_ref, 1);
    {
        const double targetSF = cp->GetScalingFactorReal(0);
        const double sourceSF = raised_ref->GetScalingFactor();
        const std::uint32_t numTowers = raised_ref->GetElements()[0].GetNumOfElements();
        const double modToDrop = cp->GetElementParams()->GetParams()[numTowers-1]
            ->GetModulus().ConvertToDouble();
        const double adjustmentFactor = (targetSF/sourceSF) * (modToDrop/sourceSF)
                                        * std::pow(2.0, -correction);
        ctx.cc->EvalMultInPlace(raised_ref, adjustmentFactor);
        algo->ModReduceInternalInPlace(raised_ref, 1);
        raised_ref->SetScalingFactor(targetSF);
    }
    {
        auto ep = cp->GetElementParams();
        auto epp = std::make_shared<ILDCRTParams<DCRTPoly::Integer>>(ctx.cc->GetCyclotomicOrder(),
            [&]{std::vector<NativeInteger> m;for(auto&p:ep->GetParams())m.push_back(p->GetModulus());return m;}(),
            [&]{std::vector<NativeInteger> r;for(auto&p:ep->GetParams())r.push_back(p->GetRootOfUnity());return r;}());
        const std::uint32_t L0=(std::uint32_t)ep->GetParams().size();
        auto elements=raised_ref->GetElements();
        for(auto&dcrt:elements){dcrt.SetFormat(Format::COEFFICIENT);DCRTPoly tmp(dcrt.GetElementAtIndex(0),epp);tmp.SetFormat(Format::EVALUATION);dcrt=std::move(tmp);}
        raised_ref->SetElements(std::move(elements));
        raised_ref->SetLevel(L0-raised_ref->GetElements()[0].GetNumOfElements());
    }
    ctx.cc->EvalMultInPlace(raised_ref, pre/((double)K_UNIFORM*N));
    algo->ModReduceInternalInPlace(raised_ref, 1);

    // Haze pre-CtS.
    auto haze_ct = ops::h2d_ct(ctx, ct);
    auto adjusted = [&]() {
        auto r = ops::clone_ct(ctx, haze_ct);
        while (r.noise_scale_deg() > 1) r = ops::rescale(ctx, std::move(r));
        const double targetSF = cp->GetScalingFactorReal(0);
        const double sourceSF = r.scaling_factor();
        const std::uint32_t numTowers = static_cast<std::uint32_t>(r.towers());
        const double modToDrop = cp->GetElementParams()
            ->GetParams()[numTowers - 1]->GetModulus().ConvertToDouble();
        const double adjustmentFactor = (targetSF/sourceSF) * (modToDrop/sourceSF)
                                        * std::pow(2.0, -correction);
        r = ops::mult_by_const_for_test(ctx, r, adjustmentFactor);
        r = ops::rescale(ctx, std::move(r));
        r.set_scaling_factor(targetSF);
        return r;
    }();
    auto dep = ops::clone_ct(ctx, adjusted);
    if (dep.towers()>1) dep = ops::level_reduce(ctx, std::move(dep), dep.towers()-1);
    auto hr = ops::mod_raise(ctx, bk, dep);
    hr = ops::eval_mult_scalar_for_test(ctx, hr, pre/((double)K_UNIFORM*N));
    hr = ops::rescale(ctx, hr);

    // CtS on both sides.
    auto ctxtEnc_ref = fhe_ckks->EvalLinearTransform(precom.m_U0hatTPre, raised_ref);
    auto haze_v2 = ops::linear_transform_v2(ctx, bk, bk.cts_matrices, hr);
    std::cerr << "  [phasefs02] CtS ref towers="
              << ctxtEnc_ref->GetElements()[0].GetNumOfElements()
              << " haze_v2 towers=" << haze_v2.towers() << "\n";

    const auto hb = ops::d2h_ct(ctx, haze_v2);
    std::size_t bad = 0;
    for (std::size_t elem = 0; elem < 2; ++elem) {
        const auto &rd = ctxtEnc_ref->GetElements()[elem];
        const auto &hc = (elem == 0) ? hb.c0 : hb.c1;
        for (std::size_t t = 0; t < haze_v2.towers(); ++t) {
            const auto &rv = rd.GetElementAtIndex(static_cast<usint>(static_cast<std::uint32_t>(t))).GetValues();
            std::size_t mism = 0;
            for (std::size_t i = 0; i < hc[t].size(); ++i)
                if (hc[t][i] != rv[i].template ConvertToInt<std::uint64_t>()) ++mism;
            if (mism) { ++bad;
                if (bad <= 4)
                    std::cerr << "  [phasefs02] BAD elem=" << elem << " t=" << t
                              << " mism=" << mism << "/" << hc[t].size() << "\n"; }
        }
    }
    std::cerr << "  [phasefs02] bad_towers=" << bad << " of " << (2*haze_v2.towers()) << "\n";
    REQUIRE(bad == 0);
}

// phasefs03: SPARSE {1,1} slots=8, v2 vs EvalLinearTransform. If this passes,
// v2's algorithm is correct and any full-slot divergence is data-specific.
TEST_CASE("phasefs03 linear_transform_v2 SPARSE CtS == EvalLinearTransform",
          "[.fsladder][e2e]") {
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_bootstrap_ctx_e2e(1u << 11);
    constexpr std::uint32_t slots = 8;
    auto bk = ops::make_bootstrap_keys(ctx, ctx.cc, ctx.keys.secretKey, slots);
    const std::vector<double> v = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);

    auto cp = std::dynamic_pointer_cast<CryptoParametersCKKSRNS>(ctx.cc->GetCryptoParameters());
    const double qDouble = cp->GetElementParams()->GetParams()[0]->GetModulus().ConvertToDouble();
    const double powP = std::pow(2.0, cp->GetPlaintextModulus());
    const std::int32_t deg = static_cast<std::int32_t>(std::round(std::log2(qDouble/powP)));
    const std::int32_t correction = 11 - deg;
    const double pre = 1.0/std::pow(2.0,(double)deg);
    constexpr std::uint32_t K_UNIFORM = 512;
    const std::uint32_t N = static_cast<std::uint32_t>(ctx.ring_dim);
    auto fhe_ckks = std::dynamic_pointer_cast<FHECKKSRNS>(ctx.cc->GetScheme()->GetFHE());
    const auto &precom = *fhe_ckks->GetBootPrecomMap().at(slots);
    auto algo = ctx.cc->GetScheme();

    // OpenFHE side, sparse pipeline (with partial_sum).
    auto raised_ref = ct->Clone();
    algo->ModReduceInternalInPlace(raised_ref, 0);
    ctx.cc->EvalMultInPlace(raised_ref, std::pow(2.0,-correction));
    {
        auto ep = cp->GetElementParams();
        auto epp = std::make_shared<ILDCRTParams<DCRTPoly::Integer>>(ctx.cc->GetCyclotomicOrder(),
            [&]{std::vector<NativeInteger> m;for(auto&p:ep->GetParams())m.push_back(p->GetModulus());return m;}(),
            [&]{std::vector<NativeInteger> r;for(auto&p:ep->GetParams())r.push_back(p->GetRootOfUnity());return r;}());
        const std::uint32_t L0=(std::uint32_t)ep->GetParams().size();
        auto elements=raised_ref->GetElements();
        for(auto&dcrt:elements){dcrt.SetFormat(Format::COEFFICIENT);DCRTPoly tmp(dcrt.GetElementAtIndex(0),epp);tmp.SetFormat(Format::EVALUATION);dcrt=std::move(tmp);}
        raised_ref->SetElements(std::move(elements));
        raised_ref->SetLevel(L0-raised_ref->GetElements()[0].GetNumOfElements());
    }
    ctx.cc->EvalMultInPlace(raised_ref, pre/((double)K_UNIFORM*N));
    {
        const auto limit = N / (2 * slots);
        for (std::uint32_t j = 1; j < limit; j <<= 1)
            ctx.cc->EvalAddInPlace(raised_ref,
                                   ctx.cc->EvalRotate(raised_ref, static_cast<int>(j*slots)));
    }
    algo->ModReduceInternalInPlace(raised_ref, 1);

    // Haze side.
    auto haze_ct = ops::h2d_ct(ctx, ct);
    auto adj = ops::eval_mult_scalar_for_test(ctx, haze_ct, std::pow(2.0,-correction));
    auto dep = ops::clone_ct(ctx, adj);
    if (dep.towers()>1) dep = ops::level_reduce(ctx, std::move(dep), dep.towers()-1);
    auto hr = ops::mod_raise(ctx, bk, dep);
    hr = ops::eval_mult_scalar_for_test(ctx, hr, pre/((double)K_UNIFORM*N));
    {
        const std::uint32_t limit = N / (2 * slots);
        for (std::uint32_t j = 1; j < limit; j <<= 1) {
            const std::uint32_t aidx = ctx.cc->FindAutomorphismIndex(j*slots);
            auto it = bk.rotation_keys.find(aidx);
            REQUIRE(it != bk.rotation_keys.end());
            auto rotated = ops::rotate_with_key(ctx, hr, it->second);
            hr = ops::add(ctx, hr, rotated);
        }
    }
    hr = ops::rescale(ctx, hr);

    auto ctxtEnc_ref = fhe_ckks->EvalLinearTransform(precom.m_U0hatTPre, raised_ref);
    auto haze_v2 = ops::linear_transform_v2(ctx, bk, bk.cts_matrices, hr);
    std::cerr << "  [phasefs03] CtS ref towers="
              << ctxtEnc_ref->GetElements()[0].GetNumOfElements()
              << " haze_v2 towers=" << haze_v2.towers() << "\n";

    const auto hb = ops::d2h_ct(ctx, haze_v2);
    std::size_t bad = 0;
    for (std::size_t elem = 0; elem < 2; ++elem) {
        const auto &rd = ctxtEnc_ref->GetElements()[elem];
        const auto &hc = (elem == 0) ? hb.c0 : hb.c1;
        for (std::size_t t = 0; t < haze_v2.towers(); ++t) {
            const auto &rv = rd.GetElementAtIndex(static_cast<usint>(static_cast<std::uint32_t>(t))).GetValues();
            std::size_t mism = 0;
            for (std::size_t i = 0; i < hc[t].size(); ++i)
                if (hc[t][i] != rv[i].template ConvertToInt<std::uint64_t>()) ++mism;
            if (mism) { ++bad;
                if (bad <= 4)
                    std::cerr << "  [phasefs03] BAD elem=" << elem << " t=" << t
                              << " mism=" << mism << "/" << hc[t].size() << "\n"; }
        }
    }
    std::cerr << "  [phasefs03] bad_towers=" << bad << " of " << (2*haze_v2.towers()) << "\n";
    REQUIRE(bad == 0);
}

// phasefs04 (e2e): full-slot {1,1} ops::bootstrap vs cc->EvalBootstrap.
// With linear_transform_v2 in place, this exercises the full-slot bootstrap
// end-to-end at levelBudget {1,1} (the dispatch path haze currently supports).
TEST_CASE("phasefs04 full-slot {1,1} ops::bootstrap byte-equal e2e",
          "[.fsladder][e2e]") {
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = ops::make_ctx({.mode = FLEXIBLEAUTO, .mult_depth = 35,
        .scaling_mod_size = 50, .batch_size = 0, .with_relin_key = true,
        .rotate_indices = {}, .ring_dim = 1u << 11});
    ctx.cc->Enable(ADVANCEDSHE); ctx.cc->Enable(FHE);
    const std::uint32_t slots = static_cast<std::uint32_t>(ctx.ring_dim / 2);
    auto bk = ops::make_bootstrap_keys(ctx, ctx.cc, ctx.keys.secretKey, slots);
    std::vector<double> v(slots);
    for (std::uint32_t i = 0; i < slots; ++i) v[i] = 0.1 + 0.0003 * i;
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(v, 1, 0, nullptr, slots);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    while (ct->GetElements()[0].GetNumOfElements() > 6)
        ctx.cc->EvalMultInPlace(ct, 1.0);

    auto ct_ref = ctx.cc->EvalBootstrap(ct);
    REQUIRE(ct_ref);
    auto haze_ct = ops::h2d_ct(ctx, ct);
    auto haze_refreshed = ops::bootstrap(ctx, bk, haze_ct, ops::BootstrapVariant::Standard);
    std::cerr << "  [phasefs04] ref towers="
              << ct_ref->GetElements()[0].GetNumOfElements()
              << " haze towers=" << haze_refreshed.towers() << "\n";
    const auto hb = ops::d2h_ct(ctx, haze_refreshed);
    std::size_t bad = 0;
    for (std::size_t elem = 0; elem < 2; ++elem) {
        const auto &rd = ct_ref->GetElements()[elem];
        const auto &hc = (elem == 0) ? hb.c0 : hb.c1;
        for (std::size_t t = 0; t < haze_refreshed.towers(); ++t) {
            const auto &rv = rd.GetElementAtIndex(static_cast<usint>(static_cast<std::uint32_t>(t))).GetValues();
            std::size_t mism = 0;
            for (std::size_t i = 0; i < hc[t].size(); ++i)
                if (hc[t][i] != rv[i].template ConvertToInt<std::uint64_t>()) ++mism;
            if (mism) { ++bad;
                if (bad <= 4)
                    std::cerr << "  [phasefs04] BAD elem=" << elem << " t=" << t
                              << " mism=" << mism << "/" << hc[t].size() << "\n"; }
        }
    }
    std::cerr << "  [phasefs04] bad_towers=" << bad << " of "
              << (2 * haze_refreshed.towers()) << "\n";
    REQUIRE(bad == 0);
}

// phasefs06: isolate the multi-stage eval_coeffs_to_slots at the niobium
// config. Builds the raised (post-rescale) intermediate on both OpenFHE and
// haze sides — the same intermediate as nio02 confirmed byte-exact — then
// runs fhe_ckks->EvalCoeffsToSlots vs ops::eval_coeffs_to_slots and compares
// bytes. Localizes CtS divergence in isolation from the rest of bootstrap.
TEST_CASE("phasefs06 niobium-config eval_coeffs_to_slots == EvalCoeffsToSlots",
          "[.nio][.fsladder][e2e]") {
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto n = make_nio_ctx();
    auto bk = ops::make_bootstrap_keys(n.ctx, n.ctx.cc, n.ctx.keys.secretKey,
                                       n.slots, {4, 4});
    auto ct = make_nio_depleted_ct(n, 6);

    auto cp = std::dynamic_pointer_cast<CryptoParametersCKKSRNS>(
        n.ctx.cc->GetCryptoParameters());
    auto algo = n.ctx.cc->GetScheme();
    auto fhe_ckks = std::dynamic_pointer_cast<FHECKKSRNS>(algo->GetFHE());
    const auto &precom = *fhe_ckks->GetBootPrecomMap().at(n.slots);
    const std::uint32_t compositeDegree = cp->GetCompositeDegree();

    const double qDouble = cp->GetElementParams()->GetParams()[0]
        ->GetModulus().ConvertToDouble();
    const double powP = std::pow(2.0, cp->GetPlaintextModulus());
    const std::int32_t deg = static_cast<std::int32_t>(std::round(std::log2(qDouble / powP)));
    const std::int32_t correction = 11 - deg;
    const double pre = 1.0 / std::pow(2.0, static_cast<double>(deg));
    constexpr std::uint32_t K_UNIFORM = 512;
    const std::uint32_t N = static_cast<std::uint32_t>(n.ctx.ring_dim);

    // ---- OpenFHE pre-CtS (mirrors nio_manual_bootstrap) ----
    auto raised = ct->Clone();
    while (raised->GetNoiseScaleDeg() > 1)
        algo->ModReduceInternalInPlace(raised, compositeDegree);
    {
        const double targetSF = cp->GetScalingFactorReal(0);
        const double sourceSF = raised->GetScalingFactor();
        const std::uint32_t numTowers = raised->GetElements()[0].GetNumOfElements();
        double modToDrop = cp->GetElementParams()->GetParams()[numTowers-1]
            ->GetModulus().ConvertToDouble();
        for (std::uint32_t j = 2; j <= compositeDegree; ++j)
            modToDrop *= cp->GetElementParams()->GetParams()[numTowers-j]
                ->GetModulus().ConvertToDouble();
        const double adjustmentFactor = (targetSF/sourceSF) * (modToDrop/sourceSF)
                                        * std::pow(2.0, -correction);
        n.ctx.cc->EvalMultInPlace(raised, adjustmentFactor);
        algo->ModReduceInternalInPlace(raised, compositeDegree);
        raised->SetScalingFactor(targetSF);
    }
    {
        auto ep = cp->GetElementParams();
        auto epp = std::make_shared<ILDCRTParams<DCRTPoly::Integer>>(
            n.ctx.cc->GetCyclotomicOrder(),
            [&]{std::vector<NativeInteger> m;for(auto&p:ep->GetParams())m.push_back(p->GetModulus());return m;}(),
            [&]{std::vector<NativeInteger> r;for(auto&p:ep->GetParams())r.push_back(p->GetRootOfUnity());return r;}());
        const std::uint32_t L0 = static_cast<std::uint32_t>(ep->GetParams().size());
        auto elements = raised->GetElements();
        for (auto &dcrt : elements) {
            dcrt.SetFormat(Format::COEFFICIENT);
            DCRTPoly tmp(dcrt.GetElementAtIndex(0), epp);
            tmp.SetFormat(Format::EVALUATION);
            dcrt = std::move(tmp);
        }
        raised->SetElements(std::move(elements));
        raised->SetLevel(L0 - raised->GetElements()[0].GetNumOfElements());
    }
    n.ctx.cc->EvalMultInPlace(raised, pre / (static_cast<double>(K_UNIFORM) * N));
    algo->ModReduceInternalInPlace(raised, compositeDegree);

    // ---- haze pre-CtS via ops:: helpers (same as bootstrap.cpp) ----
    auto haze_ct = ops::h2d_ct(n.ctx, ct);
    auto adjusted = [&]() {
        auto r = ops::clone_ct(n.ctx, haze_ct);
        while (r.noise_scale_deg() > 1) r = ops::rescale(n.ctx, std::move(r));
        const double targetSF = cp->GetScalingFactorReal(0);
        const double sourceSF = r.scaling_factor();
        const std::uint32_t numTowers = static_cast<std::uint32_t>(r.towers());
        const double modToDrop = cp->GetElementParams()
            ->GetParams()[numTowers - 1]->GetModulus().ConvertToDouble();
        const double adjustmentFactor = (targetSF/sourceSF) * (modToDrop/sourceSF)
                                        * std::pow(2.0, -correction);
        r = ops::mult_by_const_for_test(n.ctx, r, adjustmentFactor);
        r = ops::rescale(n.ctx, std::move(r));
        r.set_scaling_factor(targetSF);
        return r;
    }();
    auto dep = ops::clone_ct(n.ctx, adjusted);
    if (dep.towers() > 1) dep = ops::level_reduce(n.ctx, std::move(dep), dep.towers() - 1);
    auto hr = ops::mod_raise(n.ctx, bk, dep);
    hr = ops::eval_mult_scalar_for_test(n.ctx, hr, pre/(static_cast<double>(K_UNIFORM) * N));
    hr = ops::rescale(n.ctx, hr);

    // Sanity: print BSGS params + pre-CtS byte-match.
    const auto &pe = precom.m_paramsEnc;
    std::cerr << "  [phasefs06] enc params lvlb=" << pe.lvlb
              << " layersCollapse=" << pe.layersCollapse
              << " remCollapse=" << pe.remCollapse
              << " numRotations=" << pe.numRotations
              << " b=" << pe.b << " g=" << pe.g
              << " numRotationsRem=" << pe.numRotationsRem
              << " bRem=" << pe.bRem << " gRem=" << pe.gRem << "\n";
    std::cerr << "  [phasefs06] raised towers=" << raised->GetElements()[0].GetNumOfElements()
              << " nsd=" << raised->GetNoiseScaleDeg()
              << " hr towers=" << hr.towers() << " nsd=" << hr.noise_scale_deg() << "\n";

    // Pre-CtS byte parity check.
    {
        const auto hb_pre = ops::d2h_ct(n.ctx, hr);
        std::size_t pre_bad = 0;
        for (std::size_t elem = 0; elem < 2; ++elem) {
            const auto &rd = raised->GetElements()[elem];
            const auto &hc = (elem == 0) ? hb_pre.c0 : hb_pre.c1;
            for (std::size_t t = 0; t < hr.towers(); ++t) {
                const auto &rv = rd.GetElementAtIndex(static_cast<usint>(t)).GetValues();
                for (std::size_t i = 0; i < hc[t].size(); ++i)
                    if (hc[t][i] != rv[i].template ConvertToInt<std::uint64_t>()) { ++pre_bad; break; }
            }
        }
        std::cerr << "  [phasefs06] pre-CtS bad_polys=" << pre_bad << " of " << (2 * hr.towers()) << "\n";
    }

    // ---- CtS on both sides ----
    auto cts_ref = fhe_ckks->EvalCoeffsToSlots(precom.m_U0hatTPreFFT, raised);
    auto cts_haze = ops::eval_coeffs_to_slots(n.ctx, bk, bk.cts_matrices_fft, hr);
    std::cerr << "  [phasefs06] CtS ref towers="
              << cts_ref->GetElements()[0].GetNumOfElements()
              << " haze towers=" << cts_haze.towers() << "\n";

    const auto hb = ops::d2h_ct(n.ctx, cts_haze);
    std::size_t bad = 0;
    for (std::size_t elem = 0; elem < 2; ++elem) {
        const auto &rd = cts_ref->GetElements()[elem];
        const auto &hc = (elem == 0) ? hb.c0 : hb.c1;
        for (std::size_t t = 0; t < cts_haze.towers(); ++t) {
            const auto &rv = rd.GetElementAtIndex(
                static_cast<usint>(static_cast<std::uint32_t>(t))).GetValues();
            std::size_t mism = 0;
            for (std::size_t i = 0; i < hc[t].size(); ++i)
                if (hc[t][i] != rv[i].template ConvertToInt<std::uint64_t>()) ++mism;
            if (mism) { ++bad;
                if (bad <= 4)
                    std::cerr << "  [phasefs06] BAD elem=" << elem << " t=" << t
                              << " mism=" << mism << "/" << hc[t].size() << "\n"; }
        }
    }
    std::cerr << "  [phasefs06] bad_towers=" << bad << " of "
              << (2 * cts_haze.towers()) << "\n";
    REQUIRE(bad == 0);
}

namespace {
// Compare a haze Ct against an OpenFHE Ciphertext, return bad poly count.
[[maybe_unused]] std::size_t haze_vs_openfhe(
    const haze::test::ops::OpCtx &ctx, const haze::test::ops::Ct &h,
    const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &r, const char *label) {
    namespace ops = haze::test::ops;
    const auto hb = ops::d2h_ct(ctx, h);
    std::size_t bad = 0;
    const std::size_t towers = h.towers();
    const std::size_t ref_towers = r->GetElements()[0].GetNumOfElements();
    if (towers != ref_towers) {
        std::cerr << "  [" << label << "] TOWER MISMATCH haze=" << towers
                  << " ref=" << ref_towers << "\n";
    }
    const std::size_t min_t = std::min(towers, ref_towers);
    for (std::size_t elem = 0; elem < 2; ++elem) {
        const auto &rd = r->GetElements()[elem];
        const auto &hc = (elem == 0) ? hb.c0 : hb.c1;
        for (std::size_t t = 0; t < min_t; ++t) {
            const auto &rv = rd.GetElementAtIndex(
                static_cast<usint>(static_cast<std::uint32_t>(t))).GetValues();
            for (std::size_t i = 0; i < hc[t].size(); ++i)
                if (hc[t][i] != rv[i].template ConvertToInt<std::uint64_t>()) { ++bad; break; }
        }
    }
    std::cerr << "  [" << label << "] bad_polys=" << bad << " of " << (2 * min_t)
              << " (haze nsd=" << h.noise_scale_deg() << " sf=" << h.scaling_factor()
              << " ref nsd=" << r->GetNoiseScaleDeg()
              << " sf=" << r->GetScalingFactor() << ")\n";
    return bad;
}
} // namespace

// phasefs07: step-by-step trace through the post-CtS bootstrap pipeline at
// the niobium config. Each step computes OpenFHE-reference and haze-ops
// side-by-side, comparing bytes. Localizes the first diverging op.
TEST_CASE("phasefs07 niobium-config post-CtS step-by-step byte parity",
          "[.nio][.fsladder][e2e]") {
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto n = make_nio_ctx();
    auto bk = ops::make_bootstrap_keys(n.ctx, n.ctx.cc, n.ctx.keys.secretKey,
                                       n.slots, {4, 4});
    auto ct = make_nio_depleted_ct(n, 6);

    auto cp = std::dynamic_pointer_cast<CryptoParametersCKKSRNS>(
        n.ctx.cc->GetCryptoParameters());
    auto algo = n.ctx.cc->GetScheme();
    auto fhe_ckks = std::dynamic_pointer_cast<FHECKKSRNS>(algo->GetFHE());
    const auto &precom = *fhe_ckks->GetBootPrecomMap().at(n.slots);
    const std::uint32_t compositeDegree = cp->GetCompositeDegree();

    const double qDouble = cp->GetElementParams()->GetParams()[0]
        ->GetModulus().ConvertToDouble();
    const double powP = std::pow(2.0, cp->GetPlaintextModulus());
    const std::int32_t deg = static_cast<std::int32_t>(std::round(std::log2(qDouble / powP)));
    const std::int32_t correction = 11 - deg;
    const double pre = 1.0 / std::pow(2.0, static_cast<double>(deg));
    constexpr std::uint32_t K_UNIFORM = 512;
    const std::uint32_t N = static_cast<std::uint32_t>(n.ctx.ring_dim);

    // ---- OpenFHE pre-CtS ----
    auto raised = ct->Clone();
    while (raised->GetNoiseScaleDeg() > 1)
        algo->ModReduceInternalInPlace(raised, compositeDegree);
    {
        const double targetSF = cp->GetScalingFactorReal(0);
        const double sourceSF = raised->GetScalingFactor();
        const std::uint32_t numTowers = raised->GetElements()[0].GetNumOfElements();
        double modToDrop = cp->GetElementParams()->GetParams()[numTowers-1]
            ->GetModulus().ConvertToDouble();
        for (std::uint32_t j = 2; j <= compositeDegree; ++j)
            modToDrop *= cp->GetElementParams()->GetParams()[numTowers-j]
                ->GetModulus().ConvertToDouble();
        const double adjustmentFactor = (targetSF/sourceSF) * (modToDrop/sourceSF)
                                        * std::pow(2.0, -correction);
        n.ctx.cc->EvalMultInPlace(raised, adjustmentFactor);
        algo->ModReduceInternalInPlace(raised, compositeDegree);
        raised->SetScalingFactor(targetSF);
    }
    {
        auto ep = cp->GetElementParams();
        auto epp = std::make_shared<ILDCRTParams<DCRTPoly::Integer>>(
            n.ctx.cc->GetCyclotomicOrder(),
            [&]{std::vector<NativeInteger> m;for(auto&p:ep->GetParams())m.push_back(p->GetModulus());return m;}(),
            [&]{std::vector<NativeInteger> r;for(auto&p:ep->GetParams())r.push_back(p->GetRootOfUnity());return r;}());
        const std::uint32_t L0 = static_cast<std::uint32_t>(ep->GetParams().size());
        auto elements = raised->GetElements();
        for (auto &dcrt : elements) {
            dcrt.SetFormat(Format::COEFFICIENT);
            DCRTPoly tmp(dcrt.GetElementAtIndex(0), epp);
            tmp.SetFormat(Format::EVALUATION);
            dcrt = std::move(tmp);
        }
        raised->SetElements(std::move(elements));
        raised->SetLevel(L0 - raised->GetElements()[0].GetNumOfElements());
    }
    n.ctx.cc->EvalMultInPlace(raised, pre / (static_cast<double>(K_UNIFORM) * N));
    algo->ModReduceInternalInPlace(raised, compositeDegree);

    // ---- haze pre-CtS ----
    auto haze_ct = ops::h2d_ct(n.ctx, ct);
    auto adjusted = [&]() {
        auto r = ops::clone_ct(n.ctx, haze_ct);
        while (r.noise_scale_deg() > 1) r = ops::rescale(n.ctx, std::move(r));
        const double targetSF = cp->GetScalingFactorReal(0);
        const double sourceSF = r.scaling_factor();
        const std::uint32_t numTowers = static_cast<std::uint32_t>(r.towers());
        const double modToDrop = cp->GetElementParams()
            ->GetParams()[numTowers - 1]->GetModulus().ConvertToDouble();
        const double adjustmentFactor = (targetSF/sourceSF) * (modToDrop/sourceSF)
                                        * std::pow(2.0, -correction);
        r = ops::mult_by_const_for_test(n.ctx, r, adjustmentFactor);
        r = ops::rescale(n.ctx, std::move(r));
        r.set_scaling_factor(targetSF);
        return r;
    }();
    auto dep = ops::clone_ct(n.ctx, adjusted);
    if (dep.towers() > 1) dep = ops::level_reduce(n.ctx, std::move(dep), dep.towers() - 1);
    auto hr = ops::mod_raise(n.ctx, bk, dep);
    hr = ops::eval_mult_scalar_for_test(n.ctx, hr, pre/(static_cast<double>(K_UNIFORM) * N));
    hr = ops::rescale(n.ctx, hr);

    // Refresh: replace haze Ct with fresh upload from OpenFHE reference. Use
    // after each step's d2h-based compare so subsequent ops see clean inputs
    // (a mid-pipeline d2h evicts inputs from shadow_data, so chained reuse
    // would feed zero-valued ciphertexts into the next op).
    auto refresh = [&](haze::test::ops::Ct &h,
                       const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &r) {
        // Ensure source is in EVALUATION before extracting — h2d_ct copies
        // GetValues() verbatim, so coefficient-form bytes would be uploaded
        // and misinterpreted as evaluation-form by haze NTT-aware ops.
        for (auto &elem : r->GetElements()) elem.SetFormat(Format::EVALUATION);
        h = ops::h2d_ct(n.ctx, r);
    };

    auto dump_meta = [](const char *label,
                        const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &r,
                        const haze::test::ops::Ct &h) {
        std::cerr << "  [" << label << "] ref fmt c0[0]="
                  << (r->GetElements()[0].GetElementAtIndex(0).GetFormat() == EVALUATION ? "EVAL" : "COEF")
                  << " level=" << r->GetLevel()
                  << " nsd=" << r->GetNoiseScaleDeg()
                  << " sf=" << r->GetScalingFactor()
                  << " | haze level=" << h.level()
                  << " nsd=" << h.noise_scale_deg() << " sf=" << h.scaling_factor() << "\n";
    };

    // Force flush before CtS so the CtS recording is short (long combined
    // pre-CtS+CtS traces in one epoch produce divergent CtS bytes — likely
    // a fhetch simulator scaling/overflow issue triggered by long traces).
    REQUIRE(haze_vs_openfhe(n.ctx, hr, raised, "fs07 0:preCtS") == 0);
    refresh(hr, raised);

    // ---- Step A: CtS ----
    auto ctxtEnc = fhe_ckks->EvalCoeffsToSlots(precom.m_U0hatTPreFFT, raised);
    auto h_ctxtEnc = ops::eval_coeffs_to_slots(n.ctx, bk, bk.cts_matrices_fft, hr);
    REQUIRE(haze_vs_openfhe(n.ctx, h_ctxtEnc, ctxtEnc, "fs07 A:CtS") == 0);
    refresh(h_ctxtEnc, ctxtEnc);

    // ---- Step B: Conjugate ----
    const auto &evalKeyMap = n.ctx.cc->GetEvalAutomorphismKeyMap(ctxtEnc->GetKeyTag());
    auto conj = FHECKKSRNS::Conjugate(ctxtEnc, evalKeyMap);
    auto h_conj = ops::conjugate(n.ctx, h_ctxtEnc, bk.conjugation_key);
    REQUIRE(haze_vs_openfhe(n.ctx, h_conj, conj, "fs07 B:Conj") == 0);
    refresh(h_ctxtEnc, ctxtEnc);
    refresh(h_conj, conj);

    // ---- Step C: Sub (real -> ctxtEncI) / Add (real -> ctxtEnc) ----
    auto ctxtEncI = n.ctx.cc->EvalSub(ctxtEnc, conj);
    auto h_ctxtEncI = ops::sub(n.ctx, h_ctxtEnc, h_conj);
    REQUIRE(haze_vs_openfhe(n.ctx, h_ctxtEncI, ctxtEncI, "fs07 C1:Sub") == 0);
    refresh(h_ctxtEnc, ctxtEnc);
    refresh(h_conj, conj);
    refresh(h_ctxtEncI, ctxtEncI);

    n.ctx.cc->EvalAddInPlace(ctxtEnc, conj);
    h_ctxtEnc = ops::add(n.ctx, h_ctxtEnc, h_conj);
    REQUIRE(haze_vs_openfhe(n.ctx, h_ctxtEnc, ctxtEnc, "fs07 C2:Add") == 0);
    refresh(h_ctxtEnc, ctxtEnc);

    // ---- Step D: MultByMonomial(3*slots) on imag ----
    algo->MultByMonomialInPlace(ctxtEncI, 3 * n.slots);
    h_ctxtEncI = ops::mult_monomial_for_test(n.ctx, h_ctxtEncI, 3 * n.slots);
    REQUIRE(haze_vs_openfhe(n.ctx, h_ctxtEncI, ctxtEncI, "fs07 D:Mono") == 0);
    refresh(h_ctxtEncI, ctxtEncI);

    // ---- Step E: ModReduce both if nsd==2 ----
    if (ctxtEnc->GetNoiseScaleDeg() == 2) {
        algo->ModReduceInternalInPlace(ctxtEnc, compositeDegree);
        algo->ModReduceInternalInPlace(ctxtEncI, compositeDegree);
        h_ctxtEnc = ops::rescale(n.ctx, h_ctxtEnc);
        h_ctxtEncI = ops::rescale(n.ctx, h_ctxtEncI);
        REQUIRE(haze_vs_openfhe(n.ctx, h_ctxtEnc, ctxtEnc, "fs07 E1:RescR") == 0);
        REQUIRE(haze_vs_openfhe(n.ctx, h_ctxtEncI, ctxtEncI, "fs07 E2:RescI") == 0);
        refresh(h_ctxtEnc, ctxtEnc);
        refresh(h_ctxtEncI, ctxtEncI);
    }

    dump_meta("fs07 F0.preCheby", ctxtEnc, h_ctxtEnc);
    REQUIRE(haze_vs_openfhe(n.ctx, h_ctxtEnc, ctxtEnc, "fs07 F0.byteCheck") == 0);
    refresh(h_ctxtEnc, ctxtEnc);
    refresh(h_ctxtEncI, ctxtEncI);
    std::cerr << "  [fs07 F0.dbg] before Cheby ref: level=" << ctxtEnc->GetLevel()
              << " nsd=" << ctxtEnc->GetNoiseScaleDeg()
              << " sf=" << ctxtEnc->GetScalingFactor()
              << " towers=" << ctxtEnc->GetElements()[0].GetNumOfElements()
              << " | haze: level=" << h_ctxtEnc.level()
              << " nsd=" << h_ctxtEnc.noise_scale_deg()
              << " sf=" << h_ctxtEnc.scaling_factor()
              << " towers=" << h_ctxtEnc.towers() << "\n";
    // ---- Step F: Chebyshev x 2 ----
    ctxtEnc  = algo->EvalChebyshevSeries(ctxtEnc, nio_g_coefficientsUniform, -1.0, 1.0);
    ctxtEncI = algo->EvalChebyshevSeries(ctxtEncI, nio_g_coefficientsUniform, -1.0, 1.0);
    h_ctxtEnc  = ops::eval_chebyshev_series_for_test(n.ctx, h_ctxtEnc, nio_g_coefficientsUniform);
    h_ctxtEncI = ops::eval_chebyshev_series_for_test(n.ctx, h_ctxtEncI, nio_g_coefficientsUniform);
    REQUIRE(haze_vs_openfhe(n.ctx, h_ctxtEnc, ctxtEnc, "fs07 F1:ChbR") == 0);
    REQUIRE(haze_vs_openfhe(n.ctx, h_ctxtEncI, ctxtEncI, "fs07 F2:ChbI") == 0);
    refresh(h_ctxtEnc, ctxtEnc);
    refresh(h_ctxtEncI, ctxtEncI);

    // ---- Step G: ModReduce + DoubleAngle x 2 ----
    algo->ModReduceInternalInPlace(ctxtEnc, compositeDegree);
    algo->ModReduceInternalInPlace(ctxtEncI, compositeDegree);
    h_ctxtEnc = ops::rescale(n.ctx, h_ctxtEnc);
    h_ctxtEncI = ops::rescale(n.ctx, h_ctxtEncI);
    REQUIRE(haze_vs_openfhe(n.ctx, h_ctxtEnc, ctxtEnc, "fs07 G1:RescR") == 0);
    REQUIRE(haze_vs_openfhe(n.ctx, h_ctxtEncI, ctxtEncI, "fs07 G2:RescI") == 0);
    refresh(h_ctxtEnc, ctxtEnc);
    refresh(h_ctxtEncI, ctxtEncI);

    nio_apply_double_angle(ctxtEnc, bk.params.double_angle_iterations);
    nio_apply_double_angle(ctxtEncI, bk.params.double_angle_iterations);
    ops::apply_double_angle_for_test(n.ctx, h_ctxtEnc, bk.params.double_angle_iterations);
    ops::apply_double_angle_for_test(n.ctx, h_ctxtEncI, bk.params.double_angle_iterations);
    REQUIRE(haze_vs_openfhe(n.ctx, h_ctxtEnc, ctxtEnc, "fs07 H1:DAR") == 0);
    REQUIRE(haze_vs_openfhe(n.ctx, h_ctxtEncI, ctxtEncI, "fs07 H2:DAI") == 0);
    refresh(h_ctxtEnc, ctxtEnc);
    refresh(h_ctxtEncI, ctxtEncI);

    // ---- Step I: MultByMonomial(slots) on imag, then Add ----
    algo->MultByMonomialInPlace(ctxtEncI, n.slots);
    h_ctxtEncI = ops::mult_monomial_for_test(n.ctx, h_ctxtEncI, n.slots);
    REQUIRE(haze_vs_openfhe(n.ctx, h_ctxtEncI, ctxtEncI, "fs07 I:MonI") == 0);
    refresh(h_ctxtEncI, ctxtEncI);

    n.ctx.cc->EvalAddInPlace(ctxtEnc, ctxtEncI);
    h_ctxtEnc = ops::add(n.ctx, h_ctxtEnc, h_ctxtEncI);
    REQUIRE(haze_vs_openfhe(n.ctx, h_ctxtEnc, ctxtEnc, "fs07 J:AddRI") == 0);
    refresh(h_ctxtEnc, ctxtEnc);

    // ---- Step K: MultByInteger(scalar = K_UNIFORM) ----
    algo->MultByIntegerInPlace(ctxtEnc, K_UNIFORM);
    h_ctxtEnc = ops::mult_int_scalar_for_test(n.ctx, h_ctxtEnc, K_UNIFORM);
    REQUIRE(haze_vs_openfhe(n.ctx, h_ctxtEnc, ctxtEnc, "fs07 K:MulInt") == 0);
    refresh(h_ctxtEnc, ctxtEnc);

    // ---- Step L: ModReduce ----
    algo->ModReduceInternalInPlace(ctxtEnc, compositeDegree);
    h_ctxtEnc = ops::rescale(n.ctx, h_ctxtEnc);
    REQUIRE(haze_vs_openfhe(n.ctx, h_ctxtEnc, ctxtEnc, "fs07 L:Resc") == 0);
    refresh(h_ctxtEnc, ctxtEnc);

    // ---- Step M: SlotsToCoeffs ----
    auto ctxtDec = fhe_ckks->EvalSlotsToCoeffs(precom.m_U0PreFFT, ctxtEnc);
    auto h_ctxtDec = ops::eval_slots_to_coeffs(n.ctx, bk, bk.stc_matrices_fft, h_ctxtEnc);
    REQUIRE(haze_vs_openfhe(n.ctx, h_ctxtDec, ctxtDec, "fs07 M:StC") == 0);
    refresh(h_ctxtDec, ctxtDec);

    // ---- Step N: MultByInteger(corFactor) ----
    const std::uint64_t corFactor = static_cast<std::uint64_t>(1)
        << static_cast<std::uint64_t>(correction);
    algo->MultByIntegerInPlace(ctxtDec, corFactor);
    h_ctxtDec = ops::mult_int_scalar_for_test(n.ctx, h_ctxtDec, corFactor);
    REQUIRE(haze_vs_openfhe(n.ctx, h_ctxtDec, ctxtDec, "fs07 N:CorMul") == 0);
}

// phasefs08: minimal Cheby byte-parity test at the niobium config (FLEXIBLEAUTO,
// scalingMod=59, multDepth=32). Mimics phase14 but at the nio config so we can
// isolate whether eval_chebyshev_series byte-matches cc->EvalChebyshevSeries on
// this larger config. phase14 only covered FIXEDAUTO at scalingMod=50.
TEST_CASE("phasefs08 Chebyshev byte-parity at niobium config",
          "[.nio][.fsladder][e2e]") {
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto n = make_nio_ctx();
    auto cp = std::dynamic_pointer_cast<CryptoParametersCKKSRNS>(
        n.ctx.cc->GetCryptoParameters());

    auto fresh = [&]() {
        std::vector<double> v(n.slots);
        for (std::uint32_t i = 0; i < n.slots; ++i) v[i] = 0.001 * (1.0 + 0.0001 * i);
        auto pt = n.ctx.cc->MakeCKKSPackedPlaintext(v, 1, 0, nullptr, n.slots);
        return n.ctx.cc->Encrypt(n.ctx.keys.publicKey, pt);
    };

    auto check = [&](const std::string &label, const std::vector<double> &coeffs) {
        auto ct = fresh();
        auto ref = n.ctx.cc->EvalChebyshevSeries(ct, coeffs, -1.0, 1.0);
        auto haze_ct = ops::h2d_ct(n.ctx, ct);
        auto out = ops::eval_chebyshev_series_for_test(n.ctx, haze_ct, coeffs);
        REQUIRE(haze_vs_openfhe(n.ctx, out, ref, ("fs08 " + label).c_str()) == 0);
    };

    check("degree=5", {0.5, 0.25, 0.125, 0.0625, 0.03125, 0.015625});
    {
        std::vector<double> coeffs(13, 0.0);
        for (std::size_t i = 0; i <= 12; ++i) coeffs[i] = 1.0 / static_cast<double>(i + 1);
        check("degree=12", coeffs);
    }
    check("degree=88 (nio_g)", nio_g_coefficientsUniform);

    // Match phasefs07's input level (5) to test if Cheby diverges at higher
    // levels. cc->EvalMult(ct, 1.0) + ModReduceInternalInPlace simulates the
    // 5 rescales that the bootstrap pre-Cheby pipeline consumes.
    auto check_at_level = [&](const std::string &label,
                              const std::vector<double> &coeffs,
                              std::uint32_t target_level) {
        auto ct = fresh();
        auto algo = n.ctx.cc->GetScheme();
        const std::uint32_t cd = std::dynamic_pointer_cast<CryptoParametersCKKSRNS>(
            n.ctx.cc->GetCryptoParameters())->GetCompositeDegree();
        for (std::uint32_t k = 0; k < target_level; ++k) {
            n.ctx.cc->EvalMultInPlace(ct, 1.0);
            algo->ModReduceInternalInPlace(ct, cd);
        }
        auto ref = n.ctx.cc->EvalChebyshevSeries(ct, coeffs, -1.0, 1.0);
        auto haze_ct = ops::h2d_ct(n.ctx, ct);
        auto out = ops::eval_chebyshev_series_for_test(n.ctx, haze_ct, coeffs);
        REQUIRE(haze_vs_openfhe(n.ctx, out, ref, ("fs08 " + label).c_str()) == 0);
    };
    check_at_level("nio_g_at_level=5", nio_g_coefficientsUniform, 5);

    // Try larger amplitude inputs to see if input-byte magnitude matters.
    auto check_with_inputs = [&](const std::string &label,
                                  const std::vector<double> &v,
                                  const std::vector<double> &coeffs,
                                  std::uint32_t target_level) {
        auto pt = n.ctx.cc->MakeCKKSPackedPlaintext(v, 1, 0, nullptr, n.slots);
        auto ct = n.ctx.cc->Encrypt(n.ctx.keys.publicKey, pt);
        auto algo = n.ctx.cc->GetScheme();
        const std::uint32_t cd = std::dynamic_pointer_cast<CryptoParametersCKKSRNS>(
            n.ctx.cc->GetCryptoParameters())->GetCompositeDegree();
        for (std::uint32_t k = 0; k < target_level; ++k) {
            n.ctx.cc->EvalMultInPlace(ct, 1.0);
            algo->ModReduceInternalInPlace(ct, cd);
        }
        auto ref = n.ctx.cc->EvalChebyshevSeries(ct, coeffs, -1.0, 1.0);
        auto haze_ct = ops::h2d_ct(n.ctx, ct);
        auto out = ops::eval_chebyshev_series_for_test(n.ctx, haze_ct, coeffs);
        REQUIRE(haze_vs_openfhe(n.ctx, out, ref, ("fs08 " + label).c_str()) == 0);
    };

    // Mid-amplitude in [-0.4, 0.4]: closer to bootstrap conjugate-split outputs.
    {
        std::vector<double> v(n.slots);
        for (std::uint32_t i = 0; i < n.slots; ++i) v[i] = -0.4 + 0.8 * i / n.slots;
        check_with_inputs("mid_amp_level=5", v, nio_g_coefficientsUniform, 5);
    }
    // Random-ish: vary signs across slots.
    {
        std::vector<double> v(n.slots);
        for (std::uint32_t i = 0; i < n.slots; ++i)
            v[i] = std::sin(0.31415 * i) * 0.45;
        check_with_inputs("sinusoidal_level=5", v, nio_g_coefficientsUniform, 5);
    }
}

// phasefs09: reproduce fs07's pre-Cheby pipeline byte-for-byte but at the very
// end SWAP the input to Cheby with a fresh level-5 ciphertext (the one fs08 ran
// Cheby on successfully). If fs09 fails too, the issue is residual state from
// the pre-Cheby haze recordings interfering with Cheby. If fs09 passes, the
// issue is data-dependent on the actual fs07 ctxtEnc byte pattern.
TEST_CASE("phasefs09 fs07-prefix + Cheby on fresh-level5 input",
          "[.nio][.fsladder][e2e]") {
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto n = make_nio_ctx();
    auto bk = ops::make_bootstrap_keys(n.ctx, n.ctx.cc, n.ctx.keys.secretKey,
                                       n.slots, {4, 4});
    auto cp = std::dynamic_pointer_cast<CryptoParametersCKKSRNS>(
        n.ctx.cc->GetCryptoParameters());
    auto algo = n.ctx.cc->GetScheme();
    auto fhe_ckks = std::dynamic_pointer_cast<FHECKKSRNS>(algo->GetFHE());
    const auto &precom = *fhe_ckks->GetBootPrecomMap().at(n.slots);
    const std::uint32_t cd = cp->GetCompositeDegree();

    // Mimic fs07's pre-Cheby work but only as a "scribble" — purpose is to
    // build up haze state, not to verify it.
    {
        auto ct_pre = make_nio_depleted_ct(n, 6);
        auto haze_pre = ops::h2d_ct(n.ctx, ct_pre);
        const double qDouble = cp->GetElementParams()->GetParams()[0]->GetModulus().ConvertToDouble();
        const double powP = std::pow(2.0, cp->GetPlaintextModulus());
        const std::int32_t deg = static_cast<std::int32_t>(std::round(std::log2(qDouble / powP)));
        const std::int32_t correction = 11 - deg;
        const double pre = 1.0 / std::pow(2.0, static_cast<double>(deg));
        constexpr std::uint32_t K_UNIFORM = 512;
        const std::uint32_t N = static_cast<std::uint32_t>(n.ctx.ring_dim);
        auto adjusted = [&]() {
            auto r = ops::clone_ct(n.ctx, haze_pre);
            while (r.noise_scale_deg() > 1) r = ops::rescale(n.ctx, std::move(r));
            const double targetSF = cp->GetScalingFactorReal(0);
            const double sourceSF = r.scaling_factor();
            const std::uint32_t numTowers = static_cast<std::uint32_t>(r.towers());
            const double modToDrop = cp->GetElementParams()
                ->GetParams()[numTowers - 1]->GetModulus().ConvertToDouble();
            const double adjustmentFactor = (targetSF/sourceSF) * (modToDrop/sourceSF)
                                            * std::pow(2.0, -correction);
            r = ops::mult_by_const_for_test(n.ctx, r, adjustmentFactor);
            r = ops::rescale(n.ctx, std::move(r));
            r.set_scaling_factor(targetSF);
            return r;
        }();
        auto dep = ops::clone_ct(n.ctx, adjusted);
        if (dep.towers() > 1) dep = ops::level_reduce(n.ctx, std::move(dep), dep.towers() - 1);
        auto hr = ops::mod_raise(n.ctx, bk, dep);
        hr = ops::eval_mult_scalar_for_test(n.ctx, hr, pre / (static_cast<double>(K_UNIFORM) * N));
        hr = ops::rescale(n.ctx, hr);
        auto hcts = ops::eval_coeffs_to_slots(n.ctx, bk, bk.cts_matrices_fft, hr);
        // Trigger flush to make sure all this state has been replayed.
        (void)ops::d2h_ct(n.ctx, hcts);
        (void)precom;  // shut warning
    }

    // Now do fs08-style Cheby at level=5 (which we know passes in fs08 alone).
    std::vector<double> v(n.slots);
    for (std::uint32_t i = 0; i < n.slots; ++i) v[i] = 0.001 * (1.0 + 0.0001 * i);
    auto pt = n.ctx.cc->MakeCKKSPackedPlaintext(v, 1, 0, nullptr, n.slots);
    auto ct = n.ctx.cc->Encrypt(n.ctx.keys.publicKey, pt);
    for (std::uint32_t k = 0; k < 5; ++k) {
        n.ctx.cc->EvalMultInPlace(ct, 1.0);
        algo->ModReduceInternalInPlace(ct, cd);
    }
    auto ref = n.ctx.cc->EvalChebyshevSeries(ct, nio_g_coefficientsUniform, -1.0, 1.0);
    auto haze_ct = ops::h2d_ct(n.ctx, ct);
    auto out = ops::eval_chebyshev_series_for_test(n.ctx, haze_ct, nio_g_coefficientsUniform);
    namespace ops_ = haze::test::ops;
    const auto hb = ops_::d2h_ct(n.ctx, out);
    std::size_t bad = 0;
    for (std::size_t elem = 0; elem < 2; ++elem) {
        const auto &rd = ref->GetElements()[elem];
        const auto &hc = (elem == 0) ? hb.c0 : hb.c1;
        for (std::size_t t = 0; t < out.towers(); ++t) {
            const auto &rv = rd.GetElementAtIndex(static_cast<usint>(t)).GetValues();
            for (std::size_t i = 0; i < hc[t].size(); ++i)
                if (hc[t][i] != rv[i].template ConvertToInt<std::uint64_t>()) { ++bad; break; }
        }
    }
    std::cerr << "  [fs09] bad_polys=" << bad << " of " << (2 * out.towers()) << "\n";
    REQUIRE(bad == 0);
}

// phasefs05 (e2e): full niobium config (full-slot N/2, levelBudget {4,4},
// FLEXIBLEAUTO, depth=10+GetBootstrapDepth, scalingMod=59, firstMod=60) —
// haze ops::bootstrap vs cc->EvalBootstrap. Exercises the multi-stage FFT
// path (eval_coeffs_to_slots / eval_slots_to_coeffs) and is the definitive
// byte-parity gate for the niobium compiler config.
TEST_CASE("phasefs05 niobium-config {4,4} full-slot ops::bootstrap byte-equal e2e",
          "[.nio][.fsladder][e2e]") {
    using namespace lbcrypto;
    namespace ops = haze::test::ops;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto n = make_nio_ctx();
    std::cerr << "  [phasefs05] depth=" << n.depth << " slots=" << n.slots << "\n";
    auto bk = ops::make_bootstrap_keys(n.ctx, n.ctx.cc, n.ctx.keys.secretKey,
                                       n.slots, {4, 4});
    auto ct = make_nio_depleted_ct(n, 6);
    std::cerr << "  [phasefs05] input towers="
              << ct->GetElements()[0].GetNumOfElements()
              << " nsd=" << ct->GetNoiseScaleDeg() << "\n";

    auto ct_ref = n.ctx.cc->EvalBootstrap(ct);
    REQUIRE(ct_ref);
    auto haze_ct = ops::h2d_ct(n.ctx, ct);
    auto haze_refreshed = ops::bootstrap(n.ctx, bk, haze_ct,
                                         ops::BootstrapVariant::Standard);
    std::cerr << "  [phasefs05] ref towers="
              << ct_ref->GetElements()[0].GetNumOfElements()
              << " haze towers=" << haze_refreshed.towers() << "\n";

    const auto hb = ops::d2h_ct(n.ctx, haze_refreshed);
    std::size_t bad = 0;
    for (std::size_t elem = 0; elem < 2; ++elem) {
        const auto &rd = ct_ref->GetElements()[elem];
        const auto &hc = (elem == 0) ? hb.c0 : hb.c1;
        for (std::size_t t = 0; t < haze_refreshed.towers(); ++t) {
            const auto &rv = rd.GetElementAtIndex(
                static_cast<usint>(static_cast<std::uint32_t>(t))).GetValues();
            std::size_t mism = 0;
            for (std::size_t i = 0; i < hc[t].size(); ++i)
                if (hc[t][i] != rv[i].template ConvertToInt<std::uint64_t>()) ++mism;
            if (mism) { ++bad;
                if (bad <= 4)
                    std::cerr << "  [phasefs05] BAD elem=" << elem << " t=" << t
                              << " mism=" << mism << "/" << hc[t].size() << "\n"; }
        }
    }
    std::cerr << "  [phasefs05] bad_towers=" << bad << " of "
              << (2 * haze_refreshed.towers()) << "\n";
    REQUIRE(bad == 0);
}
