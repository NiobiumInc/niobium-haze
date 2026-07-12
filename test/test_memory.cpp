// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
#include "allocator_test_access.hpp"
#include "common/handle.hpp"
#include "core/allocator.hpp"
#include "core/device.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>       // IWYU pragma: keep
#include <haze/haze_types.h> // IWYU pragma: keep
#include <vector>

// HAZE's device allocator is single-size: every allocation is one
// polynomial of (ring_dim * sizeof(uint64_t)) bytes, configured via
// hazeSetRingDimension. Tests here always set ring_dim before
// allocating to make that contract explicit.

TEST_CASE("hazeMalloc returns non-null pointer", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    void *p = nullptr;
    REQUIRE(hazeMalloc(&p, 32768) == HAZE_SUCCESS);
    REQUIRE(p != nullptr);
    REQUIRE(hazeFree(p) == HAZE_SUCCESS);
}

TEST_CASE("hazeMalloc null ptr arg returns error", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(nullptr, 32768) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("hazeMalloc before hazeSetRingDimension is rejected", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS); // ensure ring_dim cleared
    void *p = nullptr;
    REQUIRE(hazeMalloc(&p, 32768) == HAZE_ERROR_CONFIGERR);
    hazeGetLastError();
}

TEST_CASE("hazeMalloc with size != polynomial bytes is rejected", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    void *p = nullptr;
    // ring_dim=4096 → polynomial bytes = 32768; 8KB request fails.
    REQUIRE(hazeMalloc(&p, 8192) == HAZE_ERROR_SIZE_MISMATCH);
    hazeGetLastError();
}

TEST_CASE("distinct allocations produce non-overlapping addresses", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    void *p1 = nullptr;
    void *p2 = nullptr;
    REQUIRE(hazeMalloc(&p1, 32768) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&p2, 32768) == HAZE_SUCCESS);
    REQUIRE(p1 != p2);
    REQUIRE(hazeFree(p1) == HAZE_SUCCESS);
    REQUIRE(hazeFree(p2) == HAZE_SUCCESS);
}

TEST_CASE("free then re-allocate recycles pooled address", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);

    void *p1 = nullptr;
    REQUIRE(hazeMalloc(&p1, 32768) == HAZE_SUCCESS);
    REQUIRE(hazeFree(p1) == HAZE_SUCCESS);

    void *p2 = nullptr;
    REQUIRE(hazeMalloc(&p2, 32768) == HAZE_SUCCESS);
    REQUIRE(p1 == p2);
    REQUIRE(hazeFree(p2) == HAZE_SUCCESS);
}

TEST_CASE("H2D then D2H round-trip preserves data", "[unit]") {
    constexpr size_t kN = 4096;
    constexpr size_t kBytes = kN * sizeof(uint64_t);

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kN) == HAZE_SUCCESS);
    void *dev = nullptr;
    REQUIRE(hazeMalloc(&dev, kBytes) == HAZE_SUCCESS);

    std::vector<uint64_t> src(kN);
    for (size_t i = 0; i < kN; ++i)
        src[i] = static_cast<uint64_t>(i) + 1;

    REQUIRE(hazeMemcpy(dev, src.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    std::vector<uint64_t> dst(kN, 0);
    REQUIRE(hazeMemcpy(dst.data(), dev, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    REQUIRE(dst == src);
    REQUIRE(hazeFree(dev) == HAZE_SUCCESS);
}

TEST_CASE("hazeMemset fills shadow buffer", "[unit]") {
    constexpr size_t kBytes = 32768;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    void *dev = nullptr;
    REQUIRE(hazeMalloc(&dev, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMemset(dev, 0xAB, kBytes) == HAZE_SUCCESS);

    std::vector<uint8_t> result(kBytes, 0);
    REQUIRE(hazeMemcpy(result.data(), dev, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    REQUIRE(std::ranges::all_of(result, [](uint8_t b) { return b == 0xAB; }));
    REQUIRE(hazeFree(dev) == HAZE_SUCCESS);
}

TEST_CASE("hazeHostAlloc/Free round-trip", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    void *p = nullptr;
    REQUIRE(hazeHostAlloc(&p, 4096, 0) == HAZE_SUCCESS);
    REQUIRE(p != nullptr);
    hazeFreeHost(p);
}

TEST_CASE("hazeMallocAsync/FreeAsync behave like sync versions", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    hazeStream_t stream = nullptr;
    REQUIRE(hazeStreamCreate(&stream) == HAZE_SUCCESS);

    void *p = nullptr;
    REQUIRE(hazeMallocAsync(&p, 32768, stream) == HAZE_SUCCESS);
    REQUIRE(p != nullptr);
    REQUIRE(hazeFreeAsync(p, stream) == HAZE_SUCCESS);

    REQUIRE(hazeStreamDestroy(stream) == HAZE_SUCCESS);
}

TEST_CASE("create and destroy 100 streams without leaks", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    for (int i = 0; i < 100; ++i) {
        hazeStream_t s = nullptr;
        REQUIRE(hazeStreamCreate(&s) == HAZE_SUCCESS);
        REQUIRE(s != nullptr);
        REQUIRE(hazeStreamDestroy(s) == HAZE_SUCCESS);
    }
}

TEST_CASE("create and destroy 100 events without leaks", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    for (int i = 0; i < 100; ++i) {
        hazeEvent_t e = nullptr;
        REQUIRE(hazeEventCreate(&e) == HAZE_SUCCESS);
        REQUIRE(e != nullptr);
        REQUIRE(hazeEventDestroy(e) == HAZE_SUCCESS);
    }
}

TEST_CASE("hazeStreamSynchronize is a no-op returning HAZE_SUCCESS", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    hazeStream_t s = nullptr;
    REQUIRE(hazeStreamCreate(&s) == HAZE_SUCCESS);
    REQUIRE(hazeStreamSynchronize(s) == HAZE_SUCCESS);
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);
    REQUIRE(hazeStreamDestroy(s) == HAZE_SUCCESS);
}

TEST_CASE("hazeDeviceSynchronize is a no-op returning HAZE_SUCCESS", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeDeviceSynchronize() == HAZE_SUCCESS);
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);
}

TEST_CASE("hazeGetDeviceProperties returns FPGA values", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    hazeDeviceProp prop{};
    REQUIRE(hazeGetDeviceProperties(&prop, 0) == HAZE_SUCCESS);
    REQUIRE(prop.totalGlobalMem == 16ULL * 1024 * 1024 * 1024);
    REQUIRE(prop.numRegisters == 64);
    REQUIRE(prop.maxCiphertextModuli == 64);
    REQUIRE(prop.numHBMBanks == 8);
    REQUIRE(prop.numSupportedRingDims == 7);
    // N=4096 corresponds to exponent 12
    bool found_12 = false;
    for (int i = 0; i < prop.numSupportedRingDims; ++i) {
        if (prop.supportedRingDimExponents[i] == 12) {
            found_12 = true;
            break;
        }
    }
    REQUIRE(found_12);
}

TEST_CASE("hazeGetDeviceCount returns 1", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    int count = 0;
    REQUIRE(hazeGetDeviceCount(&count) == HAZE_SUCCESS);
    REQUIRE(count == 1);
}

TEST_CASE("NULL stream argument uses default stream", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeStreamSynchronize(nullptr) == HAZE_SUCCESS);
    hazeEvent_t e = nullptr;
    REQUIRE(hazeEventCreate(&e) == HAZE_SUCCESS);
    REQUIRE(hazeEventRecord(e, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeStreamWaitEvent(nullptr, e, 0) == HAZE_SUCCESS);
    REQUIRE(hazeEventDestroy(e) == HAZE_SUCCESS);
}

TEST_CASE("hazePointerGetAttributes reports device type for hazeMalloc'd pointer", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    void *dev = nullptr;
    REQUIRE(hazeMalloc(&dev, 32768) == HAZE_SUCCESS);

    hazePointerAttributes attrs{};
    REQUIRE(hazePointerGetAttributes(&attrs, dev) == HAZE_SUCCESS);
    REQUIRE(attrs.type == HAZE_MEMORY_TYPE_DEVICE);

    REQUIRE(hazeFree(dev) == HAZE_SUCCESS);
}

TEST_CASE("hazePointerGetAttributes reports unregistered for foreign pointers", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    // A stack pointer was never registered with hazeHostAlloc, so it
    // reports UNREGISTERED — matches cudaPointerGetAttributes since CUDA 11.
    int stack_local = 0;
    hazePointerAttributes attrs{};
    REQUIRE(hazePointerGetAttributes(&attrs, &stack_local) == HAZE_SUCCESS);
    REQUIRE(attrs.type == HAZE_MEMORY_TYPE_UNREGISTERED);
}

TEST_CASE("hazePointerGetAttributes reports host type for hazeHostAlloc'd pointer", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    void *p = nullptr;
    REQUIRE(hazeHostAlloc(&p, 4096, 0) == HAZE_SUCCESS);
    hazePointerAttributes attrs{};
    REQUIRE(hazePointerGetAttributes(&attrs, p) == HAZE_SUCCESS);
    REQUIRE(attrs.type == HAZE_MEMORY_TYPE_HOST);
    hazeFreeHost(p);
}

TEST_CASE("hazePointerGetAttributes rejects null attrs out-pointer", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    int stack_local = 0;
    REQUIRE(hazePointerGetAttributes(nullptr, &stack_local) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

// ---------------------------------------------------------------------------
// hazeMallocMrp / hazeFreeMrp — batched MRP-group allocation.
// ---------------------------------------------------------------------------

TEST_CASE("hazeMallocMrp allocates a usable group that hazeFreeMrp releases", "[unit]") {
    constexpr size_t kN = 4096;
    constexpr size_t kBytes = kN * sizeof(uint64_t);
    constexpr size_t kCount = 4;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kN) == HAZE_SUCCESS);

    void *ptrs[kCount] = {};
    REQUIRE(hazeMallocMrp(ptrs, kCount, kBytes) == HAZE_SUCCESS);
    for (size_t i = 0; i < kCount; ++i) {
        REQUIRE(ptrs[i] != nullptr);
        for (size_t j = i + 1; j < kCount; ++j)
            REQUIRE(ptrs[i] != ptrs[j]);
    }

    // Each poly is a normal allocation: H2D then D2H round-trips.
    std::vector<uint64_t> src(kN);
    for (size_t i = 0; i < kN; ++i)
        src[i] = static_cast<uint64_t>(i) + 1;
    for (void *ptr : ptrs) {
        REQUIRE(hazeMemcpy(ptr, src.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
        std::vector<uint64_t> dst(kN, 0);
        REQUIRE(hazeMemcpy(dst.data(), ptr, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
        REQUIRE(dst == src);
    }
    REQUIRE(hazeFreeMrp(ptrs, kCount) == HAZE_SUCCESS);
}

TEST_CASE("hazeMallocMrp/hazeFreeMrp round-trip recycles the group", "[unit]") {
    constexpr size_t kBytes = 4096 * sizeof(uint64_t);
    constexpr size_t kCount = 3;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);

    void *g1[kCount] = {};
    REQUIRE(hazeMallocMrp(g1, kCount, kBytes) == HAZE_SUCCESS);
    const std::vector<void *> first(g1, g1 + kCount);
    REQUIRE(hazeFreeMrp(g1, kCount) == HAZE_SUCCESS);

    void *g2[kCount] = {};
    REQUIRE(hazeMallocMrp(g2, kCount, kBytes) == HAZE_SUCCESS);
    for (void *addr : g2)
        REQUIRE(std::ranges::find(first, addr) != first.end());
    REQUIRE(hazeFreeMrp(g2, kCount) == HAZE_SUCCESS);
}

TEST_CASE("hazeMallocMrp with count 0 is a success no-op", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    void *const sentinel = reinterpret_cast<void *>(0x5A5A);
    void *ptrs[1] = {sentinel};
    REQUIRE(hazeMallocMrp(ptrs, 0, 32768) == HAZE_SUCCESS);
    REQUIRE(ptrs[0] == sentinel); // left untouched
}

TEST_CASE("hazeMallocMrp before hazeSetRingDimension is rejected", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    void *ptrs[2] = {};
    REQUIRE(hazeMallocMrp(ptrs, 2, 32768) == HAZE_ERROR_CONFIGERR);
    hazeGetLastError();
}

TEST_CASE("hazeMallocMrp with null ptrs is rejected", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    REQUIRE(hazeMallocMrp(nullptr, 4, 32768) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("hazeMallocMrp with wrong size is rejected and allocates nothing", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    const auto &alloc = haze::DeviceAllocator::instance();
    REQUIRE(haze::test::AllocatorTestAccess::alloc_set_size(alloc) == 0);

    void *ptrs[3] = {reinterpret_cast<void *>(0xA), reinterpret_cast<void *>(0xB),
                     reinterpret_cast<void *>(0xC)};
    // ring_dim=4096 -> polynomial bytes = 32768; an 8KB request fails.
    REQUIRE(hazeMallocMrp(ptrs, 3, 8192) == HAZE_ERROR_SIZE_MISMATCH);
    hazeGetLastError();
    // Validation fails before any reservation: ptrs untouched, nothing live.
    REQUIRE(ptrs[0] == reinterpret_cast<void *>(0xA));
    REQUIRE(ptrs[2] == reinterpret_cast<void *>(0xC));
    REQUIRE(haze::test::AllocatorTestAccess::alloc_set_size(alloc) == 0);
}

TEST_CASE("hazeMallocMrp rolls back reserved polys on a mid-batch failure", "[unit]") {
    constexpr size_t kBytes = 4096 * sizeof(uint64_t);
    auto &alloc = haze::DeviceAllocator::instance();
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);

    // p stays live; r is freed so the batch can recycle it as residue 0.
    void *p = nullptr;
    void *r = nullptr;
    REQUIRE(hazeMalloc(&p, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&r, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeFree(r) == HAZE_SUCCESS);
    const size_t live_before = haze::test::AllocatorTestAccess::alloc_set_size(alloc);

    // Wedge a still-live addr (p) in front of the recyclable r: the batch
    // recycles r for residue 0, then pops p for residue 1 and hits the
    // "recycled addr already live" desync, forcing rollback of residue 0.
    haze::test::AllocatorTestAccess::push_front_pool_entry(alloc, haze::to_dev_addr(p));

    void *ptrs[2] = {reinterpret_cast<void *>(0x1111), reinterpret_cast<void *>(0x2222)};
    REQUIRE(hazeMallocMrp(ptrs, 2, kBytes) == HAZE_ERROR_INTERNAL);
    hazeGetLastError();

    // Rollback freed the recycled residue: nothing extra is live, ptrs untouched.
    REQUIRE(haze::test::AllocatorTestAccess::alloc_set_size(alloc) == live_before);
    REQUIRE(ptrs[0] == reinterpret_cast<void *>(0x1111));

    // Clear the injected corruption before the next case.
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("hazeFreeMrp with null ptrs is rejected", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeFreeMrp(nullptr, 2) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("hazeFreeMrp with count 0 is a success no-op", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    void *ptrs[1] = {nullptr};
    REQUIRE(hazeFreeMrp(ptrs, 0) == HAZE_SUCCESS);
}

TEST_CASE("hazeFreeMrp skips null entries and frees the rest", "[unit]") {
    constexpr size_t kBytes = 4096 * sizeof(uint64_t);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    void *a = nullptr;
    void *b = nullptr;
    REQUIRE(hazeMalloc(&a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&b, kBytes) == HAZE_SUCCESS);

    void *group[3] = {a, nullptr, b}; // middle entry null -> skipped
    REQUIRE(hazeFreeMrp(group, 3) == HAZE_SUCCESS);

    // Both real addresses were freed into the pool: the next two allocations
    // recycle them (proving neither the null entry nor its neighbours were
    // dropped).
    void *r0 = nullptr;
    void *r1 = nullptr;
    REQUIRE(hazeMalloc(&r0, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&r1, kBytes) == HAZE_SUCCESS);
    REQUIRE((r0 == a || r0 == b));
    REQUIRE((r1 == a || r1 == b));
    REQUIRE(r0 != r1);
    REQUIRE(hazeFree(r0) == HAZE_SUCCESS);
    REQUIRE(hazeFree(r1) == HAZE_SUCCESS);
}

TEST_CASE("hazeMallocMrp rejects an out-of-range count without allocating", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    const auto &alloc = haze::DeviceAllocator::instance();
    void *ptrs[1] = {reinterpret_cast<void *>(0x1234)};

    // count = SIZE_MAX and count = cap+1 both reject before any reserve, so
    // the giant reservation is never attempted (no abort, no OOM).
    REQUIRE(hazeMallocMrp(ptrs, SIZE_MAX, 32768) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    REQUIRE(hazeMallocMrp(ptrs, static_cast<size_t>(haze::kMaxCiphertextModuli) + 1, 32768) ==
            HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();

    REQUIRE(ptrs[0] == reinterpret_cast<void *>(0x1234)); // untouched
    REQUIRE(haze::test::AllocatorTestAccess::alloc_set_size(alloc) == 0);
}

TEST_CASE("hazeFreeMrp rejects an out-of-range count", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    void *ptrs[1] = {nullptr};
    REQUIRE(hazeFreeMrp(ptrs, SIZE_MAX) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    REQUIRE(hazeFreeMrp(ptrs, static_cast<size_t>(haze::kMaxCiphertextModuli) + 1) ==
            HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("hazeFree of an already-freed address is rejected", "[unit]") {
    constexpr size_t kBytes = 4096 * sizeof(uint64_t);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    void *p = nullptr;
    REQUIRE(hazeMalloc(&p, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeFree(p) == HAZE_SUCCESS);
    REQUIRE(hazeFree(p) == HAZE_ERROR_UNKNOWN_ADDRESS); // double free rejected
    hazeGetLastError();
}

TEST_CASE("malloc after free does not alias a second live allocation", "[unit]") {
    constexpr size_t kBytes = 4096 * sizeof(uint64_t);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    void *a = nullptr;
    REQUIRE(hazeMalloc(&a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeFree(a) == HAZE_SUCCESS);
    void *b = nullptr;
    void *c = nullptr;
    REQUIRE(hazeMalloc(&b, kBytes) == HAZE_SUCCESS); // recycles a
    REQUIRE(hazeMalloc(&c, kBytes) == HAZE_SUCCESS); // fresh
    REQUIRE(b != c);                                 // two live allocations, no aliasing
    REQUIRE(hazeFree(b) == HAZE_SUCCESS);
    REQUIRE(hazeFree(c) == HAZE_SUCCESS);
}

TEST_CASE("hazeFreeMrp of an already-freed group is rejected and does not alias", "[unit]") {
    constexpr size_t kBytes = 4096 * sizeof(uint64_t);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    void *g[1] = {};
    REQUIRE(hazeMallocMrp(g, 1, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeFreeMrp(g, 1) == HAZE_SUCCESS);
    REQUIRE(hazeFreeMrp(g, 1) == HAZE_ERROR_UNKNOWN_ADDRESS); // double free rejected
    hazeGetLastError();

    // The rejected second free must not have re-pooled the addr: two live
    // allocations get distinct addresses.
    void *x = nullptr;
    void *y = nullptr;
    REQUIRE(hazeMalloc(&x, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&y, kBytes) == HAZE_SUCCESS);
    REQUIRE(x != y);
    REQUIRE(hazeFree(x) == HAZE_SUCCESS);
    REQUIRE(hazeFree(y) == HAZE_SUCCESS);
}

TEST_CASE("hazeFreeMrp frees every entry and returns the first error", "[unit]") {
    constexpr size_t kBytes = 4096 * sizeof(uint64_t);
    auto &alloc = haze::DeviceAllocator::instance();
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    void *a = nullptr;
    void *bad = nullptr;
    void *b = nullptr;
    REQUIRE(hazeMalloc(&a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&bad, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&b, kBytes) == HAZE_SUCCESS);
    // Pre-free `bad` so freeing it again inside the group is an error.
    REQUIRE(hazeFree(bad) == HAZE_SUCCESS);
    const size_t live_before = haze::test::AllocatorTestAccess::alloc_set_size(alloc); // a, b

    void *group[3] = {a, bad, b};
    REQUIRE(hazeFreeMrp(group, 3) == HAZE_ERROR_UNKNOWN_ADDRESS); // first (and only) error
    hazeGetLastError();
    // Despite bad's error, a and b were still freed: two fewer live.
    REQUIRE(haze::test::AllocatorTestAccess::alloc_set_size(alloc) == live_before - 2);
}
