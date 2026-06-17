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

/// Build a CKKS CryptoContext for (ring_dim, desired_modulus), write
/// cryptocontext.dat, and return the OpenFHE-picked modulus via
/// `picked_modulus` (non-null) so callers can align via hazeSetCiphertextModulus.
/// Must be re-called after every hazeDeviceReset (setup_integration_compute_config
/// enforces this; the post-recording hook is cleared by reset).
HAZE_API hazeError_t hazeReplayBridgeInitCryptoContext(uint64_t ring_dim, uint64_t desired_modulus,
                                                       uint64_t *picked_modulus) HAZE_NOEXCEPT;

/// Drop the bridge's cached CryptoContexts so they don't outlive a
/// hazeDeviceReset; idempotent and safe to call before init.
HAZE_API void hazeReplayBridgeReset(void) HAZE_NOEXCEPT;

/// Re-install the post-recording hook and re-capture the bridge's
/// CryptoContext into the (freshly scrubbed) compiler singleton. The
/// per-flush engine scrub calls niobium::compiler().reset(), which
/// drops the hook InitCryptoContext installed; this puts it back from
/// the bridge's stored state. No-op success when InitCryptoContext has
/// not been called (flushing without the bridge is legal — MRP outputs
/// then synthesize no templates, exactly as before init).
HAZE_API hazeError_t hazeReplayBridgeReinstallHook(void) HAZE_NOEXCEPT;

/// Return 1 iff the post-recording hook reported any failure since the
/// last call, then reset the flag. Per-failure detail is in the log.
HAZE_API int hazeReplayBridgeTakeHookHadError(void) HAZE_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#endif // HAZE_REPLAY_BRIDGE_H
