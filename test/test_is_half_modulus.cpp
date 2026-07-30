// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// hazeIsHalfModulus tests: [unit] argument validation and [integration] golden
// values (exact 0/1 per coefficient) across aux-prime-above-q, aux-prime-below-q,
// small-prime, in-place, and shared-source cases. The trace-shape and
// hardware-format (montgomery/bit-reversal) tiers live further down, tagged
// [hwfmt] like test_hardware_format.cpp.

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
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr uint64_t kRingDim = 4096;
constexpr std::size_t kBytes = kRingDim * sizeof(uint64_t);

// Suite NTT-friendly primes (q ≡ 1 mod 2N for N=4096); matches test_compute.cpp.
constexpr uint64_t kQ0 = 576460752303415297ULL;
constexpr uint64_t kQ1 = 576460752303439873ULL;
constexpr uint64_t kQ2 = 576460752303702017ULL;
// Small NTT-friendly prime (just above 2^30, ≡ 1 mod 8192): as q it exercises a
// small half-point, as the auto-selected aux prime it exercises h >= p immediate
// reduction. ~2^30 is the practical floor here — the replay bridge's OpenFHE
// template synthesis throws (LastPrime overflow) for tiny primes like 65537.
constexpr uint64_t kSmallQ = 1073750017ULL;
// 59-bit NTT-friendly prime with q ≡ 1 (mod kSmallQ): q = 32771 * kSmallQ * 8192 + 1.
// Against aux p = kSmallQ it drives the rare immediate branches in one case:
// qinv == 1, p | h, h % p == 0, and the neg_half == 0 copy special case.
constexpr uint64_t kCongruentQ = 288241371603542017ULL;

uint64_t is_half_ref(uint64_t x, uint64_t q) {
    return (x > (q - 1) / 2) ? 1U : 0U;
}

// Two-prime configuration; the aux prime for mod_idx=0 is q1 (lowest index != 0).
void setup_two_prime_config(uint64_t q0, uint64_t q1) {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const uint64_t moduli[] = {q0, q1};
    const hazeFheParams fhe = {.ring_dim = kRingDim, .moduli = moduli, .moduli_count = 2};
    const hazeReplayConfig replay = {.target = haze::test::target_from_env(), .reduced_noise = 1};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_SUCCESS);
    uint64_t scaffold = 0; // built then overwritten from the trace; not used for results
    REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, q0, &scaffold) == HAZE_SUCCESS);
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

TEST_CASE("hazeIsHalfModulus: golden values, aux prime above q", "[integration]") {
    haze::test::setup_integration_mrp3_config(kRingDim, kQ0);
    run_golden_case(kQ0, /*mod_idx=*/0, /*seed=*/424242ULL); // p = kQ1 > q
}

TEST_CASE("hazeIsHalfModulus: golden values, aux prime below q", "[integration]") {
    haze::test::setup_integration_mrp3_config(kRingDim, kQ0);
    run_golden_case(kQ1, /*mod_idx=*/1, /*seed=*/911223ULL); // p = kQ0 < q
    run_golden_case(kQ2, /*mod_idx=*/2, /*seed=*/171717ULL); // p = kQ0 < q
}

TEST_CASE("hazeIsHalfModulus: golden values, small q against 59-bit aux prime", "[integration]") {
    setup_two_prime_config(kSmallQ, kQ0);
    run_golden_case(kSmallQ, /*mod_idx=*/0, /*seed=*/555555ULL); // p = kQ0 >> q
}

TEST_CASE("hazeIsHalfModulus: golden values, small aux prime (h >= p)", "[integration]") {
    setup_two_prime_config(kQ0, kSmallQ);
    run_golden_case(kQ0, /*mod_idx=*/0, /*seed=*/777777ULL); // p = kSmallQ, h >> p
}

TEST_CASE("hazeIsHalfModulus: golden values, q ≡ 1 mod p (qinv == 1, p | h)", "[integration]") {
    setup_two_prime_config(kCongruentQ, kSmallQ);
    run_golden_case(kCongruentQ, /*mod_idx=*/0, /*seed=*/999999ULL);
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

TEST_CASE("hazeIsHalfModulus without a second configured modulus returns error", "[unit]") {
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

    // mod_idx 0 is valid; the failure is the missing aux modulus.
    REQUIRE(hazeIsHalfModulus(d_dst, d_src, 0, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();

    REQUIRE(hazeFree(d_src) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_dst) == HAZE_SUCCESS);
}

// ===========================================================================
// Record-time trace shape ([unit][hwfmt]): the emitted instruction sequence and
// its ordinary-form immediates are a replay-driver contract (the montgomery
// path structurally recognizes the centered-switch quadruples), so pin them.
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
// search keeps the test free of a JSON library dependency (same approach as
// test_hardware_format.cpp).
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

// Record one hazeIsHalfModulus(mod_idx=0) into a uniquely named program dir
// (target FUNC_SIM so the format toggles are accepted; hazeWriteProgram needs
// no replay binary) and return the program directory.
std::filesystem::path record_ihm_program(const std::string &program_name, bool montgomery,
                                         bool bit_reversal) {
    const uint64_t moduli[] = {kQ0, kQ1, kQ2};
    const hazeFheParams fhe = {.ring_dim = kRingDim, .moduli = moduli, .moduli_count = 3};
    const hazeReplayConfig replay = {.target = "FUNC_SIM",
                                     .program_name = program_name.c_str(),
                                     .program_version = "0.1",
                                     .program_description = "is_half_modulus trace-shape test",
                                     .montgomery = montgomery ? 1 : 0,
                                     .bit_reversal = bit_reversal ? 1 : 0,
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

// Immediates of the lowering for q = kQ0, p = kQ1 (aux = lowest index != 0):
// h (subps + gadget shifts), -h mod p (gadget unshifts, exactly twice),
// h mod p (the +h re-add), qinv (the final scale).
void check_ihm_trace_shape(const std::string &trace, bool four_op) {
    const uint64_t q = kQ0;
    const uint64_t p = kQ1;
    const uint64_t h = (q - 1) / 2;
    const uint64_t neg_half = p - (h % p);
    const uint64_t qinv = niobium::mod_arith::modinv_prime(q % p, p);

    REQUIRE(trace.find(", " + std::to_string(h) + ",") != std::string::npos);
    REQUIRE(count_occurrences(trace, ", " + std::to_string(neg_half) + ",") == 2);
    REQUIRE(trace.find(", " + std::to_string(h % p) + ",") != std::string::npos);
    REQUIRE(trace.find(", " + std::to_string(qinv) + ",") != std::string::npos);

    // Montgomery-form immediates never appear client-side; the driver
    // substitutes them at replay.
    const auto sw_hw = niobium::mod_arith::compute_switchmodulus_immediates(q, p,
                                                                            /*montgomery=*/true);
    REQUIRE(trace.find(", " + std::to_string(sw_hw.imm[2]) + ",") == std::string::npos);
    REQUIRE(trace.find(", " + std::to_string(sw_hw.imm[3]) + ",") == std::string::npos);

    // Opcode census of the 10-op (ThreeOp) / 12-op (FourOp montgomery) lowering.
    CHECK(count_occurrences(trace, "sr_subps ") == 1);
    CHECK(count_occurrences(trace, "sr_addps ") == 5);
    CHECK(count_occurrences(trace, "sr_mulps ") == (four_op ? 5U : 3U));
    CHECK(count_occurrences(trace, "sr_subp ") == 1);
}

} // namespace

TEST_CASE("hazeIsHalfModulus record-time: ordinary-form ThreeOp trace", "[unit][hwfmt]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const auto dir = record_ihm_program("haze_ihm_threeop", /*montgomery=*/false,
                                        /*bit_reversal=*/false);
    check_ihm_trace_shape(slurp(dir / "haze_ihm_threeop.fhetch"), /*four_op=*/false);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("hazeIsHalfModulus record-time: montgomery keeps ordinary immediates, FourOp shape",
          "[unit][hwfmt]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const auto dir = record_ihm_program("haze_ihm_fourop", /*montgomery=*/true,
                                        /*bit_reversal=*/true);
    check_ihm_trace_shape(slurp(dir / "haze_ihm_fourop.fhetch"), /*four_op=*/true);
    // niobium_hw describes the recording (always ordinary-form); the dispatch
    // flag alone carries the replay-side format selection.
    REQUIRE(json_value_text(slurp(dir / "fhetch_replay.json"), "niobium_hw") == "false");
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

// ===========================================================================
// Transport hardware mode ([integration][hwfmt]: requires make test-transport).
// Montgomery + bit-reversal are exact bijections decoded before probes are
// written, so results must be byte-identical to ordinary mode — the A/B case is
// the end-to-end gate that the driver recognizes the hand-emitted gadget
// quadruples and substitutes SwitchModulus correctly.
// ===========================================================================

namespace {

bool transport_target_active() {
    const char *target = std::getenv("HAZE_TARGET");
    return target != nullptr && target[0] != '\0' && std::string_view{target} != "local";
}

// Record + flush one hazeIsHalfModulus(mod_idx=0) through the transport under
// the given data-format toggles (default shared "haze" program dir) and return
// the D2H result.
std::vector<uint64_t> run_ihm_computation(const char *target, bool montgomery, bool bit_reversal) {
    const uint64_t moduli[] = {kQ0, kQ1, kQ2};
    const hazeFheParams fhe = {.ring_dim = kRingDim, .moduli = moduli, .moduli_count = 3};
    const hazeReplayConfig replay = {.target = target,
                                     .montgomery = montgomery ? 1 : 0,
                                     .bit_reversal = bit_reversal ? 1 : 0,
                                     .reduced_noise = 1};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_SUCCESS);
    uint64_t scaffold = 0;
    REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, kQ0, &scaffold) == HAZE_SUCCESS);

    const std::vector<uint64_t> input = boundary_input(kQ0, /*seed=*/909090ULL);
    void *d_src = nullptr;
    void *d_dst = nullptr;
    REQUIRE(hazeMalloc(&d_src, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_dst, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_src, input.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeIsHalfModulus(d_dst, d_src, 0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(d_dst) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    std::vector<uint64_t> got(kRingDim, 0xDEADBEEFULL);
    REQUIRE(hazeMemcpy(got.data(), d_dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_src) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_dst) == HAZE_SUCCESS);
    return got;
}

} // namespace

TEST_CASE("hazeIsHalfModulus transport: byte-exact vs oracle under data format",
          "[integration][hwfmt]") {
    if (!transport_target_active())
        SKIP("data format requires a transport target (run under make test-transport)");
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);

    const auto got = run_ihm_computation(haze::test::target_from_env(), /*montgomery=*/true,
                                         /*bit_reversal=*/true);
    const std::vector<uint64_t> input = boundary_input(kQ0, /*seed=*/909090ULL);
    for (std::size_t k = 0; k < kRingDim; ++k) {
        INFO("slot " << k << " input " << input[k]);
        REQUIRE(got[k] == is_half_ref(input[k], kQ0));
    }
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("hazeIsHalfModulus transport: A/B byte-exact vs ordinary mode", "[integration][hwfmt]") {
    if (!transport_target_active())
        SKIP("data format requires a transport target (run under make test-transport)");

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const auto ordinary = run_ihm_computation(haze::test::target_from_env(), /*montgomery=*/false,
                                              /*bit_reversal=*/false);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const auto encoded = run_ihm_computation(haze::test::target_from_env(), /*montgomery=*/true,
                                             /*bit_reversal=*/true);
    REQUIRE(ordinary == encoded);
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

    const std::vector<void *> d_src = haze::test::allocate_and_h2d_residues(inputs);
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

TEST_CASE("hazeIsHalfModulusMrp: per-limb golden values, shared configured aux", "[integration]") {
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

    // Auto-selected aux = base[2] (kQ2), the lowest configured index not in the base.
    check_ihm_mrp_results(limb_base, /*seed=*/818181ULL,
                          run_ihm_mrp(limb_base, /*seed=*/818181ULL));
}

TEST_CASE("hazeIsHalfModulusMrp: in-place dst == src", "[integration]") {
    const std::vector<uint64_t> base = haze::test::setup_integration_mrp3_config(kRingDim, kQ0);
    const std::vector<uint64_t> limb_base = {base[0], base[1]};
    std::vector<std::vector<uint64_t>> inputs(limb_base.size());
    for (std::size_t i = 0; i < limb_base.size(); ++i)
        inputs[i] = boundary_input(limb_base[i], /*seed=*/848484ULL + i);

    const std::vector<void *> d_x = haze::test::allocate_and_h2d_residues(inputs);
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

TEST_CASE("hazeIsHalfModulusMrp: small auto-selected aux (h >= p per limb)", "[integration]") {
    // Config {kSmallQ, kQ1, kQ2} with base {kQ1, kQ2}: the auto-selected aux is
    // kSmallQ (lowest configured index not in the base), exercising h >= p
    // immediate reduction on every limb.
    haze::test::setup_integration_mrp3_config(kRingDim, kSmallQ);
    const std::vector<uint64_t> limb_base = {kQ1, kQ2};
    check_ihm_mrp_results(limb_base, /*seed=*/828282ULL,
                          run_ihm_mrp(limb_base, /*seed=*/828282ULL));
}

TEST_CASE("hazeIsHalfModulusMrp: base prime outside the configured chain", "[integration]") {
    // Base primes are caller-defined raw values (registered through the trace's
    // modulus table); only the aux comes from the configuration. Config
    // {kQ0, kQ1, kQ2} with base {kSmallQ, kQ1} auto-selects aux = kQ0.
    haze::test::setup_integration_mrp3_config(kRingDim, kQ0);
    const std::vector<uint64_t> limb_base = {kSmallQ, kQ1};
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
    // Base covering every configured modulus leaves no aux prime to select.
    const uint64_t full_base[] = {kQ0, kQ1, kQ2};
    void *dst3[] = {d_a, d_b, d_a};
    const void *src3[] = {d_a, d_b, d_a};
    REQUIRE(hazeIsHalfModulusMrp(dst3, src3, full_base, 3, nullptr) == HAZE_ERROR_INVALID_VALUE);
    // Bad src residues surface from the source-resolution path — the same
    // lookup build_mrp_locked uses (no element pre-check) — and must not
    // mutate any dst. An allocated-but-unwritten residue reports
    // SOURCE_UNAVAILABLE; a null (never-allocated) residue reports
    // UNKNOWN_ADDRESS. Write d_a first so each case isolates one bad element.
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

TEST_CASE("hazeIsHalfModulusMrp transport: A/B byte-exact vs ordinary mode",
          "[integration][hwfmt]") {
    if (!transport_target_active())
        SKIP("data format requires a transport target (run under make test-transport)");

    const std::vector<uint64_t> limb_base = {kQ0, kQ1};
    auto run_once = [&](bool montgomery, bool bit_reversal) {
        const uint64_t moduli[] = {kQ0, kQ1, kQ2};
        const hazeFheParams fhe = {.ring_dim = kRingDim, .moduli = moduli, .moduli_count = 3};
        const hazeReplayConfig replay = {.target = haze::test::target_from_env(),
                                         .montgomery = montgomery ? 1 : 0,
                                         .bit_reversal = bit_reversal ? 1 : 0,
                                         .reduced_noise = 1};
        REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_SUCCESS);
        uint64_t scaffold = 0;
        REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, kQ0, &scaffold) == HAZE_SUCCESS);
        return run_ihm_mrp(limb_base, /*seed=*/838383ULL); // auto aux = kQ2
    };

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const auto ordinary = run_once(/*montgomery=*/false, /*bit_reversal=*/false);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const auto encoded = run_once(/*montgomery=*/true, /*bit_reversal=*/true);
    REQUIRE(ordinary == encoded);
    check_ihm_mrp_results(limb_base, /*seed=*/838383ULL, encoded);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
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
