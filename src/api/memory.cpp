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
#include "core/device.hpp"
#include "core/epoch.hpp"
#include "core/mrp_polymap.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <unistd.h>
#include <vector>

extern "C" hazeError_t hazeMalloc(void **ptr, size_t size) noexcept {
    if (ptr == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    auto result = haze::allocator().allocate(size);
    if (!result)
        return set_error(haze::to_public_error(result.error()));
    *ptr = haze::to_void_ptr(*result);
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeFree(void *ptr) noexcept {
    if (ptr != nullptr)
        haze::epoch().invalidate(haze::to_dev_addr(ptr));
    return set_internal_result(haze::allocator().free(haze::to_dev_addr(ptr)));
}

extern "C" hazeError_t hazeMallocMrp(void **ptrs, size_t num_residues, size_t size) noexcept {
    // ptrs is checked before num_residues, so (NULL, 0) returns INVALID_VALUE.
    if (ptrs == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    // Bound before reserving so an absurd count cannot throw length_error
    // across this noexcept boundary.
    if (num_residues > static_cast<size_t>(haze::kMaxCiphertextModuli))
        return set_error(HAZE_ERROR_INVALID_VALUE);
    auto result = haze::allocator().allocate_many(num_residues, size);
    if (!result)
        return set_error(haze::to_public_error(result.error()));
    // On success the allocator returns exactly `num_residues` addresses; only
    // then is `ptrs` written (rollback leaves it untouched).
    for (size_t i = 0; i < num_residues; ++i)
        ptrs[i] = haze::to_void_ptr((*result)[i]);
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeFreeMrp(void *const *ptrs, size_t num_residues) noexcept {
    // ptrs is checked before num_residues, so (NULL, 0) returns INVALID_VALUE.
    if (ptrs == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    // Same noexcept bound as hazeMallocMrp.
    if (num_residues > static_cast<size_t>(haze::kMaxCiphertextModuli))
        return set_error(HAZE_ERROR_INVALID_VALUE);
    // Mirror hazeFree: drop each live address's epoch polymap binding first;
    // epoch().invalidate takes and releases the epoch lock before free_many's
    // allocator lock, preserving the epoch -> allocator lock order.
    std::vector<haze::DevAddr> addrs;
    addrs.reserve(num_residues);
    for (size_t i = 0; i < num_residues; ++i) {
        const haze::DevAddr addr = haze::to_dev_addr(ptrs[i]);
        if (ptrs[i] != nullptr)
            haze::epoch().invalidate(addr);
        addrs.push_back(addr);
    }
    return set_internal_result(haze::allocator().free_many(addrs));
}

extern "C" hazeError_t hazeMallocAsync(void **ptr, size_t size, hazeStream_t /*stream*/) noexcept {
    return hazeMalloc(ptr, size);
}

extern "C" hazeError_t hazeFreeAsync(void *ptr, hazeStream_t /*stream*/) noexcept {
    return hazeFree(ptr);
}

extern "C" hazeError_t hazeHostAlloc(void **ptr, size_t size, unsigned int /*flags*/) noexcept {
    if (ptr == nullptr || size == 0)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    // Page-aligned for DMA / O_DIRECT compatibility on pinned-host buffers; sysconf
    // returns the right page size on both Apple Silicon (16K) and Linux x86_64 (4K).
    static const size_t kHostAllocAlignment = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    void *p = nullptr;
    // posix_memalign comes from <cstdlib> on our toolchain, but include-cleaner would
    // redirect to <stdlib.h> (which modernize-deprecated-headers rejects); suppress it here.
    if (posix_memalign(&p, kHostAllocAlignment, size) != 0) // NOLINT(misc-include-cleaner)
        return set_error(HAZE_ERROR_OUT_OF_MEMORY);
    haze::allocator().register_host_pointer(p);
    *ptr = p;
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeFreeHost(void *ptr) noexcept {
    if (ptr == nullptr)
        return HAZE_SUCCESS; // match cudaFreeHost(NULL)
    // Only hazeHostAlloc pointers may reach libc free(); anything else
    // would abort the process instead of returning an error.
    if (!haze::allocator().unregister_host_pointer(ptr))
        return set_error(HAZE_ERROR_INVALID_VALUE);
    // posix_memalign-allocated; libc free is the matched deallocator.
    free(ptr); // NOLINT(cppcoreguidelines-no-malloc)
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazePointerGetAttributes(hazePointerAttributes *attrs,
                                                const void *ptr) noexcept {
    return set_internal_result(haze::allocator().pointer_attributes(attrs, ptr));
}

extern "C" hazeError_t hazeMemcpy(void *dst, const void *src, size_t count,
                                  hazeMemcpyKind kind) noexcept {
    if (dst == nullptr || src == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);

    auto &alloc = haze::allocator();

    if (kind == HAZE_MEMCPY_HOST_TO_DEVICE) {
        const haze::DevAddr dev = haze::to_dev_addr(dst);
        if (auto h2d = alloc.copy_h2d(dev, src, count); !h2d)
            return set_internal_result(h2d);
        // Zero-byte H2D wrote nothing, so there is nothing to tag.
        if (count == 0)
            return HAZE_SUCCESS;
        return set_internal_result(haze::tag_h2d_input(dev));
    }

    if (kind == HAZE_MEMCPY_DEVICE_TO_HOST) {
        // D2H is a pure shadow read; see haze::copy_to_host for the contract.
        return set_internal_result(haze::copy_to_host(dst, haze::to_dev_addr(src), count));
    }

    if (kind == HAZE_MEMCPY_DEVICE_TO_DEVICE) {
        return set_internal_result(
            haze::copy_device_to_device(haze::to_dev_addr(dst), haze::to_dev_addr(src), count));
    }

    return set_error(HAZE_ERROR_INVALID_VALUE);
}

extern "C" hazeError_t hazeMemcpyAsync(void *dst, const void *src, size_t count,
                                       hazeMemcpyKind kind, hazeStream_t /*stream*/) noexcept {
    return hazeMemcpy(dst, src, count, kind);
}

extern "C" hazeError_t hazeMemcpyMrp(void *const *dst, const void *const *src, size_t count,
                                     hazeMemcpyKind kind, const uint64_t *base,
                                     size_t base_len) noexcept {
    if (dst == nullptr || src == nullptr || base == nullptr || base_len == 0)
        return set_error(HAZE_ERROR_INVALID_VALUE);

    if (kind == HAZE_MEMCPY_HOST_TO_DEVICE)
        return set_internal_result(haze::copy_h2d_mrp(dst, src, count, base_len));
    if (kind == HAZE_MEMCPY_DEVICE_TO_HOST)
        return set_internal_result(haze::copy_to_host_mrp(dst, src, count, base_len));
    if (kind == HAZE_MEMCPY_DEVICE_TO_DEVICE)
        return set_internal_result(
            haze::copy_device_to_device_mrp(dst, src, count, base, base_len));

    return set_error(HAZE_ERROR_INVALID_VALUE);
}

extern "C" hazeError_t hazeMemset(void *dev_ptr, int value, size_t count) noexcept {
    if (dev_ptr == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    const haze::DevAddr dev = haze::to_dev_addr(dev_ptr);
    if (auto result = haze::allocator().memset(dev, value, count); !result)
        return set_internal_result(result);
    // Zero-byte memset changed nothing — keep the epoch binding.
    if (count == 0)
        return HAZE_SUCCESS;
    haze::epoch().invalidate(dev);
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeMemsetAsync(void *dev_ptr, int value, size_t count,
                                       hazeStream_t /*stream*/) noexcept {
    return hazeMemset(dev_ptr, value, count);
}

extern "C" hazeError_t hazeMemcpyPeerAsync(void * /*dst*/, int /*dst_device*/, const void * /*src*/,
                                           int /*src_device*/, size_t /*count*/,
                                           hazeStream_t /*stream*/) noexcept {
    return set_error(HAZE_ERROR_NOT_SUPPORTED);
}
