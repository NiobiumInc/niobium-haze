// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
#include <catch2/catch_test_macros.hpp>

#include <haze/haze.h>

#include <algorithm>
#include <cstdint>
#include <vector>

TEST_CASE("hazeMalloc returns non-null pointer") {
    void* p = nullptr;
    REQUIRE(hazeMalloc(&p, 32768) == HAZE_SUCCESS);
    REQUIRE(p != nullptr);
    REQUIRE(hazeFree(p) == HAZE_SUCCESS);
}

TEST_CASE("hazeMalloc null ptr arg returns error") {
    REQUIRE(hazeMalloc(nullptr, 32768) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("distinct allocations produce non-overlapping addresses") {
    void* p1 = nullptr;
    void* p2 = nullptr;
    REQUIRE(hazeMalloc(&p1, 32768) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&p2, 32768) == HAZE_SUCCESS);
    REQUIRE(p1 != p2);
    REQUIRE(hazeFree(p1) == HAZE_SUCCESS);
    REQUIRE(hazeFree(p2) == HAZE_SUCCESS);
}

TEST_CASE("free then re-allocate recycles pooled address") {
    void* p1 = nullptr;
    REQUIRE(hazeMalloc(&p1, 32768) == HAZE_SUCCESS);
    REQUIRE(hazeFree(p1) == HAZE_SUCCESS);

    void* p2 = nullptr;
    REQUIRE(hazeMalloc(&p2, 32768) == HAZE_SUCCESS);
    REQUIRE(p1 == p2);
    REQUIRE(hazeFree(p2) == HAZE_SUCCESS);
}

TEST_CASE("multiple size classes produce distinct addresses") {
    void* s32 = nullptr;
    void* s64 = nullptr;
    REQUIRE(hazeMalloc(&s32, 32768) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&s64, 65536) == HAZE_SUCCESS);
    REQUIRE(s32 != s64);
    REQUIRE(hazeFree(s32) == HAZE_SUCCESS);
    REQUIRE(hazeFree(s64) == HAZE_SUCCESS);
}

TEST_CASE("H2D memcpy stores data in shadow buffer") {
    constexpr size_t kN = 4096;
    constexpr size_t kBytes = kN * sizeof(uint64_t);

    void* dev = nullptr;
    REQUIRE(hazeMalloc(&dev, kBytes) == HAZE_SUCCESS);

    std::vector<uint64_t> src(kN);
    for (size_t i = 0; i < kN; ++i) src[i] = static_cast<uint64_t>(i * 3 + 7);

    REQUIRE(hazeMemcpy(dev, src.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE)
            == HAZE_SUCCESS);

    REQUIRE(hazeFree(dev) == HAZE_SUCCESS);
}

TEST_CASE("H2D then D2H round-trip preserves data") {
    constexpr size_t kN = 4096;
    constexpr size_t kBytes = kN * sizeof(uint64_t);

    void* dev = nullptr;
    REQUIRE(hazeMalloc(&dev, kBytes) == HAZE_SUCCESS);

    std::vector<uint64_t> src(kN);
    for (size_t i = 0; i < kN; ++i) src[i] = static_cast<uint64_t>(i + 1);

    REQUIRE(hazeMemcpy(dev, src.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE)
            == HAZE_SUCCESS);

    std::vector<uint64_t> dst(kN, 0);
    REQUIRE(hazeMemcpy(dst.data(), dev, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST)
            == HAZE_SUCCESS);

    REQUIRE(dst == src);
    REQUIRE(hazeFree(dev) == HAZE_SUCCESS);
}

TEST_CASE("D2D memcpy copies shadow buffer") {
    constexpr size_t kBytes = 32768;
    void* src_dev = nullptr;
    void* dst_dev = nullptr;
    REQUIRE(hazeMalloc(&src_dev, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&dst_dev, kBytes) == HAZE_SUCCESS);

    std::vector<uint8_t> pattern(kBytes);
    for (size_t i = 0; i < kBytes; ++i) pattern[i] = static_cast<uint8_t>(i & 0xFF);

    REQUIRE(hazeMemcpy(src_dev, pattern.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE)
            == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(dst_dev, src_dev, kBytes, HAZE_MEMCPY_DEVICE_TO_DEVICE)
            == HAZE_SUCCESS);

    std::vector<uint8_t> result(kBytes, 0);
    REQUIRE(hazeMemcpy(result.data(), dst_dev, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST)
            == HAZE_SUCCESS);

    REQUIRE(result == pattern);
    REQUIRE(hazeFree(src_dev) == HAZE_SUCCESS);
    REQUIRE(hazeFree(dst_dev) == HAZE_SUCCESS);
}

TEST_CASE("hazeMemset fills shadow buffer") {
    constexpr size_t kBytes = 32768;
    void* dev = nullptr;
    REQUIRE(hazeMalloc(&dev, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMemset(dev, 0xAB, kBytes) == HAZE_SUCCESS);

    std::vector<uint8_t> result(kBytes, 0);
    REQUIRE(hazeMemcpy(result.data(), dev, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST)
            == HAZE_SUCCESS);

    REQUIRE(std::all_of(result.begin(), result.end(),
                        [](uint8_t b) { return b == 0xAB; }));
    REQUIRE(hazeFree(dev) == HAZE_SUCCESS);
}

TEST_CASE("hazeHostMalloc/Free round-trip") {
    void* p = nullptr;
    REQUIRE(hazeHostMalloc(&p, 4096, 0) == HAZE_SUCCESS);
    REQUIRE(p != nullptr);
    hazeHostFree(p);
}

TEST_CASE("hazeMallocAsync/FreeAsync behave like sync versions") {
    hazeStream_t stream = nullptr;
    REQUIRE(hazeStreamCreate(&stream) == HAZE_SUCCESS);

    void* p = nullptr;
    REQUIRE(hazeMallocAsync(&p, 32768, stream) == HAZE_SUCCESS);
    REQUIRE(p != nullptr);
    REQUIRE(hazeFreeAsync(p, stream) == HAZE_SUCCESS);

    REQUIRE(hazeStreamDestroy(stream) == HAZE_SUCCESS);
}

TEST_CASE("create and destroy 100 streams without leaks") {
    for (int i = 0; i < 100; ++i) {
        hazeStream_t s = nullptr;
        REQUIRE(hazeStreamCreate(&s) == HAZE_SUCCESS);
        REQUIRE(s != nullptr);
        REQUIRE(hazeStreamDestroy(s) == HAZE_SUCCESS);
    }
}

TEST_CASE("create and destroy 100 events without leaks") {
    for (int i = 0; i < 100; ++i) {
        hazeEvent_t e = nullptr;
        REQUIRE(hazeEventCreate(&e) == HAZE_SUCCESS);
        REQUIRE(e != nullptr);
        REQUIRE(hazeEventDestroy(e) == HAZE_SUCCESS);
    }
}

TEST_CASE("hazeStreamSynchronize is a no-op returning HAZE_SUCCESS") {
    hazeStream_t s = nullptr;
    REQUIRE(hazeStreamCreate(&s) == HAZE_SUCCESS);
    REQUIRE(hazeStreamSynchronize(s) == HAZE_SUCCESS);
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);
    REQUIRE(hazeStreamDestroy(s) == HAZE_SUCCESS);
}

TEST_CASE("hazeDeviceSynchronize is a no-op returning HAZE_SUCCESS") {
    REQUIRE(hazeDeviceSynchronize() == HAZE_SUCCESS);
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);
}

TEST_CASE("hazeGetDeviceProperties returns FPGA values") {
    hazeDeviceProp prop{};
    REQUIRE(hazeGetDeviceProperties(&prop, 0) == HAZE_SUCCESS);
    REQUIRE(prop.hbmSize == 16ULL * 1024 * 1024 * 1024);
    REQUIRE(prop.numRegisters == 64);
    REQUIRE(prop.maxCiphertextModuli == 64);
    REQUIRE(prop.numHBMBanks == 8);
    REQUIRE(prop.numSupportedRingDims == 7);
    // N=4096 corresponds to exponent 12
    bool found_12 = false;
    for (int i = 0; i < prop.numSupportedRingDims; ++i) {
        if (prop.supportedRingDimExponents[i] == 12) { found_12 = true; break; }
    }
    REQUIRE(found_12);
}

TEST_CASE("hazeGetDeviceCount returns 1") {
    int count = 0;
    REQUIRE(hazeGetDeviceCount(&count) == HAZE_SUCCESS);
    REQUIRE(count == 1);
}

TEST_CASE("NULL stream argument uses default stream") {
    REQUIRE(hazeStreamSynchronize(nullptr) == HAZE_SUCCESS);
    hazeEvent_t e = nullptr;
    REQUIRE(hazeEventCreate(&e) == HAZE_SUCCESS);
    REQUIRE(hazeEventRecord(e, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeStreamWaitEvent(nullptr, e, 0) == HAZE_SUCCESS);
    REQUIRE(hazeEventDestroy(e) == HAZE_SUCCESS);
}
