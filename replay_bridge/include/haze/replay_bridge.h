// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
// The contents of this file and all related materials provided herein (the
// "Product") may not be used except pursuant to a separate written
// agreement signed by a duly authorized officer of Niobium Microsystems,
// Inc. (a "License Agreement").
//
// haze_replay_bridge — OpenFHE boundary for haze: synthesizes cryptocontext.dat
// and per-output ciphertext_templates/<name>.template that the replay path
// (in-process simulator or HTTP transport) consumes to emit serialized_probes/
// <name>.ct. libhaze stays FHETCH-only and links the bridge for
// niobium::fhetch::result(...) resolution.

#ifndef HAZE_REPLAY_BRIDGE_H
#define HAZE_REPLAY_BRIDGE_H

#include <haze/haze_types.h>
// replay_bridge.h is a C-compatible public header (consumed by both
// C and C++ TUs); the C-style stdint.h form must stay.
// NOLINTNEXTLINE(modernize-deprecated-headers)
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Build a CKKS CryptoContext for (ring_dim, desired_modulus) and write
/// cryptocontext.dat. `desired_modulus` only seeds OpenFHE's scaffold
/// construction; the scaffold prime it picks is overwritten from the trace at
/// synthesis and never consulted for results — callers set the authoritative
/// primes in hazeFheParams::moduli, and this call does not report the
/// scaffold value back. Requires hazeConfigureDevice() first, and must be
/// re-called after every hazeDeviceReset (setup_integration_compute_config
/// enforces this; the post-recording hook is cleared by reset).
HAZE_API hazeError_t hazeReplayBridgeInitCryptoContext(uint64_t ring_dim,
                                                       uint64_t desired_modulus) HAZE_NOEXCEPT;

/// Drop the bridge's cached CryptoContexts so they don't outlive a
/// hazeDeviceReset, which calls this: releases every CryptoContext registered in
/// haze's own OpenFHE (which also clears that copy's static key maps and NTT
/// tables, rebuilt on demand) and clears the hook's error flag. The installed
/// post-recording hook stays, so a later flush still synthesizes its inputs.
/// Idempotent and safe to call before init. Control plane, like hazeDeviceReset:
/// it takes no lock, so it must not run concurrently with compute or a flush.
/// libhaze.so absorbs its OpenFHE with every symbol hidden, so a caller's own
/// OpenFHE contexts are untouched.
HAZE_API void hazeReplayBridgeReset(void) HAZE_NOEXCEPT;

/// Return 1 iff the post-recording hook reported any failure since the
/// last call, then reset the flag. Per-failure detail is in the log.
HAZE_API int hazeReplayBridgeTakeHookHadError(void) HAZE_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#endif // HAZE_REPLAY_BRIDGE_H
