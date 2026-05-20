// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Targeted unit test for the bridge key-extract APIs. Builds a CC, gens
// the relin + rotation keys, calls the Extract APIs, asserts shape and
// base-prime content match the CC's CryptoParametersRNS.

#include "openfhe.h"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge_cc.hpp>
#include <memory>
#include <vector>

namespace {

lbcrypto::CryptoContext<lbcrypto::DCRTPoly> build_cc() {
    using namespace lbcrypto;
    CCParams<CryptoContextCKKSRNS> params;
    params.SetMultiplicativeDepth(2);
    params.SetScalingModSize(50);
    params.SetBatchSize(8);
    params.SetScalingTechnique(FIXEDAUTO);
    auto cc = GenCryptoContext(params);
    REQUIRE(cc);
    cc->Enable(PKE);
    cc->Enable(KEYSWITCH);
    cc->Enable(LEVELEDSHE);
    return cc;
}

std::vector<uint64_t> q_base_of(const lbcrypto::CryptoContext<lbcrypto::DCRTPoly> &cc) {
    const auto &params = cc->GetCryptoParameters()->GetElementParams()->GetParams();
    std::vector<uint64_t> out;
    out.reserve(params.size());
    for (const auto &p : params)
        out.push_back(p->GetModulus().ConvertToInt());
    return out;
}

std::vector<uint64_t> p_base_of(const lbcrypto::CryptoContext<lbcrypto::DCRTPoly> &cc) {
    auto rns = std::dynamic_pointer_cast<lbcrypto::CryptoParametersRNS>(cc->GetCryptoParameters());
    REQUIRE(rns);
    const auto &params = rns->GetParamsP()->GetParams();
    std::vector<uint64_t> out;
    out.reserve(params.size());
    for (const auto &p : params)
        out.push_back(p->GetModulus().ConvertToInt());
    return out;
}

void check_limb_shape(const haze::HybridKeyswitchLimbs &limbs,
                      const lbcrypto::CryptoContext<lbcrypto::DCRTPoly> &cc) {
    const auto rns =
        std::dynamic_pointer_cast<lbcrypto::CryptoParametersRNS>(cc->GetCryptoParameters());
    REQUIRE(rns);
    const std::size_t num_part_q = rns->GetNumPartQ();
    REQUIRE(num_part_q > 0);

    REQUIRE(limbs.a_limbs.size() == num_part_q);
    REQUIRE(limbs.b_limbs.size() == num_part_q);
    REQUIRE(limbs.q_base == q_base_of(cc));
    REQUIRE(limbs.p_base == p_base_of(cc));

    const std::size_t qp_towers = limbs.q_base.size() + limbs.p_base.size();
    const std::size_t ring_dim = cc->GetRingDimension();
    for (std::size_t part = 0; part < num_part_q; ++part) {
        INFO("partition " << part);
        REQUIRE(limbs.a_limbs[part].size() == qp_towers);
        REQUIRE(limbs.b_limbs[part].size() == qp_towers);
        for (std::size_t t = 0; t < qp_towers; ++t) {
            REQUIRE(limbs.a_limbs[part][t].size() == ring_dim);
            REQUIRE(limbs.b_limbs[part][t].size() == ring_dim);
        }
    }
}

} // namespace

TEST_CASE("bridge: ExtractEvalMultKey returns Q∥P limbs matching the CC", "[integration]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto cc = build_cc();
    auto keys = cc->KeyGen();
    cc->EvalMultKeyGen(keys.secretKey);

    haze::HybridKeyswitchLimbs limbs;
    REQUIRE(haze::hazeReplayBridgeExtractEvalMultKey(cc, keys.secretKey, limbs) == HAZE_SUCCESS);
    check_limb_shape(limbs, cc);
}

TEST_CASE("bridge: ExtractEvalMultKey fails without prior EvalMultKeyGen", "[integration]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto cc = build_cc();
    auto keys = cc->KeyGen();

    haze::HybridKeyswitchLimbs limbs;
    REQUIRE(haze::hazeReplayBridgeExtractEvalMultKey(cc, keys.secretKey, limbs) != HAZE_SUCCESS);
    REQUIRE(limbs.a_limbs.empty());
    REQUIRE(limbs.b_limbs.empty());
}

TEST_CASE("bridge: ExtractAutomorphismKey returns limbs for each registered slot",
          "[integration]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto cc = build_cc();
    auto keys = cc->KeyGen();

    const std::vector<int32_t> slots = {1, -2};
    cc->EvalAtIndexKeyGen(keys.secretKey, slots);

    for (int32_t slot : slots) {
        INFO("slot " << slot);
        const uint32_t auto_index = cc->FindAutomorphismIndex(static_cast<uint32_t>(slot));
        haze::HybridKeyswitchLimbs limbs;
        REQUIRE(haze::hazeReplayBridgeExtractAutomorphismKey(cc, keys.secretKey, auto_index,
                                                             limbs) == HAZE_SUCCESS);
        check_limb_shape(limbs, cc);
    }
}

TEST_CASE("bridge: ExtractAutomorphismKey fails for unknown automorphism index", "[integration]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto cc = build_cc();
    auto keys = cc->KeyGen();

    haze::HybridKeyswitchLimbs limbs;
    REQUIRE(haze::hazeReplayBridgeExtractAutomorphismKey(cc, keys.secretKey, /*auto_index=*/1,
                                                         limbs) != HAZE_SUCCESS);
}
