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

extern "C" hazeError_t hazeMalloc(void** ptr, size_t /*size*/) noexcept {
    if (ptr) *ptr = nullptr;
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeFree(void* /*ptr*/) noexcept {
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeMallocAsync(void** ptr, size_t /*size*/,
                                        hazeStream_t /*stream*/) noexcept {
    if (ptr) *ptr = nullptr;
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeFreeAsync(void* /*ptr*/,
                                      hazeStream_t /*stream*/) noexcept {
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeHostMalloc(void** ptr, size_t /*size*/,
                                       unsigned int /*flags*/) noexcept {
    if (ptr) *ptr = nullptr;
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeHostFree(void* /*ptr*/) noexcept {
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazePointerGetAttributes(hazePointerAttributes* attrs,
                                                  const void* /*ptr*/) noexcept {
    if (attrs) *attrs = {};
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeMemcpy(void* /*dst*/, const void* /*src*/,
                                   size_t /*count*/,
                                   hazeMemcpyKind /*kind*/) noexcept {
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeMemcpyAsync(void* /*dst*/, const void* /*src*/,
                                        size_t /*count*/,
                                        hazeMemcpyKind /*kind*/,
                                        hazeStream_t /*stream*/) noexcept {
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeMemset(void* /*dev_ptr*/, int /*value*/,
                                   size_t /*count*/) noexcept {
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeMemsetAsync(void* /*dev_ptr*/, int /*value*/,
                                        size_t /*count*/,
                                        hazeStream_t /*stream*/) noexcept {
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeMemcpyPeerAsync(void* /*dst*/, int /*dst_device*/,
                                             const void* /*src*/,
                                             int /*src_device*/,
                                             size_t /*count*/,
                                             hazeStream_t /*stream*/) noexcept {
    return set_error(HAZE_ERROR_NOT_SUPPORTED);
}
