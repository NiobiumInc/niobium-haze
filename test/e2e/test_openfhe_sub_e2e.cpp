// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// EvalSub parity across the four scaling modes via ops::sub.

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

TEMPLATE_TEST_CASE("openfhe sub e2e", "[integration][e2e]", haze::test::scaling::FixedManual,
                   haze::test::scaling::FixedAuto, haze::test::scaling::FlexibleAuto,
                   haze::test::scaling::FlexibleAutoExt) {
    using P = TestType;
    using namespace lbcrypto;
    namespace ops = haze::test::ops;

    INFO("policy: " << P::kName);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);

    auto ctx = ops::make_ctx({.mode = P::kTech,
                              .mult_depth = 1,
                              .scaling_mod_size = 50,
                              .batch_size = 8,
                              .ring_dim = ops::RingDimChoice::OpenFHEDerives()});
    INFO("ring_dim=" << ctx.ring_dim << " towers=" << ctx.q_base.size());

    const std::vector<double> a_vals = {3.0, 5.0, 7.0, 9.0, 11.0, 13.0, 15.0, 17.0};
    const std::vector<double> b_vals = {0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5};
    REQUIRE(a_vals.size() == b_vals.size());

    Plaintext pt_a = ctx.cc->MakeCKKSPackedPlaintext(a_vals);
    Plaintext pt_b = ctx.cc->MakeCKKSPackedPlaintext(b_vals);
    auto ct_a = ctx.cc->Encrypt(ctx.keys.publicKey, pt_a);
    auto ct_b = ctx.cc->Encrypt(ctx.keys.publicKey, pt_b);

    auto ct_ref = ctx.cc->EvalSub(ct_a, ct_b);
    REQUIRE(ct_ref);

    auto a = ops::h2d_ct(ctx, ct_a);
    auto b = ops::h2d_ct(ctx, ct_b);
    auto haze_diff = ops::sub(ctx, a, b);
    ops::flush_cts({&haze_diff});
    const auto haze_bytes = ops::d2h_ct(ctx, haze_diff);

    REQUIRE(haze_diff.openfhe_level(ctx.q_base.size()) == ct_ref->GetLevel());

    auto ct_haze = ct_ref->Clone();
    ops::inject_ct(ctx, haze_bytes, ct_haze);
    for (std::size_t e = 0; e < 2; ++e) {
        for (std::size_t t = 0; t < haze_diff.towers(); ++t) {
            INFO("element " << e << " tower " << t);
            const auto &haze_np =
                ct_haze->GetElements()[e].GetElementAtIndex(static_cast<usint>(t));
            const auto &ref_np = ct_ref->GetElements()[e].GetElementAtIndex(static_cast<usint>(t));
            REQUIRE(haze_np.GetValues() == ref_np.GetValues());
        }
    }

    Plaintext pt_haze;
    Plaintext pt_ref;
    ctx.cc->Decrypt(ctx.keys.secretKey, ct_haze, &pt_haze);
    ctx.cc->Decrypt(ctx.keys.secretKey, ct_ref, &pt_ref);
    pt_haze->SetLength(a_vals.size());
    pt_ref->SetLength(a_vals.size());

    const auto slots_haze = pt_haze->GetRealPackedValue();
    const auto slots_ref = pt_ref->GetRealPackedValue();
    for (std::size_t i = 0; i < a_vals.size(); ++i) {
        INFO("slot " << i);
        REQUIRE_THAT(slots_haze[i], Catch::Matchers::WithinAbs(slots_ref[i], 1e-9));
        REQUIRE_THAT(slots_haze[i], Catch::Matchers::WithinAbs(a_vals[i] - b_vals[i], 1e-6));
    }
}
