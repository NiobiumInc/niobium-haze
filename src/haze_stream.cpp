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

extern "C" hazeError_t hazeStreamCreate(hazeStream_t* stream) noexcept {
    // Task-00 stub: writes nullptr and reports success so tests can link.
    // Task-01 replaces this with a real allocation that writes a valid
    // handle, matching CUDA's cudaStreamCreate contract (allocate + return).
    if (stream) *stream = nullptr;
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeStreamCreateWithPriority(hazeStream_t* stream,
                                                      unsigned int /*flags*/,
                                                      int /*priority*/) noexcept {
    if (stream) *stream = nullptr;
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeStreamDestroy(hazeStream_t /*stream*/) noexcept {
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeStreamSynchronize(hazeStream_t /*stream*/) noexcept {
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeStreamWaitEvent(hazeStream_t /*stream*/,
                                             hazeEvent_t /*event*/,
                                             unsigned int /*flags*/) noexcept {
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeEventCreate(hazeEvent_t* event) noexcept {
    if (event) *event = nullptr;
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeEventCreateWithFlags(hazeEvent_t* event,
                                                  unsigned int /*flags*/) noexcept {
    if (event) *event = nullptr;
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeEventDestroy(hazeEvent_t /*event*/) noexcept {
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeEventRecord(hazeEvent_t /*event*/,
                                        hazeStream_t /*stream*/) noexcept {
    return HAZE_SUCCESS;
}
