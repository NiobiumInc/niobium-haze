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
// hazeDeviceSynchronize is a no-op returning HAZE_SUCCESS. CUDA blocks until
// async device work drains; HAZE records synchronously and has nothing running
// in the background, so there is nothing to wait for. It does NOT execute the
// recording — run it explicitly with hazeFlush() before reading results back.
//
// hazeDeviceReset clears the state that is still process-global (the
// compiler backend, the fhetch engine, the replay bridge, streams,
// events, active device) AND the thread-local last-error flag. Engine
// state lives in hazeContext_t objects — destroy those individually.
// Mirrors cudaDeviceReset in shape.

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
// hazeMalloc returns one FHETCH-addressable polynomial from the
// context's address space. `size` must equal the context's polynomial
// size (= ring_dim * sizeof(uint64_t)); a mismatch returns
// HAZE_ERROR_INVALID_VALUE. Non-polynomial host-side scratch should use
// hazeHostAlloc (or ordinary host allocation).
//
// Async variants (hazeMallocAsync, hazeFreeAsync, hazeMemcpyAsync,
// hazeMemsetAsync, hazeMemcpyPeerAsync). The `stream` parameter is
// accepted for CUDA-shape parity but is intentionally not honoured for
// ordering: HAZE is a recording layer that emits FHETCH IR, not an
// execution engine. Stream-relative ordering is meaningless until
// hazeFlush() materializes the recorded program. The async entries
// behave identically to their sync counterparts.
//
// hazePointerGetAttributes returns HAZE_SUCCESS for any non-null
// `attrs` argument. Pointers obtained from hazeMalloc / hazeMallocAsync
// report HAZE_MEMORY_TYPE_DEVICE; pointers from hazeHostAlloc report
// HAZE_MEMORY_TYPE_HOST; any other pointer (stack, heap from a foreign
// allocator, etc.) reports HAZE_MEMORY_TYPE_UNREGISTERED — matches
// cudaPointerGetAttributes' behaviour from CUDA 11 onward.

HAZE_API hazeError_t hazeMalloc(hazeContext_t ctx, void **ptr, size_t size) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeFree(hazeContext_t ctx, void *ptr) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeMallocAsync(hazeContext_t ctx, void **ptr, size_t size,
                                     hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeFreeAsync(hazeContext_t ctx, void *ptr, hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeHostAlloc(hazeContext_t ctx, void **ptr, size_t size,
                                   unsigned int flags) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeFreeHost(hazeContext_t ctx, void *ptr) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazePointerGetAttributes(hazeContext_t ctx, hazePointerAttributes *attrs,
                                              const void *ptr) HAZE_NOEXCEPT;

// Data transfer: H2D, D2H, D2D, memset, peer copies.
//
// Output model (explicit): HAZE records compute lazily and does not infer
// which results you want back. The canonical pattern is
// compute -> hazeTagOutput(ptr) -> hazeFlush() -> hazeMemcpy(D2H). A D2H of an
// address that was not tagged-and-flushed returns HAZE_ERROR_NOT_FLUSHED. A
// plain H2D-then-D2H round-trip needs no tag/flush — the uploaded bytes are
// returned as-is.

HAZE_API hazeError_t hazeMemcpy(hazeContext_t ctx, void *dst, const void *src, size_t count,
                                hazeMemcpyKind kind) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeMemcpyAsync(hazeContext_t ctx, void *dst, const void *src, size_t count,
                                     hazeMemcpyKind kind, hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeMemset(hazeContext_t ctx, void *dev_ptr, int value,
                                size_t count) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeMemsetAsync(hazeContext_t ctx, void *dev_ptr, int value, size_t count,
                                     hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeMemcpyPeerAsync(hazeContext_t ctx, void *dst, int dst_device,
                                         const void *src, int src_device, size_t count,
                                         hazeStream_t stream) HAZE_NOEXCEPT;

// Per-residue (MRP) variant of hazeMemcpy: `dst`/`src` are arrays of
// `base_len` poly pointers and `count` is bytes-per-residue, applied to every
// residue under `kind`. `base` (the per-residue primes, see the MRP block
// below) is consulted only by D2D, which records a per-residue pass-through
// copy under base[i] and registers the dst as an MRP output group.
HAZE_API hazeError_t hazeMemcpyMrp(hazeContext_t ctx, void *const *dst, const void *const *src,
                                   size_t count, hazeMemcpyKind kind, const uint64_t *base,
                                   size_t base_len) HAZE_NOEXCEPT;

// Declare `ptr` an output of the in-flight recording (tagging any one residue
// of an MRP value tags the whole value). HAZE_ERROR_SOURCE_UNAVAILABLE if `ptr`
// names no recorded value; a later H2D to a tagged address drops the tag. A
// tag exports the value's final binding and shape at flush time — if a later op
// re-registers the tagged addrs as a different-shaped multi-residue value, the
// readback reflects that latest registration, and if a later op claims a tagged
// residue into a different value entirely, the original group's multi-residue
// view is dropped (its residues still materialize individually).
HAZE_API hazeError_t hazeTagOutput(hazeContext_t ctx, void *ptr) HAZE_NOEXCEPT;

// Run the recorded program and populate the tagged outputs' shadow buffers;
// no-op when nothing is recorded or tagged.
//
// Concurrency contract (CUDA-like). Recording is thread-safe with
// per-thread program order: operations issued by one thread are
// recorded in issue order; operations on DISTINCT buffers may proceed
// concurrently, including across DIFFERENT contexts — recording state
// is per-context and the record path touches no process-global engine.
// Reusing the same device address from two threads without your own
// synchronization is undefined (the recording stays memory-safe, but
// which value wins is unspecified — exactly like racing CUDA kernels on
// one buffer without streams or events).
//
// hazeFlush has two phases. The EMIT phase (sealing the tape and writing
// the .fhetch project) goes through the single process-global FHETCH
// engine and is serialized on one internal lock, so the emits of
// concurrent flushes — same context or different — run one at a time
// (each is ~hundreds of ms). The REPLAY phase (running the recorded
// program and reading results back) is the long pole.
//
// Default: replay runs in-process under that same lock, so flushes
// serialize end to end — simplest and fastest for single-threaded use.
//
// Isolated mode (env HAZE_REPLAY_ISOLATED=1): the long replay runs in a
// fresh worker PROCESS off the lock (fhetch_sim for the local target,
// nbcc_fhetch_replay for transport), so concurrent flushes overlap their
// (potentially minutes-long) replays. Each worker has its own address
// space — and thus its own OpenFHE transform caches — so concurrent
// replays cannot race shared engine state. Reading the workers' results
// back DOES re-take the engine lock (deserializing a ciphertext touches
// OpenFHE's process-global caches), but that is a cheap file read, not
// the long pole. This is safe with NO qualifiers, with ONE caller
// requirement: concurrently-flushed contexts MUST use DISTINCT program
// directories (hazeSetProgramDirectory) so their on-disk projects don't
// collide.
//
// In either mode, hazeFlush racing compute on the addresses being
// flushed (same context) is undefined.
HAZE_API hazeError_t hazeFlush(hazeContext_t ctx) HAZE_NOEXCEPT;

/* Context management. A hazeContext_t is one recording program: its own
 * FHE parameters, device address space, value bindings, tape, and kernel
 * cache. The parameters are immutable for the context's lifetime —
 * there is no piecewise configure step and nothing to mutate after the
 * first compute call.
 *
 * `moduli[i]` is the modulus the SRP compute calls name via mod_idx == i.
 * Destroy invalidates every address allocated from the context; using
 * one afterwards is undefined. Destroying a context with an open
 * hazeKernelBegin bracket or unflushed recording discards them.
 * The process-global pieces fhetch still owns (one trace engine, the
 * replay bridge, streams/events) are scrubbed and re-bound from the
 * flushing context on every hazeFlush, so contexts cannot observe each
 * other's leftovers through them. */
HAZE_API hazeError_t hazeContextCreate(hazeContext_t *ctx, uint64_t ring_dim,
                                       const uint64_t *moduli, size_t n_moduli) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeContextDestroy(hazeContext_t ctx) HAZE_NOEXCEPT;

// Per-context program metadata and target selection. FHE parameters
// (ring dimension, modulus chain) are fixed by hazeContextCreate.

/* Set program metadata recorded in the FHETCH trace produced by HAZE.
 * Defaults: name="haze", version="0.1", description="HAZE runtime".
 * Must be called before the first H2D or compute call to take effect (the
 * first of either brings up the compiler backend). */
HAZE_API hazeError_t hazeSetProgramInfo(hazeContext_t ctx, const char *name, const char *version,
                                        const char *description) HAZE_NOEXCEPT;

/* Override where the recorded project is written (default <cwd>/<program_name>/);
 * `dir` is used verbatim. Must be called before the first H2D or compute call
 * (which brings up the compiler backend). */
HAZE_API hazeError_t hazeSetProgramDirectory(hazeContext_t ctx, const char *dir) HAZE_NOEXCEPT;

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
 *   2. HAZE_TARGET env var    - read at the first hazeFlush() if (1) is unset.
 *   3. "local"                - default if both above are unset.
 *
 * Behaviour-by-target dispatch happens inside hazeFlush(), which finalises the
 * recording, runs the replay, and populates the tagged outputs' shadow buffers;
 * a subsequent hazeMemcpy(D2H) then reads them. */
HAZE_API hazeError_t hazeSetTarget(hazeContext_t ctx, const char *target) HAZE_NOEXCEPT;

/* Data-representation toggles for recorded traces: Montgomery-form RNS
 * residues and bit-reversed coefficient order. Independent opt-ins.
 *
 * When enabled, traces are recorded in the chosen representation (input
 * residues Montgomery-encoded and/or bit-reversed, immediates Montgomery-
 * encoded, basis-convert lowered to its 4-op mod-switch form). Replay decodes
 * outputs back to the ordinary representation, so D2H results are byte-
 * identical to a run with the toggles off — only the recorded form changes.
 *
 * Constraints:
 *   - The in-process "local" simulator runs ordinary-form traces only; the
 *     first compute/flush returns HAZE_ERROR_NOT_SUPPORTED. Use a transport
 *     target.
 *   - Replay supports the ordinary form or both toggles together; enabling
 *     exactly one is recordable (for trace inspection via hazeWriteProgram)
 *     but rejected at replay.
 *
 * Per flag: explicit setter > HAZE_MONTGOMERY / HAZE_BIT_REVERSAL env
 * ("1"/"true") > off. Call before the first H2D or compute. */
HAZE_API hazeError_t hazeSetMontgomery(hazeContext_t ctx, int enable) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeSetBitReversal(hazeContext_t ctx, int enable) HAZE_NOEXCEPT;

/* Finalize the recording and write the project directory (.fhetch trace,
 * inputs, ciphertext templates, cryptocontext) WITHOUT running replay; only
 * hazeTagOutput()-declared outputs are emitted, so tag them first. Use it to
 * record where replay isn't available locally and replay elsewhere (e.g.
 * `nbcc_fhetch_replay --project=<dir> --target=<device>` on a remote host).
 * No-op when not recording; nothing is materialized in-process, so a later
 * in-process D2H of an output returns HAZE_ERROR_NOT_FLUSHED. */
HAZE_API hazeError_t hazeWriteProgram(hazeContext_t ctx) HAZE_NOEXCEPT;

// Streams: lifecycle and ordering primitives. HAZE is a recording layer
// that emits FHETCH IR; nothing executes until hazeFlush() dispatches
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

HAZE_API hazeError_t hazeAdd(hazeContext_t ctx, void *dst, const void *src1, const void *src2,
                             int mod_idx, hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeSub(hazeContext_t ctx, void *dst, const void *src1, const void *src2,
                             int mod_idx, hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeMul(hazeContext_t ctx, void *dst, const void *src1, const void *src2,
                             int mod_idx, hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeAddScalar(hazeContext_t ctx, void *dst, const void *src, uint64_t scalar,
                                   int mod_idx, hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeSubScalar(hazeContext_t ctx, void *dst, const void *src, uint64_t scalar,
                                   int mod_idx, hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeMulScalar(hazeContext_t ctx, void *dst, const void *src, uint64_t scalar,
                                   int mod_idx, hazeStream_t stream) HAZE_NOEXCEPT;

// Number-theoretic transform (NTT) and its inverse.

HAZE_API hazeError_t hazeNTT(hazeContext_t ctx, void *dst, const void *src, int mod_idx,
                             hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeINTT(hazeContext_t ctx, void *dst, const void *src, int mod_idx,
                              hazeStream_t stream) HAZE_NOEXCEPT;

// Automorphism / rotation. hazeAutomorph(_, k) records the eval-form
// Galois action f(X) → f(X^k): in slot terms, output slot i reads from
// input slot j where 2j+1 ≡ k·(2i+1) (mod 2N). Specific values of k give
// CKKS-style slot rotations; in general the action is the permutation
// determined by k.
//
// `index` (= k) must be odd in [1, 2N-1]; the action is then invertible.
// Applying automorph(k) then automorph(k') with k·k' ≡ 1 (mod 2N) yields
// the identity, so to invert the action — i.e. permute in the opposite
// direction — pass the multiplicative inverse of k modulo 2N as `index`.

HAZE_API hazeError_t hazeAutomorph(hazeContext_t ctx, void *dst, const void *src, uint64_t index,
                                   hazeStream_t stream) HAZE_NOEXCEPT;

// Multi-residue polynomial (MRP) variants of the pointwise / scalar / NTT /
// automorph ops above. Each MRP allocation lives across `base_len` separate
// haze allocations (one per residue / prime in `base`). The `dst` and
// `src*` arguments are non-null arrays of `base_len` poly pointers; `base`
// is a non-null array of `base_len` ciphertext-modulus primes (same primes
// passed to hazeSetCiphertextModulus). Scalar variants take a parallel
// `scalars` array of length `base_len` — `scalars[i]` is reduced
// (semantically) modulo `base[i]`.
//
// hazeAutomorphMrp is the same eval-form Galois action as hazeAutomorph
// applied to every residue — see the hazeAutomorph comment above for the
// `index` convention and the inversion rule (pass the multiplicative
// inverse of k modulo 2N to permute the other way).
//
// hazeRotAutomorphCoeffMrp is the negacyclic LEFT shift in coefficient
// form (offset in [0, N-1]; output[i] reads input at (i+offset) mod N,
// with a sign flip on wraparound since X^N = -1). Equivalently,
// multiplication by X^{-offset} in R_q. Distinct semantics, distinct IR
// op (mr_rot_automorph_coeff).
//
// Each call records a single `mr_*` IR op that fans out to per-residue
// `sr_*` instructions inside niobium-fhetch.

HAZE_API hazeError_t hazeAddMrp(hazeContext_t ctx, void *const *dst, const void *const *src1,
                                const void *const *src2, const uint64_t *base, size_t base_len,
                                hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeSubMrp(hazeContext_t ctx, void *const *dst, const void *const *src1,
                                const void *const *src2, const uint64_t *base, size_t base_len,
                                hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeMulMrp(hazeContext_t ctx, void *const *dst, const void *const *src1,
                                const void *const *src2, const uint64_t *base, size_t base_len,
                                hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeAddScalarMrp(hazeContext_t ctx, void *const *dst, const void *const *src,
                                      const uint64_t *scalars, const uint64_t *base,
                                      size_t base_len, hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeSubScalarMrp(hazeContext_t ctx, void *const *dst, const void *const *src,
                                      const uint64_t *scalars, const uint64_t *base,
                                      size_t base_len, hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeMulScalarMrp(hazeContext_t ctx, void *const *dst, const void *const *src,
                                      const uint64_t *scalars, const uint64_t *base,
                                      size_t base_len, hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeNTTMrp(hazeContext_t ctx, void *const *dst, const void *const *src,
                                const uint64_t *base, size_t base_len,
                                hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeINTTMrp(hazeContext_t ctx, void *const *dst, const void *const *src,
                                 const uint64_t *base, size_t base_len,
                                 hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeAutomorphMrp(hazeContext_t ctx, void *const *dst, const void *const *src,
                                      uint64_t index, const uint64_t *base, size_t base_len,
                                      hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeRotAutomorphCoeffMrp(hazeContext_t ctx, void *const *dst,
                                              const void *const *src, uint64_t offset,
                                              const uint64_t *base, size_t base_len,
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

HAZE_API hazeError_t hazeBasisConvert(hazeContext_t ctx, void *const *dst, const void *const *src,
                                      const void *params, hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeModDown(hazeContext_t ctx, void *const *dst, const void *const *src,
                                 const void *params, hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeModUp(hazeContext_t ctx, void *const *dst, const void *const *src,
                               const void *params, hazeStream_t stream) HAZE_NOEXCEPT;

// Kernel memoization. A kernel is a record-time template: bracket a
// deterministic sequence of compute calls with hazeKernelBegin /
// hazeKernelEnd and the bracketed sub-tape is cached under
// (name, key). A later Begin with the same key returns
// HAZE_KERNEL_REPLAY and instantiates the cached sub-tape against the
// new inputs/outputs WITHOUT running the body — O(1) user code per
// repeat call. Rules:
//   - key_hash/key_bytes cover everything the body's recording depends
//     on besides buffer identities (scalars, moduli, shapes). Full key
//     bytes are stored and compared — a hash collision can never alias
//     two kernels.
//   - Closed body: between Begin and End the body may touch ONLY the
//     residues passed as inputs (all of which must already be recorded
//     values) and the declared outputs. Allocation, H2D/D2H, free, and
//     foreign buffers inside a body are rejected.
//   - Outputs are caller-pre-allocated and passed to End on BOTH
//     dispositions; End binds + tags them as recording outputs.
//   - Bodies may WRITE only declared outputs (an in-place update of an
//     input means declaring it as both — In + Out positions, or InOut
//     in the typed layer) and may READ only declared inputs and their
//     own results; violations fail End with
//     HAZE_ERROR_SOURCE_UNAVAILABLE, and the body's bindings are rolled
//     back so continued recording fails loudly rather than silently.
//   - The recording's recovered per-address moduli are baked into the
//     template (e.g. an automorph of an input recovers that input's
//     recorded modulus at RECORD time). Keys must therefore cover the
//     inputs' moduli — the typed layer always does this.
//   - hazeFlush/hazeWriteProgram inside an open bracket fail with
//     HAZE_ERROR_INVALID_VALUE.
//   - Nesting a Begin inside an open kernel returns
//     HAZE_ERROR_NOT_SUPPORTED. Begin/End pairs must not interleave
//     across threads.
//   - hazeSetKernelMemo(0) (or env HAZE_KERNEL_MEMO=0) disables the
//     cache: Begin always records, End only tags — the cold-recording
//     control. hazeSetKernelValidate(1) (or HAZE_KERNEL_VALIDATE=1)
//     re-runs cache-hit bodies and structurally compares against the
//     cached sub-tape, failing End with HAZE_ERROR_KERNEL_VALIDATION on
//     divergence (catches stateful / nondeterministic bodies).
//   - The cache and any open bracket die with their context.

HAZE_API hazeError_t hazeKernelBegin(hazeContext_t ctx, const char *name, uint64_t key_hash,
                                     const uint8_t *key_bytes, size_t key_bytes_len,
                                     const hazeKernelInput *inputs, size_t n_inputs,
                                     hazeKernelDisposition *disposition) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeKernelEnd(hazeContext_t ctx, const hazeKernelOutput *outputs,
                                   size_t n_outputs) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeKernelAbort(hazeContext_t ctx) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeSetKernelMemo(hazeContext_t ctx, int enable) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeSetKernelValidate(hazeContext_t ctx, int enable) HAZE_NOEXCEPT;

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
