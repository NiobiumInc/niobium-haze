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
#include <catch2/catch_test_macros.hpp>

#include <haze/haze.h>

#include <string_view>
#include <thread>

TEST_CASE("hazeGetLastError returns HAZE_SUCCESS by default") {
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);
}

TEST_CASE("hazeGetLastError clears after read") {
    // Use a graph stub to set a real error, then verify clear-on-read semantics.
    const hazeError_t ignored = hazeBeginCapture(nullptr);
    (void)ignored;
    REQUIRE(hazeGetLastError() == HAZE_ERROR_NOT_SUPPORTED);
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);
}

TEST_CASE("hazeGetErrorString returns \"unknown error\" for out-of-range codes") {
    REQUIRE(std::string_view(hazeGetErrorString(static_cast<hazeError_t>(999)))
            == "unknown error");
}

TEST_CASE("hazeGetDeviceCount compiles and links") {
    int count = -1;
    REQUIRE(hazeGetDeviceCount(&count) == HAZE_SUCCESS);
    REQUIRE(count == 0);
}

// Graph capture returns NOT_SUPPORTED rather than silently no-op'ing on
// purpose: a no-op hazeBeginCapture would hand back a bogus SUCCESS, and
// the eventual hazeEndCapture would give the caller a null/empty graph
// that looks real — corrupting any graph-replay code path. An explicit
// error surfaces the missing feature immediately.
TEST_CASE("graph API returns HAZE_ERROR_NOT_SUPPORTED") {
    REQUIRE(hazeBeginCapture(nullptr)          == HAZE_ERROR_NOT_SUPPORTED);
    hazeGetLastError();
    REQUIRE(hazeEndCapture(nullptr, nullptr)   == HAZE_ERROR_NOT_SUPPORTED);
    hazeGetLastError();
    REQUIRE(hazeGraphDestroy(nullptr)          == HAZE_ERROR_NOT_SUPPORTED);
    hazeGetLastError();
}

TEST_CASE("multi-device stubs return HAZE_ERROR_NOT_SUPPORTED") {
    int can_access = -1;
    REQUIRE(hazeDeviceCanAccessPeer(&can_access, 0, 1) == HAZE_ERROR_NOT_SUPPORTED);
    hazeGetLastError();
    REQUIRE(hazeDeviceEnablePeerAccess(1, 0) == HAZE_ERROR_NOT_SUPPORTED);
    hazeGetLastError();
}

TEST_CASE("successful stubs do not pollute error state") {
    // Ensure a sequence of successful calls leaves hazeGetLastError as HAZE_SUCCESS.
    void* ptr = nullptr;
    REQUIRE(hazeMalloc(&ptr, 1024)   == HAZE_SUCCESS);
    REQUIRE(hazeFree(ptr)            == HAZE_SUCCESS);
    REQUIRE(hazeGetLastError()       == HAZE_SUCCESS);
}

// CUDA exposes a thread-local error state via cudaGetLastError with
// clear-on-read semantics; HAZE mirrors that so FIDESlib's error-handling
// idioms port unchanged. Functions also return their error directly — the
// thread-local state is a convenience, not the primary error channel.
// Reference: https://parallelprogrammer.substack.com/p/cuda-error-handling-a-definitive
TEST_CASE("error state is thread-local") {
    // Set an error in the main thread.
    hazeBeginCapture(nullptr);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_NOT_SUPPORTED);

    // Error from another thread must not bleed into main thread.
    hazeError_t child_err = HAZE_SUCCESS;
    std::thread t([&child_err] {
        // Child thread has its own clean error state.
        child_err = hazeGetLastError();
        // Set an error from the child; main thread must not see it.
        hazeBeginCapture(nullptr);
    });
    t.join();

    REQUIRE(child_err == HAZE_SUCCESS);
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);
}
