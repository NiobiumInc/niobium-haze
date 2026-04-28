// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
#include <catch2/catch_test_macros.hpp>

#include <haze/haze.h>        // IWYU pragma: keep
#include <haze/haze_types.h>  // IWYU pragma: keep

TEST_CASE("hazeSetRingDimension accepts valid power-of-2 dimensions") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096)  == HAZE_SUCCESS);
    // RYANPR: You should be able to check here that you get the right answer
    REQUIRE(hazeSetRingDimension(8192)  == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(16384) == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(32768) == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(65536) == HAZE_SUCCESS);
}

TEST_CASE("hazeSetRingDimension rejects unsupported dimensions") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(0)    == HAZE_ERROR_INVALID_VALUE);
    // RYANPR: Check that this is the correct last error.
    hazeGetLastError();
    REQUIRE(hazeSetRingDimension(3000) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    REQUIRE(hazeSetRingDimension(5000) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    REQUIRE(hazeSetRingDimension(100) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("hazeSetCiphertextModulus stores values") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    constexpr uint64_t kQ0 = 576460752303415297ULL;  // a typical CKKS prime
    REQUIRE(hazeSetCiphertextModulus(0, kQ0) == HAZE_SUCCESS);
    // RYANPR: This might not be a prime, use a second one. Also be able to get the prime to check it is actually set.
    REQUIRE(hazeSetCiphertextModulus(1, kQ0 - 2) == HAZE_SUCCESS);
}

TEST_CASE("hazeSetCiphertextModulus rejects negative index") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(-1, 1) == HAZE_ERROR_INVALID_VALUE);
    // RYANPR: Check the error is HAZE_ERROR_INVALID_VALUE
    hazeGetLastError();
}

TEST_CASE("hazeSetCiphertextModulus rejects zero modulus") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(0, 0) == HAZE_ERROR_INVALID_VALUE);
    // RYANPR: Check the error is HAZE_ERROR_INVALID_VALUE. Do that with all tests in this file.
    hazeGetLastError();
}

TEST_CASE("hazeSetTwiddleFactors accepts valid indices") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetTwiddleFactors(0, 3) == HAZE_SUCCESS);
    REQUIRE(hazeSetTwiddleFactors(1, 5) == HAZE_SUCCESS);
}

TEST_CASE("hazeSetTwiddleFactors rejects negative index") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetTwiddleFactors(-1, 3) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("hazeConfigureDevice fails without ring dimension") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("hazeConfigureDevice succeeds when ring dimension is set") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);
}

TEST_CASE("hazeSetProgramInfo accepts well-formed strings") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    // RYANPR: Check that this is correctly set
    REQUIRE(hazeSetProgramInfo("my-program", "1.2.3", "experimental run") == HAZE_SUCCESS);
}

TEST_CASE("hazeSetProgramInfo rejects null arguments") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetProgramInfo(nullptr, "1.0", "desc") == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    REQUIRE(hazeSetProgramInfo("name", nullptr, "desc") == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    REQUIRE(hazeSetProgramInfo("name", "1.0", nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("hazeSetTarget accepts a target string") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    // RYANPR: Check this is correctly set
    REQUIRE(hazeSetTarget("FHE_SIM") == HAZE_SUCCESS);
}

TEST_CASE("hazeSetTarget rejects null") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetTarget(nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("hazeDeviceReset returns success and re-permits configuration") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    // After reset, configure_device should fail again until ring_dim re-set.
    REQUIRE(hazeConfigureDevice() == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);
}
