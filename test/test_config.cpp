// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
#include "integration_helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <haze/haze.h>       // IWYU pragma: keep
#include <haze/haze_types.h> // IWYU pragma: keep

// The FHE parameters (ring dimension, modulus chain) are fixed by
// hazeContextCreate — there is no piecewise configure step and no
// getters; creation success/failure is the observable contract. The
// per-context metadata setters (program info, target, data format)
// remain and are validated here.

TEST_CASE("hazeContextCreate accepts every supported ring dimension", "[unit]") {
    constexpr uint64_t kQ0 = 576460752303415297ULL;
    constexpr uint64_t kQ1 = 576460752303439873ULL;
    for (uint64_t n : {4096ULL, 8192ULL, 16384ULL, 32768ULL, 65536ULL}) {
        const uint64_t moduli[] = {kQ0, kQ1};
        hazeContext_t ctx = nullptr;
        REQUIRE(hazeContextCreate(&ctx, n, moduli, 2) == HAZE_SUCCESS);
        REQUIRE(hazeContextDestroy(ctx) == HAZE_SUCCESS);
    }
}

TEST_CASE("hazeContextCreate rejects unsupported ring dimensions", "[unit]") {
    constexpr uint64_t kQ0 = 576460752303415297ULL;
    const uint64_t moduli[] = {kQ0};
    for (uint64_t n : {0ULL, 100ULL, 3000ULL, 5000ULL}) {
        hazeContext_t ctx = nullptr;
        REQUIRE(hazeContextCreate(&ctx, n, moduli, 1) == HAZE_ERROR_INVALID_VALUE);
        REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    }
}

TEST_CASE("hazeContextCreate rejects a zero modulus anywhere in the chain", "[unit]") {
    constexpr uint64_t kQ0 = 576460752303415297ULL;
    const uint64_t moduli[] = {kQ0, 0};
    hazeContext_t ctx = nullptr;
    REQUIRE(hazeContextCreate(&ctx, 4096, moduli, 2) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
}

TEST_CASE("metadata setters accept well-formed inputs", "[unit]") {
    haze::test::recreate_ctx(4096, {});
    REQUIRE(hazeSetProgramInfo(haze::test::ctx(), "my-program", "1.2.3", "experimental run") ==
            HAZE_SUCCESS);
    REQUIRE(hazeSetTarget(haze::test::ctx(), "FHE_SIM") == HAZE_SUCCESS);
    // No public getters; HAZE_SUCCESS is the only assertion. The
    // companion "rejects ..." TEST_CASEs cover the validation gates.
}

TEST_CASE("hazeSetProgramInfo rejects null arguments", "[unit]") {
    haze::test::recreate_ctx(4096, {});
    REQUIRE(hazeSetProgramInfo(haze::test::ctx(), nullptr, "1.0", "desc") ==
            HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeSetProgramInfo(haze::test::ctx(), "name", nullptr, "desc") ==
            HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeSetProgramInfo(haze::test::ctx(), "name", "1.0", nullptr) ==
            HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
}

TEST_CASE("hazeSetTarget rejects null", "[unit]") {
    haze::test::recreate_ctx(4096, {});
    REQUIRE(hazeSetTarget(haze::test::ctx(), nullptr) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
}

TEST_CASE("metadata setters reject a null context", "[unit]") {
    REQUIRE(hazeSetProgramInfo(nullptr, "n", "v", "d") == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeSetTarget(nullptr, "FHE_SIM") == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeSetProgramDirectory(nullptr, "/tmp") == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeSetMontgomery(nullptr, 1) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeSetBitReversal(nullptr, 1) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
}

TEST_CASE("contexts are independent: destroy and recreate freely", "[unit]") {
    constexpr uint64_t kQ0 = 576460752303415297ULL;
    const uint64_t moduli[] = {kQ0};
    hazeContext_t a = nullptr;
    hazeContext_t b = nullptr;
    REQUIRE(hazeContextCreate(&a, 4096, moduli, 1) == HAZE_SUCCESS);
    REQUIRE(hazeContextCreate(&b, 8192, moduli, 1) == HAZE_SUCCESS);
    REQUIRE(a != b);
    REQUIRE(hazeContextDestroy(a) == HAZE_SUCCESS);
    REQUIRE(hazeContextDestroy(b) == HAZE_SUCCESS);
}
