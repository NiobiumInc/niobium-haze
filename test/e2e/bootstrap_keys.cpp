// Copyright (C) 2026, All rights reserved by Niobium Microsystems.

#include "bootstrap.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <haze/replay_bridge_cc.hpp>
#include <openfhe.h>
#include <scheme/ckksrns/ckksrns-fhe.h>
#include <vector>

namespace haze::test::ops {

namespace {

// Upload the full Q∥P-basis encoded plaintext. Hoisted linear_transform
// consumes it as-is for EvalMultExt-style multiplication in the extended
// basis; the trailing P-towers participate in the keyswitch math.
Allocs upload_pt_chain(const OpCtx &ctx, const lbcrypto::ReadOnlyPlaintext &pt) {
    auto pt_elem = pt->GetElement<lbcrypto::DCRTPoly>();
    pt_elem.SetFormat(Format::EVALUATION);
    const std::size_t pt_towers = pt_elem.GetNumOfElements();
    std::vector<std::vector<uint64_t>> chain(pt_towers);
    for (std::size_t t = 0; t < pt_towers; ++t) {
        const auto &np = pt_elem.GetElementAtIndex(static_cast<usint>(t));
        const auto &vals = np.GetValues();
        REQUIRE(vals.GetLength() == ctx.ring_dim);
        chain[t].resize(ctx.ring_dim);
        for (std::size_t i = 0; i < ctx.ring_dim; ++i) {
            chain[t][i] = vals[i].template ConvertToInt<std::uint64_t>();
        }
    }
    return Allocs(chain);
}

} // namespace

BootstrapKeys make_bootstrap_keys(const OpCtx &ctx,
                                  const lbcrypto::CryptoContext<lbcrypto::DCRTPoly> &cc,
                                  const lbcrypto::PrivateKey<lbcrypto::DCRTPoly> &sk,
                                  std::uint32_t slots,
                                  const std::vector<std::uint32_t> &level_budget) {
    using namespace lbcrypto;

    REQUIRE(cc);
    REQUIRE(sk);
    REQUIRE(slots > 0);
    REQUIRE(level_budget.size() == 2);
    REQUIRE(level_budget[0] == 1);
    REQUIRE(level_budget[1] == 1);

    // correctionFactor=11 matches FIDESlib's CKKSBootstrapTest fixture and
    // is required so cc->EvalBootstrap doesn't reject our depth=25 setup.
    cc->EvalBootstrapSetup(level_budget, {0, 0}, slots, 11, true, false);
    cc->EvalBootstrapKeyGen(sk, slots);

    BootstrapKeys bk;
    bk.params.slots = slots;
    bk.params.level_budget = level_budget;
    bk.params.cyclotomic_order = static_cast<std::uint64_t>(cc->GetCyclotomicOrder());

    REQUIRE(haze::hazeReplayBridgeExtractEvalMultKey(cc, sk, bk.relin_key) == HAZE_SUCCESS);

    const auto M = static_cast<std::uint32_t>(bk.params.cyclotomic_order);
    REQUIRE(haze::hazeReplayBridgeExtractAutomorphismKey(cc, sk, M - 1, bk.conjugation_key) ==
            HAZE_SUCCESS);

    // Iterate every automorphism key cc registered against the sk's tag.
    // After EvalBootstrapKeyGen this contains the BSGS rotation set plus
    // the M-1 conjugation entry; we filter out M-1 since it lives in bk
    // separately.
    const auto &auto_map = CryptoContextImpl<DCRTPoly>::GetEvalAutomorphismKeyMap(sk->GetKeyTag());
    for (const auto &[auto_index, key] : auto_map) {
        if (auto_index == M - 1)
            continue;
        if (bk.rotation_keys.find(auto_index) != bk.rotation_keys.end())
            continue;
        RotationKeyEntry entry;
        entry.auto_index = auto_index;
        REQUIRE(haze::hazeReplayBridgeExtractAutomorphismKey(cc, sk, auto_index, entry.limbs) ==
                HAZE_SUCCESS);
        bk.rotation_keys.emplace(auto_index, std::move(entry));
    }

    auto fhe_base = cc->GetScheme()->GetFHE();
    REQUIRE(fhe_base);
    auto fhe_ckks = std::dynamic_pointer_cast<FHECKKSRNS>(fhe_base);
    REQUIRE(fhe_ckks);
    const auto &precom_map = fhe_ckks->GetBootPrecomMap();
    auto it = precom_map.find(slots);
    REQUIRE(it != precom_map.end());
    const auto &precom = *it->second;
    bk.params.chebyshev_degree = static_cast<std::uint32_t>(precom.m_paramsEnc.g);

    bk.cts_matrices.reserve(precom.m_U0hatTPre.size());
    for (const auto &pt : precom.m_U0hatTPre)
        bk.cts_matrices.push_back(upload_pt_chain(ctx, pt));
    bk.stc_matrices.reserve(precom.m_U0Pre.size());
    for (const auto &pt : precom.m_U0Pre)
        bk.stc_matrices.push_back(upload_pt_chain(ctx, pt));

    auto rns_params = std::dynamic_pointer_cast<CryptoParametersCKKSRNS>(cc->GetCryptoParameters());
    REQUIRE(rns_params);
    const auto &pmodq = rns_params->GetPModq();
    bk.p_mod_q.reserve(pmodq.size());
    for (const auto &v : pmodq)
        bk.p_mod_q.push_back(v.ConvertToInt());

    return bk;
}

} // namespace haze::test::ops
