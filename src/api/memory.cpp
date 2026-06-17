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
#include "common/errors.hpp"
#include "common/handle.hpp"
#include "core/allocator.hpp"
#include "core/context.hpp"
#include "core/mrp_polymap.hpp"
#include "core/record.hpp"

#include <cstdint>
#include <cstdlib>
#include <expected>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <unistd.h>

extern "C" hazeError_t hazeMalloc(hazeContext_t ctx, void **ptr, size_t size) noexcept {
    if (ctx == nullptr || ptr == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    auto result = ctx->allocator.allocate(size);
    if (!result)
        return set_error(haze::to_public_error(result.error()));
    *ptr = haze::to_void_ptr(*result);
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeFree(hazeContext_t ctx, void *ptr) noexcept {
    if (ctx == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    if (ptr != nullptr)
        haze::record_invalidate(*ctx, haze::to_dev_addr(ptr));
    return set_internal_result(ctx->allocator.free(haze::to_dev_addr(ptr)));
}

extern "C" hazeError_t hazeMallocAsync(hazeContext_t ctx, void **ptr, size_t size,
                                       hazeStream_t /*stream*/) noexcept {
    return hazeMalloc(ctx, ptr, size);
}

extern "C" hazeError_t hazeFreeAsync(hazeContext_t ctx, void *ptr,
                                     hazeStream_t /*stream*/) noexcept {
    return hazeFree(ctx, ptr);
}

extern "C" hazeError_t hazeHostAlloc(hazeContext_t ctx, void **ptr, size_t size,
                                     unsigned int /*flags*/) noexcept {
    if (ctx == nullptr || ptr == nullptr || size == 0)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    // Page-aligned allocation for DMA / O_DIRECT compatibility on
    // pinned-host buffers. Apple Silicon uses 16K pages, Linux x86_64
    // uses 4K — sysconf returns the right value either way.
    static const size_t kHostAllocAlignment = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    void *p = nullptr;
    // posix_memalign is exposed via <cstdlib> on the toolchain we
    // build with, but include-cleaner does not model POSIX extensions
    // and would direct us at <stdlib.h>, which modernize-deprecated-
    // headers in turn rejects. Suppress the cleaner here only.
    if (posix_memalign(&p, kHostAllocAlignment, size) != 0) // NOLINT(misc-include-cleaner)
        return set_error(HAZE_ERROR_OUT_OF_MEMORY);
    ctx->allocator.register_host_pointer(p);
    *ptr = p;
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeFreeHost(hazeContext_t ctx, void *ptr) noexcept {
    if (ctx == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    ctx->allocator.unregister_host_pointer(ptr);
    // posix_memalign-allocated; libc free is the matched deallocator.
    free(ptr); // NOLINT(cppcoreguidelines-no-malloc)
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazePointerGetAttributes(hazeContext_t ctx, hazePointerAttributes *attrs,
                                                const void *ptr) noexcept {
    if (ctx == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return set_internal_result(ctx->allocator.pointer_attributes(attrs, ptr));
}

extern "C" hazeError_t hazeMemcpy(hazeContext_t hctx, void *dst, const void *src, size_t count,
                                  hazeMemcpyKind kind) noexcept {
    if (hctx == nullptr || dst == nullptr || src == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);

    haze::Context &ctx = *hctx;
    auto &alloc = ctx.allocator;

    if (kind == HAZE_MEMCPY_HOST_TO_DEVICE) {
        const haze::DevAddr dev = haze::to_dev_addr(dst);
        if (auto h2d = alloc.copy_h2d(dev, src, count); !h2d)
            return set_internal_result(h2d);
        return set_internal_result(haze::tag_h2d_input(ctx, dev));
    }

    if (kind == HAZE_MEMCPY_DEVICE_TO_HOST) {
        // D2H is a pure shadow read; see haze::copy_to_host for the contract.
        return set_internal_result(haze::copy_to_host(ctx, dst, haze::to_dev_addr(src), count));
    }

    if (kind == HAZE_MEMCPY_DEVICE_TO_DEVICE) {
        return set_internal_result(haze::copy_device_to_device(ctx, haze::to_dev_addr(dst),
                                                               haze::to_dev_addr(src), count));
    }

    return set_error(HAZE_ERROR_INVALID_VALUE);
}

extern "C" hazeError_t hazeMemcpyAsync(hazeContext_t ctx, void *dst, const void *src, size_t count,
                                       hazeMemcpyKind kind, hazeStream_t /*stream*/) noexcept {
    return hazeMemcpy(ctx, dst, src, count, kind);
}

extern "C" hazeError_t hazeMemcpyMrp(hazeContext_t hctx, void *const *dst, const void *const *src,
                                     size_t count, hazeMemcpyKind kind, const uint64_t *base,
                                     size_t base_len) noexcept {
    if (hctx == nullptr || dst == nullptr || src == nullptr || base == nullptr || base_len == 0)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    haze::Context &ctx = *hctx;

    if (kind == HAZE_MEMCPY_HOST_TO_DEVICE)
        return set_internal_result(haze::copy_h2d_mrp(ctx, dst, src, count, base_len));
    if (kind == HAZE_MEMCPY_DEVICE_TO_HOST)
        return set_internal_result(haze::copy_to_host_mrp(ctx, dst, src, count, base_len));
    if (kind == HAZE_MEMCPY_DEVICE_TO_DEVICE)
        return set_internal_result(haze::copy_device_to_device_mrp(ctx, dst, src, base, base_len));

    return set_error(HAZE_ERROR_INVALID_VALUE);
}

extern "C" hazeError_t hazeMemset(hazeContext_t hctx, void *dev_ptr, int value,
                                  size_t count) noexcept {
    if (hctx == nullptr || dev_ptr == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    haze::Context &ctx = *hctx;
    const haze::DevAddr dev = haze::to_dev_addr(dev_ptr);
    if (auto result = ctx.allocator.memset(dev, value, count); !result)
        return set_internal_result(result);
    haze::record_invalidate(ctx, dev);
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeMemsetAsync(hazeContext_t ctx, void *dev_ptr, int value, size_t count,
                                       hazeStream_t /*stream*/) noexcept {
    return hazeMemset(ctx, dev_ptr, value, count);
}

extern "C" hazeError_t hazeMemcpyPeerAsync(hazeContext_t /*ctx*/, void * /*dst*/,
                                           int /*dst_device*/, const void * /*src*/,
                                           int /*src_device*/, size_t /*count*/,
                                           hazeStream_t /*stream*/) noexcept {
    return set_error(HAZE_ERROR_NOT_SUPPORTED);
}
