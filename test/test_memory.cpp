// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
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
    REQUIRE(hazeMalloc(&p, 8192) == HAZE_ERROR_ALLOC_TOO_SMALL);
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
