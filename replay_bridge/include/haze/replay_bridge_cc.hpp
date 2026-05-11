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

#include <haze/haze_types.h>
#include <openfhe.h>

namespace haze {

// Register a caller-built CryptoContext + KeyPair for template synthesis;
// the bridge uses these instead of building its own. Replaces
// hazeReplayBridgeInitCryptoContext when the caller needs control over
// scaling technique, chain length, or other CC parameters.
//
// The CC's chain length must be >= the largest residue count any output
// uses; smaller-residue templates are produced by trimming towers from a
// fresh encryption. Callers are responsible for aligning
// hazeSetCiphertextModulus(i, q) with the CC's chain.
HAZE_API hazeError_t
hazeReplayBridgeRegisterCryptoContext(const lbcrypto::CryptoContext<lbcrypto::DCRTPoly> &cc,
                                      const lbcrypto::KeyPair<lbcrypto::DCRTPoly> &keys) noexcept;

} // namespace haze
