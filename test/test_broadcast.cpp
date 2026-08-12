// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// hazeBroadcast{Add,Sub,Rsub,Mul}Mrp tests: [unit] validation and trace-shape
// pinning, [integration] golden values.

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
// Small NTT-friendly prime (~2^30, ≡ 1 mod 8192); the replay bridge's floor.
constexpr uint64_t kSmallQ = 1073750017ULL;
// 48-bit NTT-friendly prime with kSmallQ | (p-1)/2: lifting from it into a
// kSmallQ limb zeroes the gadget's unshift immediate (the sim copy path).
constexpr uint64_t kCongruentP = 211107843342337ULL;

enum class Op : std::uint8_t { Add, Sub, Rsub, Mul };

hazeError_t call_broadcast(Op op, void *const *dst, const void *const *src, const void *operand,
                           int in_range, const uint64_t *base, std::size_t len) {
    switch (op) {
    case Op::Add:
        return hazeBroadcastAddMrp(dst, src, operand, in_range, base, len, nullptr);
    case Op::Sub:
        return hazeBroadcastSubMrp(dst, src, operand, in_range, base, len, nullptr);
    case Op::Rsub:
        return hazeBroadcastRsubMrp(dst, src, operand, in_range, base, len, nullptr);
    case Op::Mul:
        return hazeBroadcastMulMrp(dst, src, operand, in_range, base, len, nullptr);
    }
    return HAZE_ERROR_INVALID_VALUE;
}

// One-shot configuration over an arbitrary chain (bridge scaffold from moduli[0]).
void setup_chain_config(const std::vector<uint64_t> &moduli) {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const hazeFheParams fhe = {
        .ring_dim = kRingDim, .moduli = moduli.data(), .moduli_count = moduli.size()};
    const hazeReplayConfig replay = {.target = haze::test::target_from_env(), .reduced_noise = 1};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_SUCCESS);
    uint64_t scaffold = 0;
    REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, moduli[0], &scaffold) == HAZE_SUCCESS);
}

// H2D `vals` and record them under moduli[mod_idx] via a zero-add (the values
// pass through verbatim); the broadcast recovers the operand's ring from that
// recorded modulus. Returns {raw, bound}; caller frees both.
std::pair<void *, void *> bind_operand(const std::vector<uint64_t> &vals, int mod_idx) {
    void *d_raw = nullptr;
    void *d_m = nullptr;
    REQUIRE(hazeMalloc(&d_raw, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_m, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_raw, vals.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeAddScalar(d_m, d_raw, 0, mod_idx, nullptr) == HAZE_SUCCESS);
    return {d_raw, d_m};
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

// Run one broadcast over pseudorandom limbs and an operand recorded under
// moduli[p_idx] (= p), flush, and require exact equality against the lift+op
// oracle on every coefficient. The operand limb for base[i] == p is used
// verbatim (no lift), matching the pass-through.
void run_broadcast_golden(Op op, const std::vector<uint64_t> &base, uint64_t p, int p_idx,
                          const std::vector<uint64_t> &operand_vals, int in_range, uint64_t seed) {
    std::vector<std::vector<uint64_t>> inputs(base.size());
    for (std::size_t i = 0; i < base.size(); ++i)
        inputs[i] = haze::test::make_residue(base[i], seed + i, kRingDim);

    const std::vector<void *> d_src = haze::test::allocate_and_h2d_residues(inputs);
    const std::vector<void *> d_dst = haze::test::allocate_dst_residues(base.size(), kBytes);
    const auto [d_raw, d_m] = bind_operand(operand_vals, p_idx);

    const std::vector<const void *> src_view = haze::test::to_const(d_src);
    REQUIRE(call_broadcast(op, d_dst.data(), src_view.data(), d_m, in_range, base.data(),
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
    REQUIRE(hazeFree(d_raw) == HAZE_SUCCESS);
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

namespace {

// Multiply every limb by a hazeIsHalfModulus mask and check each coefficient
// against the host predicate. Split by in_range so the per-target matrix is
// explicit: in_range=1 elides the lift and is ordinary-form only.
void run_mask_broadcast(int in_range) {
    haze::test::setup_integration_mrp3_config(kRingDim, kQ0); // {kQ0, kQ1, kQ2}

    // Mask: SRP predicate of a value under kQ0; recorded under the generated aux.
    // make_residue alone lands entirely below h, which would make every mask bit
    // 0 and the multiply below vacuous; lift the odd slots into (h, kQ0-1].
    std::vector<uint64_t> pred_in = haze::test::make_residue(kQ0, 424242ULL, kRingDim);
    const uint64_t h_q0 = (kQ0 - 1) / 2;
    for (std::size_t k = 1; k < kRingDim; k += 2)
        pred_in[k] = h_q0 + 1 + (pred_in[k] % (kQ0 - h_q0 - 1));
    pred_in[0] = h_q0;     // exact threshold: predicate is strict >, so bit 0
    pred_in[2] = kQ0 - 1;  // top of the ring: bit 1
    pred_in[4] = h_q0 + 1; // first value above the threshold: bit 1
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

    REQUIRE(hazeBroadcastMulMrp(d_dst.data(), src_view.data(), d_mask, in_range, base.data(),
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

} // namespace

TEST_CASE("hazeBroadcastMulMrp: hazeIsHalfModulus mask, lifted", "[integration]") {
    run_mask_broadcast(/*in_range=*/0);
}

TEST_CASE("hazeBroadcastMulMrp: hazeIsHalfModulus mask, lift elided", "[integration]") {
    haze::test::skip_if_hw_elision();
    run_mask_broadcast(/*in_range=*/1);
}

TEST_CASE("hazeBroadcast: general values, small operand prime (p < q)", "[integration]") {
    setup_chain_config({kQ0, kQ1, kSmallQ});
    const std::vector<uint64_t> base = {kQ0, kQ1};
    const std::vector<uint64_t> vals = boundary_operand(kSmallQ, 626262ULL);
    for (Op op : {Op::Add, Op::Sub, Op::Rsub, Op::Mul})
        run_broadcast_golden(op, base, kSmallQ, /*p_idx=*/2, vals, /*in_range=*/0,
                             /*seed=*/700000ULL + static_cast<uint64_t>(op));
}

TEST_CASE("hazeBroadcast: general values, large operand prime (p > q)", "[integration]") {
    haze::test::setup_integration_mrp3_config(kRingDim, kQ0);
    const std::vector<uint64_t> base = {kQ0, kQ1};
    const std::vector<uint64_t> vals = boundary_operand(kQ2, 636363ULL);
    for (Op op : {Op::Add, Op::Sub, Op::Rsub, Op::Mul})
        run_broadcast_golden(op, base, kQ2, /*p_idx=*/2, vals, /*in_range=*/0,
                             /*seed=*/710000ULL + static_cast<uint64_t>(op));
}

TEST_CASE("hazeBroadcast: zero unshift immediate (base prime divides half the operand prime)",
          "[integration]") {
    // kSmallQ | (kCongruentP-1)/2 zeroes the kSmallQ limb's unshift immediate,
    // hitting the simulator's addps-imm-0 copy path inside the gadget.
    setup_chain_config({kSmallQ, kQ0, kCongruentP});
    const std::vector<uint64_t> base = {kSmallQ, kQ0};
    const std::vector<uint64_t> vals = boundary_operand(kCongruentP, 646400ULL);
    for (Op op : {Op::Add, Op::Sub, Op::Rsub, Op::Mul})
        run_broadcast_golden(op, base, kCongruentP, /*p_idx=*/2, vals, /*in_range=*/0,
                             /*seed=*/740000ULL + static_cast<uint64_t>(op));
}

TEST_CASE("hazeBroadcast: direct path with general in-range values", "[integration]") {
    // operand_in_range with non-binary coefficients: all values satisfy the
    // range contract (<= (p-1)/2 and below every base prime), so the lift is
    // elided and the results must match the lift oracle.
    haze::test::skip_if_hw_elision();
    setup_chain_config({kQ0, kQ2, kSmallQ});
    const std::vector<uint64_t> base = {kQ0, kQ2};
    std::vector<uint64_t> vals =
        haze::test::make_residue(((kSmallQ - 1) / 2) + 1, 656600ULL, kRingDim);
    vals[0] = 0;
    vals[1] = 1;
    vals[2] = (kSmallQ - 1) / 2; // the contract's upper edge
    for (Op op : {Op::Add, Op::Sub, Op::Rsub, Op::Mul})
        run_broadcast_golden(op, base, kSmallQ, /*p_idx=*/2, vals, /*in_range=*/1,
                             /*seed=*/750000ULL + static_cast<uint64_t>(op));
}

TEST_CASE("hazeBroadcast: operand modulus inside the base (pass-through limb)", "[integration]") {
    haze::test::setup_integration_mrp3_config(kRingDim, kQ0);
    const std::vector<uint64_t> base = {kQ0, kQ1};
    const std::vector<uint64_t> vals = boundary_operand(kQ1, 646464ULL);
    run_broadcast_golden(Op::Sub, base, kQ1, /*p_idx=*/1, vals, /*in_range=*/0,
                         /*seed=*/730000ULL);
}

TEST_CASE("hazeBroadcast: H2D overwrite clears a stale recorded operand modulus", "[integration]") {
    haze::test::skip_if_hw_elision();
    // An address that held a compute result (recorded under the aux prime)
    // and is then H2D-overwritten is a fresh raw operand again: the recovery
    // rejects it until a modulus-carrying op records a ring for the new bytes.
    setup_chain_config({kQ0, kQ2, kSmallQ});
    const std::vector<uint64_t> pred_in = haze::test::make_residue(kQ0, 767676ULL, kRingDim);
    void *d_t = nullptr;
    void *d_in = nullptr;
    REQUIRE(hazeMalloc(&d_t, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d_in, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_in, pred_in.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeIsHalfModulus(d_t, d_in, 0, nullptr) == HAZE_SUCCESS); // recorded under the aux

    const std::vector<uint64_t> mask = binary_mask(0x7E7EULL);
    REQUIRE(hazeMemcpy(d_t, mask.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    const std::vector<uint64_t> base = {kQ0, kQ2};
    std::vector<std::vector<uint64_t>> inputs(base.size());
    for (std::size_t i = 0; i < base.size(); ++i)
        inputs[i] = haze::test::make_residue(base[i], 777700ULL + i, kRingDim);
    const std::vector<void *> d_src = haze::test::allocate_and_h2d_residues(inputs);
    const std::vector<void *> d_dst = haze::test::allocate_dst_residues(base.size(), kBytes);
    const std::vector<const void *> src_view = haze::test::to_const(d_src);
    // The stale kQ2 recording is gone: the raw bytes are rejected...
    REQUIRE(hazeBroadcastAddMrp(d_dst.data(), src_view.data(), d_t, /*operand_in_range=*/1,
                                base.data(), base.size(), nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    // ...until a zero-add records a ring for them (kSmallQ at index 2).
    REQUIRE(hazeAddScalar(d_t, d_t, 0, 2, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeBroadcastAddMrp(d_dst.data(), src_view.data(), d_t, /*operand_in_range=*/1,
                                base.data(), base.size(), nullptr) == HAZE_SUCCESS);
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
    haze::test::skip_if_hw_elision();
    setup_chain_config({kQ0, kQ1, kSmallQ});
    const std::vector<uint64_t> base = {kQ0, kQ1};
    std::vector<std::vector<uint64_t>> inputs(base.size());
    for (std::size_t i = 0; i < base.size(); ++i)
        inputs[i] = haze::test::make_residue(base[i], 656565ULL + i, kRingDim);
    const std::vector<uint64_t> mask = binary_mask(0xA5A5ULL);

    const std::vector<void *> d_x = haze::test::allocate_and_h2d_residues(inputs);
    const auto [d_raw, d_m] = bind_operand(mask, /*mod_idx=*/2); // kSmallQ

    const std::vector<const void *> src_view = haze::test::to_const(d_x);
    REQUIRE(hazeBroadcastAddMrp(d_x.data(), src_view.data(), d_m, /*operand_in_range=*/1,
                                base.data(), base.size(), nullptr) == HAZE_SUCCESS);
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
    REQUIRE(hazeFree(d_raw) == HAZE_SUCCESS);
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

    REQUIRE(hazeBroadcastAddMrp(nullptr, src_polys, d_m, 0, base, 2, nullptr) ==
            HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeBroadcastAddMrp(dst_polys, nullptr, d_m, 0, base, 2, nullptr) ==
            HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeBroadcastAddMrp(dst_polys, src_polys, nullptr, 0, base, 2, nullptr) ==
            HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeBroadcastAddMrp(dst_polys, src_polys, d_m, 0, nullptr, 2, nullptr) ==
            HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeBroadcastAddMrp(dst_polys, src_polys, d_m, 0, base, 0, nullptr) ==
            HAZE_ERROR_INVALID_VALUE);
    // d_m was never computed on: no recorded modulus, so the recovery rejects it.
    REQUIRE(hazeBroadcastAddMrp(dst_polys, src_polys, d_m, 0, base, 2, nullptr) ==
            HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();

    REQUIRE(hazeFree(d_a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_b) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_m) == HAZE_SUCCESS);
}

TEST_CASE("hazeBroadcast rejects an operand recorded under an even modulus", "[integration]") {
    // The centered lift's h = (p-1)/2 needs an odd operand ring; an operand
    // bound under an even chain entry is rejected at recovery.
    setup_chain_config({kQ0, kQ1, 2}); // 2: the one even prime the config accepts

    const uint64_t base[] = {kQ0, kQ1};
    const std::vector<void *> d_src =
        haze::test::allocate_and_h2d_residues({haze::test::make_residue(kQ0, 1ULL, kRingDim),
                                               haze::test::make_residue(kQ1, 2ULL, kRingDim)});
    const std::vector<void *> d_dst = haze::test::allocate_dst_residues(2, kBytes);
    const std::vector<const void *> src_view = haze::test::to_const(d_src);
    const auto [d_raw, d_m] = bind_operand(binary_mask(0x1B1BULL), /*mod_idx=*/2);

    REQUIRE(hazeBroadcastMulMrp(d_dst.data(), src_view.data(), d_m, 0, base, 2, nullptr) ==
            HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();

    haze::test::free_all_residues(d_src);
    haze::test::free_all_residues(d_dst);
    REQUIRE(hazeFree(d_raw) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_m) == HAZE_SUCCESS);
}

// ===========================================================================
// Record-time trace shape ([unit]): pin the per-limb lift emission.
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

// Record one hazeBroadcastAddMrp (operand bound under moduli[p_idx] by a
// zero-add — every trace below therefore carries one extra sr_addps) into a
// uniquely named program dir and return the trace text. The non-local target
// keeps this off the simulator path; hazeWriteProgram never replays.
std::string record_broadcast_trace(const std::string &program_name, int in_range,
                                   const std::vector<uint64_t> &moduli, int p_idx,
                                   const std::vector<uint64_t> &base) {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const hazeFheParams fhe = {
        .ring_dim = kRingDim, .moduli = moduli.data(), .moduli_count = moduli.size()};
    const hazeReplayConfig replay = {.target = "FUNC_SIM",
                                     .program_name = program_name.c_str(),
                                     .program_version = "0.1",
                                     .program_description = "broadcast trace-shape test",
                                     .reduced_noise = 1};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_SUCCESS);
    uint64_t scaffold = 0;
    REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, moduli[0], &scaffold) == HAZE_SUCCESS);

    std::vector<std::vector<uint64_t>> inputs(base.size());
    for (std::size_t i = 0; i < base.size(); ++i)
        inputs[i] = haze::test::make_residue(base[i], 686868ULL + i, kRingDim);
    const std::vector<void *> d_src = haze::test::allocate_and_h2d_residues(inputs);
    const std::vector<void *> d_dst = haze::test::allocate_dst_residues(base.size(), kBytes);
    const auto [d_raw, d_m] = bind_operand(binary_mask(0x5A5AULL), p_idx);

    const std::vector<const void *> src_view = haze::test::to_const(d_src);
    REQUIRE(hazeBroadcastAddMrp(d_dst.data(), src_view.data(), d_m, in_range, base.data(),
                                base.size(), nullptr) == HAZE_SUCCESS);
    for (void *out : d_dst)
        REQUIRE(hazeTagOutput(out) == HAZE_SUCCESS);
    REQUIRE(hazeWriteProgram() == HAZE_SUCCESS);

    haze::test::free_all_residues(d_src);
    haze::test::free_all_residues(d_dst);
    REQUIRE(hazeFree(d_raw) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_m) == HAZE_SUCCESS);
    return slurp(std::filesystem::path{program_name} / (program_name + ".fhetch"));
}

} // namespace

TEST_CASE("hazeBroadcast record-time: per-limb lift emission", "[unit]") {
    const std::vector<uint64_t> base = {kQ0, kQ2};
    const uint64_t half_p = (kQ1 - 1) / 2;

    SECTION("flag off: 3-op gadget per limb + pointwise") {
        const std::string trace =
            record_broadcast_trace("haze_bc_threeop", 0, {kQ0, kQ1, kQ2}, /*p_idx=*/1, base);
        CHECK(count_occurrences(trace, "sr_addps ") == 5); // bind + 2 shifts + 2 unshifts
        CHECK(count_occurrences(trace, "sr_mulps ") == 2); // 2 rebases
        CHECK(count_occurrences(trace, "sr_addp ") == 2);  // pointwise
        // The ordinary-form shift immediate: the trace the hardware driver's
        // switchmod matcher consumes, and recomputes its own constants from.
        REQUIRE(trace.find(", " + std::to_string(half_p) + ",") != std::string::npos);
    }
    SECTION("flag on: no lift at all") {
        const std::string trace =
            record_broadcast_trace("haze_bc_direct", 1, {kQ0, kQ1, kQ2}, /*p_idx=*/1, base);
        CHECK(count_occurrences(trace, "sr_addps ") == 1); // the bind only
        CHECK(count_occurrences(trace, "sr_mulps ") == 0);
        CHECK(count_occurrences(trace, "sr_addp ") == 2);
        REQUIRE(trace.find(std::to_string(half_p)) == std::string::npos);
    }
    SECTION("operand modulus in the base: pass-through limb lifts nothing") {
        const std::string trace =
            record_broadcast_trace("haze_bc_passthrough", 0, {kQ0, kQ1, kQ2}, /*p_idx=*/2, base);
        CHECK(count_occurrences(trace, "sr_addps ") == 3); // bind + one gadget (kQ0 limb)
        CHECK(count_occurrences(trace, "sr_mulps ") == 1);
        CHECK(count_occurrences(trace, "sr_addp ") == 2);
    }
    SECTION("zero unshift immediate stays inside the gadget shape") {
        const std::string trace = record_broadcast_trace(
            "haze_bc_zeroimm", 0, {kSmallQ, kQ0, kCongruentP}, /*p_idx=*/2, {kSmallQ, kQ0});
        CHECK(count_occurrences(trace, "sr_addps ") == 5);
        CHECK(count_occurrences(trace, "sr_mulps ") == 2);
        CHECK(count_occurrences(trace, "sr_addp ") == 2);
        // The bind and the zeroed unshift (kSmallQ | (kCongruentP-1)/2).
        CHECK(count_occurrences(trace, ", 0, m=") == 2);
    }
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

// ===========================================================================
// Transport-only ([integration]: requires make test-transport).
// ===========================================================================

TEST_CASE("hazeBroadcast transport: non-synthesizable operand modulus fails loudly",
          "[integration]") {
    if (!haze::test::transport_target_active())
        SKIP("input synthesis runs on transport targets (run under make test-transport)");

    // Flag off records the operand under its own modulus, which the replay
    // bridge must synthesize; a prime not ≡ 1 mod 2N fails at flush — cleanly,
    // never silently.
    constexpr uint64_t kBadP = 1000003ULL; // prime, not ≡ 1 mod 8192
    setup_chain_config({kQ0, kQ1, kBadP});

    const std::vector<uint64_t> base = {kQ0, kQ1};
    std::vector<std::vector<uint64_t>> inputs(base.size());
    for (std::size_t i = 0; i < base.size(); ++i)
        inputs[i] = haze::test::make_residue(base[i], 797979ULL + i, kRingDim);
    const std::vector<void *> d_src = haze::test::allocate_and_h2d_residues(inputs);
    const std::vector<void *> d_dst = haze::test::allocate_dst_residues(base.size(), kBytes);
    const std::vector<uint64_t> vals = haze::test::make_residue(kBadP, 808080ULL, kRingDim);
    const auto [d_raw, d_m] = bind_operand(vals, /*mod_idx=*/2);

    const std::vector<const void *> src_view = haze::test::to_const(d_src);
    REQUIRE(hazeBroadcastAddMrp(d_dst.data(), src_view.data(), d_m, 0, base.data(), base.size(),
                                nullptr) == HAZE_SUCCESS);
    for (void *out : d_dst)
        REQUIRE(hazeTagOutput(out) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() != HAZE_SUCCESS);
    hazeGetLastError();

    haze::test::free_all_residues(d_src);
    haze::test::free_all_residues(d_dst);
    REQUIRE(hazeFree(d_raw) == HAZE_SUCCESS);
    REQUIRE(hazeFree(d_m) == HAZE_SUCCESS);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}
