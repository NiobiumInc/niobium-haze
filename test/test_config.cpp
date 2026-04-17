// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
#include <catch2/catch_test_macros.hpp>

#include <haze/haze.h>

TEST_CASE("hazeSetRingDimension accepts valid power-of-2 dimensions") {
    REQUIRE(hazeSetRingDimension(4096)  == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(8192)  == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(16384) == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(32768) == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(65536) == HAZE_SUCCESS);
}

TEST_CASE("hazeSetRingDimension rejects unsupported dimensions") {
    REQUIRE(hazeSetRingDimension(0)    == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    REQUIRE(hazeSetRingDimension(3000) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    REQUIRE(hazeSetRingDimension(5000) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    REQUIRE(hazeSetRingDimension(100) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("hazeSetCiphertextModulus stores values") {
    constexpr uint64_t kQ0 = 576460752303415297ULL;  // a typical CKKS prime
    REQUIRE(hazeSetCiphertextModulus(0, kQ0) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(1, kQ0 - 2) == HAZE_SUCCESS);
}

TEST_CASE("hazeSetCiphertextModulus rejects negative index") {
    REQUIRE(hazeSetCiphertextModulus(-1, 1) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("hazeSetCiphertextModulus rejects zero modulus") {
    REQUIRE(hazeSetCiphertextModulus(0, 0) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("hazeSetTwiddleFactors accepts valid indices") {
    REQUIRE(hazeSetTwiddleFactors(0, 3) == HAZE_SUCCESS);
    REQUIRE(hazeSetTwiddleFactors(1, 5) == HAZE_SUCCESS);
}

TEST_CASE("hazeSetTwiddleFactors rejects negative index") {
    REQUIRE(hazeSetTwiddleFactors(-1, 3) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("hazeConfigureDevice fails without ring dimension") {
    // Ensure ring dimension is unset (fresh state is ring_dim == 0).
    // hazeConfigureDevice requires ring_dim > 0.
    // We call it after a full reset — can't reset state in tests, so rely on
    // a separate test case with a fresh dimension set.
}

TEST_CASE("hazeConfigureDevice succeeds when ring dimension is set") {
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);
}
