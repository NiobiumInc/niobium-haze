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
#include "haze_internal.hpp"

// ---------------------------------------------------------------------------
// API conventions — why void* and why a stream parameter
// ---------------------------------------------------------------------------
// Compute functions take device pointers as `void*` to match the CUDA host
// API (cudaMalloc returns void**, kernel arguments on the device side cast
// explicitly). This keeps FIDESlib's existing pointer plumbing portable
// with no type wrapping at the call site.
//
// Every compute function accepts a `hazeStream_t` so call-site syntax
// ports 1:1 from CUDA kernel launches (`kernel<<<g,b,0,stream>>>(args)`).
// HAZE itself treats the stream as a no-op: materialization happens only
// on hazeMemcpy(DeviceToHost), not at stream boundaries. The parameter is
// preserved to leave room for explicit ordering in future hardware modes
// without a public API break.
// ---------------------------------------------------------------------------
// Point-wise arithmetic (Section 5.1)
// ---------------------------------------------------------------------------

extern "C" hazeError_t hazeAdd(void* /*dst*/, const void* /*src1*/,
                                const void* /*src2*/, int /*mod_idx*/,
                                hazeStream_t /*stream*/) noexcept {
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeSub(void* /*dst*/, const void* /*src1*/,
                                const void* /*src2*/, int /*mod_idx*/,
                                hazeStream_t /*stream*/) noexcept {
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeMul(void* /*dst*/, const void* /*src1*/,
                                const void* /*src2*/, int /*mod_idx*/,
                                hazeStream_t /*stream*/) noexcept {
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeAddScalar(void* /*dst*/, const void* /*src*/,
                                      uint64_t /*scalar*/, int /*mod_idx*/,
                                      hazeStream_t /*stream*/) noexcept {
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeSubScalar(void* /*dst*/, const void* /*src*/,
                                      uint64_t /*scalar*/, int /*mod_idx*/,
                                      hazeStream_t /*stream*/) noexcept {
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeMulScalar(void* /*dst*/, const void* /*src*/,
                                      uint64_t /*scalar*/, int /*mod_idx*/,
                                      hazeStream_t /*stream*/) noexcept {
    return HAZE_SUCCESS;
}

// ---------------------------------------------------------------------------
// NTT / INTT (Section 5.2)
// ---------------------------------------------------------------------------

extern "C" hazeError_t hazeNTT(void* /*dst*/, const void* /*src*/,
                                 int /*mod_idx*/,
                                 hazeStream_t /*stream*/) noexcept {
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeINTT(void* /*dst*/, const void* /*src*/,
                                  int /*mod_idx*/,
                                  hazeStream_t /*stream*/) noexcept {
    return HAZE_SUCCESS;
}

// ---------------------------------------------------------------------------
// Automorphism / rotation (Section 5.3)
// ---------------------------------------------------------------------------

extern "C" hazeError_t hazeAutomorph(void* /*dst*/, const void* /*src*/,
                                      uint64_t /*index*/,
                                      hazeStream_t /*stream*/) noexcept {
    return HAZE_SUCCESS;
}

// ---------------------------------------------------------------------------
// CRT basis conversion (Section 5.4)
// ---------------------------------------------------------------------------

extern "C" hazeError_t hazeBasisConvert(void* /*dst*/, const void* /*src*/,
                                         const void* /*params*/,
                                         hazeStream_t /*stream*/) noexcept {
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeModDown(void* /*dst*/, const void* /*src*/,
                                    const void* /*params*/,
                                    hazeStream_t /*stream*/) noexcept {
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeModUp(void* /*dst*/, const void* /*src*/,
                                   const void* /*params*/,
                                   hazeStream_t /*stream*/) noexcept {
    return HAZE_SUCCESS;
}

// ---------------------------------------------------------------------------
// Graph API stubs (Section 6) — not yet supported
// ---------------------------------------------------------------------------

extern "C" hazeError_t hazeBeginCapture(hazeStream_t /*stream*/) noexcept {
    return set_error(HAZE_ERROR_NOT_SUPPORTED);
}

extern "C" hazeError_t hazeEndCapture(hazeStream_t /*stream*/,
                                       hazeGraph_t* graph) noexcept {
    if (graph) *graph = nullptr;
    return set_error(HAZE_ERROR_NOT_SUPPORTED);
}

extern "C" hazeError_t hazeGraphCompile(hazeGraph_t /*graph*/,
                                         hazeGraphExec_t* exec) noexcept {
    if (exec) *exec = nullptr;
    return set_error(HAZE_ERROR_NOT_SUPPORTED);
}

extern "C" hazeError_t hazeExecLaunch(hazeGraphExec_t /*exec*/,
                                       hazeStream_t /*stream*/) noexcept {
    return set_error(HAZE_ERROR_NOT_SUPPORTED);
}

extern "C" hazeError_t hazeExecUpdate(hazeGraphExec_t /*exec*/,
                                       hazeGraph_t /*graph*/) noexcept {
    return set_error(HAZE_ERROR_NOT_SUPPORTED);
}

extern "C" hazeError_t hazeExecDestroy(hazeGraphExec_t /*exec*/) noexcept {
    return set_error(HAZE_ERROR_NOT_SUPPORTED);
}

extern "C" hazeError_t hazeGraphDestroy(hazeGraph_t /*graph*/) noexcept {
    return set_error(HAZE_ERROR_NOT_SUPPORTED);
}
