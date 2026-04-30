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
// hazeDeviceSynchronize is a no-op today. CUDA blocks until the device
// is idle; HAZE has no equivalent notion until hazeMemcpy(D2H) flushes
// a recording. Returns HAZE_SUCCESS so calling code structured as
// "compute -> sync -> D2H" continues to work; the sync is implicit in
// the D2H itself.
//
// hazeDeviceReset clears all process-global HAZE state (allocator pool,
// epoch, compiler backend, configuration, streams, events, active
// device) AND the thread-local last-error flag. Mirrors cudaDeviceReset.

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
// hazeMalloc returns one FHETCH-addressable polynomial. `size` must equal
// the configured polynomial size (= ring_dim * sizeof(uint64_t)). Calls
// before hazeSetRingDimension return HAZE_ERROR_CONFIGERR; calls with a
// size that does not match the configured polynomial size return
// HAZE_ERROR_INVALID_VALUE. Non-polynomial host-side scratch should use
// hazeHostAlloc (or ordinary host allocation).
//
// Async variants (hazeMallocAsync, hazeFreeAsync, hazeMemcpyAsync,
// hazeMemsetAsync, hazeMemcpyPeerAsync). The `stream` parameter is
// accepted for CUDA-shape parity but is intentionally not honoured for
// ordering: HAZE is a recording layer that emits FHETCH IR, not an
// execution engine. Stream-relative ordering is meaningless until a
// hazeMemcpy(D2H) materializes the recorded program. The async entries
// behave identically to their sync counterparts.
//
// hazePointerGetAttributes returns HAZE_SUCCESS for any non-null
// `attrs` argument. Pointers obtained from hazeMalloc / hazeMallocAsync
// report HAZE_MEMORY_TYPE_DEVICE; pointers from hazeHostAlloc report
// HAZE_MEMORY_TYPE_HOST; any other pointer (stack, heap from a foreign
// allocator, etc.) reports HAZE_MEMORY_TYPE_UNREGISTERED — matches
// cudaPointerGetAttributes' behaviour from CUDA 11 onward.

HAZE_API hazeError_t hazeMalloc(void **ptr, size_t size) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeFree(void *ptr) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeMallocAsync(void **ptr, size_t size, hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeFreeAsync(void *ptr, hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeHostAlloc(void **ptr, size_t size, unsigned int flags) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeFreeHost(void *ptr) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazePointerGetAttributes(hazePointerAttributes *attrs,
                                              const void *ptr) HAZE_NOEXCEPT;

// Data transfer: H2D, D2H, D2D, memset, peer copies.

HAZE_API hazeError_t hazeMemcpy(void *dst, const void *src, size_t count,
                                hazeMemcpyKind kind) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeMemcpyAsync(void *dst, const void *src, size_t count, hazeMemcpyKind kind,
                                     hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeMemset(void *dev_ptr, int value, size_t count) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeMemsetAsync(void *dev_ptr, int value, size_t count,
                                     hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeMemcpyPeerAsync(void *dst, int dst_device, const void *src, int src_device,
                                         size_t count, hazeStream_t stream) HAZE_NOEXCEPT;

// Device configuration: ring dimension, ciphertext moduli, twiddle factors,
// program metadata, target selection, lifecycle reset.

HAZE_API hazeError_t hazeSetRingDimension(uint64_t n) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeSetCiphertextModulus(int index, uint64_t modulus) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeSetTwiddleFactors(int index, uint64_t generator) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeConfigureDevice(void) HAZE_NOEXCEPT;

/* Set program metadata recorded in the FHETCH trace produced by HAZE.
 * Defaults: name="haze", version="0.1", description="HAZE runtime".
 * Must be called before the first compute call to take effect. */
HAZE_API hazeError_t hazeSetProgramInfo(const char *name, const char *version,
                                        const char *description) HAZE_NOEXCEPT;

/* Select the niobium-compiler target for replay.
 *
 * Targets fall into two tiers by where they execute:
 *
 *   1. In-process FHETCH simulator (DEFAULT):
 *        "local"        libnbfhetch loads the .fhetch trace into its
 *                       in-process instruction-set simulator,
 *                       executes it, and writes ciphertext probes
 *                       under <program_dir>/serialized_probes/.
 *                       hazeMemcpy(D2H) returns simulator-computed
 *                       values. No compiler-side binary or HTTP
 *                       transport required.
 *
 *   2. Compiler-side simulators via HTTP transport:
 *        "FHE_SIM"      OpenFHE simulator via nbcc_fhetch_replay.
 *        "FUNC_SIM"     Functional simulator.
 *        "FPGA_TRI"     FPGA target.
 *        "fhetch_sim"   FHETCH instruction-set simulator on the
 *                       compiler side.
 *                       These dispatch the recorded project over the
 *                       HTTP transport (see niobium-client/scripts/
 *                       fhetch_server.sh) and require a built
 *                       nbcc_fhetch_replay binary on PATH plus a
 *                       running FHETCH server.
 *
 * Resolution order:
 *   1. hazeSetTarget(target)  - explicit programmatic call.
 *   2. HAZE_TARGET env var    - read on first hazeReplay if (1) unset.
 *   3. "local"                - default if both above are unset.
 *
 * See hazeReplay() for behaviour-by-target dispatch. */
HAZE_API hazeError_t hazeSetTarget(const char *target) HAZE_NOEXCEPT;

/* Trigger replay of the recorded operations.
 *
 * Finalises the current epoch's .fhetch trace and dispatches it
 * according to the configured target (see hazeSetTarget for the
 * two-tier table and resolution order). At a glance:
 *
 *   target == "local":   in-process simulator populates shadows.
 *   target == <other>:   HTTP transport to nbcc_fhetch_replay.
 *
 * Calling hazeReplay() with no recording in progress is a no-op
 * returning HAZE_SUCCESS. */
HAZE_API hazeError_t hazeReplay(void) HAZE_NOEXCEPT;

// Streams: lifecycle and ordering primitives. HAZE is a recording layer
// that emits FHETCH IR; nothing executes until hazeMemcpy(D2H) flushes
// the recording. Stream-relative ordering is therefore not modelled,
// and hazeStreamSynchronize / hazeStreamWaitEvent are no-ops returning
// HAZE_SUCCESS. The handle and signature surface is preserved for
// CUDA-shape porting parity.

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

// Automorphism / rotation.

HAZE_API hazeError_t hazeAutomorph(void *dst, const void *src, uint64_t index,
                                   hazeStream_t stream) HAZE_NOEXCEPT;

// CRT basis conversion (composite operations: ModUp, ModDown,
// generalised basis convert).
//
// Each operates on multi-residue polynomials whose component
// single-residue polynomials live in separate HAZE allocations. `src`
// is a non-null array of input poly pointers (length defined by the
// matching params field — `src_base_len`); `dst` is a non-null array
// of output poly pointers (length per the params, see each struct's
// doc). `params` carries the moduli bases and other scalar metadata
// only — never polynomial pointers.

HAZE_API hazeError_t hazeBasisConvert(void *const *dst, const void *const *src, const void *params,
                                      hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeModDown(void *const *dst, const void *const *src, const void *params,
                                 hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeModUp(void *const *dst, const void *const *src, const void *params,
                               hazeStream_t stream) HAZE_NOEXCEPT;

// Graph recording and execution. Names mirror CUDA's graph API. All
// entries currently return HAZE_ERROR_NOT_SUPPORTED — graph capture is
// a future task.

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
