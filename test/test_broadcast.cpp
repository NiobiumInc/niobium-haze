// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// hazeBroadcast{Add,Sub,Rsub,Mul}Mrp tests: [unit] validation and trace-shape
// pinning, [integration] golden values, [hwfmt] transport cases.

#include "integration_helpers.hpp"
#include "mod_arith_ref.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
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
// Small NTT-friendly prime (~2^30, ≡ 1 mod 8192); the replay bridge's floor.
constexpr uint64_t kSmallQ = 1073750017ULL;
// 48-bit NTT-friendly prime with kSmallQ | (p-1)/2: lifting from it into a
// kSmallQ limb zeroes the gadget's unshift immediate (the sim copy path).
constexpr uint64_t kCongruentP = 211107843342337ULL;

enum class Op : std::uint8_t { Add, Sub, Rsub, Mul };

hazeError_t call_broadcast(Op op, void *const *dst, const void *const *src, const void *operand,
                           uint64_t p, int in_range, const uint64_t *base, std::size_t len) {
    switch (op) {
    case Op::Add:
        return hazeBroadcastAddMrp(dst, src, operand, p, in_range, base, len, nullptr);
    case Op::Sub:
        return hazeBroadcastSubMrp(dst, src, operand, p, in_range, base, len, nullptr);
    case Op::Rsub:
        return hazeBroadcastRsubMrp(dst, src, operand, p, in_range, base, len, nullptr);
    case Op::Mul:
        return hazeBroadcastMulMrp(dst, src, operand, p, in_range, base, len, nullptr);
    }
    return HAZE_ERROR_INVALID_VALUE;
}

// Centered lift oracle: m for m <= (p-1)/2, else m - p, reduced mod q.
uint64_t lift_ref(uint64_t m, uint64_t p, uint64_t q) {
    if (m <= (p - 1) / 2)
        return m % q;
    const uint64_t neg = (p - m) % q;
    return neg == 0 ? 0 : q - neg;
}

uint64_t op_ref(Op op, uint64_t a, uint64_t b, uint64_t q) {
    switch (op) {
    case Op::Add:
        return (a + b) % q;
    case Op::Sub:
        return (a >= b) ? a - b : a + (q - b);
    case Op::Rsub:
        return (b >= a) ? b - a : b + (q - a);
    case Op::Mul:
        return niobium::mod_arith::mulmod(a, b, q);
    }
    return 0;
}

// Run one broadcast over pseudorandom limbs and operand, flush, and require
// exact equality against the lift+op oracle on every coefficient. The operand
// limb for base[i] == p is used verbatim (no lift), matching the pass-through.
void run_broadcast_golden(Op op, const std::vector<uint64_t> &base, uint64_t p,
                          const std::vector<uint64_t> &operand_vals, int in_range, uint64_t seed) {
    std::vector<std::vector<uint64_t>> inputs(base.size());
    for (std::size_t i = 0; i < base.size(); ++i)
        inputs[i] = haze::test::make_residue(base[i], seed + i, kRingDim);

    const std::vector<void *> d_src = haze::test::allocate_and_h2d_residues(inputs);
    const std::vector<void *> d_dst = haze::test::allocate_dst_residues(base.size(), kBytes);
    void *d_m = nullptr;
    REQUIRE(hazeMalloc(&d_m, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_m, operand_vals.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) ==
            HAZE_SUCCESS);

    const std::vector<const void *> src_view = haze::test::to_const(d_src);
    REQUIRE(call_broadcast(op, d_dst.data(), src_view.data(), d_m, p, in_range, base.data(),
                           base.size()) == HAZE_SUCCESS);
    for (void *out : d_dst)
        REQUIRE(hazeTagOutput(out) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    for (std::size_t i = 0; i < base.size(); ++i) {
        const uint64_t q = base[i];
        std::vector<uint64_t> got(kRingDim, 0xDEADBEEFULL);
        REQUIRE(hazeMemcpy(got.data(), d_dst[i], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) ==
                HAZE_SUCCESS);
        for (std::size_t k = 0; k < kRingDim; ++k) {
            const uint64_t lifted = (q == p) ? operand_vals[k] : lift_ref(operand_vals[k], p, q);
            INFO("op " << static_cast<int>(op) << " residue " << i << " (mod " << q << ") slot "
                       << k << " x " << inputs[i][k] << " m " << operand_vals[k]);
            REQUIRE(got[k] == op_ref(op, inputs[i][k], lifted, q));
        }
    }

    haze::test::free_all_residues(d_src);
    haze::test::free_all_residues(d_dst);
    REQUIRE(hazeFree(d_m) == HAZE_SUCCESS);
}

std::vector<uint64_t> binary_mask(uint64_t seed) {
    std::vector<uint64_t> m(kRingDim);
    for (std::size_t k = 0; k < kRingDim; ++k)
        m[k] = ((seed >> (k % 17U)) ^ k) & 1U;
    return m;
}

// Pseudorandom operand mod p with the centered-lift boundary values pinned
// into the leading slots (the sign flips between h and h+1, h = (p-1)/2).
std::vector<uint64_t> boundary_operand(uint64_t p, uint64_t seed) {
    std::vector<uint64_t> vals = haze::test::make_residue(p, seed, kRingDim);
    const uint64_t h = (p - 1) / 2;
    const uint64_t edges[] = {0, 1, h - 1, h, h + 1, p - 2, p - 1};
    for (std::size_t i = 0; i < 7; ++i)
        vals[i] = edges[i];
    return vals;
}

} // namespace

// ===========================================================================
// Golden values ([integration]: runs under test-sim and test-transport).
// ===========================================================================

TEST_CASE("hazeBroadcastMulMrp: hazeIsHalfModulus mask applied to all limbs", "[integration]") {
    haze::test::setup_integration_mrp3_config(kRingDim, kQ0); // {kQ0, kQ1, kQ2}

    // Mask: SRP predicate of a value under kQ0; recorded under the auto aux kQ1.
    const std::vector<uint64_t> pred_in = haze::test::make_residue(kQ0, 424242ULL, kRingDim);
    void *d_pred_in = nullptr;
    void *d_mask = nullptr;
    REQUIRE(hazeMalloc(&d_pred_in, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_mask, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_pred_in, pred_in.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) ==
            HAZE_SUCCESS);
    REQUIRE(hazeIsHalfModulus(d_mask, d_pred_in, 0, nullptr) == HAZE_SUCCESS);

    // Ciphertext limbs under {kQ0, kQ2}; both lift from kQ1.
    const std::vector<uint64_t> base = {kQ0, kQ2};
    std::vector<std::vector<uint64_t>> inputs(base.size());
    for (std::size_t i = 0; i < base.size(); ++i)
        inputs[i] = haze::test::make_residue(base[i], 515151ULL + i, kRingDim);
    const std::vector<void *> d_src = haze::test::allocate_and_h2d_residues(inputs);
    const std::vector<void *> d_dst = haze::test::allocate_dst_residues(base.size(), kBytes);
    const std::vector<const void *> src_view = haze::test::to_const(d_src);

    const int in_range = GENERATE(0, 1);
    REQUIRE(hazeBroadcastMulMrp(d_dst.data(), src_view.data(), d_mask, kQ1, in_range, base.data(),
                                base.size(), nullptr) == HAZE_SUCCESS);
    for (void *out : d_dst)
        REQUIRE(hazeTagOutput(out) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    const uint64_t h = (kQ0 - 1) / 2;
    for (std::size_t i = 0; i < base.size(); ++i) {
        std::vector<uint64_t> got(kRingDim, 0xDEADBEEFULL);
        REQUIRE(hazeMemcpy(got.data(), d_dst[i], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) ==
                HAZE_SUCCESS);
        for (std::size_t k = 0; k < kRingDim; ++k) {
            const uint64_t bit = (pred_in[k] > h) ? 1U : 0U;
            INFO("in_range " << in_range << " residue " << i << " slot " << k);
            REQUIRE(got[k] == niobium::mod_arith::mulmod(inputs[i][k], bit, base[i]));
        }
    }

    haze::test::free_all_residues(d_src);
    haze::test::free_all_residues(d_dst);
    REQUIRE(hazeFree(d_pred_in) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_mask) == HAZE_SUCCESS);
}

TEST_CASE("hazeBroadcast: general values, small operand prime (p < q)", "[integration]") {
    haze::test::setup_integration_mrp3_config(kRingDim, kQ0);
    const std::vector<uint64_t> base = {kQ0, kQ1};
    const std::vector<uint64_t> vals = boundary_operand(kSmallQ, 626262ULL);
    for (Op op : {Op::Add, Op::Sub, Op::Rsub, Op::Mul})
        run_broadcast_golden(op, base, kSmallQ, vals, /*in_range=*/0,
                             /*seed=*/700000ULL + static_cast<uint64_t>(op));
}

TEST_CASE("hazeBroadcast: general values, large operand prime (p > q)", "[integration]") {
    haze::test::setup_integration_mrp3_config(kRingDim, kQ0);
    const std::vector<uint64_t> base = {kQ0, kQ1};
    const std::vector<uint64_t> vals = boundary_operand(kQ2, 636363ULL);
    for (Op op : {Op::Add, Op::Sub, Op::Rsub, Op::Mul})
        run_broadcast_golden(op, base, kQ2, vals, /*in_range=*/0,
                             /*seed=*/710000ULL + static_cast<uint64_t>(op));
}

TEST_CASE("hazeBroadcast: zero unshift immediate (base prime divides half the operand prime)",
          "[integration]") {
    // kSmallQ | (kCongruentP-1)/2 zeroes the kSmallQ limb's unshift immediate,
    // hitting the simulator's addps-imm-0 copy path inside the gadget.
    haze::test::setup_integration_mrp3_config(kRingDim, kSmallQ);
    const std::vector<uint64_t> base = {kSmallQ, kQ0};
    const std::vector<uint64_t> vals = boundary_operand(kCongruentP, 646400ULL);
    for (Op op : {Op::Add, Op::Sub, Op::Rsub, Op::Mul})
        run_broadcast_golden(op, base, kCongruentP, vals, /*in_range=*/0,
                             /*seed=*/740000ULL + static_cast<uint64_t>(op));
}

TEST_CASE("hazeBroadcast: direct path with general in-range values", "[integration]") {
    // operand_in_range with non-binary coefficients: all values satisfy the
    // contract (<= (p-1)/2 and below every base prime), so the lift is elided
    // on the ordinary format and the results must match the lift oracle.
    haze::test::setup_integration_mrp3_config(kRingDim, kQ0);
    const std::vector<uint64_t> base = {kQ0, kQ2};
    std::vector<uint64_t> vals =
        haze::test::make_residue(((kSmallQ - 1) / 2) + 1, 656600ULL, kRingDim);
    vals[0] = 0;
    vals[1] = 1;
    vals[2] = (kSmallQ - 1) / 2; // the contract's upper edge
    for (Op op : {Op::Add, Op::Sub, Op::Rsub, Op::Mul})
        run_broadcast_golden(op, base, kSmallQ, vals, /*in_range=*/1,
                             /*seed=*/750000ULL + static_cast<uint64_t>(op));
}

TEST_CASE("hazeBroadcast: operand modulus inside the base (pass-through limb)", "[integration]") {
    haze::test::setup_integration_mrp3_config(kRingDim, kQ0);
    const std::vector<uint64_t> base = {kQ0, kQ1};
    const std::vector<uint64_t> vals = boundary_operand(kQ1, 646464ULL);
    run_broadcast_golden(Op::Sub, base, kQ1, vals, /*in_range=*/0, /*seed=*/730000ULL);
}

TEST_CASE("hazeBroadcast: H2D overwrite clears a stale recorded operand modulus", "[integration]") {
    // An address that held a compute result (recorded under the aux prime)
    // and is then H2D-overwritten is a fresh raw operand again; the
    // consistency guard must accept the caller's modulus for the new bytes.
    haze::test::setup_integration_mrp3_config(kRingDim, kQ0);
    const std::vector<uint64_t> pred_in = haze::test::make_residue(kQ0, 767676ULL, kRingDim);
    void *d_t = nullptr;
    void *d_in = nullptr;
    REQUIRE(hazeMalloc(&d_t, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_in, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_in, pred_in.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeIsHalfModulus(d_t, d_in, 0, nullptr) == HAZE_SUCCESS); // recorded under kQ1

    const std::vector<uint64_t> mask = binary_mask(0x7E7EULL);
    REQUIRE(hazeMemcpy(d_t, mask.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    const std::vector<uint64_t> base = {kQ0, kQ2};
    std::vector<std::vector<uint64_t>> inputs(base.size());
    for (std::size_t i = 0; i < base.size(); ++i)
        inputs[i] = haze::test::make_residue(base[i], 777700ULL + i, kRingDim);
    const std::vector<void *> d_src = haze::test::allocate_and_h2d_residues(inputs);
    const std::vector<void *> d_dst = haze::test::allocate_dst_residues(base.size(), kBytes);
    const std::vector<const void *> src_view = haze::test::to_const(d_src);
    REQUIRE(hazeBroadcastAddMrp(d_dst.data(), src_view.data(), d_t, kSmallQ,
                                /*operand_in_range=*/1, base.data(), base.size(),
                                nullptr) == HAZE_SUCCESS);
    for (void *out : d_dst)
        REQUIRE(hazeTagOutput(out) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);
    for (std::size_t i = 0; i < base.size(); ++i) {
        std::vector<uint64_t> got(kRingDim, 0xDEADBEEFULL);
        REQUIRE(hazeMemcpy(got.data(), d_dst[i], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) ==
                HAZE_SUCCESS);
        for (std::size_t k = 0; k < kRingDim; ++k) {
            INFO("residue " << i << " slot " << k);
            REQUIRE(got[k] == (inputs[i][k] + mask[k]) % base[i]);
        }
    }
    haze::test::free_all_residues(d_src);
    haze::test::free_all_residues(d_dst);
    REQUIRE(hazeFree(d_t) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_in) == HAZE_SUCCESS);
}

TEST_CASE("hazeBroadcast: in-place dst == src", "[integration]") {
    haze::test::setup_integration_mrp3_config(kRingDim, kQ0);
    const std::vector<uint64_t> base = {kQ0, kQ1};
    std::vector<std::vector<uint64_t>> inputs(base.size());
    for (std::size_t i = 0; i < base.size(); ++i)
        inputs[i] = haze::test::make_residue(base[i], 656565ULL + i, kRingDim);
    const std::vector<uint64_t> mask = binary_mask(0xA5A5ULL);

    const std::vector<void *> d_x = haze::test::allocate_and_h2d_residues(inputs);
    void *d_m = nullptr;
    REQUIRE(hazeMalloc(&d_m, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_m, mask.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    const std::vector<const void *> src_view = haze::test::to_const(d_x);
    REQUIRE(hazeBroadcastAddMrp(d_x.data(), src_view.data(), d_m, kSmallQ,
                                /*operand_in_range=*/1, base.data(), base.size(),
                                nullptr) == HAZE_SUCCESS);
    for (void *out : d_x)
        REQUIRE(hazeTagOutput(out) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    for (std::size_t i = 0; i < base.size(); ++i) {
        std::vector<uint64_t> got(kRingDim, 0xDEADBEEFULL);
        REQUIRE(hazeMemcpy(got.data(), d_x[i], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
        for (std::size_t k = 0; k < kRingDim; ++k) {
            INFO("residue " << i << " slot " << k);
            REQUIRE(got[k] == (inputs[i][k] + mask[k]) % base[i]);
        }
    }
    haze::test::free_all_residues(d_x);
    REQUIRE(hazeFree(d_m) == HAZE_SUCCESS);
}

// ===========================================================================
// Argument validation ([unit]).
// ===========================================================================

TEST_CASE("hazeBroadcast rejects invalid arguments", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const uint64_t moduli[] = {kQ0, kQ1, kQ2};
    const hazeFheParams fhe = {.ring_dim = kRingDim, .moduli = moduli, .moduli_count = 3};
    REQUIRE(hazeConfigureDevice(&fhe, nullptr) == HAZE_SUCCESS);

    const uint64_t base[] = {kQ0, kQ1};
    void *d_a = nullptr;
    void *d_b = nullptr;
    void *d_m = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_m, kBytes) == HAZE_SUCCESS);
    void *dst_polys[] = {d_a, d_b};
    const void *src_polys[] = {d_a, d_b};

    REQUIRE(hazeBroadcastAddMrp(nullptr, src_polys, d_m, kQ2, 0, base, 2, nullptr) ==
            HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeBroadcastAddMrp(dst_polys, nullptr, d_m, kQ2, 0, base, 2, nullptr) ==
            HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeBroadcastAddMrp(dst_polys, src_polys, nullptr, kQ2, 0, base, 2, nullptr) ==
            HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeBroadcastAddMrp(dst_polys, src_polys, d_m, kQ2, 0, nullptr, 2, nullptr) ==
            HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeBroadcastAddMrp(dst_polys, src_polys, d_m, kQ2, 0, base, 0, nullptr) ==
            HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeBroadcastAddMrp(dst_polys, src_polys, d_m, 0, 0, base, 2, nullptr) ==
            HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();

    REQUIRE(hazeFree(d_a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_b) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_m) == HAZE_SUCCESS);
}

TEST_CASE("hazeBroadcast rejects an operand modulus that disagrees with the recorded one",
          "[integration]") {
    haze::test::setup_integration_mrp3_config(kRingDim, kQ0);

    // Compute a mask under the auto aux kQ1, then lie about its modulus.
    const std::vector<uint64_t> pred_in = haze::test::make_residue(kQ0, 676767ULL, kRingDim);
    void *d_pred_in = nullptr;
    void *d_mask = nullptr;
    REQUIRE(hazeMalloc(&d_pred_in, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_mask, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_pred_in, pred_in.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) ==
            HAZE_SUCCESS);
    REQUIRE(hazeIsHalfModulus(d_mask, d_pred_in, 0, nullptr) == HAZE_SUCCESS);

    const uint64_t base[] = {kQ0, kQ2};
    const std::vector<void *> d_src =
        haze::test::allocate_and_h2d_residues({haze::test::make_residue(kQ0, 1ULL, kRingDim),
                                               haze::test::make_residue(kQ2, 2ULL, kRingDim)});
    const std::vector<void *> d_dst = haze::test::allocate_dst_residues(2, kBytes);
    const std::vector<const void *> src_view = haze::test::to_const(d_src);

    REQUIRE(hazeBroadcastMulMrp(d_dst.data(), src_view.data(), d_mask, kQ2, 0, base, 2, nullptr) ==
            HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();

    haze::test::free_all_residues(d_src);
    haze::test::free_all_residues(d_dst);
    REQUIRE(hazeFree(d_pred_in) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_mask) == HAZE_SUCCESS);
}

// ===========================================================================
// Record-time trace shape ([unit][hwfmt]): pin the per-limb lift emission.
// ===========================================================================

namespace {

std::string slurp(const std::filesystem::path &path) {
    std::ifstream in(path);
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::size_t count_occurrences(const std::string &haystack, const std::string &needle) {
    std::size_t count = 0;
    for (std::size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size()))
        ++count;
    return count;
}

// Record one hazeBroadcastAddMrp (operand under kQ1, base {kQ0, kQ2}) into a
// uniquely named program dir and return the trace text.
std::string record_broadcast_trace(const std::string &program_name, bool montgomery, int in_range,
                                   uint64_t operand_modulus, const std::vector<uint64_t> &base) {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const uint64_t moduli[] = {kQ0, kQ1, kQ2};
    const hazeFheParams fhe = {.ring_dim = kRingDim, .moduli = moduli, .moduli_count = 3};
    const hazeReplayConfig replay = {.target = "FUNC_SIM",
                                     .program_name = program_name.c_str(),
                                     .program_version = "0.1",
                                     .program_description = "broadcast trace-shape test",
                                     .montgomery = montgomery ? 1 : 0,
                                     .bit_reversal = montgomery ? 1 : 0,
                                     .reduced_noise = 1};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_SUCCESS);
    uint64_t scaffold = 0;
    REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, kQ0, &scaffold) == HAZE_SUCCESS);

    std::vector<std::vector<uint64_t>> inputs(base.size());
    for (std::size_t i = 0; i < base.size(); ++i)
        inputs[i] = haze::test::make_residue(base[i], 686868ULL + i, kRingDim);
    const std::vector<void *> d_src = haze::test::allocate_and_h2d_residues(inputs);
    const std::vector<void *> d_dst = haze::test::allocate_dst_residues(base.size(), kBytes);
    void *d_m = nullptr;
    REQUIRE(hazeMalloc(&d_m, kBytes) == HAZE_SUCCESS);
    const std::vector<uint64_t> mask = binary_mask(0x5A5AULL);
    REQUIRE(hazeMemcpy(d_m, mask.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    const std::vector<const void *> src_view = haze::test::to_const(d_src);
    REQUIRE(hazeBroadcastAddMrp(d_dst.data(), src_view.data(), d_m, operand_modulus, in_range,
                                base.data(), base.size(), nullptr) == HAZE_SUCCESS);
    for (void *out : d_dst)
        REQUIRE(hazeTagOutput(out) == HAZE_SUCCESS);
    REQUIRE(hazeWriteProgram() == HAZE_SUCCESS);

    haze::test::free_all_residues(d_src);
    haze::test::free_all_residues(d_dst);
    REQUIRE(hazeFree(d_m) == HAZE_SUCCESS);
    return slurp(std::filesystem::path{program_name} / (program_name + ".fhetch"));
}

} // namespace

TEST_CASE("hazeBroadcast record-time: per-limb lift emission", "[unit][hwfmt]") {
    const std::vector<uint64_t> base = {kQ0, kQ2};
    const uint64_t half_p = (kQ1 - 1) / 2;

    SECTION("flag off, ordinary: 3-op gadget per limb + pointwise") {
        const std::string trace =
            record_broadcast_trace("haze_bc_threeop", /*montgomery=*/false, 0, kQ1, base);
        CHECK(count_occurrences(trace, "sr_addps ") == 4); // 2 shifts + 2 unshifts
        CHECK(count_occurrences(trace, "sr_mulps ") == 2); // 2 rebases
        CHECK(count_occurrences(trace, "sr_addp ") == 2);  // pointwise
        REQUIRE(trace.find(", " + std::to_string(half_p) + ",") != std::string::npos);
        const auto sw_hw =
            niobium::mod_arith::compute_switchmodulus_immediates(kQ1, kQ0, /*montgomery=*/true);
        REQUIRE(trace.find(", " + std::to_string(sw_hw.imm[2]) + ",") == std::string::npos);
    }
    SECTION("flag off, montgomery: 4-op gadget per limb + pointwise") {
        const std::string trace =
            record_broadcast_trace("haze_bc_fourop", /*montgomery=*/true, 0, kQ1, base);
        CHECK(count_occurrences(trace, "sr_addps ") == 4);
        CHECK(count_occurrences(trace, "sr_mulps ") == 4); // leading identities + rebases
        CHECK(count_occurrences(trace, "sr_addp ") == 2);
    }
    SECTION("flag on, ordinary: no lift at all") {
        const std::string trace =
            record_broadcast_trace("haze_bc_direct", /*montgomery=*/false, 1, kQ1, base);
        CHECK(count_occurrences(trace, "sr_addps ") == 0);
        CHECK(count_occurrences(trace, "sr_mulps ") == 0);
        CHECK(count_occurrences(trace, "sr_addp ") == 2);
        REQUIRE(trace.find(std::to_string(half_p)) == std::string::npos);
    }
    SECTION("flag on, montgomery: the switch is the data-format conversion") {
        const std::string trace =
            record_broadcast_trace("haze_bc_direct_mont", /*montgomery=*/true, 1, kQ1, base);
        CHECK(count_occurrences(trace, "sr_addps ") == 4);
        CHECK(count_occurrences(trace, "sr_mulps ") == 4);
        CHECK(count_occurrences(trace, "sr_addp ") == 2);
    }
    SECTION("operand modulus in the base: pass-through limb lifts nothing") {
        const std::string trace = record_broadcast_trace("haze_bc_passthrough",
                                                         /*montgomery=*/false, 0, kQ2, base);
        CHECK(count_occurrences(trace, "sr_addps ") == 2); // one gadget only (kQ0 limb)
        CHECK(count_occurrences(trace, "sr_mulps ") == 1);
        CHECK(count_occurrences(trace, "sr_addp ") == 2);
    }
    SECTION("pass-through limb under montgomery: same-ring use needs no format switch") {
        const std::string trace = record_broadcast_trace("haze_bc_passthrough_mont",
                                                         /*montgomery=*/true, 0, kQ2, base);
        CHECK(count_occurrences(trace, "sr_addps ") == 2); // one FourOp gadget (kQ0 limb)
        CHECK(count_occurrences(trace, "sr_mulps ") == 2);
        CHECK(count_occurrences(trace, "sr_addp ") == 2);
    }
    SECTION("zero unshift immediate stays inside the gadget shape") {
        const std::string trace = record_broadcast_trace("haze_bc_zeroimm", /*montgomery=*/false, 0,
                                                         kCongruentP, {kSmallQ, kQ0});
        CHECK(count_occurrences(trace, "sr_addps ") == 4);
        CHECK(count_occurrences(trace, "sr_mulps ") == 2);
        CHECK(count_occurrences(trace, "sr_addp ") == 2);
        REQUIRE(trace.find(", 0, m=") != std::string::npos); // kSmallQ | (kCongruentP-1)/2
    }
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

// ===========================================================================
// Transport hardware mode ([integration][hwfmt]: requires make test-transport).
// ===========================================================================

namespace {

bool transport_target_active() {
    const char *target = std::getenv("HAZE_TARGET");
    return target != nullptr && target[0] != '\0' && std::string_view{target} != "local";
}

// Record + flush one broadcast under the given toggles and return the
// per-limb D2H results.
std::vector<std::vector<uint64_t>>
run_broadcast_transport(bool montgomery, int in_range, Op op, uint64_t p,
                        const std::vector<uint64_t> &base,
                        const std::vector<uint64_t> &operand_vals) {
    const uint64_t moduli[] = {kQ0, kQ1, kQ2};
    const hazeFheParams fhe = {.ring_dim = kRingDim, .moduli = moduli, .moduli_count = 3};
    const hazeReplayConfig replay = {.target = haze::test::target_from_env(),
                                     .montgomery = montgomery ? 1 : 0,
                                     .bit_reversal = montgomery ? 1 : 0,
                                     .reduced_noise = 1};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_SUCCESS);
    uint64_t scaffold = 0;
    REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, kQ0, &scaffold) == HAZE_SUCCESS);

    std::vector<std::vector<uint64_t>> inputs(base.size());
    for (std::size_t i = 0; i < base.size(); ++i)
        inputs[i] = haze::test::make_residue(base[i], 696969ULL + i, kRingDim);
    const std::vector<void *> d_src = haze::test::allocate_and_h2d_residues(inputs);
    const std::vector<void *> d_dst = haze::test::allocate_dst_residues(base.size(), kBytes);
    void *d_m = nullptr;
    REQUIRE(hazeMalloc(&d_m, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_m, operand_vals.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) ==
            HAZE_SUCCESS);

    const std::vector<const void *> src_view = haze::test::to_const(d_src);
    REQUIRE(call_broadcast(op, d_dst.data(), src_view.data(), d_m, p, in_range, base.data(),
                           base.size()) == HAZE_SUCCESS);
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
    REQUIRE(hazeFree(d_m) == HAZE_SUCCESS);
    return results;
}

} // namespace

TEST_CASE("hazeBroadcast transport: A/B byte-exact vs ordinary mode", "[integration][hwfmt]") {
    if (!transport_target_active())
        SKIP("data format requires a transport target (run under make test-transport)");

    struct Arm {
        Op op;
        uint64_t p;
        std::vector<uint64_t> base;
        std::vector<uint64_t> vals;
        int in_range;
    };
    const std::vector<Arm> arms = {
        {Op::Mul, kQ1, {kQ0, kQ2}, binary_mask(0x3C3CULL), 0},
        {Op::Mul, kQ1, {kQ0, kQ2}, binary_mask(0x3C3CULL), 1},
        {Op::Sub, kQ2, {kQ0, kQ2}, boundary_operand(kQ2, 787878ULL), 0}, // pass-through limb
    };
    for (std::size_t a = 0; a < arms.size(); ++a) {
        const Arm &arm = arms[a];
        REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
        const auto ordinary = run_broadcast_transport(/*montgomery=*/false, arm.in_range, arm.op,
                                                      arm.p, arm.base, arm.vals);
        REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
        const auto encoded = run_broadcast_transport(/*montgomery=*/true, arm.in_range, arm.op,
                                                     arm.p, arm.base, arm.vals);
        INFO("arm " << a << " in_range " << arm.in_range);
        REQUIRE(ordinary == encoded);
    }
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("hazeBroadcast transport: non-synthesizable operand modulus fails loudly",
          "[integration][hwfmt]") {
    if (!transport_target_active())
        SKIP("input synthesis runs on transport targets (run under make test-transport)");

    // Flag off records the operand under its own modulus, which the replay
    // bridge must synthesize; a prime not ≡ 1 mod 2N fails at flush — cleanly,
    // never silently.
    constexpr uint64_t kBadP = 1000003ULL; // prime, not ≡ 1 mod 8192
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const uint64_t moduli[] = {kQ0, kQ1, kQ2};
    const hazeFheParams fhe = {.ring_dim = kRingDim, .moduli = moduli, .moduli_count = 3};
    const hazeReplayConfig replay = {.target = haze::test::target_from_env(), .reduced_noise = 1};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_SUCCESS);
    uint64_t scaffold = 0;
    REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, kQ0, &scaffold) == HAZE_SUCCESS);

    const std::vector<uint64_t> base = {kQ0, kQ1};
    std::vector<std::vector<uint64_t>> inputs(base.size());
    for (std::size_t i = 0; i < base.size(); ++i)
        inputs[i] = haze::test::make_residue(base[i], 797979ULL + i, kRingDim);
    const std::vector<void *> d_src = haze::test::allocate_and_h2d_residues(inputs);
    const std::vector<void *> d_dst = haze::test::allocate_dst_residues(base.size(), kBytes);
    void *d_m = nullptr;
    REQUIRE(hazeMalloc(&d_m, kBytes) == HAZE_SUCCESS);
    const std::vector<uint64_t> vals = haze::test::make_residue(kBadP, 808080ULL, kRingDim);
    REQUIRE(hazeMemcpy(d_m, vals.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    const std::vector<const void *> src_view = haze::test::to_const(d_src);
    REQUIRE(hazeBroadcastAddMrp(d_dst.data(), src_view.data(), d_m, kBadP, 0, base.data(),
                                base.size(), nullptr) == HAZE_SUCCESS);
    for (void *out : d_dst)
        REQUIRE(hazeTagOutput(out) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() != HAZE_SUCCESS);
    hazeGetLastError();

    haze::test::free_all_residues(d_src);
    haze::test::free_all_residues(d_dst);
    REQUIRE(hazeFree(d_m) == HAZE_SUCCESS);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}
