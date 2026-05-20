// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// ct × ct mult with hybrid-keyswitch relin across the five (mode × rescale)
// variants. FIXEDMANUAL+rescale appends an explicit ops::rescale.

#include "openfhe.h"
#include "ops.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cstddef>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <vector>

namespace {

struct FixedManualNoRescale {
    static constexpr auto kTech = lbcrypto::FIXEDMANUAL;
    static constexpr bool kPostRescale = false;
    static constexpr char const *kName = "FIXEDMANUAL no-rescale";
    static lbcrypto::Ciphertext<lbcrypto::DCRTPoly>
    apply_openfhe(const lbcrypto::CryptoContext<lbcrypto::DCRTPoly> &cc,
                  const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &ct1,
                  const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &ct2) {
        return cc->EvalMult(ct1, ct2);
    }
};

struct FixedManualWithRescale {
    static constexpr auto kTech = lbcrypto::FIXEDMANUAL;
    static constexpr bool kPostRescale = true;
    static constexpr char const *kName = "FIXEDMANUAL with-rescale";
    static lbcrypto::Ciphertext<lbcrypto::DCRTPoly>
    apply_openfhe(const lbcrypto::CryptoContext<lbcrypto::DCRTPoly> &cc,
                  const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &ct1,
                  const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &ct2) {
        auto r = cc->EvalMult(ct1, ct2);
        cc->RescaleInPlace(r);
        return r;
    }
};

struct FixedAutoNoRescale {
    static constexpr auto kTech = lbcrypto::FIXEDAUTO;
    static constexpr bool kPostRescale = false;
    static constexpr char const *kName = "FIXEDAUTO";
    static lbcrypto::Ciphertext<lbcrypto::DCRTPoly>
    apply_openfhe(const lbcrypto::CryptoContext<lbcrypto::DCRTPoly> &cc,
                  const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &ct1,
                  const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &ct2) {
        return cc->EvalMult(ct1, ct2);
    }
};

struct FlexibleAutoNoRescale {
    static constexpr auto kTech = lbcrypto::FLEXIBLEAUTO;
    static constexpr bool kPostRescale = false;
    static constexpr char const *kName = "FLEXIBLEAUTO";
    static lbcrypto::Ciphertext<lbcrypto::DCRTPoly>
    apply_openfhe(const lbcrypto::CryptoContext<lbcrypto::DCRTPoly> &cc,
                  const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &ct1,
                  const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &ct2) {
        return cc->EvalMult(ct1, ct2);
    }
};

struct FlexibleAutoExtAutoRescale {
    static constexpr auto kTech = lbcrypto::FLEXIBLEAUTOEXT;
    static constexpr bool kPostRescale = false;
    static constexpr char const *kName = "FLEXIBLEAUTOEXT";
    static lbcrypto::Ciphertext<lbcrypto::DCRTPoly>
    apply_openfhe(const lbcrypto::CryptoContext<lbcrypto::DCRTPoly> &cc,
                  const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &ct1,
                  const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &ct2) {
        return cc->EvalMult(ct1, ct2);
    }
};

} // namespace

TEMPLATE_TEST_CASE("openfhe mul e2e", "[integration][e2e]", FixedManualNoRescale,
                   FixedManualWithRescale, FixedAutoNoRescale, FlexibleAutoNoRescale,
                   FlexibleAutoExtAutoRescale) {
    using P = TestType;
    using namespace lbcrypto;
    namespace ops = haze::test::ops;

    INFO("policy: " << P::kName);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);

    auto ctx = ops::make_ctx({.mode = P::kTech,
                              .mult_depth = 2,
                              .scaling_mod_size = 50,
                              .batch_size = 8,
                              .with_relin_key = true});
    INFO("ring_dim=" << ctx.ring_dim << " |Q|=" << ctx.q_base.size()
                     << " |P|=" << ctx.p_base.size());

    const std::vector<double> x1_vals = {0.25, 0.5, 0.75, 1.0, 2.0, 3.0, 4.0, 5.0};
    const std::vector<double> x2_vals = {1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, 5.0};

    Plaintext pt_x1 = ctx.cc->MakeCKKSPackedPlaintext(x1_vals);
    Plaintext pt_x2 = ctx.cc->MakeCKKSPackedPlaintext(x2_vals);
    auto ct1 = ctx.cc->Encrypt(ctx.keys.publicKey, pt_x1);
    auto ct2 = ctx.cc->Encrypt(ctx.keys.publicKey, pt_x2);

    auto ct_ref = P::apply_openfhe(ctx.cc, ct1, ct2);
    REQUIRE(ct_ref);
    REQUIRE(ct_ref->GetElements().size() == 2);

    auto a = ops::h2d_ct(ctx, ct1);
    auto b = ops::h2d_ct(ctx, ct2);
    auto haze_prod = ops::mult(ctx, a, b);
    if constexpr (P::kPostRescale) {
        haze_prod = ops::rescale(ctx, haze_prod);
    }
    const auto haze_bytes = ops::d2h_ct(ctx, haze_prod);

    REQUIRE(haze_prod.openfhe_level(ctx.q_base.size()) == ct_ref->GetLevel());

    auto ct_haze = ct_ref->Clone();
    ops::inject_ct(ctx, haze_bytes, ct_haze);
    for (std::size_t e = 0; e < 2; ++e) {
        for (std::size_t t = 0; t < haze_prod.towers(); ++t) {
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
    pt_haze->SetLength(x1_vals.size());
    pt_ref->SetLength(x1_vals.size());

    const auto slots_haze = pt_haze->GetRealPackedValue();
    const auto slots_ref = pt_ref->GetRealPackedValue();
    for (std::size_t i = 0; i < x1_vals.size(); ++i) {
        INFO("slot " << i);
        REQUIRE_THAT(slots_haze[i], Catch::Matchers::WithinAbs(slots_ref[i], 1e-9));
        REQUIRE_THAT(slots_haze[i], Catch::Matchers::WithinAbs(x1_vals[i] * x2_vals[i], 1e-6));
    }
}
