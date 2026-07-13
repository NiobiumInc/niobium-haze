// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
// The contents of this file and all related materials provided herein (the
// "Product") may not be used except pursuant to a separate written
// agreement signed by a duly authorized officer of Niobium Microsystems,
// Inc. (a "License Agreement").
// Without limiting the foregoing, you may not, at any time or for any
// reason, directly or indirectly, in whole or in part: (i) copy, modify,
// or create derivative works of the Product; (ii) rent, lease, lend, sell,
// sublicense, assign, distribute, publish, transfer, or otherwise make
// available the Product; (iii) reverse engineer, disassemble, decompile,
// decode, or adapt the Product; or (iv) remove any proprietary notices
// from the Product.
#ifndef HAZE_H
#define HAZE_H

#include <haze/haze_types.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Device management.
//
// hazeDeviceSynchronize is a no-op returning HAZE_SUCCESS (HAZE records synchronously, so there
// is no background work to wait for); it does NOT execute the recording — call hazeFlush()
// before reading results back.
//
// hazeDeviceReset mirrors cudaDeviceReset: it clears all process-global HAZE state (allocator
// pool, epoch, compiler backend, configuration, streams, events, active device) and the
// thread-local last-error flag.

HAZE_API hazeError_t hazeGetDeviceCount(int *count) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeSetDevice(int device) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeGetDevice(int *device) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeGetDeviceProperties(hazeDeviceProp *prop, int device) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeDeviceSynchronize(void) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeDeviceReset(void) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeDeviceEnablePeerAccess(int peer, unsigned int flags) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeDeviceCanAccessPeer(int *can_access, int device, int peer) HAZE_NOEXCEPT;

// Device memory: allocation, free, host-pinned buffers, pointer attributes.
//
// hazeMalloc returns one FHETCH-addressable polynomial: `size` must equal the configured
// polynomial size (= ring_dim * sizeof(uint64_t)) or HAZE_ERROR_SIZE_MISMATCH is returned, and
// calls before hazeConfigureDevice return HAZE_ERROR_CONFIGERR; non-polynomial host-side
// scratch should use hazeHostAlloc (or ordinary host allocation).
//
// The async variants behave identically to their sync counterparts: `stream` is accepted for
// CUDA-shape parity but intentionally not honoured for ordering, because HAZE records FHETCH IR
// and stream-relative ordering is meaningless until hazeFlush() materializes the program.
//
// hazePointerGetAttributes returns HAZE_SUCCESS for any non-null `attrs`: hazeMalloc /
// hazeMallocAsync pointers report HAZE_MEMORY_TYPE_DEVICE, hazeHostAlloc pointers report
// HAZE_MEMORY_TYPE_HOST, and any other pointer reports HAZE_MEMORY_TYPE_UNREGISTERED, matching
// cudaPointerGetAttributes from CUDA 11 onward.

HAZE_API hazeError_t hazeMalloc(void **ptr, size_t size) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeFree(void *ptr) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeMallocAsync(void **ptr, size_t size, hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeFreeAsync(void *ptr, hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeHostAlloc(void **ptr, size_t size, unsigned int flags) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeFreeHost(void *ptr) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazePointerGetAttributes(hazePointerAttributes *attrs,
                                              const void *ptr) HAZE_NOEXCEPT;

// Data transfer: H2D, D2H, D2D, memset, peer copies.
//
// Outputs are explicit — HAZE records compute lazily and does not infer which results you want
// back: the canonical pattern is compute -> hazeTagOutput(ptr) -> hazeFlush() ->
// hazeMemcpy(D2H), and a D2H of an address that was not tagged-and-flushed returns
// HAZE_ERROR_NOT_FLUSHED. A plain H2D-then-D2H round-trip needs no tag/flush — the uploaded
// bytes are returned as-is. A compute result or D2D copy landing on an address drops any
// previously uploaded bytes there, so D2H errors with HAZE_ERROR_NOT_FLUSHED until tag+flush
// rather than returning stale data.
//
// D2D copies whole polynomials: `count` must equal the polynomial size (partial copies are
// unexpressible in the IR — HAZE_ERROR_INVALID_VALUE; oversized is HAZE_ERROR_SIZE_MISMATCH).
// H2D/D2H accept count <= polynomial size, and a zero `count` (memcpy or memset) is a success
// no-op, per CUDA.

HAZE_API hazeError_t hazeMemcpy(void *dst, const void *src, size_t count,
                                hazeMemcpyKind kind) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeMemcpyAsync(void *dst, const void *src, size_t count, hazeMemcpyKind kind,
                                     hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeMemset(void *dev_ptr, int value, size_t count) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeMemsetAsync(void *dev_ptr, int value, size_t count,
                                     hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeMemcpyPeerAsync(void *dst, int dst_device, const void *src, int src_device,
                                         size_t count, hazeStream_t stream) HAZE_NOEXCEPT;

// Per-residue (MRP) variant of hazeMemcpy: `dst`/`src` are arrays of `base_len` poly pointers
// and `count` is bytes-per-residue, applied to every residue under `kind`. `base` (the
// per-residue primes, see the MRP block below) is consulted only by D2D, which records a
// per-residue pass-through copy under base[i] and registers the dst as an MRP output group.
HAZE_API hazeError_t hazeMemcpyMrp(void *const *dst, const void *const *src, size_t count,
                                   hazeMemcpyKind kind, const uint64_t *base,
                                   size_t base_len) HAZE_NOEXCEPT;

// hazeMallocMrp reserves `num_residues` device polynomials for one MRP group under a single
// allocator lock and writes their addresses into `ptrs[0..num_residues)` (all non-null on
// success); `num_residues` is the group's residue count (the `base_len` passed to the MRP
// compute ops). `size` is bytes per residue and follows hazeMalloc's rules: it must equal the
// configured polynomial size (ring_dim * sizeof(uint64_t)) or HAZE_ERROR_SIZE_MISMATCH is
// returned, and hazeConfigureDevice must precede the first allocation (else
// HAZE_ERROR_CONFIGERR). Like hazeMalloc/hazeFree these are pure allocator operations, never
// recorded into the trace. If the i-th reservation fails, the 0..i-1 already reserved are
// freed and the error returned, leaving `ptrs` unwritten. `num_residues == 0` is a success
// no-op (empty group), but `ptrs == NULL` is checked first, so (NULL, 0) returns
// HAZE_ERROR_INVALID_VALUE; a `num_residues` above the device modulus envelope
// (hazeDeviceProp::maxCiphertextModuli) is rejected the same way.
HAZE_API hazeError_t hazeMallocMrp(void **ptrs, size_t num_residues, size_t size) HAZE_NOEXCEPT;

// hazeFreeMrp frees the MRP group in `ptrs[0..num_residues)` (addresses from hazeMallocMrp or
// any `num_residues` hazeMalloc results) under a single allocator lock: every address is freed
// even if one fails, with the first error returned; NULL entries are skipped, matching
// hazeFree(NULL); freeing a not-currently-allocated address (e.g. a double free) returns
// HAZE_ERROR_UNKNOWN_ADDRESS. `num_residues == 0` is a no-op, but `ptrs == NULL` is checked
// first so (NULL, 0) returns HAZE_ERROR_INVALID_VALUE; a `num_residues` above the device
// modulus envelope is rejected the same way.
HAZE_API hazeError_t hazeFreeMrp(void *const *ptrs, size_t num_residues) HAZE_NOEXCEPT;

// Declare `ptr` an output of the in-flight recording (tagging any one residue of an MRP value
// tags the whole value); HAZE_ERROR_SOURCE_UNAVAILABLE if `ptr` names no recorded value, and a
// later H2D to a tagged address drops the tag. A tag exports the value's final binding and
// shape at flush time: if a later op re-registers the tagged addrs as a different-shaped
// multi-residue value the readback reflects that latest registration, and if a later op claims
// a tagged residue into a different value entirely the original group's multi-residue view is
// dropped (its residues still materialize individually).
HAZE_API hazeError_t hazeTagOutput(void *ptr) HAZE_NOEXCEPT;

// Run the recorded program and populate the tagged outputs' shadow buffers.
// A flush with nothing tagged is a no-op that leaves the in-flight recording
// intact (it is not an epoch reset — use hazeDeviceReset to discard).
HAZE_API hazeError_t hazeFlush(void) HAZE_NOEXCEPT;

/* One-shot device configuration. Fill a hazeFheParams (required) with the ring
 * dimension, ciphertext moduli, and optional twiddle generators, and optionally
 * a hazeReplayConfig with the target, program metadata, and data-format toggles
 * (pass NULL to accept all defaults, including target "local"). Then call this
 * once — the caller owns both structs; haze copies what it needs and retains no
 * pointer, so no configuration state is held before this call.
 *
 * Must run before any compute, hazeMemcpy(H2D), hazeMalloc, or
 * hazeReplayBridgeInitCryptoContext. Validates the whole config (ring dimension
 * in the device envelope; ciphertext moduli non-zero, contiguous, and unique)
 * and returns HAZE_ERROR_CONFIGERR on a bad config or HAZE_ERROR_INVALID_VALUE
 * on a malformed struct (NULL fhe, or a NULL array with a non-zero count);
 * nothing is installed on failure, so a corrected struct may be retried.
 * Re-calling reinstalls the configuration; changing the ring dimension while
 * allocations are live returns HAZE_ERROR_CONFIGERR (free them or reset first).
 *
 * hazeReplayConfig::target selects the niobium-compiler replay target:
 *   1. "local" (the default): libnbfhetch loads the .fhetch trace into its
 *      in-process instruction-set simulator, executes it, and writes ciphertext
 *      probes under <program_dir>/serialized_probes/, so hazeMemcpy(D2H) returns
 *      simulator-computed values with no compiler binary or HTTP transport.
 *   2. "FHE_SIM"/"FUNC_SIM"/"FPGA_TRI"/"fhetch_sim": dispatched over the HTTP
 *      transport (see niobium-client/scripts/fhetch_server.sh), requiring a
 *      built nbcc_fhetch_replay binary on PATH plus a running FHETCH server.
 * Target dispatch happens inside hazeFlush(). The montgomery/bit_reversal
 * toggles record alternate trace representations (Montgomery-form residues,
 * bit-reversed coefficient order); replay decodes outputs back to ordinary form
 * so D2H results are byte-identical. The in-process "local" simulator runs
 * ordinary-form traces only — either toggle on "local" is reported at the first
 * compute as HAZE_ERROR_NOT_SUPPORTED; use a transport target. reduced_noise
 * selects centered FBC matching OpenFHE WITH_REDUCED_NOISE. */
HAZE_API hazeError_t hazeConfigureDevice(const hazeFheParams *fhe,
                                         const hazeReplayConfig *replay) HAZE_NOEXCEPT;

/* Finalize the recording and write the project directory (.fhetch trace, inputs, ciphertext
 * templates, cryptocontext) WITHOUT running replay; only hazeTagOutput()-declared outputs are
 * emitted, so tag them first. Use it to record where replay isn't available locally and replay
 * elsewhere (e.g. `nbcc_fhetch_replay --project=<dir> --target=<device>` on a remote host).
 * No-op when not recording; nothing is materialized in-process, so a later in-process D2H of
 * an output returns HAZE_ERROR_NOT_FLUSHED. */
HAZE_API hazeError_t hazeWriteProgram(void) HAZE_NOEXCEPT;

// Streams: nothing executes until hazeFlush() dispatches the recording, so stream-relative
// ordering is not modelled — hazeStreamSynchronize / hazeStreamWaitEvent are no-ops returning
// HAZE_SUCCESS, and the handle/signature surface exists for CUDA-shape porting parity.

HAZE_API hazeError_t hazeStreamCreate(hazeStream_t *stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeStreamCreateWithPriority(hazeStream_t *stream, unsigned int flags,
                                                  int priority) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeStreamDestroy(hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeStreamSynchronize(hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeStreamWaitEvent(hazeStream_t stream, hazeEvent_t event,
                                         unsigned int flags) HAZE_NOEXCEPT;

// Events: same shape as streams; no-op semantics today.

HAZE_API hazeError_t hazeEventCreate(hazeEvent_t *event) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeEventCreateWithFlags(hazeEvent_t *event, unsigned int flags) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeEventDestroy(hazeEvent_t event) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeEventRecord(hazeEvent_t event, hazeStream_t stream) HAZE_NOEXCEPT;

// Error handling. Mirrors CUDA's last-error pattern.

HAZE_API hazeError_t hazeGetLastError(void) HAZE_NOEXCEPT;
HAZE_NODISCARD HAZE_API const char *hazeGetErrorString(hazeError_t error) HAZE_NOEXCEPT;

// Point-wise polynomial arithmetic and scalar variants.

HAZE_API hazeError_t hazeAdd(void *dst, const void *src1, const void *src2, int mod_idx,
                             hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeSub(void *dst, const void *src1, const void *src2, int mod_idx,
                             hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeMul(void *dst, const void *src1, const void *src2, int mod_idx,
                             hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeAddScalar(void *dst, const void *src, uint64_t scalar, int mod_idx,
                                   hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeSubScalar(void *dst, const void *src, uint64_t scalar, int mod_idx,
                                   hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeMulScalar(void *dst, const void *src, uint64_t scalar, int mod_idx,
                                   hazeStream_t stream) HAZE_NOEXCEPT;

// Number-theoretic transform (NTT) and its inverse.

HAZE_API hazeError_t hazeNTT(void *dst, const void *src, int mod_idx,
                             hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeINTT(void *dst, const void *src, int mod_idx,
                              hazeStream_t stream) HAZE_NOEXCEPT;

// hazeAutomorph(_, k) records the eval-form Galois action f(X) → f(X^k): output slot i reads
// from input slot j where 2j+1 ≡ k·(2i+1) (mod 2N), and specific values of k give CKKS-style
// slot rotations. `index` (= k) must be odd in [1, 2N-1], which makes the action invertible —
// to permute in the opposite direction, pass the multiplicative inverse of k modulo 2N.

HAZE_API hazeError_t hazeAutomorph(void *dst, const void *src, uint64_t index,
                                   hazeStream_t stream) HAZE_NOEXCEPT;

// Multi-residue polynomial (MRP) variants of the pointwise / scalar / NTT / automorph ops
// above: each MRP value spans `base_len` separate haze allocations (one per residue), `dst` /
// `src*` are non-null arrays of `base_len` poly pointers, `base` is a non-null array of
// `base_len` ciphertext-modulus primes (the same primes passed in hazeFheParams::moduli),
// and scalar variants take a parallel `scalars` array with `scalars[i]` reduced (semantically)
// modulo `base[i]`. Each call records a single `mr_*` IR op that fans out to per-residue
// `sr_*` instructions inside niobium-fhetch.
//
// hazeAutomorphMrp applies hazeAutomorph's eval-form Galois action to every residue (see its
// comment above for the `index` convention and inversion rule). hazeRotAutomorphCoeffMrp is a
// distinct IR op (mr_rot_automorph_coeff) with distinct semantics: the negacyclic LEFT shift
// in coefficient form — `offset` in [0, N-1], output[i] reads input at (i+offset) mod N with a
// sign flip on wraparound since X^N = -1, equivalently multiplication by X^{-offset} in R_q.

HAZE_API hazeError_t hazeAddMrp(void *const *dst, const void *const *src1, const void *const *src2,
                                const uint64_t *base, size_t base_len,
                                hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeSubMrp(void *const *dst, const void *const *src1, const void *const *src2,
                                const uint64_t *base, size_t base_len,
                                hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeMulMrp(void *const *dst, const void *const *src1, const void *const *src2,
                                const uint64_t *base, size_t base_len,
                                hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeAddScalarMrp(void *const *dst, const void *const *src,
                                      const uint64_t *scalars, const uint64_t *base,
                                      size_t base_len, hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeSubScalarMrp(void *const *dst, const void *const *src,
                                      const uint64_t *scalars, const uint64_t *base,
                                      size_t base_len, hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeMulScalarMrp(void *const *dst, const void *const *src,
                                      const uint64_t *scalars, const uint64_t *base,
                                      size_t base_len, hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeNTTMrp(void *const *dst, const void *const *src, const uint64_t *base,
                                size_t base_len, hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeINTTMrp(void *const *dst, const void *const *src, const uint64_t *base,
                                 size_t base_len, hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeAutomorphMrp(void *const *dst, const void *const *src, uint64_t index,
                                      const uint64_t *base, size_t base_len,
                                      hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeRotAutomorphCoeffMrp(void *const *dst, const void *const *src,
                                              uint64_t offset, const uint64_t *base,
                                              size_t base_len, hazeStream_t stream) HAZE_NOEXCEPT;

// CRT basis conversion (composite operations: ModUp, ModDown, generalised basis convert).
// Each operates on multi-residue polynomials whose component single-residue polynomials live
// in separate HAZE allocations: `src` is a non-null array of `src_base_len` input poly
// pointers, `dst` a non-null array of output poly pointers (length per the matching params
// struct's doc), and `params` carries the moduli bases and other scalar metadata only — never
// polynomial pointers.

HAZE_API hazeError_t hazeBasisConvert(void *const *dst, const void *const *src, const void *params,
                                      hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeModDown(void *const *dst, const void *const *src, const void *params,
                                 hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeModUp(void *const *dst, const void *const *src, const void *params,
                               hazeStream_t stream) HAZE_NOEXCEPT;

// Graph recording and execution mirror CUDA's graph API; every entry currently returns
// HAZE_ERROR_NOT_SUPPORTED.

HAZE_API hazeError_t hazeStreamBeginCapture(hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeStreamEndCapture(hazeStream_t stream, hazeGraph_t *graph) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeGraphInstantiate(hazeGraphExec_t *exec, hazeGraph_t graph) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeGraphLaunch(hazeGraphExec_t exec, hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeGraphExecUpdate(hazeGraphExec_t exec, hazeGraph_t graph) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeGraphExecDestroy(hazeGraphExec_t exec) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeGraphDestroy(hazeGraph_t graph) HAZE_NOEXCEPT;

// Profiling / multi-device stubs.

HAZE_API hazeError_t hazeGetPerformanceCounters(void *counters) HAZE_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#endif /* HAZE_H */
