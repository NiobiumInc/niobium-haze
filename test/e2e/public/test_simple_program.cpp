// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Black-box "real program" capstone for the e2e suite: the FIDESlib
// `simple.cpp` op sequence (cAdd / cSub / cScalar / cMul+relin / cRot1 / cRot2)
// run end-to-end through the PUBLIC haze C ABI against a consumer's OWN stock
// OpenFHE. Crypto (encrypt / keygen / decrypt) is the linked stock OpenFHE;
// compute is haze (libhaze.so). Verification is by DECRYPTION only — this exe
// links a different OpenFHE than haze's hidden recorder, so bit-exact RNS
// equality is intentionally not asserted (that lives in haze_internal_tests).
//
// Reference program: references/FIDESlib/examples/simple/src/simple.cpp,
// mirrored bit-exact (on the instrumented build) by
// test/e2e/test_basic_operations_sequential_e2e.cpp.

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

TEMPLATE_TEST_CASE("e2e simple.cpp program through the public C ABI (stock OpenFHE)",
                   "[e2e][capstone]", haze::test::scaling::FixedManual,
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

    // FIXEDMANUAL doesn't auto-rescale after a mul, so do it explicitly to land
    // at the same level the other modes reach automatically.
    constexpr bool kPostRescaleMul = (P::kTech == lbcrypto::FIXEDMANUAL);

    // Stock-OpenFHE references: used both as correctly-shaped/leveled injection
    // shells and as a sanity oracle. (No CPROBES here, so these never touch
    // haze's trace.)
    auto cAdd_ref = ctx.cc->EvalAdd(ct1, ct2);
    auto cSub_ref = ctx.cc->EvalSub(ct1, ct2);
    auto cScalar_ref = ctx.cc->EvalMult(ct1, pt_k);
    auto cMul_ref = ctx.cc->EvalMult(ct1, ct2);
    if constexpr (kPostRescaleMul)
        ctx.cc->RescaleInPlace(cMul_ref);
    auto cRot1_ref = ctx.cc->EvalRotate(ct1, 1);
    auto cRot2_ref = ctx.cc->EvalRotate(ct1, -2);

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

    // Structural cross-check: haze tracks the same level (tower count) as stock.
    REQUIRE(haze_cAdd.openfhe_level(ctx.q_base.size()) == cAdd_ref->GetLevel());
    REQUIRE(haze_cSub.openfhe_level(ctx.q_base.size()) == cSub_ref->GetLevel());
    REQUIRE(haze_cScalar.openfhe_level(ctx.q_base.size()) == cScalar_ref->GetLevel());
    REQUIRE(haze_cMul.openfhe_level(ctx.q_base.size()) == cMul_ref->GetLevel());
    REQUIRE(haze_cRot1.openfhe_level(ctx.q_base.size()) == cRot1_ref->GetLevel());
    REQUIRE(haze_cRot2.openfhe_level(ctx.q_base.size()) == cRot2_ref->GetLevel());

    // Inject the haze-computed limbs into a stock-shaped shell, decrypt with the
    // stock secret key, and assert the slots match the expected plaintext.
    auto check = [&](const char *label, const ops::CtBytes &bytes,
                     const Ciphertext<DCRTPoly> &shell, const std::vector<double> &expected) {
        INFO("op: " << label);
        auto ct_haze = shell->Clone();
        ops::inject_ct(ctx, bytes, ct_haze);

        Plaintext pt_haze;
        ctx.cc->Decrypt(ctx.keys.secretKey, ct_haze, &pt_haze);
        pt_haze->SetLength(expected.size());
        const auto slots = pt_haze->GetRealPackedValue();
        REQUIRE(slots.size() == expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            INFO("slot " << i);
            REQUIRE_THAT(slots[i], Catch::Matchers::WithinAbs(expected[i], 1e-6));
        }
    };

    const std::size_t n = x1_vals.size();
    std::vector<double> e_add(n);
    std::vector<double> e_sub(n);
    std::vector<double> e_scalar(n);
    std::vector<double> e_mul(n);
    std::vector<double> e_rot1(n);
    std::vector<double> e_rot2(n);
    for (std::size_t i = 0; i < n; ++i) {
        e_add[i] = x1_vals[i] + x2_vals[i];
        e_sub[i] = x1_vals[i] - x2_vals[i];
        e_scalar[i] = k * x1_vals[i];
        e_mul[i] = x1_vals[i] * x2_vals[i];
        e_rot1[i] = x1_vals[(i + 1) % n];     // rotate left by 1
        e_rot2[i] = x1_vals[(i + n - 2) % n]; // rotate right by 2
    }

    check("cAdd", bytes_cAdd, cAdd_ref, e_add);
    check("cSub", bytes_cSub, cSub_ref, e_sub);
    check("cScalar", bytes_cScalar, cScalar_ref, e_scalar);
    check("cMul", bytes_cMul, cMul_ref, e_mul);
    check("cRot1", bytes_cRot1, cRot1_ref, e_rot1);
    check("cRot2", bytes_cRot2, cRot2_ref, e_rot2);
}
