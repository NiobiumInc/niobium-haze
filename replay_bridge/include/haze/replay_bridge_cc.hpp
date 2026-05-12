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

// Register a caller-built CryptoContext for template synthesis; the bridge
// uses this CC for every captured shape instead of building its own
// per-shape chain. The CC's chain length must be >= the largest residue
// count any output uses (smaller-residue templates are trimmed from this
// chain), and the caller is responsible for aligning the CC's ring_dim
// with hazeSetRingDimension and hazeSetCiphertextModulus(i, q) with the
// primes the chain carries.
//
// Replaces any previously-registered CC (or one built via the C-entry
// hazeReplayBridgeInitCryptoContext). Must be re-called after every
// hazeDeviceReset, which clears the bridge's CC slot and the
// post-recording hook.
//
// No KeyPair argument: the bridge builds CT shells directly via
// CiphertextImpl + SetElements (no Encrypt), so keys are never consulted.
HAZE_API hazeError_t hazeReplayBridgeRegisterCryptoContext(
    const lbcrypto::CryptoContext<lbcrypto::DCRTPoly> &cc) noexcept;

} // namespace haze
