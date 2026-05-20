// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
// The contents of this file and all related materials provided herein (the
// "Product") may not be used except pursuant to a separate written
// agreement signed by a duly authorized officer of Niobium Microsystems,
// Inc. (a "License Agreement").
//
// Bridge C++ entry point for callers that build their own CryptoContext
// (FIDESlib, integration tests with a specific scaling technique, etc.).
// libhaze still consumes only the C ABI in replay_bridge.h; this header
// is for downstream C++ TUs that link the bridge directly.

#pragma once

#include <cstddef>
#include <cstdint>
#include <haze/haze_types.h>
#include <openfhe.h>
#include <vector>

namespace haze {

// Register a caller-built CryptoContext for template synthesis; replaces
// any previously-registered CC. Caller aligns the CC's ring_dim with
// hazeSetRingDimension and hazeSetCiphertextModulus(i, q). Must be
// re-called after every hazeDeviceReset.
HAZE_API hazeError_t hazeReplayBridgeRegisterCryptoContext(
    const lbcrypto::CryptoContext<lbcrypto::DCRTPoly> &cc) noexcept;

// Hybrid-keyswitch key limbs over Q∥P, in OpenFHE's EVALUATION format.
// Used for both EvalMult (relin) and EvalAutomorphism (rotation) keys —
// both share aVector/bVector layout. Partition layout is implicit
// (FIDESlib's `RawKeySwitchKey` convention): numPartQ = a_limbs.size(),
// alpha = (|Q| + numPartQ - 1) / numPartQ, partition `i` covers
// `q_base[i*alpha .. min((i+1)*alpha, |Q|))`.
struct HybridKeyswitchLimbs {
    // [part][tower][coeff]. tower covers `q_base` then `p_base`.
    std::vector<std::vector<std::vector<uint64_t>>> a_limbs;
    std::vector<std::vector<std::vector<uint64_t>>> b_limbs;

    std::vector<uint64_t> q_base;
    std::vector<uint64_t> p_base;
};

// Requires cc->EvalMultKeyGen(secretKey) to have been called and HYBRID
// keyswitch. `out` is overwritten only on HAZE_SUCCESS.
HAZE_API hazeError_t hazeReplayBridgeExtractEvalMultKey(
    const lbcrypto::CryptoContext<lbcrypto::DCRTPoly> &cc,
    const lbcrypto::PrivateKey<lbcrypto::DCRTPoly> &secretKey, HybridKeyswitchLimbs &out) noexcept;

// Requires cc->EvalAtIndexKeyGen for `auto_index` to have been called and
// HYBRID keyswitch. `out` is overwritten only on HAZE_SUCCESS.
HAZE_API hazeError_t hazeReplayBridgeExtractAutomorphismKey(
    const lbcrypto::CryptoContext<lbcrypto::DCRTPoly> &cc,
    const lbcrypto::PrivateKey<lbcrypto::DCRTPoly> &secretKey, std::uint32_t auto_index,
    HybridKeyswitchLimbs &out) noexcept;

} // namespace haze
