// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
// The contents of this file and all related materials provided herein (the
// "Product") may not be used except pursuant to a separate written
// agreement signed by a duly authorized officer of Niobium Microsystems,
// Inc. (a "License Agreement").
//
// haze_replay_bridge — OpenFHE-using helper that synthesizes the artifacts
// downstream replay paths need to produce serialized_probes/ from a haze
// recording. Specifically:
//
//   1. <program_dir>/cryptocontext.dat
//      A minimal CKKS CryptoContext<DCRTPoly> matching haze's configured
//      ring dimension and (single) modulus.
//
//   2. <program_dir>/ciphertext_templates/<name>.template
//      An empty Ciphertext<DCRTPoly> shell. The replay path fills it with
//      simulator-computed polynomial values and re-serializes to
//      <program_dir>/serialized_probes/<name>.ct.
//
// Same artifact shapes are consumed by both the in-process simulator
// path (HAZE_TARGET=local, libnbfhetch fills + serializes the
// templates) AND the HTTP transport path (HAZE_TARGET=FUNC_SIM etc.,
// nbcc_fhetch_replay does the same). The bridge is therefore agnostic
// to which replay tier the caller selects.
//
// The bridge is the OpenFHE boundary for haze: libhaze sources stay
// FHETCH-only (no OpenFHE includes), but libhaze links the bridge to
// resolve niobium::fhetch::result(...) references from epoch.cpp.
//
// Usage (in an integration test):
//
//     // Once per program directory: build CC, learn the picked modulus.
//     uint64_t picked = 0;
//     hazeReplayBridgeInitCryptoContext(kRingDim, kDesiredModulus, &picked);
//     hazeSetCiphertextModulus(kModIdx, picked);
//
//     // Per-input .bin / .ids and per-output .template files are written
//     // automatically inside hazeReplay() via a post-recording hook the
//     // bridge registers during InitCryptoContext. Tests just compute and
//     // call hazeReplay().
//     hazeReplay();   // in-process simulator or transport, both work

#ifndef HAZE_REPLAY_BRIDGE_H
#define HAZE_REPLAY_BRIDGE_H

#include <haze/haze_types.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Build a single-limb CKKS CryptoContext targeting the given ring dimension
/// and modulus, and serialize it to <program_dir>/cryptocontext.dat.
///
/// OpenFHE chooses the actual tower modulus from primes near 2^bits(modulus),
/// so the picked modulus may differ from the requested one. The picked value
/// is returned via `picked_modulus` (must be non-null) so callers can align
/// haze's configured modulus with what the .ct files will use on round-trip
/// (call hazeSetCiphertextModulus with this value).
///
/// Subsequent calls with the same (ring_dim, desired_modulus) reuse the
/// cached CryptoContext. Calls with different parameters rebuild it; the
/// previously serialized cryptocontext.dat is overwritten.
///
/// niobium::compiler() must already be initialized (i.e. at least one haze
/// compute call must have happened, or hazeConfigureDevice / hazeReplay
/// must have been called) so the program directory resolves correctly.
///
/// Must be called AFTER every hazeDeviceReset(): reset clears the post-
/// recording hook this function registers, and without re-registration
/// hazeReplay() ships an incomplete project (no .bin / .ids / .template
/// files), which the compiler-side rejects. The setup_integration_compute_config
/// helper enforces this ordering automatically.
HAZE_API hazeError_t hazeReplayBridgeInitCryptoContext(
    uint64_t  ring_dim,
    uint64_t  desired_modulus,
    uint64_t* picked_modulus) HAZE_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#endif  // HAZE_REPLAY_BRIDGE_H
