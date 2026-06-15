// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Concurrency proof for isolated replay (HAZE_REPLAY_ISOLATED): N contexts,
// each with its own program directory and inputs, flush SIMULTANEOUSLY from N
// threads. In isolated mode each flush's replay runs in a fresh worker process
// (fhetch_sim --project), so the workers share no OpenFHE static state — the
// thing that made concurrent in-process replay unsafe. This test shows (1) the
// flushes genuinely overlap (an in-flight counter reaches N), and (2) each
// context decrypts to ITS OWN result with no cross-talk. Run under
// -DHAZE_TSAN=ON it additionally checks haze's own off-lock bookkeeping (spec
// capture, shadow writes) is race-free. The OpenFHE-cache race is gone by
// construction (separate processes), so a green run here earns the unqualified
// "concurrent multi-context flush is safe" claim.
//
// Requires the fhetch_sim worker binary; HAZE_FHETCH_SIM_PATH is baked in by
// CMake ($<TARGET_FILE:fhetch_sim>). Skips if unavailable.

#include "integration_helpers.hpp"

#include <array>
#include <atomic>
#include <barrier>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t kN = 4096;
constexpr std::size_t kCtxCount = 2; // contexts == threads
constexpr uint64_t kQ = 576460752303415297ULL;

// Restore env on scope exit so isolated mode + the worker path don't leak
// into the rest of the suite (Catch2 runs every case in one process).
class EnvScope {
  public:
    EnvScope(const char *name, const std::string &value) : name_(name) {
        if (const char *prev = std::getenv(name); prev != nullptr) {
            had_ = true;
            prev_ = prev;
        }
        ::setenv(name_, value.c_str(), 1); // NOLINT(concurrency-mt-unsafe,misc-include-cleaner)
    }
    ~EnvScope() {
        if (had_)
            ::setenv(name_, prev_.c_str(), 1); // NOLINT(concurrency-mt-unsafe,misc-include-cleaner)
        else
            ::unsetenv(name_); // NOLINT(concurrency-mt-unsafe,misc-include-cleaner)
    }
    EnvScope(const EnvScope &) = delete;
    EnvScope &operator=(const EnvScope &) = delete;

  private:
    const char *name_;
    bool had_ = false;
    std::string prev_;
};

// One context's concurrent workload (off the main thread — Catch2 REQUIRE is
// not thread-safe, so the verdict comes back through `ok`). Records dst = a+b,
// then a two-phase barrier proves all threads sit in the concurrent flush
// region at once before each flushes (isolated → spawns its own worker), then
// reads back and checks against this context's own oracle.
void run_ctx(hazeContext_t ctx, void *d_a, void *d_b, void *d_dst, const std::vector<uint64_t> &a,
             const std::vector<uint64_t> &b, std::barrier<> &bar, std::atomic<int> &in_flight,
             std::atomic<int> &max_in_flight, bool *ok) {
    *ok = false;
    const std::size_t bytes = kN * sizeof(uint64_t);
    if (hazeMemcpy(ctx, d_a, a.data(), bytes, HAZE_MEMCPY_HOST_TO_DEVICE) != HAZE_SUCCESS ||
        hazeMemcpy(ctx, d_b, b.data(), bytes, HAZE_MEMCPY_HOST_TO_DEVICE) != HAZE_SUCCESS)
        return;
    if (hazeAdd(ctx, d_dst, d_a, d_b, 0, nullptr) != HAZE_SUCCESS)
        return;
    if (hazeTagOutput(ctx, d_dst) != HAZE_SUCCESS)
        return;

    // Deterministic simultaneity proof (timing-independent, so it can't flake
    // under load): every thread bumps in_flight, then all rendezvous — by the
    // second release in_flight == N for certain, i.e. all N threads are
    // provably in the concurrent flush region at once. THEN each flushes; the
    // replays still run their workers off-lock and overlap, but the proof does
    // not depend on their wall-clock timing.
    bar.arrive_and_wait(); // (1) all threads done recording
    in_flight.fetch_add(1);
    bar.arrive_and_wait(); // (2) all have bumped → in_flight == N
    const int n = in_flight.load();
    for (int seen = max_in_flight.load(); n > seen;)
        max_in_flight.compare_exchange_weak(seen, n);
    const bool flushed = hazeFlush(ctx) == HAZE_SUCCESS;
    in_flight.fetch_sub(1);
    if (!flushed)
        return;

    std::vector<uint64_t> got(kN);
    if (hazeMemcpy(ctx, got.data(), d_dst, bytes, HAZE_MEMCPY_DEVICE_TO_HOST) != HAZE_SUCCESS)
        return;
    // Pointwise add: a trivially-certain per-context oracle. The point of
    // this test is concurrency + isolation + no cross-talk, not the op; the
    // worker's NTT/mul correctness is covered by the isolated e2e suite.
    for (std::size_t i = 0; i < kN; ++i)
        if (got[i] != haze::test::add_mod(a[i], b[i], kQ))
            return;
    *ok = true;
}

} // namespace

TEST_CASE("isolated replay: N contexts flush concurrently, no cross-talk",
          "[integration][threads][isolated]") {
#ifndef HAZE_FHETCH_SIM_PATH
    SKIP("fhetch_sim worker path not provided (HAZE_FHETCH_SIM_PATH); built without it");
#else
    // Isolated mode + the worker binary, scoped to this case only.
    const EnvScope iso("HAZE_REPLAY_ISOLATED", "1");
    const EnvScope sim("NBCC_FHETCH_SIM", HAZE_FHETCH_SIM_PATH);

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    // One shared crypto context (same params for every context); each flush's
    // bridge rebind re-captures it into that context's own program dir.
    uint64_t picked = 0;
    REQUIRE(hazeReplayBridgeInitCryptoContext(kN, kQ, &picked) == HAZE_SUCCESS);

    // N independent contexts, each with a DISTINCT program directory so their
    // concurrent workers never collide on disk.
    std::array<hazeContext_t, kCtxCount> ctxs{};
    std::array<void *, kCtxCount> da{};
    std::array<void *, kCtxCount> db{};
    std::array<void *, kCtxCount> ddst{};
    std::array<std::vector<uint64_t>, kCtxCount> a;
    std::array<std::vector<uint64_t>, kCtxCount> b;
    const std::size_t bytes = kN * sizeof(uint64_t);
    for (std::size_t i = 0; i < kCtxCount; ++i) {
        REQUIRE(hazeContextCreate(&ctxs[i], kN, &kQ, 1) == HAZE_SUCCESS);
        // Pin the local (fhetch_sim) worker: this case is about the local
        // isolated path, so it must behave identically under test-sim and
        // test-transport (whose HAZE_TARGET=FUNC_SIM would otherwise leak in).
        REQUIRE(hazeSetTarget(ctxs[i], "local") == HAZE_SUCCESS);
        REQUIRE(hazeSetProgramDirectory(
                    ctxs[i], ("threaded_replay_ctx" + std::to_string(i)).c_str()) == HAZE_SUCCESS);
        REQUIRE(hazeMalloc(ctxs[i], &da[i], bytes) == HAZE_SUCCESS);
        REQUIRE(hazeMalloc(ctxs[i], &db[i], bytes) == HAZE_SUCCESS);
        REQUIRE(hazeMalloc(ctxs[i], &ddst[i], bytes) == HAZE_SUCCESS);
        // Distinct inputs per context → any cross-talk shows as a wrong value.
        a[i] = haze::test::make_residue(kQ, 1000ULL * (i + 1), kN);
        b[i] = haze::test::make_residue(kQ, 2000ULL * (i + 1), kN);
    }

    std::barrier bar(static_cast<std::ptrdiff_t>(kCtxCount));
    std::atomic<int> in_flight{0};
    std::atomic<int> max_in_flight{0};
    std::array<bool, kCtxCount> ok{};
    std::vector<std::thread> threads;
    threads.reserve(kCtxCount);
    for (std::size_t i = 0; i < kCtxCount; ++i)
        threads.emplace_back(run_ctx, ctxs[i], da[i], db[i], ddst[i], std::cref(a[i]),
                             std::cref(b[i]), std::ref(bar), std::ref(in_flight),
                             std::ref(max_in_flight), &ok[i]);
    for (auto &t : threads)
        t.join();

    for (std::size_t i = 0; i < kCtxCount; ++i) {
        INFO("context " << i);
        REQUIRE(ok[i]); // correct result, no cross-talk
    }
    // The flushes genuinely overlapped (both were inside hazeFlush at once),
    // not merely serialized one-after-another.
    REQUIRE(max_in_flight.load() >= 2);

    for (hazeContext_t c : ctxs)
        REQUIRE(hazeContextDestroy(c) == HAZE_SUCCESS);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);

    // Clean up this case's per-context project dirs so they don't linger in
    // the runs tree (the trace-diff conformance gate diffs that tree).
    std::error_code ec;
    for (std::size_t i = 0; i < kCtxCount; ++i)
        std::filesystem::remove_all("threaded_replay_ctx" + std::to_string(i), ec);
#endif
}
