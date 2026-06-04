// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Test-side, version-neutral extraction of OpenFHE HYBRID keyswitch keys
// (relin + automorphism) into raw uint64 limbs. Uses only public OpenFHE
// accessors, so it compiles against BOTH the niobium-instrumented OpenFHE
// (haze_internal_tests) and a stock upstream OpenFHE (haze_e2e_tests).
//
// Moved out of replay_bridge/src/openfhe_template.cpp's deleted lbcrypto-typed
// bridge so the test -> haze boundary is limbs + scalars only: the test builds
// its own CryptoContext/keys, extracts the key limbs here, and replays them
// through haze's MRP C ABI (see test/e2e/ops.cpp::hybrid_keyswitch). haze never
// holds the key, so no haze C ABI is needed for it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <haze/haze_types.h>
#include <memory>
#include <openfhe.h>
#include <vector>

namespace haze::test {

// Hybrid-keyswitch key limbs over Q || P, in OpenFHE's EVALUATION format.
// Used for both EvalMult (relin) and EvalAutomorphism (rotation) keys -- both
// share the aVector/bVector layout. Partition layout is implicit (FIDESlib's
// RawKeySwitchKey convention): numPartQ = a_limbs.size(),
// alpha = (|Q| + numPartQ - 1) / numPartQ, partition i covers
// q_base[i*alpha .. min((i+1)*alpha, |Q|)).
struct HybridKeyswitchLimbs {
    // [part][tower][coeff]; tower covers q_base then p_base.
    std::vector<std::vector<std::vector<uint64_t>>> a_limbs;
    std::vector<std::vector<std::vector<uint64_t>>> b_limbs;

    std::vector<uint64_t> q_base;
    std::vector<uint64_t> p_base;
};

namespace detail {

// Per-tower uint64 limbs of a DCRTPoly. OpenFHE keyswitch keys are already in
// EVALUATION format. Builds into a local, moves on success.
inline bool extract_dcrtpoly_limbs(const lbcrypto::DCRTPoly &poly, std::size_t expected_towers,
                                   std::size_t ring_dim, std::vector<std::vector<uint64_t>> &out) {
    if (poly.GetNumOfElements() != expected_towers)
        return false;
    std::vector<std::vector<uint64_t>> tmp(expected_towers, std::vector<uint64_t>(ring_dim));
    for (std::size_t t = 0; t < expected_towers; ++t) {
        const auto &np = poly.GetElementAtIndex(static_cast<uint32_t>(t));
        const auto &vals = np.GetValues();
        if (vals.GetLength() != ring_dim)
            return false;
        for (std::size_t i = 0; i < ring_dim; ++i) {
            tmp[t][i] = vals[i].template ConvertToInt<uint64_t>();
        }
    }
    out = std::move(tmp);
    return true;
}

// Walk an EvalKey (relin or rotation) into a HybridKeyswitchLimbs. `out` is
// overwritten only on HAZE_SUCCESS.
inline hazeError_t extract_keyswitch_key_into(
    const lbcrypto::CryptoContext<lbcrypto::DCRTPoly> &cc,
    const std::shared_ptr<lbcrypto::EvalKeyImpl<lbcrypto::DCRTPoly>> &eval_key,
    HybridKeyswitchLimbs &out) noexcept {
    try {
        const auto crypto_params =
            std::dynamic_pointer_cast<lbcrypto::CryptoParametersRNS>(cc->GetCryptoParameters());
        if (!crypto_params)
            return HAZE_ERROR_INVALID_VALUE;
        if (crypto_params->GetKeySwitchTechnique() != lbcrypto::HYBRID)
            return HAZE_ERROR_NOT_SUPPORTED;
        if (!eval_key)
            return HAZE_ERROR_INVALID_VALUE;
        const auto element_params = crypto_params->GetElementParams();
        const auto params_p = crypto_params->GetParamsP();
        if (!element_params || !params_p)
            return HAZE_ERROR_INVALID_VALUE;
        const std::size_t ring_dim = element_params->GetRingDimension();
        if (ring_dim == 0)
            return HAZE_ERROR_INVALID_VALUE;
        const std::uint32_t num_part_q = crypto_params->GetNumPartQ();
        if (num_part_q == 0)
            return HAZE_ERROR_INVALID_VALUE;
        const auto &a_vec = eval_key->GetAVector();
        const auto &b_vec = eval_key->GetBVector();
        if (a_vec.size() != num_part_q || b_vec.size() != num_part_q)
            return HAZE_ERROR_INVALID_VALUE;

        // Build into a local; move into `out` only on full success.
        HybridKeyswitchLimbs tmp;
        const auto &q_params = element_params->GetParams();
        const auto &p_params = params_p->GetParams();
        tmp.q_base.reserve(q_params.size());
        for (const auto &p : q_params)
            tmp.q_base.push_back(p->GetModulus().template ConvertToInt<uint64_t>());
        tmp.p_base.reserve(p_params.size());
        for (const auto &p : p_params)
            tmp.p_base.push_back(p->GetModulus().template ConvertToInt<uint64_t>());

        const std::size_t qp_towers = tmp.q_base.size() + tmp.p_base.size();
        tmp.a_limbs.resize(num_part_q);
        tmp.b_limbs.resize(num_part_q);
        for (std::uint32_t part = 0; part < num_part_q; ++part) {
            if (!extract_dcrtpoly_limbs(a_vec[part], qp_towers, ring_dim, tmp.a_limbs[part]))
                return HAZE_ERROR_INVALID_VALUE;
            if (!extract_dcrtpoly_limbs(b_vec[part], qp_towers, ring_dim, tmp.b_limbs[part]))
                return HAZE_ERROR_INVALID_VALUE;
        }
        out = std::move(tmp);
        return HAZE_SUCCESS;
    } catch (...) {
        return HAZE_ERROR_INTERNAL;
    }
}

} // namespace detail

// Requires cc->EvalMultKeyGen(secretKey) and HYBRID keyswitch. `out` is
// overwritten only on HAZE_SUCCESS.
inline hazeError_t
extract_evalmult_key_limbs(const lbcrypto::CryptoContext<lbcrypto::DCRTPoly> &cc,
                           const lbcrypto::PrivateKey<lbcrypto::DCRTPoly> &secretKey,
                           HybridKeyswitchLimbs &out) noexcept {
    if (!cc || !secretKey)
        return HAZE_ERROR_INVALID_VALUE;
    try {
        // GetEvalMultKeyVector throws when no key is registered for this tag.
        const auto &eval_keys =
            lbcrypto::CryptoContextImpl<lbcrypto::DCRTPoly>::GetEvalMultKeyVector(
                secretKey->GetKeyTag());
        if (eval_keys.empty())
            return HAZE_ERROR_INVALID_VALUE;
        return detail::extract_keyswitch_key_into(cc, eval_keys[0], out);
    } catch (...) {
        return HAZE_ERROR_INVALID_VALUE;
    }
}

// Requires cc->EvalAtIndexKeyGen for `auto_index` and HYBRID keyswitch. `out`
// is overwritten only on HAZE_SUCCESS.
inline hazeError_t
extract_automorphism_key_limbs(const lbcrypto::CryptoContext<lbcrypto::DCRTPoly> &cc,
                               const lbcrypto::PrivateKey<lbcrypto::DCRTPoly> &secretKey,
                               std::uint32_t auto_index, HybridKeyswitchLimbs &out) noexcept {
    if (!cc || !secretKey)
        return HAZE_ERROR_INVALID_VALUE;
    try {
        const auto key_map_ptr =
            lbcrypto::CryptoContextImpl<lbcrypto::DCRTPoly>::GetEvalAutomorphismKeyMapPtr(
                secretKey->GetKeyTag());
        if (!key_map_ptr)
            return HAZE_ERROR_INVALID_VALUE;
        const auto it = key_map_ptr->find(auto_index);
        if (it == key_map_ptr->end())
            return HAZE_ERROR_INVALID_VALUE;
        return detail::extract_keyswitch_key_into(cc, it->second, out);
    } catch (...) {
        return HAZE_ERROR_INTERNAL;
    }
}

} // namespace haze::test
