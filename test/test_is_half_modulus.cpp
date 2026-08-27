// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// hazeIsHalfModulus / hazeIsHalfModulusMrp tests: [unit] validation and
// trace-shape pinning, [integration] golden values.

#include "integration_helpers.hpp"
#include "mod_arith_ref.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr uint64_t kRingDim = 4096;
constexpr std::size_t kBytes = kRingDim * sizeof(uint64_t);

// Suite NTT-friendly primes (q ≡ 1 mod 2N for N=4096); matches test_compute.cpp.
constexpr uint64_t kQ0 = 576460752303415297ULL;
constexpr uint64_t kQ1 = 576460752303439873ULL;
constexpr uint64_t kQ2 = 576460752303702017ULL;
// The device-generated aux prime for ring_dim 4096: the smallest prime >= 2^30
// (the replay bridge floor) with p ≡ 1 mod 8192. Configuring it as a data
// modulus makes the generator skip to the next candidate (1073815553).
constexpr uint64_t kSmallQ = 1073750017ULL;
// 59-bit NTT-friendly prime with q ≡ 1 (mod kSmallQ): against aux p = kSmallQ
// it drives qinv == 1, p | h, and the zero-immediate gadget branches at once.
constexpr uint64_t kCongruentQ = 288241371603542017ULL;
// 2*kQ0 + 1: composite (3*5*7*13 divide it) — the "safe-prime-looking" modulus
// a caller might configure; hazeConfigureDevice must reject it (a composite
// chain entry would make Fermat inverses silently wrong).
constexpr uint64_t kCompositeAux = (2 * kQ0) + 1;

uint64_t is_half_ref(uint64_t x, uint64_t q) {
    return (x > (q - 1) / 2) ? 1U : 0U;
}

// Pseudorandom residues mod q with the threshold boundary cases pinned into the
// leading slots: 0, 1, h-1, h, h+1, q-2, q-1 where h = (q-1)/2 (result flips
// between h and h+1).
std::vector<uint64_t> boundary_input(uint64_t q, uint64_t seed) {
    std::vector<uint64_t> input = haze::test::make_residue(q, seed, kRingDim);
    const uint64_t h = (q - 1) / 2;
    const uint64_t edges[] = {0, 1, h - 1, h, h + 1, q - 2, q - 1};
    for (std::size_t i = 0; i < 7; ++i)
        input[i] = edges[i];
    return input;
}

// Record one hazeIsHalfModulus over boundary+pseudorandom input, flush, and
// require exact 0/1 equality against the host oracle on every coefficient.
void run_golden_case(uint64_t q, int mod_idx, uint64_t seed) {
    const std::vector<uint64_t> input = boundary_input(q, seed);

    void *d_src = nullptr;
    void *d_dst = nullptr;
    REQUIRE(hazeMalloc(&d_src, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_src, input.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    REQUIRE(hazeIsHalfModulus(d_dst, d_src, mod_idx, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(d_dst) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    std::vector<uint64_t> got(kRingDim, 0xDEADBEEFULL);
    REQUIRE(hazeMemcpy(got.data(), d_dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    for (std::size_t k = 0; k < kRingDim; ++k) {
        INFO("q " << q << " mod_idx " << mod_idx << " slot " << k << " input " << input[k]);
        REQUIRE(got[k] == is_half_ref(input[k], q));
    }

    REQUIRE(hazeFree(d_src) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_dst) == HAZE_SUCCESS);
}

} // namespace

// ===========================================================================
// Golden values ([integration]: runs under test-sim and test-transport).
// ===========================================================================

TEST_CASE("hazeIsHalfModulus: golden values, generated aux below q", "[integration]") {
    haze::test::setup_integration_mrp3_config(kRingDim, kQ0);
    // aux = kSmallQ (generated, ~2^30): p < q and h >= p for every data prime.
    run_golden_case(kQ0, /*mod_idx=*/0, /*seed=*/424242ULL);
    run_golden_case(kQ1, /*mod_idx=*/1, /*seed=*/911223ULL);
    run_golden_case(kQ2, /*mod_idx=*/2, /*seed=*/171717ULL);
}

TEST_CASE("hazeIsHalfModulus: golden values, generated aux above q (collision skip)",
          "[integration]") {
    // kSmallQ as a data modulus collides with the generator's first candidate,
    // so the aux becomes the next qualifying prime (1073815553) — above q.
    haze::test::setup_integration_compute_config(kRingDim, kSmallQ);
    run_golden_case(kSmallQ, /*mod_idx=*/0, /*seed=*/555555ULL);
}

TEST_CASE("hazeIsHalfModulus: golden values, q ≡ 1 mod p (qinv == 1, p | h)", "[integration]") {
    // kSmallQ | (kCongruentQ-1)/2, and the generated aux for this chain is
    // exactly kSmallQ — driving qinv == 1 and the zero-immediate branches.
    haze::test::setup_integration_compute_config(kRingDim, kCongruentQ);
    run_golden_case(kCongruentQ, /*mod_idx=*/0, /*seed=*/999999ULL);
}

TEST_CASE("hazeConfigureDevice rejects a composite modulus", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const uint64_t moduli[] = {kQ0, kCompositeAux, kQ1};
    const hazeFheParams fhe = {.ring_dim = kRingDim, .moduli = moduli, .moduli_count = 3};
    REQUIRE(hazeConfigureDevice(&fhe, nullptr) == HAZE_ERROR_CONFIGERR);
    hazeGetLastError();
}

TEST_CASE("hazeIsHalfModulus rejects an even modulus", "[unit]") {
    // 2 is the only prime the config accepts that the odd-centering guard
    // must still reject.
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const uint64_t moduli[] = {2, kQ0};
    const hazeFheParams fhe = {.ring_dim = kRingDim, .moduli = moduli, .moduli_count = 2};
    REQUIRE(hazeConfigureDevice(&fhe, nullptr) == HAZE_SUCCESS);

    void *d_src = nullptr;
    void *d_dst = nullptr;
    REQUIRE(hazeMalloc(&d_src, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst, kBytes) == HAZE_SUCCESS);
    std::vector<uint64_t> a(kRingDim, 1);
    REQUIRE(hazeMemcpy(d_src, a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    // mod_idx 0 names the even prime: rejected by the odd-centering guard.
    REQUIRE(hazeIsHalfModulus(d_dst, d_src, 0, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    const uint64_t base[] = {2};
    void *dst_polys[] = {d_dst};
    const void *src_polys[] = {d_src};
    REQUIRE(hazeIsHalfModulusMrp(dst_polys, src_polys, base, 1, nullptr) ==
            HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();

    REQUIRE(hazeFree(d_src) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_dst) == HAZE_SUCCESS);
}

TEST_CASE("hazeIsHalfModulus: in-place dst == src", "[integration]") {
    haze::test::setup_integration_mrp3_config(kRingDim, kQ0);
    const std::vector<uint64_t> input = boundary_input(kQ0, /*seed=*/313131ULL);

    void *d_x = nullptr;
    REQUIRE(hazeMalloc(&d_x, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_x, input.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    REQUIRE(hazeIsHalfModulus(d_x, d_x, 0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(d_x) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    std::vector<uint64_t> got(kRingDim, 0xDEADBEEFULL);
    REQUIRE(hazeMemcpy(got.data(), d_x, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    for (std::size_t k = 0; k < kRingDim; ++k) {
        INFO("slot " << k << " input " << input[k]);
        REQUIRE(got[k] == is_half_ref(input[k], kQ0));
    }
    REQUIRE(hazeFree(d_x) == HAZE_SUCCESS);
}

TEST_CASE("hazeIsHalfModulus: two results from one source in one epoch", "[integration]") {
    haze::test::setup_integration_mrp3_config(kRingDim, kQ0);
    const std::vector<uint64_t> input = boundary_input(kQ0, /*seed=*/616161ULL);

    void *d_src = nullptr;
    void *d_dst1 = nullptr;
    void *d_dst2 = nullptr;
    REQUIRE(hazeMalloc(&d_src, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst1, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst2, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_src, input.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    // Same source through two lowerings in one recording: the replay-side
    // optimizer (GVN etc.) may dedup shared subexpressions; results must not
    // change.
    REQUIRE(hazeIsHalfModulus(d_dst1, d_src, 0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeIsHalfModulus(d_dst2, d_src, 0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(d_dst1) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(d_dst2) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    for (void *dst : {d_dst1, d_dst2}) {
        std::vector<uint64_t> got(kRingDim, 0xDEADBEEFULL);
        REQUIRE(hazeMemcpy(got.data(), dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
        for (std::size_t k = 0; k < kRingDim; ++k) {
            INFO("dst " << (dst == d_dst1 ? 1 : 2) << " slot " << k << " input " << input[k]);
            REQUIRE(got[k] == is_half_ref(input[k], kQ0));
        }
    }

    REQUIRE(hazeFree(d_src) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_dst1) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_dst2) == HAZE_SUCCESS);
}

// ===========================================================================
// Argument validation ([unit]).
// ===========================================================================

TEST_CASE("hazeIsHalfModulus rejects null pointers", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const uint64_t moduli[] = {kQ0, kQ1};
    const hazeFheParams fhe = {.ring_dim = kRingDim, .moduli = moduli, .moduli_count = 2};
    REQUIRE(hazeConfigureDevice(&fhe, nullptr) == HAZE_SUCCESS);

    void *d_buf = nullptr;
    REQUIRE(hazeMalloc(&d_buf, kBytes) == HAZE_SUCCESS);

    REQUIRE(hazeIsHalfModulus(nullptr, d_buf, 0, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    REQUIRE(hazeIsHalfModulus(d_buf, nullptr, 0, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();

    REQUIRE(hazeFree(d_buf) == HAZE_SUCCESS);
}

TEST_CASE("hazeIsHalfModulus with invalid modulus index returns error", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const uint64_t moduli[] = {kQ0, kQ1};
    const hazeFheParams fhe = {.ring_dim = kRingDim, .moduli = moduli, .moduli_count = 2};
    REQUIRE(hazeConfigureDevice(&fhe, nullptr) == HAZE_SUCCESS);

    void *d_src = nullptr;
    void *d_dst = nullptr;
    REQUIRE(hazeMalloc(&d_src, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst, kBytes) == HAZE_SUCCESS);
    std::vector<uint64_t> a(kRingDim, 1);
    REQUIRE(hazeMemcpy(d_src, a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    REQUIRE(hazeIsHalfModulus(d_dst, d_src, -1, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    REQUIRE(hazeIsHalfModulus(d_dst, d_src, 63, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();

    REQUIRE(hazeFree(d_src) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_dst) == HAZE_SUCCESS);
}

TEST_CASE("hazeIsHalfModulus: a single data modulus works via the generated aux", "[integration]") {
    haze::test::setup_integration_compute_config(kRingDim, kQ0);
    run_golden_case(kQ0, /*mod_idx=*/0, /*seed=*/343434ULL);
}

TEST_CASE("hazeIsHalfModulus rejects mod_idx naming the aux slot", "[unit]") {
    // {kQ0} configures as {kQ0, aux}; the aux entry is not a data modulus.
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const uint64_t moduli[] = {kQ0};
    const hazeFheParams fhe = {.ring_dim = kRingDim, .moduli = moduli, .moduli_count = 1};
    REQUIRE(hazeConfigureDevice(&fhe, nullptr) == HAZE_SUCCESS);

    void *d_src = nullptr;
    void *d_dst = nullptr;
    REQUIRE(hazeMalloc(&d_src, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst, kBytes) == HAZE_SUCCESS);
    std::vector<uint64_t> a(kRingDim, 1);
    REQUIRE(hazeMemcpy(d_src, a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    REQUIRE(hazeIsHalfModulus(d_dst, d_src, 1, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();

    REQUIRE(hazeFree(d_src) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_dst) == HAZE_SUCCESS);
}

// ===========================================================================
// Record-time trace shape ([unit]): the emitted sequence and its ordinary-form
// immediates are a replay-driver contract, so pin them.
// ===========================================================================

namespace {

std::string slurp(const std::filesystem::path &path) {
    std::ifstream in(path);
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// JSON value text after `"key":` (up to comma/newline/brace), trimmed; string
// search keeps the test free of a JSON library dependency.
std::string json_value_text(const std::string &doc, const std::string &key) {
    const std::string needle = "\"" + key + "\"";
    const std::size_t key_pos = doc.find(needle);
    REQUIRE(key_pos != std::string::npos);
    const std::size_t colon = doc.find(':', key_pos);
    REQUIRE(colon != std::string::npos);
    const std::size_t end = doc.find_first_of(",\n}", colon);
    REQUIRE(end != std::string::npos);
    std::string value = doc.substr(colon + 1, end - colon - 1);
    const std::size_t first = value.find_first_not_of(" \t");
    const std::size_t last = value.find_last_not_of(" \t");
    REQUIRE(first != std::string::npos);
    return value.substr(first, last - first + 1);
}

std::size_t count_occurrences(const std::string &haystack, const std::string &needle) {
    std::size_t count = 0;
    for (std::size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size()))
        ++count;
    return count;
}

// Record one hazeIsHalfModulus(mod_idx=0) into a uniquely named program dir and
// return it. The non-local target keeps this off the in-process simulator path;
// hazeWriteProgram never replays, so no compiler binary is needed.
std::filesystem::path record_ihm_program(const std::string &program_name) {
    const uint64_t moduli[] = {kQ0, kQ1, kQ2};
    const hazeFheParams fhe = {.ring_dim = kRingDim, .moduli = moduli, .moduli_count = 3};
    const hazeReplayConfig replay = {.target = "FUNC_SIM",
                                     .program_name = program_name.c_str(),
                                     .program_version = "0.1",
                                     .program_description = "is_half_modulus trace-shape test",
                                     .reduced_noise = 1};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_SUCCESS);
    uint64_t scaffold = 0;
    REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, kQ0, &scaffold) == HAZE_SUCCESS);

    const std::vector<uint64_t> input = boundary_input(kQ0, /*seed=*/404040ULL);
    void *d_src = nullptr;
    void *d_dst = nullptr;
    REQUIRE(hazeMalloc(&d_src, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_src, input.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeIsHalfModulus(d_dst, d_src, 0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(d_dst) == HAZE_SUCCESS);
    REQUIRE(hazeWriteProgram() == HAZE_SUCCESS);
    return std::filesystem::path{program_name};
}

// Immediates of the lowering for q = kQ0 against the generated aux kSmallQ:
// h (subps + gadget shifts), -h mod p (gadget unshifts, exactly twice),
// h mod p (the +h re-add), qinv (the final scale).
void check_ihm_trace_shape(const std::string &trace) {
    const uint64_t q = kQ0;
    const uint64_t p = kSmallQ;
    const uint64_t h = (q - 1) / 2;
    const uint64_t neg_half = p - (h % p);
    const uint64_t qinv = niobium::mod_arith::modinv_prime(q % p, p);

    REQUIRE(trace.find(", " + std::to_string(h) + ",") != std::string::npos);
    REQUIRE(count_occurrences(trace, ", " + std::to_string(neg_half) + ",") == 2);
    REQUIRE(trace.find(", " + std::to_string(h % p) + ",") != std::string::npos);
    REQUIRE(trace.find(", " + std::to_string(qinv) + ",") != std::string::npos);

    // Opcode census of the 10-op (ThreeOp) lowering.
    CHECK(count_occurrences(trace, "sr_subps ") == 1);
    CHECK(count_occurrences(trace, "sr_addps ") == 5);
    CHECK(count_occurrences(trace, "sr_mulps ") == 3);
    CHECK(count_occurrences(trace, "sr_subp ") == 1);
}

} // namespace

// The recording is ordinary-form for every target; the driver recomputes its own
// Montgomery constants when it substitutes the switchmod block.
TEST_CASE("hazeIsHalfModulus record-time: ordinary-form ThreeOp trace", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const auto dir = record_ihm_program("haze_ihm_threeop");
    check_ihm_trace_shape(slurp(dir / "haze_ihm_threeop.fhetch"));
    REQUIRE(json_value_text(slurp(dir / "fhetch_replay.json"), "niobium_hw") == "false");
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

// ===========================================================================
// MRP variant: per-limb predicate with one shared aux prime, auto-selected as
// the lowest-indexed configured modulus not in the base (SRP pattern).
// ===========================================================================

namespace {

// Record one hazeIsHalfModulusMrp over per-limb boundary inputs, flush, and
// return the per-limb D2H results.
std::vector<std::vector<uint64_t>> run_ihm_mrp(const std::vector<uint64_t> &base, uint64_t seed) {
    std::vector<std::vector<uint64_t>> inputs(base.size());
    for (std::size_t i = 0; i < base.size(); ++i)
        inputs[i] = boundary_input(base[i], seed + i);

    const std::vector<void *> d_src = haze::test::allocate_and_h2d_residues(inputs, base);
    const std::vector<void *> d_dst = haze::test::allocate_dst_residues(base.size(), kBytes);
    const std::vector<const void *> src_view = haze::test::to_const(d_src);
    REQUIRE(hazeIsHalfModulusMrp(d_dst.data(), src_view.data(), base.data(), base.size(),
                                 nullptr) == HAZE_SUCCESS);
    for (void *out : d_dst)
        REQUIRE(hazeTagOutput(out) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    std::vector<std::vector<uint64_t>> results;
    for (void *out : d_dst) {
        std::vector<uint64_t> got(kRingDim, 0xDEADBEEFULL);
        REQUIRE(hazeMemcpy(got.data(), out, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
        results.push_back(std::move(got));
    }
    haze::test::free_all_residues(d_src);
    haze::test::free_all_residues(d_dst);
    return results;
}

void check_ihm_mrp_results(const std::vector<uint64_t> &base, uint64_t seed,
                           const std::vector<std::vector<uint64_t>> &results) {
    for (std::size_t i = 0; i < base.size(); ++i) {
        const std::vector<uint64_t> input = boundary_input(base[i], seed + i);
        for (std::size_t k = 0; k < kRingDim; ++k) {
            INFO("residue " << i << " (mod " << base[i] << ") slot " << k << " input " << input[k]);
            REQUIRE(results[i][k] == is_half_ref(input[k], base[i]));
        }
    }
}

} // namespace

TEST_CASE("hazeIsHalfModulusMrp: per-limb golden values, shared generated aux", "[integration]") {
    const std::vector<uint64_t> base =
        haze::test::setup_integration_mrp3_config(kRingDim, kQ0); // {kQ0, kQ1, kQ2}
    const std::vector<uint64_t> limb_base = {base[0], base[1]};

    // A failed call (unwritten source residue) resolves all sources before any
    // store, so it must leave every dst untouched; the valid run below then
    // materializes correct results in the same epoch.
    {
        const std::vector<void *> d_tmp = haze::test::allocate_dst_residues(2, kBytes);
        const void *bad_src[] = {d_tmp[0], d_tmp[1]};
        REQUIRE(hazeIsHalfModulusMrp(d_tmp.data(), bad_src, limb_base.data(), 2, nullptr) ==
                HAZE_ERROR_SOURCE_UNAVAILABLE);
        hazeGetLastError();
        haze::test::free_all_residues(d_tmp);
    }

    check_ihm_mrp_results(limb_base, /*seed=*/818181ULL,
                          run_ihm_mrp(limb_base, /*seed=*/818181ULL));
}

TEST_CASE("hazeIsHalfModulusMrp: in-place dst == src", "[integration]") {
    const std::vector<uint64_t> base = haze::test::setup_integration_mrp3_config(kRingDim, kQ0);
    const std::vector<uint64_t> limb_base = {base[0], base[1]};
    std::vector<std::vector<uint64_t>> inputs(limb_base.size());
    for (std::size_t i = 0; i < limb_base.size(); ++i)
        inputs[i] = boundary_input(limb_base[i], /*seed=*/848484ULL + i);

    const std::vector<void *> d_x = haze::test::allocate_and_h2d_residues(inputs, limb_base);
    const std::vector<const void *> src_view = haze::test::to_const(d_x);
    REQUIRE(hazeIsHalfModulusMrp(d_x.data(), src_view.data(), limb_base.data(), limb_base.size(),
                                 nullptr) == HAZE_SUCCESS);
    for (void *out : d_x)
        REQUIRE(hazeTagOutput(out) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    for (std::size_t i = 0; i < limb_base.size(); ++i) {
        std::vector<uint64_t> got(kRingDim, 0xDEADBEEFULL);
        REQUIRE(hazeMemcpy(got.data(), d_x[i], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
        for (std::size_t k = 0; k < kRingDim; ++k) {
            INFO("residue " << i << " slot " << k << " input " << inputs[i][k]);
            REQUIRE(got[k] == is_half_ref(inputs[i][k], limb_base[i]));
        }
    }
    haze::test::free_all_residues(d_x);
}

TEST_CASE("hazeIsHalfModulusMrp: base spanning the whole data chain", "[integration]") {
    // The scenario the configured-aux design could not serve: a predicate over
    // every data modulus at once. The generated aux lives outside the supplied
    // chain, so a full-chain base always has an extraction ring.
    const std::vector<uint64_t> base =
        haze::test::setup_integration_mrp3_config(kRingDim, kQ0); // {kQ0, kQ1, kQ2}
    check_ihm_mrp_results(base, /*seed=*/878787ULL, run_ihm_mrp(base, /*seed=*/878787ULL));
}

TEST_CASE("hazeIsHalfModulusMrp: generated aux (h >= p per limb)", "[integration]") {
    // The generated ~2^30 aux serves 59-bit limbs with h >= p immediate
    // reduction on every limb.
    haze::test::setup_integration_mrp3_config(kRingDim, kQ0);
    const std::vector<uint64_t> limb_base = {kQ1, kQ2};
    check_ihm_mrp_results(limb_base, /*seed=*/828282ULL,
                          run_ihm_mrp(limb_base, /*seed=*/828282ULL));
}

TEST_CASE("hazeIsHalfModulusMrp: base prime outside the configured chain", "[integration]") {
    // Base primes are caller-defined raw values (registered through the trace's
    // modulus table); the generated aux serves them all the same.
    haze::test::setup_integration_mrp3_config(kRingDim, kQ0);
    const std::vector<uint64_t> limb_base = {kCongruentQ, kQ1};
    check_ihm_mrp_results(limb_base, /*seed=*/868686ULL,
                          run_ihm_mrp(limb_base, /*seed=*/868686ULL));
}

TEST_CASE("hazeIsHalfModulusMrp rejects invalid arguments", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const uint64_t moduli[] = {kQ0, kQ1, kQ2};
    const hazeFheParams fhe = {.ring_dim = kRingDim, .moduli = moduli, .moduli_count = 3};
    REQUIRE(hazeConfigureDevice(&fhe, nullptr) == HAZE_SUCCESS);

    const uint64_t base[] = {kQ0, kQ1};
    void *d_a = nullptr;
    void *d_b = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_b, kBytes) == HAZE_SUCCESS);
    void *dst_polys[] = {d_a, d_b};
    const void *src_polys[] = {d_a, d_b};

    REQUIRE(hazeIsHalfModulusMrp(nullptr, src_polys, base, 2, nullptr) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeIsHalfModulusMrp(dst_polys, nullptr, base, 2, nullptr) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeIsHalfModulusMrp(dst_polys, src_polys, nullptr, 2, nullptr) ==
            HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeIsHalfModulusMrp(dst_polys, src_polys, base, 0, nullptr) ==
            HAZE_ERROR_INVALID_VALUE);
    // A base containing the generated aux prime is rejected (p must differ
    // from every limb modulus).
    const uint64_t aux_base[] = {kQ0, kSmallQ};
    REQUIRE(hazeIsHalfModulusMrp(dst_polys, src_polys, aux_base, 2, nullptr) ==
            HAZE_ERROR_INVALID_VALUE);
    // Bad src residues surface from the source-resolution path (same lookup as
    // build_mrp_locked): allocated-but-unwritten reports SOURCE_UNAVAILABLE,
    // null reports UNKNOWN_ADDRESS. d_a is written before the null case so
    // each call isolates one bad element.
    const void *unwritten_src[] = {d_a, d_b};
    REQUIRE(hazeIsHalfModulusMrp(dst_polys, unwritten_src, base, 2, nullptr) ==
            HAZE_ERROR_SOURCE_UNAVAILABLE);
    hazeGetLastError();
    std::vector<uint64_t> vals(kRingDim, 1);
    REQUIRE(hazeMemcpy(d_a, vals.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    const void *null_src[] = {d_a, nullptr};
    REQUIRE(hazeIsHalfModulusMrp(dst_polys, null_src, base, 2, nullptr) ==
            HAZE_ERROR_UNKNOWN_ADDRESS);
    hazeGetLastError();

    REQUIRE(hazeFree(d_a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_b) == HAZE_SUCCESS);
}

TEST_CASE("hazeIsHalfModulus with unknown addresses returns error", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const uint64_t moduli[] = {kQ0, kQ1};
    const hazeFheParams fhe = {.ring_dim = kRingDim, .moduli = moduli, .moduli_count = 2};
    REQUIRE(hazeConfigureDevice(&fhe, nullptr) == HAZE_SUCCESS);

    void *d_dst = nullptr;
    REQUIRE(hazeMalloc(&d_dst, kBytes) == HAZE_SUCCESS);

    // Synthetic device addresses that were never allocated / written; the
    // int-to-pointer casts are deliberate (the addresses themselves are the
    // test subject).
    // NOLINTBEGIN(performance-no-int-to-ptr)
    void *fake_src = reinterpret_cast<void *>(uintptr_t{0x4000000000ULL} + 0x8000000ULL);
    void *fake_dst = reinterpret_cast<void *>(uintptr_t{0x4000000000ULL} + 0x9000000ULL);
    // NOLINTEND(performance-no-int-to-ptr)
    REQUIRE(hazeIsHalfModulus(d_dst, fake_src, 0, nullptr) == HAZE_ERROR_UNKNOWN_ADDRESS);
    hazeGetLastError();
    REQUIRE(hazeIsHalfModulus(fake_dst, fake_src, 0, nullptr) == HAZE_ERROR_UNKNOWN_ADDRESS);
    hazeGetLastError();

    REQUIRE(hazeFree(d_dst) == HAZE_SUCCESS);
}
