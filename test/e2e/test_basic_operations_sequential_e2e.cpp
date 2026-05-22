// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// FIDESlib `simple.cpp` parity: cAdd / cSub / cScalar / cMul / cRot1 /
// cRot2 chained in a single haze epoch across all four scaling techniques.

#include "openfhe.h"
#include "ops.hpp"
#include "scaling_modes.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cstddef>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <vector>

TEMPLATE_TEST_CASE("openfhe basic-operations-sequential e2e", "[integration][e2e]",
                   haze::test::scaling::FixedManual, haze::test::scaling::FixedAuto,
                   haze::test::scaling::FlexibleAuto, haze::test::scaling::FlexibleAutoExt) {
    using P = TestType;
    using namespace lbcrypto;
    namespace ops = haze::test::ops;

    INFO("policy: " << P::kName);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);

    auto ctx = ops::make_ctx({.mode = P::kTech,
                              .mult_depth = 2,
                              .scaling_mod_size = 50,
                              .batch_size = 8,
                              .with_relin_key = true,
                              .rotate_indices = {1, -2},
                              .ring_dim = ops::RingDimChoice::OpenFHEDerives()});
    INFO("ring_dim=" << ctx.ring_dim << " |Q|=" << ctx.q_base.size()
                     << " |P|=" << ctx.p_base.size());

    const std::vector<double> x1_vals = {0.25, 0.5, 0.75, 1.0, 2.0, 3.0, 4.0, 5.0};
    const std::vector<double> x2_vals = {1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, 5.0};
    const double k = 4.0;
    const std::vector<double> k_vals(x1_vals.size(), k);

    Plaintext pt_x1 = ctx.cc->MakeCKKSPackedPlaintext(x1_vals);
    Plaintext pt_x2 = ctx.cc->MakeCKKSPackedPlaintext(x2_vals);
    Plaintext pt_k = ctx.cc->MakeCKKSPackedPlaintext(k_vals);
    auto ct1 = ctx.cc->Encrypt(ctx.keys.publicKey, pt_x1);
    auto ct2 = ctx.cc->Encrypt(ctx.keys.publicKey, pt_x2);

    // Every OpenFHE oracle runs BEFORE any haze compute (CPROBES would
    // otherwise pollute the trace).
    constexpr bool kPostRescaleMul = (P::kTech == lbcrypto::FIXEDMANUAL);

    auto cAdd_ref = ctx.cc->EvalAdd(ct1, ct2);
    auto cSub_ref = ctx.cc->EvalSub(ct1, ct2);
    auto cScalar_ref = ctx.cc->EvalMult(ct1, pt_k);
    auto cMul_ref = ctx.cc->EvalMult(ct1, ct2);
    if constexpr (kPostRescaleMul)
        ctx.cc->RescaleInPlace(cMul_ref);
    auto cRot1_ref = ctx.cc->EvalRotate(ct1, 1);
    auto cRot2_ref = ctx.cc->EvalRotate(ct1, -2);

    REQUIRE(cAdd_ref);
    REQUIRE(cSub_ref);
    REQUIRE(cScalar_ref);
    REQUIRE(cMul_ref);
    REQUIRE(cRot1_ref);
    REQUIRE(cRot2_ref);

    auto a = ops::h2d_ct(ctx, ct1);
    auto b = ops::h2d_ct(ctx, ct2);

    auto haze_cAdd = ops::add(ctx, a, b);
    auto haze_cScalar = ops::mult_scalar(ctx, a, pt_k);
    auto haze_cSub = ops::sub(ctx, a, b);
    auto haze_cMul = ops::mult(ctx, a, b);
    if constexpr (kPostRescaleMul)
        haze_cMul = ops::rescale(ctx, haze_cMul);
    auto haze_cRot1 = ops::rotate(ctx, a, 1);
    auto haze_cRot2 = ops::rotate(ctx, a, -2);

    // First d2h_ct closes the epoch and dispatches replay.
    const auto bytes_cAdd = ops::d2h_ct(ctx, haze_cAdd);
    const auto bytes_cSub = ops::d2h_ct(ctx, haze_cSub);
    const auto bytes_cScalar = ops::d2h_ct(ctx, haze_cScalar);
    const auto bytes_cMul = ops::d2h_ct(ctx, haze_cMul);
    const auto bytes_cRot1 = ops::d2h_ct(ctx, haze_cRot1);
    const auto bytes_cRot2 = ops::d2h_ct(ctx, haze_cRot2);

    REQUIRE(haze_cAdd.openfhe_level(ctx.q_base.size()) == cAdd_ref->GetLevel());
    REQUIRE(haze_cSub.openfhe_level(ctx.q_base.size()) == cSub_ref->GetLevel());
    REQUIRE(haze_cScalar.openfhe_level(ctx.q_base.size()) == cScalar_ref->GetLevel());
    REQUIRE(haze_cMul.openfhe_level(ctx.q_base.size()) == cMul_ref->GetLevel());
    REQUIRE(haze_cRot1.openfhe_level(ctx.q_base.size()) == cRot1_ref->GetLevel());
    REQUIRE(haze_cRot2.openfhe_level(ctx.q_base.size()) == cRot2_ref->GetLevel());

    auto compare_variant = [&](const char *label, const ops::CtBytes &bytes,
                               const Ciphertext<DCRTPoly> &ref,
                               const std::vector<double> &expected) {
        INFO("variant: " << label);
        auto ct_haze = ref->Clone();
        ops::inject_ct(ctx, bytes, ct_haze);

        const std::size_t out_towers = ref->GetElements()[0].GetNumOfElements();
        for (std::size_t e = 0; e < 2; ++e) {
            for (std::size_t t = 0; t < out_towers; ++t) {
                INFO("element " << e << " tower " << t);
                const auto &haze_np =
                    ct_haze->GetElements()[e].GetElementAtIndex(static_cast<usint>(t));
                const auto &ref_np = ref->GetElements()[e].GetElementAtIndex(static_cast<usint>(t));
                REQUIRE(haze_np.GetValues() == ref_np.GetValues());
            }
        }

        Plaintext pt_haze;
        Plaintext pt_ref;
        ctx.cc->Decrypt(ctx.keys.secretKey, ct_haze, &pt_haze);
        ctx.cc->Decrypt(ctx.keys.secretKey, ref, &pt_ref);
        pt_haze->SetLength(x1_vals.size());
        pt_ref->SetLength(x1_vals.size());

        const auto slots_haze = pt_haze->GetRealPackedValue();
        const auto slots_ref = pt_ref->GetRealPackedValue();
        for (std::size_t i = 0; i < x1_vals.size(); ++i) {
            INFO("slot " << i);
            REQUIRE_THAT(slots_haze[i], Catch::Matchers::WithinAbs(slots_ref[i], 1e-9));
            REQUIRE_THAT(slots_haze[i], Catch::Matchers::WithinAbs(expected[i], 1e-6));
        }
    };

    std::vector<double> expected_add(x1_vals.size());
    std::vector<double> expected_sub(x1_vals.size());
    std::vector<double> expected_scalar(x1_vals.size());
    std::vector<double> expected_mul(x1_vals.size());
    // Rotation slots wrap within batch_size (= encoded slot count).
    std::vector<double> expected_rot1(x1_vals.size());
    std::vector<double> expected_rot2(x1_vals.size());
    const std::size_t n = x1_vals.size();
    for (std::size_t i = 0; i < n; ++i) {
        expected_add[i] = x1_vals[i] + x2_vals[i];
        expected_sub[i] = x1_vals[i] - x2_vals[i];
        expected_scalar[i] = k * x1_vals[i];
        expected_mul[i] = x1_vals[i] * x2_vals[i];
        expected_rot1[i] = x1_vals[(i + 1) % n];
        expected_rot2[i] = x1_vals[(i + n - 2) % n];
    }
    compare_variant("cAdd", bytes_cAdd, cAdd_ref, expected_add);
    compare_variant("cSub", bytes_cSub, cSub_ref, expected_sub);
    compare_variant("cScalar", bytes_cScalar, cScalar_ref, expected_scalar);
    compare_variant("cMul", bytes_cMul, cMul_ref, expected_mul);
    compare_variant("cRot1", bytes_cRot1, cRot1_ref, expected_rot1);
    compare_variant("cRot2", bytes_cRot2, cRot2_ref, expected_rot2);
}
