// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// ct × ct tensor product (degree-2, no relin) via the OpenFHE-internal
// `cc->GetScheme()->EvalMult(ct1, ct2)`. Bit-exact compare on d0/d1/d2.
// This is the low-level probe; the relin path lives in test_openfhe_mul_e2e.cpp.

#include "integration_helpers.hpp"
#include "openfhe.h"

#include <catch2/catch_message.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <vector>

namespace {

struct FixedManualNoRescale {
    static constexpr auto kTech = lbcrypto::FIXEDMANUAL;
    static constexpr bool kPreRescale = false;
    static constexpr bool kPostRescale = false;
    static constexpr char const *kName = "FIXEDMANUAL no-rescale";
    static lbcrypto::Ciphertext<lbcrypto::DCRTPoly>
    apply_openfhe(const lbcrypto::CryptoContext<lbcrypto::DCRTPoly> &cc,
                  const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &ct1,
                  const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &ct2) {
        lbcrypto::ConstCiphertext<lbcrypto::DCRTPoly> c1 = ct1;
        lbcrypto::ConstCiphertext<lbcrypto::DCRTPoly> c2 = ct2;
        return cc->GetScheme()->EvalMult(c1, c2);
    }
};

struct FixedManualWithRescale {
    static constexpr auto kTech = lbcrypto::FIXEDMANUAL;
    static constexpr bool kPreRescale = false;
    static constexpr bool kPostRescale = true;
    static constexpr char const *kName = "FIXEDMANUAL with-rescale";
    static lbcrypto::Ciphertext<lbcrypto::DCRTPoly>
    apply_openfhe(const lbcrypto::CryptoContext<lbcrypto::DCRTPoly> &cc,
                  const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &ct1,
                  const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &ct2) {
        lbcrypto::ConstCiphertext<lbcrypto::DCRTPoly> c1 = ct1;
        lbcrypto::ConstCiphertext<lbcrypto::DCRTPoly> c2 = ct2;
        auto r = cc->GetScheme()->EvalMult(c1, c2);
        cc->RescaleInPlace(r);
        return r;
    }
};

struct FixedAutoNoRescale {
    static constexpr auto kTech = lbcrypto::FIXEDAUTO;
    static constexpr bool kPreRescale = false;
    static constexpr bool kPostRescale = false;
    static constexpr char const *kName = "FIXEDAUTO";
    static lbcrypto::Ciphertext<lbcrypto::DCRTPoly>
    apply_openfhe(const lbcrypto::CryptoContext<lbcrypto::DCRTPoly> &cc,
                  const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &ct1,
                  const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &ct2) {
        lbcrypto::ConstCiphertext<lbcrypto::DCRTPoly> c1 = ct1;
        lbcrypto::ConstCiphertext<lbcrypto::DCRTPoly> c2 = ct2;
        return cc->GetScheme()->EvalMult(c1, c2);
    }
};

struct FlexibleAutoNoRescale {
    static constexpr auto kTech = lbcrypto::FLEXIBLEAUTO;
    static constexpr bool kPreRescale = false;
    static constexpr bool kPostRescale = false;
    static constexpr char const *kName = "FLEXIBLEAUTO";
    static lbcrypto::Ciphertext<lbcrypto::DCRTPoly>
    apply_openfhe(const lbcrypto::CryptoContext<lbcrypto::DCRTPoly> &cc,
                  const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &ct1,
                  const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &ct2) {
        lbcrypto::ConstCiphertext<lbcrypto::DCRTPoly> c1 = ct1;
        lbcrypto::ConstCiphertext<lbcrypto::DCRTPoly> c2 = ct2;
        return cc->GetScheme()->EvalMult(c1, c2);
    }
};

struct FlexibleAutoExtAutoRescale {
    static constexpr auto kTech = lbcrypto::FLEXIBLEAUTOEXT;
    static constexpr bool kPreRescale = true;
    static constexpr bool kPostRescale = false;
    static constexpr char const *kName = "FLEXIBLEAUTOEXT";
    static lbcrypto::Ciphertext<lbcrypto::DCRTPoly>
    apply_openfhe(const lbcrypto::CryptoContext<lbcrypto::DCRTPoly> &cc,
                  const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &ct1,
                  const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &ct2) {
        lbcrypto::ConstCiphertext<lbcrypto::DCRTPoly> c1 = ct1;
        lbcrypto::ConstCiphertext<lbcrypto::DCRTPoly> c2 = ct2;
        return cc->GetScheme()->EvalMult(c1, c2);
    }
};

} // namespace

TEMPLATE_TEST_CASE("openfhe mul-no-relin e2e", "[integration][e2e]", FixedManualNoRescale,
                   FixedManualWithRescale, FixedAutoNoRescale, FlexibleAutoNoRescale,
                   FlexibleAutoExtAutoRescale) {
    using P = TestType;
    using namespace lbcrypto;

    INFO("policy: " << P::kName);

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);

    CCParams<CryptoContextCKKSRNS> params;
    params.SetMultiplicativeDepth(2);
    params.SetScalingModSize(50);
    params.SetBatchSize(8);
    params.SetScalingTechnique(P::kTech);
    auto cc = GenCryptoContext(params);
    REQUIRE(cc);
    cc->Enable(PKE);
    cc->Enable(KEYSWITCH);
    cc->Enable(LEVELEDSHE);

    auto keys = cc->KeyGen();

    const uint64_t ring_dim = cc->GetRingDimension();

    const auto &eparams = cc->GetCryptoParameters()->GetElementParams()->GetParams();
    std::vector<uint64_t> base;
    base.reserve(eparams.size());
    for (const auto &p : eparams) {
        base.push_back(p->GetModulus().ConvertToInt());
    }
    REQUIRE(!base.empty());

    // Fresh context carrying the full chain; pure-C bridge seeded from the
    // first Q prime.
    haze::test::recreate_ctx(ring_dim, base);
    uint64_t picked = 0;
    REQUIRE(hazeReplayBridgeInitCryptoContext(ring_dim, base.front(), &picked) == HAZE_SUCCESS);
    REQUIRE(picked != 0);

    INFO("ring_dim=" << ring_dim << " towers=" << base.size());
    REQUIRE(base.size() >= 2);

    // Distinct per-slot values so a per-tower mix-up can't hide behind broadcast.
    const std::vector<double> x1_vals = {0.25, 0.5, 0.75, 1.0, 2.0, 3.0, 4.0, 5.0};
    const std::vector<double> x2_vals = {1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, 5.0};
    REQUIRE(x1_vals.size() == x2_vals.size());

    Plaintext pt_x1 = cc->MakeCKKSPackedPlaintext(x1_vals);
    Plaintext pt_x2 = cc->MakeCKKSPackedPlaintext(x2_vals);
    auto ct1 = cc->Encrypt(keys.publicKey, pt_x1);
    auto ct2 = cc->Encrypt(keys.publicKey, pt_x2);
    REQUIRE(ct1);
    REQUIRE(ct2);
    REQUIRE(ct1->GetElements().size() == 2);
    REQUIRE(ct2->GetElements().size() == 2);
    REQUIRE(ct1->GetElements()[0].GetNumOfElements() == base.size());
    REQUIRE(ct2->GetElements()[0].GetNumOfElements() == base.size());

    auto extract_chain = [&](const Ciphertext<DCRTPoly> &c, std::size_t elem_idx,
                             std::size_t expected_towers) {
        std::vector<std::vector<uint64_t>> chain(expected_towers);
        const auto &elem = c->GetElements()[elem_idx];
        REQUIRE(elem.GetNumOfElements() == expected_towers);
        for (std::size_t t = 0; t < expected_towers; ++t) {
            const auto &np = elem.GetElementAtIndex(static_cast<usint>(t));
            const auto &vals = np.GetValues();
            const std::size_t n = vals.GetLength();
            REQUIRE(n == ring_dim);
            chain[t].resize(n);
            for (std::size_t i = 0; i < n; ++i) {
                chain[t][i] = vals[i].template ConvertToInt<uint64_t>();
            }
        }
        return chain;
    };

    const auto a_c0 = extract_chain(ct1, 0, base.size());
    const auto a_c1 = extract_chain(ct1, 1, base.size());
    const auto b_c0 = extract_chain(ct2, 0, base.size());
    const auto b_c1 = extract_chain(ct2, 1, base.size());

    auto ct_ref = P::apply_openfhe(cc, ct1, ct2);
    REQUIRE(ct_ref);
    REQUIRE(ct_ref->GetElements().size() == 3);

    constexpr bool kRescales = P::kPreRescale || P::kPostRescale;
    const std::size_t out_towers = ct_ref->GetElements()[0].GetNumOfElements();
    REQUIRE(out_towers == (kRescales ? base.size() - 1 : base.size()));

    const auto ref_d0 = extract_chain(ct_ref, 0, out_towers);
    const auto ref_d1 = extract_chain(ct_ref, 1, out_towers);
    const auto ref_d2 = extract_chain(ct_ref, 2, out_towers);

    const std::size_t kBytes = ring_dim * sizeof(uint64_t);
    const std::vector<uint64_t> rescale_base = {base.back()};
    const std::vector<uint64_t> out_base(base.begin(),
                                         base.begin() + static_cast<std::ptrdiff_t>(out_towers));
    const hazeModDownParams md_params = {
        .src_base = base.data(),
        .src_base_len = base.size(),
        .rescale_base = rescale_base.data(),
        .rescale_base_len = rescale_base.size(),
    };

    auto rescale_chain = [&](const std::vector<void *> &src_full) {
        auto intt = haze::test::allocate_dst_residues(base.size(), kBytes);
        auto md = haze::test::allocate_dst_residues(out_towers, kBytes);
        auto ntt = haze::test::allocate_dst_residues(out_towers, kBytes);
        const auto src_c = haze::test::to_const(src_full);
        REQUIRE(hazeINTTMrp(haze::test::ctx(), intt.data(), src_c.data(), base.data(), base.size(),
                            nullptr) == HAZE_SUCCESS);
        const auto intt_c = haze::test::to_const(intt);
        REQUIRE(hazeModDown(haze::test::ctx(), md.data(), intt_c.data(), &md_params, nullptr) ==
                HAZE_SUCCESS);
        const auto md_c = haze::test::to_const(md);
        REQUIRE(hazeNTTMrp(haze::test::ctx(), ntt.data(), md_c.data(), out_base.data(),
                           out_base.size(), nullptr) == HAZE_SUCCESS);
        haze::test::free_all_residues(intt);
        haze::test::free_all_residues(md);
        return ntt;
    };

    auto d2h_chain = [&](const std::vector<void *> &dev_chain, std::size_t towers) {
        std::vector<std::vector<uint64_t>> host(towers, std::vector<uint64_t>(ring_dim));
        for (std::size_t t = 0; t < towers; ++t) {
            REQUIRE(hazeMemcpy(haze::test::ctx(), host[t].data(), dev_chain[t], kBytes,
                               HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
        }
        return host;
    };

    auto da_c0 = haze::test::allocate_and_h2d_residues(a_c0);
    auto da_c1 = haze::test::allocate_and_h2d_residues(a_c1);
    auto db_c0 = haze::test::allocate_and_h2d_residues(b_c0);
    auto db_c1 = haze::test::allocate_and_h2d_residues(b_c1);

    // kPreRescale materializes rescaled chains; otherwise the H2D inputs feed
    // the tensor directly.
    std::vector<void *> wa_c0_owned;
    std::vector<void *> wa_c1_owned;
    std::vector<void *> wb_c0_owned;
    std::vector<void *> wb_c1_owned;
    const std::vector<void *> *wa_c0 = nullptr;
    const std::vector<void *> *wa_c1 = nullptr;
    const std::vector<void *> *wb_c0 = nullptr;
    const std::vector<void *> *wb_c1 = nullptr;

    if constexpr (P::kPreRescale) {
        wa_c0_owned = rescale_chain(da_c0);
        wa_c1_owned = rescale_chain(da_c1);
        wb_c0_owned = rescale_chain(db_c0);
        wb_c1_owned = rescale_chain(db_c1);
        wa_c0 = &wa_c0_owned;
        wa_c1 = &wa_c1_owned;
        wb_c0 = &wb_c0_owned;
        wb_c1 = &wb_c1_owned;
    } else {
        wa_c0 = &da_c0;
        wa_c1 = &da_c1;
        wb_c0 = &db_c0;
        wb_c1 = &db_c1;
    }

    constexpr bool kPre = P::kPreRescale;
    const std::size_t work_towers = kPre ? out_towers : base.size();
    const std::vector<uint64_t> &work_base = kPre ? out_base : base;

    auto d0 = haze::test::allocate_dst_residues(work_towers, kBytes);
    auto t_buf = haze::test::allocate_dst_residues(work_towers, kBytes);
    auto u_buf = haze::test::allocate_dst_residues(work_towers, kBytes);
    auto d1 = haze::test::allocate_dst_residues(work_towers, kBytes);
    auto d2 = haze::test::allocate_dst_residues(work_towers, kBytes);

    {
        const auto wa_c0_c = haze::test::to_const(*wa_c0);
        const auto wa_c1_c = haze::test::to_const(*wa_c1);
        const auto wb_c0_c = haze::test::to_const(*wb_c0);
        const auto wb_c1_c = haze::test::to_const(*wb_c1);

        REQUIRE(hazeMulMrp(haze::test::ctx(), d0.data(), wa_c0_c.data(), wb_c0_c.data(),
                           work_base.data(), work_base.size(), nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeMulMrp(haze::test::ctx(), t_buf.data(), wa_c0_c.data(), wb_c1_c.data(),
                           work_base.data(), work_base.size(), nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeMulMrp(haze::test::ctx(), u_buf.data(), wa_c1_c.data(), wb_c0_c.data(),
                           work_base.data(), work_base.size(), nullptr) == HAZE_SUCCESS);
        const auto t_c = haze::test::to_const(t_buf);
        const auto u_c = haze::test::to_const(u_buf);
        REQUIRE(hazeAddMrp(haze::test::ctx(), d1.data(), t_c.data(), u_c.data(), work_base.data(),
                           work_base.size(), nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeMulMrp(haze::test::ctx(), d2.data(), wa_c1_c.data(), wb_c1_c.data(),
                           work_base.data(), work_base.size(), nullptr) == HAZE_SUCCESS);
    }
    // t_buf and u_buf are consumed by the AddMrp above only.
    haze::test::free_all_residues(t_buf);
    haze::test::free_all_residues(u_buf);

    std::vector<void *> r_d0_owned;
    std::vector<void *> r_d1_owned;
    std::vector<void *> r_d2_owned;
    const std::vector<void *> *final_d0 = &d0;
    const std::vector<void *> *final_d1 = &d1;
    const std::vector<void *> *final_d2 = &d2;
    if constexpr (P::kPostRescale) {
        r_d0_owned = rescale_chain(d0);
        r_d1_owned = rescale_chain(d1);
        r_d2_owned = rescale_chain(d2);
        final_d0 = &r_d0_owned;
        final_d1 = &r_d1_owned;
        final_d2 = &r_d2_owned;
    }

    for (const std::vector<void *> *chain : {final_d0, final_d1, final_d2})
        for (void *p : *chain)
            REQUIRE(hazeTagOutput(haze::test::ctx(), p) == HAZE_SUCCESS);
    REQUIRE(hazeFlush(haze::test::ctx()) == HAZE_SUCCESS);

    const auto haze_d0 = d2h_chain(*final_d0, out_towers);
    const auto haze_d1 = d2h_chain(*final_d1, out_towers);
    const auto haze_d2 = d2h_chain(*final_d2, out_towers);

    haze::test::free_all_residues(da_c0);
    haze::test::free_all_residues(da_c1);
    haze::test::free_all_residues(db_c0);
    haze::test::free_all_residues(db_c1);
    if constexpr (P::kPreRescale) {
        haze::test::free_all_residues(wa_c0_owned);
        haze::test::free_all_residues(wa_c1_owned);
        haze::test::free_all_residues(wb_c0_owned);
        haze::test::free_all_residues(wb_c1_owned);
    }
    haze::test::free_all_residues(d0);
    haze::test::free_all_residues(d1);
    haze::test::free_all_residues(d2);
    if constexpr (P::kPostRescale) {
        haze::test::free_all_residues(r_d0_owned);
        haze::test::free_all_residues(r_d1_owned);
        haze::test::free_all_residues(r_d2_owned);
    }

    for (std::size_t t = 0; t < out_towers; ++t) {
        INFO("tower " << t << " modulus " << base[t]);
        REQUIRE(haze_d0[t] == ref_d0[t]);
        REQUIRE(haze_d1[t] == ref_d1[t]);
        REQUIRE(haze_d2[t] == ref_d2[t]);
    }

    auto ct_haze = ct_ref->Clone();
    auto inject = [&](Ciphertext<DCRTPoly> &c, std::size_t elem_idx, std::size_t tower_idx,
                      const std::vector<uint64_t> &limbs) {
        REQUIRE(limbs.size() == ring_dim);
        auto &dcrt = c->GetElements()[elem_idx];
        auto &towers = dcrt.GetAllElements();
        auto &np = towers[tower_idx];
        NativeVector nv(static_cast<usint>(ring_dim), NativeInteger(np.GetModulus()));
        for (std::size_t i = 0; i < ring_dim; ++i) {
            nv[i] = NativeInteger(limbs[i]);
        }
        np.SetValues(nv, np.GetFormat());
    };
    for (std::size_t t = 0; t < out_towers; ++t) {
        inject(ct_haze, 0, t, haze_d0[t]);
        inject(ct_haze, 1, t, haze_d1[t]);
        inject(ct_haze, 2, t, haze_d2[t]);
    }

    Plaintext pt_haze;
    Plaintext pt_ref;
    cc->Decrypt(keys.secretKey, ct_haze, &pt_haze);
    cc->Decrypt(keys.secretKey, ct_ref, &pt_ref);
    pt_haze->SetLength(x1_vals.size());
    pt_ref->SetLength(x1_vals.size());

    const auto slots_haze = pt_haze->GetRealPackedValue();
    const auto slots_ref = pt_ref->GetRealPackedValue();
    REQUIRE(slots_haze.size() == x1_vals.size());
    REQUIRE(slots_ref.size() == x1_vals.size());

    for (std::size_t i = 0; i < x1_vals.size(); ++i) {
        INFO("slot " << i);
        REQUIRE_THAT(slots_haze[i], Catch::Matchers::WithinAbs(slots_ref[i], 1e-9));
        REQUIRE_THAT(slots_haze[i], Catch::Matchers::WithinAbs(x1_vals[i] * x2_vals[i], 1e-6));
    }
}
