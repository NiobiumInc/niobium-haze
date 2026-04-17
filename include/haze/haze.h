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

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Section 4.1 — Device management
// ---------------------------------------------------------------------------

HAZE_API hazeError_t hazeGetDeviceCount(int* count) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeSetDevice(int device) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeGetDevice(int* device) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeGetDeviceProperties(hazeDeviceProp* prop, int device) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeDeviceSynchronize(void) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeDeviceEnablePeerAccess(int peer, unsigned int flags) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeDeviceCanAccessPeer(int* can_access, int device, int peer) HAZE_NOEXCEPT;

// ---------------------------------------------------------------------------
// Section 4.2 — Memory management
// ---------------------------------------------------------------------------

HAZE_API hazeError_t hazeMalloc(void** ptr, size_t size) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeFree(void* ptr) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeMallocAsync(void** ptr, size_t size, hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeFreeAsync(void* ptr, hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeHostMalloc(void** ptr, size_t size, unsigned int flags) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeHostFree(void* ptr) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazePointerGetAttributes(hazePointerAttributes* attrs,
                                               const void* ptr) HAZE_NOEXCEPT;

// ---------------------------------------------------------------------------
// Section 4.3 — Data transfer
// ---------------------------------------------------------------------------

HAZE_API hazeError_t hazeMemcpy(void* dst, const void* src, size_t count,
                                 hazeMemcpyKind kind) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeMemcpyAsync(void* dst, const void* src, size_t count,
                                      hazeMemcpyKind kind, hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeMemset(void* dev_ptr, int value, size_t count) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeMemsetAsync(void* dev_ptr, int value, size_t count,
                                      hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeMemcpyPeerAsync(void* dst, int dst_device,
                                          const void* src, int src_device,
                                          size_t count, hazeStream_t stream) HAZE_NOEXCEPT;

// ---------------------------------------------------------------------------
// Section 4.4 — Device configuration
// ---------------------------------------------------------------------------

HAZE_API hazeError_t hazeSetRingDimension(uint64_t n) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeSetCiphertextModulus(int index, uint64_t modulus) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeSetTwiddleFactors(int index, uint64_t generator) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeConfigureDevice(void) HAZE_NOEXCEPT;

// ---------------------------------------------------------------------------
// Section 4.5 — Streams
// ---------------------------------------------------------------------------

HAZE_API hazeError_t hazeStreamCreate(hazeStream_t* stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeStreamCreateWithPriority(hazeStream_t* stream,
                                                   unsigned int flags,
                                                   int priority) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeStreamDestroy(hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeStreamSynchronize(hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeStreamWaitEvent(hazeStream_t stream, hazeEvent_t event,
                                          unsigned int flags) HAZE_NOEXCEPT;

// ---------------------------------------------------------------------------
// Section 4.6 — Events
// ---------------------------------------------------------------------------

HAZE_API hazeError_t hazeEventCreate(hazeEvent_t* event) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeEventCreateWithFlags(hazeEvent_t* event,
                                               unsigned int flags) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeEventDestroy(hazeEvent_t event) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeEventRecord(hazeEvent_t event, hazeStream_t stream) HAZE_NOEXCEPT;

// ---------------------------------------------------------------------------
// Section 4.7 — Error handling
// ---------------------------------------------------------------------------

HAZE_API hazeError_t                  hazeGetLastError(void) HAZE_NOEXCEPT;
HAZE_NODISCARD HAZE_API const char*   hazeGetErrorString(hazeError_t error) HAZE_NOEXCEPT;

// ---------------------------------------------------------------------------
// Section 5.1 — Point-wise arithmetic
// ---------------------------------------------------------------------------

HAZE_API hazeError_t hazeAdd(void* dst, const void* src1, const void* src2,
                              int mod_idx, hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeSub(void* dst, const void* src1, const void* src2,
                              int mod_idx, hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeMul(void* dst, const void* src1, const void* src2,
                              int mod_idx, hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeAddScalar(void* dst, const void* src, uint64_t scalar,
                                    int mod_idx, hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeSubScalar(void* dst, const void* src, uint64_t scalar,
                                    int mod_idx, hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeMulScalar(void* dst, const void* src, uint64_t scalar,
                                    int mod_idx, hazeStream_t stream) HAZE_NOEXCEPT;

// ---------------------------------------------------------------------------
// Section 5.2 — NTT / INTT
// ---------------------------------------------------------------------------

HAZE_API hazeError_t hazeNTT(void* dst, const void* src,
                               int mod_idx, hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeINTT(void* dst, const void* src,
                                int mod_idx, hazeStream_t stream) HAZE_NOEXCEPT;

// ---------------------------------------------------------------------------
// Section 5.3 — Automorphism (rotation)
// ---------------------------------------------------------------------------

HAZE_API hazeError_t hazeAutomorph(void* dst, const void* src, uint64_t index,
                                    hazeStream_t stream) HAZE_NOEXCEPT;

// ---------------------------------------------------------------------------
// Section 5.4 — CRT basis conversion
// ---------------------------------------------------------------------------

HAZE_API hazeError_t hazeBasisConvert(void* dst, const void* src,
                                       const void* params, hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeModDown(void* dst, const void* src,
                                  const void* params, hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeModUp(void* dst, const void* src,
                                const void* params, hazeStream_t stream) HAZE_NOEXCEPT;

// ---------------------------------------------------------------------------
// Section 6 — Graph recording and execution (stubs: HAZE_ERROR_NOT_SUPPORTED)
// ---------------------------------------------------------------------------

HAZE_API hazeError_t hazeBeginCapture(hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeEndCapture(hazeStream_t stream, hazeGraph_t* graph) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeGraphCompile(hazeGraph_t graph, hazeGraphExec_t* exec) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeExecLaunch(hazeGraphExec_t exec, hazeStream_t stream) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeExecUpdate(hazeGraphExec_t exec, hazeGraph_t graph) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeExecDestroy(hazeGraphExec_t exec) HAZE_NOEXCEPT;
HAZE_API hazeError_t hazeGraphDestroy(hazeGraph_t graph) HAZE_NOEXCEPT;

// ---------------------------------------------------------------------------
// Profiling / multi-device stubs
// ---------------------------------------------------------------------------

HAZE_API hazeError_t hazeGetPerformanceCounters(void* counters) HAZE_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#endif /* HAZE_H */
