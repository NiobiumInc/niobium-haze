// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Regression coverage for the 2026-07 invariant review: each case pins a
// contract that was previously violated — stale-shadow reads, unvalidated
// destinations, no-op flush semantics, CRT base validation, sticky
// last-error, and the configuration freeze rules.

#include "core/stream.hpp" // haze_stream_s/haze_event_s ids for the reset pin
#include "integration_helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <string_view>
#include <vector>

namespace {

constexpr uint64_t kRingDim = 4096;
constexpr std::size_t kBytes = kRingDim * sizeof(uint64_t);

// Same NTT-friendly primes (q ≡ 1 mod 2N, N=4096) as test_basis_convert.cpp.
constexpr uint64_t kQ0 = 576460752303415297ULL;
constexpr uint64_t kQ1 = 576460752303439873ULL;
constexpr uint64_t kQ2 = 576460752303702017ULL;

// A pointer never returned by hazeMalloc but inside the HBM range (plausible, not
// merely null); the int-to-ptr cast fabricates an unknown device handle.
void *fake_device_ptr() {
    return reinterpret_cast<void *>( // NOLINT(performance-no-int-to-ptr)
        uintptr_t{0x4000000000ULL} + 0x123400);
}

void unit_setup_4096() {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const hazeFheParams fhe = {.ring_dim = kRingDim};
    REQUIRE(hazeConfigureDevice(&fhe, nullptr) == HAZE_SUCCESS);
}

} // namespace

// ---------------------------------------------------------------------------
// Thread-local last-error: CUDA sticky semantics.
// ---------------------------------------------------------------------------

TEST_CASE("last error is sticky: successful calls do not clear it", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    // A failure must survive a string of successes until hazeGetLastError
    // reads-and-clears it (CUDA parity; previously any success wiped it).
    REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_ERROR_NOT_SUPPORTED);
    const hazeFheParams fhe = {.ring_dim = kRingDim};
    REQUIRE(hazeConfigureDevice(&fhe, nullptr) == HAZE_SUCCESS);
    void *p = nullptr;
    REQUIRE(hazeMalloc(&p, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeFree(p) == HAZE_SUCCESS);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_NOT_SUPPORTED);
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);
}

// ---------------------------------------------------------------------------
// Ring-dimension envelope and freeze rules.
// ---------------------------------------------------------------------------

TEST_CASE("hazeConfigureDevice rejects out-of-envelope ring dimensions", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    // Below the device envelope (2^10 .. 2^16).
    const hazeFheParams too_small = {.ring_dim = 512};
    REQUIRE(hazeConfigureDevice(&too_small, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    // Above the envelope; 2^61+ used to wrap n * sizeof(uint64_t) to 0 and
    // report success with a poisoned configuration.
    const hazeFheParams too_big = {.ring_dim = uint64_t{1} << 17};
    REQUIRE(hazeConfigureDevice(&too_big, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    const hazeFheParams wrap = {.ring_dim = uint64_t{1} << 62};
    REQUIRE(hazeConfigureDevice(&wrap, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    // The envelope bounds themselves are accepted.
    const hazeFheParams lo = {.ring_dim = uint64_t{1} << 10};
    REQUIRE(hazeConfigureDevice(&lo, nullptr) == HAZE_SUCCESS);
    const hazeFheParams hi = {.ring_dim = uint64_t{1} << 16};
    REQUIRE(hazeConfigureDevice(&hi, nullptr) == HAZE_SUCCESS);
}

TEST_CASE("hazeConfigureDevice cannot change the ring dimension while allocations are live",
          "[unit]") {
    unit_setup_4096();
    void *p = nullptr;
    REQUIRE(hazeMalloc(&p, kBytes) == HAZE_SUCCESS);
    // A different dimension would leave p's shadow sized under the old one
    // (previously a heap overread on the next D2H).
    const hazeFheParams grow = {.ring_dim = 8192};
    REQUIRE(hazeConfigureDevice(&grow, nullptr) == HAZE_ERROR_CONFIGERR);
    hazeGetLastError();
    // Reconfiguring the same dimension stays a success.
    const hazeFheParams same = {.ring_dim = kRingDim};
    REQUIRE(hazeConfigureDevice(&same, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeFree(p) == HAZE_SUCCESS);
    // With nothing live the dimension is changeable again.
    REQUIRE(hazeConfigureDevice(&grow, nullptr) == HAZE_SUCCESS);
}

TEST_CASE("hazeConfigureDevice after bring-up is rejected", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const uint64_t moduli[] = {kQ0};
    const hazeFheParams fhe = {.ring_dim = kRingDim, .moduli = moduli, .moduli_count = 1};
    const hazeReplayConfig local = {.target = "local"};
    REQUIRE(hazeConfigureDevice(&fhe, &local) == HAZE_SUCCESS);
    void *a = nullptr;
    void *b = nullptr;
    void *c = nullptr;
    REQUIRE(hazeMalloc(&a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&c, kBytes) == HAZE_SUCCESS);
    std::vector<uint64_t> data(kRingDim, 7);
    REQUIRE(hazeMemcpy(a, data.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(b, data.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    // The first compute brings the backend up, latching the config.
    REQUIRE(hazeAdd(c, a, b, 0, nullptr) == HAZE_SUCCESS);
    // A reconfigure could no longer take effect (backend already up); previously
    // a stale target here silently replayed against the baked-in one.
    const hazeReplayConfig other = {.target = "FUNC_SIM"};
    REQUIRE(hazeConfigureDevice(&fhe, &other) == HAZE_ERROR_CONFIGERR);
    hazeGetLastError();
    REQUIRE(hazeFree(a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(b) == HAZE_SUCCESS);
    REQUIRE(hazeFree(c) == HAZE_SUCCESS);
    // hazeDeviceReset re-opens the window.
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice(&fhe, &local) == HAZE_SUCCESS);
}

// ---------------------------------------------------------------------------
// Zero-count transfers and hazeFreeHost validation.
// ---------------------------------------------------------------------------

TEST_CASE("zero-count memcpy/memset are validated no-ops", "[unit]") {
    unit_setup_4096();
    void *p = nullptr;
    REQUIRE(hazeMalloc(&p, kBytes) == HAZE_SUCCESS);
    std::vector<uint64_t> host(kRingDim, 1);

    // Zero-byte H2D must not fabricate an all-zero shadow entry...
    REQUIRE(hazeMemcpy(p, host.data(), 0, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    // ...so a full-size D2H still reports "never written", not zeros.
    REQUIRE(hazeMemcpy(host.data(), p, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) ==
            HAZE_ERROR_NOT_FLUSHED);
    hazeGetLastError();
    // Zero-byte D2H and memset are success no-ops.
    REQUIRE(hazeMemcpy(host.data(), p, 0, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    REQUIRE(hazeMemset(p, 0, 0) == HAZE_SUCCESS);
    REQUIRE(hazeFree(p) == HAZE_SUCCESS);
}

TEST_CASE("hazeFreeHost rejects pointers it did not allocate", "[unit]") {
    unit_setup_4096();
    // The happy path still works.
    void *host = nullptr;
    REQUIRE(hazeHostAlloc(&host, 4096, 0) == HAZE_SUCCESS);
    REQUIRE(hazeFreeHost(host) == HAZE_SUCCESS);
    REQUIRE(hazeFreeHost(nullptr) == HAZE_SUCCESS); // cudaFreeHost(NULL) parity
    // A device handle is a synthetic address — libc free() on it aborted
    // the process before validation was added.
    void *dev = nullptr;
    REQUIRE(hazeMalloc(&dev, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeFreeHost(dev) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    REQUIRE(hazeFree(dev) == HAZE_SUCCESS);
    // An arbitrary foreign pointer is rejected too.
    int on_stack = 0;
    REQUIRE(hazeFreeHost(&on_stack) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

// ---------------------------------------------------------------------------
// Destination validation: compute and D2D.
// ---------------------------------------------------------------------------

TEST_CASE("compute to an unallocated destination is rejected", "[unit]") {
    unit_setup_4096();
    // Destination liveness is validated before anything is recorded, so no
    // sources are needed; previously this recorded into a bogus binding and
    // returned HAZE_SUCCESS, failing only at flush with UNKNOWN_ADDRESS.
    REQUIRE(hazeAdd(fake_device_ptr(), fake_device_ptr(), fake_device_ptr(), 0, nullptr) ==
            HAZE_ERROR_UNKNOWN_ADDRESS);
    hazeGetLastError();
}

TEST_CASE("hazeMemcpy(D2D) validates dst liveness and count", "[unit]") {
    unit_setup_4096();
    void *a = nullptr;
    void *b = nullptr;
    REQUIRE(hazeMalloc(&a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&b, kBytes) == HAZE_SUCCESS);

    // Unallocated destination (count was previously ignored and dst never
    // checked — this returned HAZE_SUCCESS).
    REQUIRE(hazeMemcpy(fake_device_ptr(), a, kBytes, HAZE_MEMCPY_DEVICE_TO_DEVICE) ==
            HAZE_ERROR_UNKNOWN_ADDRESS);
    hazeGetLastError();
    // Partial D2D is unexpressible in the recorded IR.
    REQUIRE(hazeMemcpy(b, a, kBytes / 2, HAZE_MEMCPY_DEVICE_TO_DEVICE) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    // Oversized counts mirror the H2D/D2H size error.
    REQUIRE(hazeMemcpy(b, a, kBytes * 2, HAZE_MEMCPY_DEVICE_TO_DEVICE) == HAZE_ERROR_SIZE_MISMATCH);
    hazeGetLastError();
    // Zero-byte D2D is a validated success no-op.
    REQUIRE(hazeMemcpy(b, a, 0, HAZE_MEMCPY_DEVICE_TO_DEVICE) == HAZE_SUCCESS);

    REQUIRE(hazeFree(a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(b) == HAZE_SUCCESS);
}

// ---------------------------------------------------------------------------
// Failed backend bring-up gates compute (montgomery + local target).
// ---------------------------------------------------------------------------

TEST_CASE("montgomery on local target fails at first compute; shadow round-trips survive",
          "[unit][hwfmt]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const hazeFheParams fhe = {.ring_dim = kRingDim};
    const hazeReplayConfig replay = {.target = "local", .montgomery = 1};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_SUCCESS);

    void *a = nullptr;
    void *b = nullptr;
    void *c = nullptr;
    REQUIRE(hazeMalloc(&a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&c, kBytes) == HAZE_SUCCESS);

    // H2D stays a plain shadow write (no backend needed)...
    std::vector<uint64_t> data(kRingDim, 9);
    REQUIRE(hazeMemcpy(a, data.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(b, data.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    // ...but compute must fail loudly (haze.h: first compute or flush reports
    // NOT_SUPPORTED); previously it returned HAZE_SUCCESS and silently dropped the
    // op.
    REQUIRE(hazeAdd(c, a, b, 0, nullptr) == HAZE_ERROR_NOT_SUPPORTED);
    hazeGetLastError();
    // The compute-free D2H round-trip still works.
    std::vector<uint64_t> out(kRingDim, 0);
    REQUIRE(hazeMemcpy(out.data(), a, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    REQUIRE(out == data);

    REQUIRE(hazeFree(a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(b) == HAZE_SUCCESS);
    REQUIRE(hazeFree(c) == HAZE_SUCCESS);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

// ---------------------------------------------------------------------------
// CRT base validation (MRP compute + basis conversion).
// ---------------------------------------------------------------------------

TEST_CASE("MRP base validation rejects zero, duplicate, and oversized bases", "[unit]") {
    unit_setup_4096();
    void *dst[2] = {nullptr, nullptr};
    const void *src[2] = {nullptr, nullptr};

    // Zero modulus: reaches % 0 (SIGFPE) inside the FBC math if let through.
    const uint64_t zero_base[2] = {kQ0, 0};
    REQUIRE(hazeAddMrp(dst, src, src, zero_base, 2, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    // Duplicate primes: fhetch keys residues by modulus; duplicates alias.
    const uint64_t dup_base[2] = {kQ0, kQ0};
    REQUIRE(hazeAddMrp(dst, src, src, dup_base, 2, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    // Length above the device modulus envelope: previously threw
    // std::length_error through the noexcept ABI (std::terminate).
    std::vector<uint64_t> big_base(65);
    for (std::size_t i = 0; i < big_base.size(); ++i)
        big_base[i] = kQ0 + (2 * i);
    std::vector<void *> big_dst(65, nullptr);
    std::vector<const void *> big_src(65, nullptr);
    REQUIRE(hazeAddMrp(big_dst.data(), big_src.data(), big_src.data(), big_base.data(),
                       big_base.size(), nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    // hazeMemcpyMrp(D2D) applies the same validation.
    REQUIRE(hazeMemcpyMrp(dst, src, kBytes, HAZE_MEMCPY_DEVICE_TO_DEVICE, dup_base, 2) ==
            HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("hazeModDown rejects duplicate primes in rescale_base", "[unit]") {
    unit_setup_4096();
    // {kQ1, kQ1} passes a set-membership subset check but strips only one
    // unique prime — the result would then carry more residues than the
    // caller's dst array holds (out-of-bounds write on the pointer array).
    const uint64_t src_base[3] = {kQ0, kQ1, kQ2};
    const uint64_t rescale_base[2] = {kQ1, kQ1};
    const void *src[3] = {nullptr, nullptr, nullptr};
    void *dst[1] = {nullptr};

    hazeModDownParams p{};
    p.src_base = src_base;
    p.src_base_len = 3;
    p.rescale_base = rescale_base;
    p.rescale_base_len = 2;
    REQUIRE(hazeModDown(dst, src, &p, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("hazeModUp rejects foreign digit primes and src/p overlap", "[unit]") {
    unit_setup_4096();
    const uint64_t src_base[2] = {kQ0, kQ1};
    const void *src[2] = {nullptr, nullptr};
    void *dst[6] = {};

    // Digit base names a prime absent from src_base: mr_subset would throw
    // std::out_of_range through the noexcept frame (std::terminate).
    {
        const uint64_t digit_bases[1] = {kQ2}; // foreign
        const size_t digit_lens[1] = {1};
        const uint64_t p_base[1] = {kQ2 + 2};
        hazeModUpParams p{};
        p.src_base = src_base;
        p.src_base_len = 2;
        p.digit_bases = digit_bases;
        p.digit_bases_total_len = 1;
        p.digit_base_lens = digit_lens;
        p.digit_count = 1;
        p.p_base = p_base;
        p.p_base_len = 1;
        REQUIRE(hazeModUp(dst, src, &p, nullptr) == HAZE_ERROR_INVALID_VALUE);
        hazeGetLastError();
    }
    // p_base sharing a prime with src_base would duplicate a residue key in
    // the per-digit union base.
    {
        const uint64_t digit_bases[1] = {kQ0};
        const size_t digit_lens[1] = {1};
        const uint64_t p_base[1] = {kQ1}; // also in src_base
        hazeModUpParams p{};
        p.src_base = src_base;
        p.src_base_len = 2;
        p.digit_bases = digit_bases;
        p.digit_bases_total_len = 1;
        p.digit_base_lens = digit_lens;
        p.digit_count = 1;
        p.p_base = p_base;
        p.p_base_len = 1;
        REQUIRE(hazeModUp(dst, src, &p, nullptr) == HAZE_ERROR_INVALID_VALUE);
        hazeGetLastError();
    }
}

// ---------------------------------------------------------------------------
// Flush / shadow-eviction semantics (need the bridge + local replay).
// ---------------------------------------------------------------------------

TEST_CASE("untagged flush is a true no-op: recording and bindings survive", "[integration]") {
    const uint64_t q = haze::test::setup_integration_compute_config(kRingDim, kQ0, 0);

    auto a = haze::test::make_residue(q, 3, kRingDim);
    auto b = haze::test::make_residue(q, 5, kRingDim);
    auto devs = haze::test::allocate_and_h2d_residues({a, b});
    auto dst = haze::test::allocate_dst_residues(1, kBytes);
    REQUIRE(hazeAdd(dst[0], devs[0], devs[1], 0, nullptr) == HAZE_SUCCESS);

    // Nothing tagged: this must not tear down the in-flight recording. Previously
    // it half-cleared haze state (a later tag failed with SOURCE_UNAVAILABLE)
    // while the vendor recorder kept the old nodes, contaminating the next epoch.
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    REQUIRE(hazeTagOutput(dst[0]) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);
    std::vector<uint64_t> out(kRingDim, 0);
    REQUIRE(hazeMemcpy(out.data(), dst[0], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    for (std::size_t k = 0; k < kRingDim; ++k) {
        REQUIRE(out[k] == haze::test::add_mod(a[k], b[k], q));
    }

    haze::test::free_all_residues(devs);
    haze::test::free_all_residues(dst);
}

TEST_CASE("compute result evicts stale H2D shadow until flush", "[integration]") {
    const uint64_t q = haze::test::setup_integration_compute_config(kRingDim, kQ0, 0);

    auto a = haze::test::make_residue(q, 3, kRingDim);
    auto b = haze::test::make_residue(q, 5, kRingDim);
    auto sentinel = std::vector<uint64_t>(kRingDim, 0xDEAD);
    auto devs = haze::test::allocate_and_h2d_residues({a, b, sentinel});
    void *dst = devs[2]; // holds the sentinel bytes via H2D

    REQUIRE(hazeAdd(dst, devs[0], devs[1], 0, nullptr) == HAZE_SUCCESS);
    // The addr now names an unmaterialized result — reading it must NOT
    // return the stale sentinel bytes (it silently did, pre-review).
    std::vector<uint64_t> out(kRingDim, 0);
    REQUIRE(hazeMemcpy(out.data(), dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) ==
            HAZE_ERROR_NOT_FLUSHED);
    hazeGetLastError();

    REQUIRE(hazeTagOutput(dst) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(out.data(), dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    for (std::size_t k = 0; k < kRingDim; ++k) {
        REQUIRE(out[k] == haze::test::add_mod(a[k], b[k], q));
    }

    haze::test::free_all_residues(devs);
}

TEST_CASE("D2D copy evicts stale dst shadow until flush", "[integration]") {
    const uint64_t q = haze::test::setup_integration_compute_config(kRingDim, kQ0, 0);

    auto a = haze::test::make_residue(q, 7, kRingDim);
    auto sentinel = std::vector<uint64_t>(kRingDim, 0xBEEF);
    auto devs = haze::test::allocate_and_h2d_residues({a, sentinel});

    REQUIRE(hazeMemcpy(devs[1], devs[0], kBytes, HAZE_MEMCPY_DEVICE_TO_DEVICE) == HAZE_SUCCESS);
    std::vector<uint64_t> out(kRingDim, 0);
    // Pre-review this read returned the stale sentinel bytes.
    REQUIRE(hazeMemcpy(out.data(), devs[1], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) ==
            HAZE_ERROR_NOT_FLUSHED);
    hazeGetLastError();

    // Same transport limitation as "hazeMemcpy(D2D) promotes an H2D'd source":
    // a raw-H2D source has no modulus to recover, so the copy stays sentinel
    // and isn't replayable off-process; the eviction above is already pinned.
    if (const char *target = std::getenv("HAZE_TARGET");
        target != nullptr && target[0] != '\0' && std::string_view{target} != "local") {
        haze::test::free_all_residues(devs);
        SKIP("sentinel-modulus D2D copy is not replayable on transport targets");
    }

    REQUIRE(hazeTagOutput(devs[1]) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(out.data(), devs[1], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    REQUIRE(out == a);

    haze::test::free_all_residues(devs);
}

TEST_CASE("bridge honors a custom program name for cryptocontext.dat", "[integration]") {
    namespace fs = std::filesystem;
    // Pre-review the bridge hard-coded set_program_info("haze"), so a custom
    // program name orphaned cryptocontext.dat under cwd/haze/ while the
    // trace landed in cwd/<name>/ — replay then found no crypto context.
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const uint64_t moduli[] = {kQ0};
    const hazeFheParams fhe = {.ring_dim = kRingDim, .moduli = moduli, .moduli_count = 1};
    const hazeReplayConfig replay = {.target = haze::test::target_from_env(),
                                     .program_name = "haze_custom_prog_review",
                                     .program_version = "0.0",
                                     .program_description = "invariant-review case",
                                     .reduced_noise = 1};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_SUCCESS);
    uint64_t scaffold = 0;
    REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, kQ0, &scaffold) == HAZE_SUCCESS);

    const uint64_t q = kQ0;
    auto a = haze::test::make_residue(q, 11, kRingDim);
    auto b = haze::test::make_residue(q, 13, kRingDim);
    auto devs = haze::test::allocate_and_h2d_residues({a, b});
    auto dst = haze::test::allocate_dst_residues(1, kBytes);
    REQUIRE(hazeAdd(dst[0], devs[0], devs[1], 0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(dst[0]) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    std::vector<uint64_t> out(kRingDim, 0);
    REQUIRE(hazeMemcpy(out.data(), dst[0], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    for (std::size_t k = 0; k < kRingDim; ++k) {
        REQUIRE(out[k] == haze::test::add_mod(a[k], b[k], q));
    }
    // The crypto context sits in the SAME program dir as the trace.
    REQUIRE(fs::exists(fs::path{"haze_custom_prog_review"} / "cryptocontext.dat"));

    haze::test::free_all_residues(devs);
    haze::test::free_all_residues(dst);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    fs::remove_all("haze_custom_prog_review");
}

TEST_CASE("failed materialize clears the epoch: retag fails, next flush is a no-op", "[unit]") {
    // An unreachable transport target makes replay fail deterministically in
    // every environment (spawn failure without a compiler; unknown-target
    // error with one), exercising the materialize error path end to end.
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const uint64_t moduli[] = {kQ0};
    const hazeFheParams fhe = {.ring_dim = kRingDim, .moduli = moduli, .moduli_count = 1};
    const hazeReplayConfig replay = {.target = "HAZE_TEST_NO_SUCH_TARGET"};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_SUCCESS);

    void *a = nullptr;
    void *b = nullptr;
    void *c = nullptr;
    REQUIRE(hazeMalloc(&a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&c, kBytes) == HAZE_SUCCESS);
    std::vector<uint64_t> data(kRingDim, 3);
    REQUIRE(hazeMemcpy(a, data.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(b, data.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(c, a, b, 0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(c) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_ERROR_INTERNAL);
    hazeGetLastError();

    // Always-clear pin: the failed flush must drop every binding — a re-tag
    // has nothing to name, the output was never materialized, and a second
    // flush is an idle no-op instead of replaying a half-torn epoch.
    REQUIRE(hazeTagOutput(c) == HAZE_ERROR_SOURCE_UNAVAILABLE);
    hazeGetLastError();
    std::vector<uint64_t> out(kRingDim, 0);
    REQUIRE(hazeMemcpy(out.data(), c, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) ==
            HAZE_ERROR_NOT_FLUSHED);
    hazeGetLastError();
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    REQUIRE(hazeFree(a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(b) == HAZE_SUCCESS);
    REQUIRE(hazeFree(c) == HAZE_SUCCESS);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("hazeDeviceReset restarts stream/event ids and the active device", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    hazeStream_t s1 = nullptr;
    hazeEvent_t e1 = nullptr;
    REQUIRE(hazeStreamCreate(&s1) == HAZE_SUCCESS);
    REQUIRE(hazeEventCreate(&e1) == HAZE_SUCCESS);
    REQUIRE(s1->id == 1);
    REQUIRE(e1->id == 1);
    REQUIRE(hazeStreamDestroy(s1) == HAZE_SUCCESS);
    REQUIRE(hazeEventDestroy(e1) == HAZE_SUCCESS);

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    hazeStream_t s2 = nullptr;
    hazeEvent_t e2 = nullptr;
    REQUIRE(hazeStreamCreate(&s2) == HAZE_SUCCESS);
    REQUIRE(hazeEventCreate(&e2) == HAZE_SUCCESS);
    REQUIRE(s2->id == 1); // counters restart, not continue
    REQUIRE(e2->id == 1);
    REQUIRE(hazeStreamDestroy(s2) == HAZE_SUCCESS);
    REQUIRE(hazeEventDestroy(e2) == HAZE_SUCCESS);
    int dev = -1;
    REQUIRE(hazeGetDevice(&dev) == HAZE_SUCCESS);
    REQUIRE(dev == 0);
}

TEST_CASE("ciphertext-modulus count is bounded by the device envelope", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    hazeDeviceProp prop{};
    REQUIRE(hazeGetDeviceProperties(&prop, 0) == HAZE_SUCCESS);
    const auto max = static_cast<std::size_t>(prop.maxCiphertextModuli);
    std::vector<uint64_t> moduli(max + 1);
    for (std::size_t i = 0; i < moduli.size(); ++i)
        moduli[i] = kQ0 + (2 * i);

    // Exactly `max` distinct primes configure cleanly...
    const hazeFheParams full = {.ring_dim = kRingDim, .moduli = moduli.data(), .moduli_count = max};
    REQUIRE(hazeConfigureDevice(&full, nullptr) == HAZE_SUCCESS);
    // ...and one past the envelope is rejected (previously an unbounded vector
    // grew; now it guards a fixed array).
    const hazeFheParams over = {
        .ring_dim = kRingDim, .moduli = moduli.data(), .moduli_count = max + 1};
    REQUIRE(hazeConfigureDevice(&over, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("bridge pre-init requires an explicit configure", "[unit][hwfmt]") {
    // The bridge pre-init reads the frozen replay config via bootstrap_compiler,
    // so it must run after hazeConfigureDevice(); calling it earlier is rejected.
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    uint64_t scaffold = 0;
    REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, kQ0, &scaffold) == HAZE_ERROR_CONFIGERR);
    hazeGetLastError();
    // After the explicit configure it succeeds.
    const uint64_t moduli[] = {kQ0};
    const hazeFheParams fhe = {.ring_dim = kRingDim, .moduli = moduli, .moduli_count = 1};
    REQUIRE(hazeConfigureDevice(&fhe, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, kQ0, &scaffold) == HAZE_SUCCESS);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}
