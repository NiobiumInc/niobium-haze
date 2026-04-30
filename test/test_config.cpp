// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
#include <catch2/catch_test_macros.hpp>
#include <haze/haze.h>       // IWYU pragma: keep
#include <haze/haze_types.h> // IWYU pragma: keep

// HAZE's public C API exposes setters for ring dimension, ciphertext
// moduli, twiddle factors, program info, and target — but no
// corresponding getters (see include/haze/haze.h:94-110). For accept
// tests, we therefore can only assert that the call returned
// HAZE_SUCCESS; the value's presence in internal state is not
// observable through the public API. Each accept test below carries a
// short note where read-back would otherwise belong.

TEST_CASE("hazeSetRingDimension accepts valid power-of-2 dimensions", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    // No public getter for ring dimension; HAZE_SUCCESS is the only assertion.
    REQUIRE(hazeSetRingDimension(8192) == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(16384) == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(32768) == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(65536) == HAZE_SUCCESS);
}

TEST_CASE("hazeSetRingDimension rejects unsupported dimensions", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(0) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeSetRingDimension(3000) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeSetRingDimension(5000) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeSetRingDimension(100) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
}

TEST_CASE("hazeSetCiphertextModulus stores values", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    // Two distinct NTT-friendly primes (q ≡ 1 mod 2N) for N=4096; the
    // same constants are used by test_basis_convert.cpp.
    constexpr uint64_t kQ0 = 576460752303415297ULL;
    constexpr uint64_t kQ1 = 576460752303439873ULL;
    REQUIRE(hazeSetCiphertextModulus(0, kQ0) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(1, kQ1) == HAZE_SUCCESS);
    // No public getter for the moduli table; HAZE_SUCCESS is the only assertion.
}

TEST_CASE("hazeSetCiphertextModulus rejects negative index", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(-1, 1) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
}

TEST_CASE("hazeSetCiphertextModulus rejects zero modulus", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(0, 0) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
}

TEST_CASE("hazeSetTwiddleFactors accepts valid indices", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetTwiddleFactors(0, 3) == HAZE_SUCCESS);
    REQUIRE(hazeSetTwiddleFactors(1, 5) == HAZE_SUCCESS);
}

TEST_CASE("hazeSetTwiddleFactors rejects negative index", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetTwiddleFactors(-1, 3) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
}

TEST_CASE("hazeConfigureDevice fails without ring dimension", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
}

TEST_CASE("hazeConfigureDevice succeeds when ring dimension is set", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);
}

TEST_CASE("hazeSetProgramInfo accepts well-formed strings", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetProgramInfo("my-program", "1.2.3", "experimental run") == HAZE_SUCCESS);
    // No public getter for program info; HAZE_SUCCESS is the only assertion.
}

TEST_CASE("hazeSetProgramInfo rejects null arguments", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetProgramInfo(nullptr, "1.0", "desc") == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeSetProgramInfo("name", nullptr, "desc") == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeSetProgramInfo("name", "1.0", nullptr) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
}

TEST_CASE("hazeSetTarget accepts a target string", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetTarget("FHE_SIM") == HAZE_SUCCESS);
    // No public getter for target; HAZE_SUCCESS is the only assertion.
}

TEST_CASE("hazeSetTarget rejects null", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetTarget(nullptr) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
}

TEST_CASE("hazeDeviceReset returns success and re-permits configuration", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    // After reset, configure_device should fail again until ring_dim re-set.
    REQUIRE(hazeConfigureDevice() == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);
}
