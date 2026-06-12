// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Concurrency smoke test for the deferred-tape record path: two threads
// record compute on DISJOINT buffers (the supported contract — see the
// hazeFlush concurrency block in haze.h), then one flush materializes
// both results. Run under -DHAZE_TSAN=ON this exercises the
// BindingTable atomics and the Graph append mutex; functionally it
// checks that interleaved recording from two threads still produces
// both correct outputs (tape order between threads is arbitrary, but
// each thread's dataflow must be intact).

#include "integration_helpers.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <thread>
#include <vector>

namespace {

// One thread's workload: upload two residues, add them, tag the result.
// Returns the dst pointer through `out`; REQUIRE cannot be used off the
// main thread (Catch2 assertions are not thread-safe), so failures are
// reported through `ok`.
void record_add_chain(uint64_t q, uint64_t seed, std::size_t n, void **out, bool *ok) {
    *ok = false;
    const std::vector<uint64_t> a = haze::test::make_residue(q, seed, n);
    const std::vector<uint64_t> b = haze::test::make_residue(q, seed + 1, n);
    const std::size_t bytes = n * sizeof(uint64_t);

    void *d_a = nullptr;
    void *d_b = nullptr;
    void *d_dst = nullptr;
    if (hazeMalloc(haze::test::ctx(), &d_a, bytes) != HAZE_SUCCESS ||
        hazeMalloc(haze::test::ctx(), &d_b, bytes) != HAZE_SUCCESS ||
        hazeMalloc(haze::test::ctx(), &d_dst, bytes) != HAZE_SUCCESS)
        return;
    if (hazeMemcpy(haze::test::ctx(), d_a, a.data(), bytes, HAZE_MEMCPY_HOST_TO_DEVICE) !=
            HAZE_SUCCESS ||
        hazeMemcpy(haze::test::ctx(), d_b, b.data(), bytes, HAZE_MEMCPY_HOST_TO_DEVICE) !=
            HAZE_SUCCESS)
        return;
    if (hazeAdd(haze::test::ctx(), d_dst, d_a, d_b, 0, nullptr) != HAZE_SUCCESS)
        return;
    if (hazeTagOutput(haze::test::ctx(), d_dst) != HAZE_SUCCESS)
        return;
    *out = d_dst;
    *ok = true;
}

} // namespace

TEST_CASE("two threads record on disjoint buffers; one flush materializes both",
          "[integration][threads]") {
    constexpr std::size_t kN = 4096;
    const uint64_t q = haze::test::setup_integration_compute_config(kN);

    void *dst_a = nullptr;
    void *dst_b = nullptr;
    bool ok_a = false;
    bool ok_b = false;
    std::thread t_a(record_add_chain, q, 100, kN, &dst_a, &ok_a);
    std::thread t_b(record_add_chain, q, 200, kN, &dst_b, &ok_b);
    t_a.join();
    t_b.join();
    REQUIRE(ok_a);
    REQUIRE(ok_b);

    REQUIRE(hazeFlush(haze::test::ctx()) == HAZE_SUCCESS);

    // Each thread's chain must be internally consistent regardless of
    // how the two threads' nodes interleaved on the tape.
    const auto check = [&](void *dst, uint64_t seed) {
        const std::vector<uint64_t> a = haze::test::make_residue(q, seed, kN);
        const std::vector<uint64_t> b = haze::test::make_residue(q, seed + 1, kN);
        std::vector<uint64_t> got(kN);
        REQUIRE(hazeMemcpy(haze::test::ctx(), got.data(), dst, kN * sizeof(uint64_t),
                           HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
        for (std::size_t i = 0; i < kN; ++i) {
            const uint64_t expected = (a[i] + b[i]) % q;
            if (got[i] != expected) {
                INFO("mismatch at slot " << i << " for seed " << seed);
                REQUIRE(got[i] == expected);
            }
        }
    };
    check(dst_a, 100);
    check(dst_b, 200);

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}
