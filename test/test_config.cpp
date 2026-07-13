// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>       // IWYU pragma: keep
#include <haze/haze_types.h> // IWYU pragma: keep
#include <vector>

// HAZE takes the whole configuration in one hazeConfigureDevice call
// (hazeFheParams + optional hazeReplayConfig) — the caller owns the structs and
// no config state is held before the call. There are no getters, so accept
// tests can only assert HAZE_SUCCESS. Per-argument problems (a bad ring
// dimension, a zero modulus, too many moduli) surface as
// HAZE_ERROR_INVALID_VALUE; whole-config invariants (duplicate moduli,
// reconfiguring under live allocations) as HAZE_ERROR_CONFIGERR. Nothing is
// installed on a failed configure.

TEST_CASE("hazeConfigureDevice accepts a well-formed config", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    // Two distinct NTT-friendly primes (q ≡ 1 mod 2N) for N=4096; the same
    // constants are used by test_basis_convert.cpp.
    constexpr uint64_t kQ0 = 576460752303415297ULL;
    constexpr uint64_t kQ1 = 576460752303439873ULL;
    const uint64_t moduli[] = {kQ0, kQ1};
    const uint64_t twiddle[] = {3, 5};
    const hazeFheParams fhe = {.ring_dim = 4096,
                               .moduli = moduli,
                               .moduli_count = 2,
                               .twiddle_generators = twiddle,
                               .twiddle_count = 2};
    const hazeReplayConfig replay = {.target = "FHE_SIM",
                                     .program_name = "my-program",
                                     .program_version = "1.2.3",
                                     .program_description = "experimental run"};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_SUCCESS);
    // No public getters; HAZE_SUCCESS is the only assertion. The "rejects ..."
    // cases below cover the validation gates.
}

TEST_CASE("hazeConfigureDevice accepts every supported ring dimension", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    for (uint64_t n : {4096ULL, 8192ULL, 16384ULL, 32768ULL, 65536ULL}) {
        const hazeFheParams fhe = {.ring_dim = n};
        REQUIRE(hazeConfigureDevice(&fhe, nullptr) == HAZE_SUCCESS);
    }
}

TEST_CASE("hazeConfigureDevice defaults the replay config when null", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const hazeFheParams fhe = {.ring_dim = 4096};
    REQUIRE(hazeConfigureDevice(&fhe, nullptr) == HAZE_SUCCESS);
}

TEST_CASE("hazeConfigureDevice rejects a null hazeFheParams", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice(nullptr, nullptr) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
}

TEST_CASE("hazeConfigureDevice rejects a null moduli array with a non-zero count", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const hazeFheParams fhe = {.ring_dim = 4096, .moduli = nullptr, .moduli_count = 2};
    REQUIRE(hazeConfigureDevice(&fhe, nullptr) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
}

TEST_CASE("hazeConfigureDevice rejects unsupported ring dimensions", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    // 0 (unset), and non-power-of-two / out-of-envelope values.
    for (uint64_t n : {0ULL, 100ULL, 3000ULL, 5000ULL}) {
        const hazeFheParams fhe = {.ring_dim = n};
        REQUIRE(hazeConfigureDevice(&fhe, nullptr) == HAZE_ERROR_INVALID_VALUE);
        REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    }
}

TEST_CASE("hazeConfigureDevice rejects a zero modulus", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const uint64_t moduli[] = {12289, 0};
    const hazeFheParams fhe = {.ring_dim = 4096, .moduli = moduli, .moduli_count = 2};
    REQUIRE(hazeConfigureDevice(&fhe, nullptr) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
}

TEST_CASE("hazeConfigureDevice rejects too many ciphertext moduli", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    // One past the device envelope (hazeDeviceProp::maxCiphertextModuli == 64);
    // all distinct and non-zero, so only the count trips the rejection.
    std::vector<uint64_t> moduli(65);
    for (std::size_t i = 0; i < moduli.size(); ++i)
        moduli[i] = 12289 + (i * 2);
    const hazeFheParams fhe = {
        .ring_dim = 4096, .moduli = moduli.data(), .moduli_count = moduli.size()};
    REQUIRE(hazeConfigureDevice(&fhe, nullptr) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
}

TEST_CASE("hazeConfigureDevice rejects duplicate ciphertext moduli", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const uint64_t dup[] = {12289, 12289};
    const hazeFheParams bad = {.ring_dim = 4096, .moduli = dup, .moduli_count = 2};
    // Each modulus is per-slot valid but collides on the whole-config uniqueness
    // check, which lives in build().
    REQUIRE(hazeConfigureDevice(&bad, nullptr) == HAZE_ERROR_CONFIGERR);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_CONFIGERR);
    // Nothing is installed on failure, so a corrected config configures cleanly.
    const uint64_t ok[] = {12289, 40961};
    const hazeFheParams good = {.ring_dim = 4096, .moduli = ok, .moduli_count = 2};
    REQUIRE(hazeConfigureDevice(&good, nullptr) == HAZE_SUCCESS);
}

TEST_CASE("hazeDeviceReset re-permits configuration", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const hazeFheParams fhe = {.ring_dim = 4096};
    REQUIRE(hazeConfigureDevice(&fhe, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    // After reset a fresh configure still succeeds.
    REQUIRE(hazeConfigureDevice(&fhe, nullptr) == HAZE_SUCCESS);
}
