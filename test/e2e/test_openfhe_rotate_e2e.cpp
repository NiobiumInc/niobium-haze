// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// EvalRotate parity (positive and negative slot index) across the four
// scaling modes via ops::rotate.

#include "openfhe.h"
#include "ops.hpp"
#include "scaling_modes.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <vector>

TEMPLATE_TEST_CASE("openfhe rotate e2e", "[integration][e2e]", haze::test::scaling::FixedManual,
                   haze::test::scaling::FixedAuto, haze::test::scaling::FlexibleAuto,
                   haze::test::scaling::FlexibleAutoExt) {
    using P = TestType;
    using namespace lbcrypto;
    namespace ops = haze::test::ops;

    INFO("policy: " << P::kName);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);

    auto ctx = ops::make_ctx({.mode = P::kTech,
                              .mult_depth = 2,
                              .scaling_mod_size = 50,
                              .batch_size = 8,
                              .with_relin_key = false,
                              .rotate_indices = {1, -2}});
    INFO("ring_dim=" << ctx.ring_dim << " |Q|=" << ctx.q_base.size()
                     << " |P|=" << ctx.p_base.size());

    const std::vector<double> x_vals = {0.25, 0.5, 0.75, 1.0, 2.0, 3.0, 4.0, 5.0};
    Plaintext pt_x = ctx.cc->MakeCKKSPackedPlaintext(x_vals);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt_x);

    auto run_one = [&](std::int32_t slot_idx) {
        INFO("slot_idx: " << slot_idx);

        auto ct_ref = ctx.cc->EvalRotate(ct, slot_idx);
        REQUIRE(ct_ref);
        REQUIRE(ct_ref->GetElements().size() == 2);

        auto a = ops::h2d_ct(ctx, ct);
        auto haze_rot = ops::rotate(ctx, a, slot_idx);
        const auto haze_bytes = ops::d2h_ct(ctx, haze_rot);

        REQUIRE(haze_rot.openfhe_level(ctx.q_base.size()) == ct_ref->GetLevel());

        auto ct_haze = ct_ref->Clone();
        ops::inject_ct(ctx, haze_bytes, ct_haze);
        for (std::size_t e = 0; e < 2; ++e) {
            for (std::size_t t = 0; t < haze_rot.towers(); ++t) {
                INFO("element " << e << " tower " << t);
                const auto &haze_np =
                    ct_haze->GetElements()[e].GetElementAtIndex(static_cast<usint>(t));
                const auto &ref_np =
                    ct_ref->GetElements()[e].GetElementAtIndex(static_cast<usint>(t));
                REQUIRE(haze_np.GetValues() == ref_np.GetValues());
            }
        }

        Plaintext pt_haze;
        Plaintext pt_ref;
        ctx.cc->Decrypt(ctx.keys.secretKey, ct_haze, &pt_haze);
        ctx.cc->Decrypt(ctx.keys.secretKey, ct_ref, &pt_ref);
        pt_haze->SetLength(x_vals.size());
        pt_ref->SetLength(x_vals.size());

        const auto slots_haze = pt_haze->GetRealPackedValue();
        const auto slots_ref = pt_ref->GetRealPackedValue();
        for (std::size_t i = 0; i < x_vals.size(); ++i) {
            INFO("slot " << i);
            REQUIRE_THAT(slots_haze[i], Catch::Matchers::WithinAbs(slots_ref[i], 1e-9));
        }
    };

    run_one(1);
    run_one(-2);
}
