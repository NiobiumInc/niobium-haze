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

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Pool allocator internals
// ---------------------------------------------------------------------------

// Virtual HBM base — above FHETCH synthetic address range (< 0x1000000000).
static constexpr uintptr_t kHbmBase = 0x4000000000ULL;

// Polynomial-aligned size classes (32 KB → 512 KB for N=4096..65536).
static constexpr size_t kSizeClasses[] = {
    32 * 1024,
    64 * 1024,
    128 * 1024,
    256 * 1024,
    512 * 1024,
};
static constexpr int kNumSizeClasses =
    static_cast<int>(sizeof(kSizeClasses) / sizeof(kSizeClasses[0]));

struct Allocation {
    uintptr_t hbm_addr{};
    size_t size{};
    std::vector<uint8_t> host_shadow;
    bool has_data = false;
};

// Protects all allocator state.
static std::mutex g_alloc_mutex;                               // NOLINT
static std::unordered_map<uintptr_t, Allocation> g_allocations; // NOLINT
static std::vector<uintptr_t> g_free_lists[kNumSizeClasses];    // NOLINT
static uintptr_t g_next_hbm_addr = kHbmBase;                   // NOLINT

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static int size_class_for(size_t bytes) noexcept {
    for (int i = 0; i < kNumSizeClasses; ++i) {
        if (bytes <= kSizeClasses[i]) return i;
    }
    return -1;
}

static size_t rounded_size(size_t bytes) noexcept {
    const int cls = size_class_for(bytes);
    return (cls >= 0) ? kSizeClasses[cls] : bytes;
}

static hazeError_t alloc_impl(void** ptr, size_t size) noexcept {
    if (!ptr || size == 0) return set_error(HAZE_ERROR_INVALID_VALUE);

    const size_t alloc_size = rounded_size(size);
    const int cls = size_class_for(size);

    std::lock_guard lock(g_alloc_mutex);

    uintptr_t addr = 0;
    if (cls >= 0 && !g_free_lists[cls].empty()) {
        addr = g_free_lists[cls].back();
        g_free_lists[cls].pop_back();
        auto& a = g_allocations.at(addr);
        a.has_data = false;
        std::fill(a.host_shadow.begin(), a.host_shadow.end(), uint8_t{0});
    } else {
        addr = g_next_hbm_addr;
        g_next_hbm_addr += alloc_size;

        Allocation a;
        a.hbm_addr = addr;
        a.size = alloc_size;
        a.host_shadow.resize(alloc_size, 0);
        a.has_data = false;
        g_allocations.emplace(addr, std::move(a));
    }

    *ptr = reinterpret_cast<void*>(addr);
    return HAZE_SUCCESS;
}

static hazeError_t free_impl(void* ptr) noexcept {
    if (!ptr) return HAZE_SUCCESS;

    const uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    std::lock_guard lock(g_alloc_mutex);

    auto it = g_allocations.find(addr);
    if (it == g_allocations.end()) return set_error(HAZE_ERROR_INVALID_VALUE);

    const int cls = size_class_for(it->second.size);
    if (cls >= 0) {
        g_free_lists[cls].push_back(addr);
    } else {
        g_allocations.erase(it);
    }
    return HAZE_SUCCESS;
}

// ---------------------------------------------------------------------------
// Public API — device memory
// ---------------------------------------------------------------------------

extern "C" hazeError_t hazeMalloc(void** ptr, size_t size) noexcept {
    return alloc_impl(ptr, size);
}

extern "C" hazeError_t hazeFree(void* ptr) noexcept {
    return free_impl(ptr);
}

extern "C" hazeError_t hazeMallocAsync(void** ptr, size_t size,
                                        hazeStream_t /*stream*/) noexcept {
    return alloc_impl(ptr, size);
}

extern "C" hazeError_t hazeFreeAsync(void* ptr,
                                      hazeStream_t /*stream*/) noexcept {
    return free_impl(ptr);
}

// ---------------------------------------------------------------------------
// Public API — host (page-aligned) memory
// ---------------------------------------------------------------------------

extern "C" hazeError_t hazeHostMalloc(void** ptr, size_t size,
                                       unsigned int /*flags*/) noexcept {
    if (!ptr || size == 0) return set_error(HAZE_ERROR_INVALID_VALUE);
    void* p = nullptr;
    if (posix_memalign(&p, 4096, size) != 0) return set_error(HAZE_ERROR_OUT_OF_MEMORY);
    *ptr = p;
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeHostFree(void* ptr) noexcept {
    free(ptr);  // NOLINT(cppcoreguidelines-no-malloc)
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazePointerGetAttributes(hazePointerAttributes* attrs,
                                                  const void* /*ptr*/) noexcept {
    if (attrs) *attrs = {};
    return HAZE_SUCCESS;
}

// ---------------------------------------------------------------------------
// Public API — data transfer
// ---------------------------------------------------------------------------

// Materialization hook: triggered by D2H memcpy (implemented in haze_materialize.cpp).
// Stub returns HAZE_SUCCESS before task 03 is integrated.
hazeError_t haze_materialize_d2h(uintptr_t dev_addr) noexcept;

extern "C" hazeError_t hazeMemcpy(void* dst, const void* src, size_t count,
                                   hazeMemcpyKind kind) noexcept {
    if (!dst || !src) return set_error(HAZE_ERROR_INVALID_VALUE);

    if (kind == HAZE_MEMCPY_HOST_TO_DEVICE) {
        const uintptr_t dev_addr = reinterpret_cast<uintptr_t>(dst);
        std::lock_guard lock(g_alloc_mutex);
        auto it = g_allocations.find(dev_addr);
        if (it == g_allocations.end()) return set_error(HAZE_ERROR_INVALID_VALUE);
        auto& a = it->second;
        if (count > a.size) return set_error(HAZE_ERROR_INVALID_VALUE);
        std::memcpy(a.host_shadow.data(), src, count);
        a.has_data = true;
        return HAZE_SUCCESS;
    }

    if (kind == HAZE_MEMCPY_DEVICE_TO_HOST) {
        const uintptr_t dev_addr = reinterpret_cast<uintptr_t>(src);
        // Sole materialization trigger (task 03). No-op before task 03.
        hazeError_t err = haze_materialize_d2h(dev_addr);
        if (err != HAZE_SUCCESS) return err;
        std::lock_guard lock(g_alloc_mutex);
        auto it = g_allocations.find(dev_addr);
        if (it == g_allocations.end()) return set_error(HAZE_ERROR_INVALID_VALUE);
        const auto& a = it->second;
        if (count > a.size) return set_error(HAZE_ERROR_INVALID_VALUE);
        std::memcpy(dst, a.host_shadow.data(), count);
        return HAZE_SUCCESS;
    }

    if (kind == HAZE_MEMCPY_DEVICE_TO_DEVICE) {
        const uintptr_t src_addr = reinterpret_cast<uintptr_t>(src);
        const uintptr_t dst_addr = reinterpret_cast<uintptr_t>(dst);
        std::lock_guard lock(g_alloc_mutex);
        auto src_it = g_allocations.find(src_addr);
        auto dst_it = g_allocations.find(dst_addr);
        if (src_it == g_allocations.end() || dst_it == g_allocations.end())
            return set_error(HAZE_ERROR_INVALID_VALUE);
        if (count > src_it->second.size || count > dst_it->second.size)
            return set_error(HAZE_ERROR_INVALID_VALUE);
        std::memcpy(dst_it->second.host_shadow.data(),
                    src_it->second.host_shadow.data(), count);
        dst_it->second.has_data = src_it->second.has_data;
        return HAZE_SUCCESS;
    }

    return set_error(HAZE_ERROR_INVALID_VALUE);
}

extern "C" hazeError_t hazeMemcpyAsync(void* dst, const void* src, size_t count,
                                        hazeMemcpyKind kind,
                                        hazeStream_t /*stream*/) noexcept {
    return hazeMemcpy(dst, src, count, kind);
}

extern "C" hazeError_t hazeMemset(void* dev_ptr, int value,
                                   size_t count) noexcept {
    if (!dev_ptr) return set_error(HAZE_ERROR_INVALID_VALUE);
    const uintptr_t addr = reinterpret_cast<uintptr_t>(dev_ptr);
    std::lock_guard lock(g_alloc_mutex);
    auto it = g_allocations.find(addr);
    if (it == g_allocations.end()) return set_error(HAZE_ERROR_INVALID_VALUE);
    auto& a = it->second;
    if (count > a.size) return set_error(HAZE_ERROR_INVALID_VALUE);
    std::memset(a.host_shadow.data(), value, count);
    a.has_data = true;
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeMemsetAsync(void* dev_ptr, int value, size_t count,
                                        hazeStream_t /*stream*/) noexcept {
    return hazeMemset(dev_ptr, value, count);
}

extern "C" hazeError_t hazeMemcpyPeerAsync(void* /*dst*/, int /*dst_device*/,
                                             const void* /*src*/,
                                             int /*src_device*/,
                                             size_t /*count*/,
                                             hazeStream_t /*stream*/) noexcept {
    return set_error(HAZE_ERROR_NOT_SUPPORTED);
}

// ---------------------------------------------------------------------------
// Internal accessors for other translation units (haze_materialize.cpp etc.)
// ---------------------------------------------------------------------------

std::mutex& haze_alloc_mutex() noexcept { return g_alloc_mutex; }

std::vector<uint8_t>* haze_shadow_buffer(uintptr_t dev_addr) noexcept {
    auto it = g_allocations.find(dev_addr);
    if (it == g_allocations.end()) return nullptr;
    return &it->second.host_shadow;
}

bool haze_shadow_has_data(uintptr_t dev_addr) noexcept {
    auto it = g_allocations.find(dev_addr);
    if (it == g_allocations.end()) return false;
    return it->second.has_data;
}

size_t haze_alloc_size(uintptr_t dev_addr) noexcept {
    auto it = g_allocations.find(dev_addr);
    if (it == g_allocations.end()) return 0;
    return it->second.size;
}

void haze_shadow_update(uintptr_t dev_addr,
                        const std::vector<uint8_t>& data) noexcept {
    auto it = g_allocations.find(dev_addr);
    if (it == g_allocations.end()) return;
    auto& a = it->second;
    const size_t copy_len = std::min(data.size(), a.host_shadow.size());
    std::memcpy(a.host_shadow.data(), data.data(), copy_len);
    a.has_data = true;
}
