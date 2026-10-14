// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
// The contents of this file and all related materials provided herein (the
// "Product") may not be used except pursuant to a separate written
// agreement signed by a duly authorized officer of Niobium Microsystems,
// Inc. (a "License Agreement").
// Without limiting the foregoing, you may not, at any time or for any
// reason, directly or indirectly, in whole or in part: (i) copy, modify,
// or create derivative works of the Product; (ii) rent, lease, lend, sell,
// sublicense, assign, distribute, publish, transfer, or otherwise make
// available the Product; (iii) reverse engineer, disassemble, decompile,
// decode, or adapt the Product; or (iv) remove any proprietary notices
// from the Product.
//
// 50 sequential sessions in one process, each with its own pinned program
// directory under one shared parent, at a 41-prime key-switching shape: a
// 37-prime chain Q plus 4 special primes P. The session in the middle has one
// flush fail deliberately and then records and flushes again, and every session
// must round-trip exactly. Each session removes its program directory when it
// ends (the replay's own output nests inside it), so disk use stays at one
// session's worth.
//
// The default case runs at N=4096 (about 10 s on `local`). The soak case runs
// the same 50 sessions at N=65536 (about 140 s on `local`, 820 MB peak RSS); it
// is hidden, so select it by tag:
//   cd build/runs && HAZE_TARGET=local ../haze_tests "[soak]"
//   HAZE_TEST_FILTER="[soak]" make test-transport MODE=release NIOBIUM_COMPILER_ROOT=<compiler>

#include "common/mod_arith.hpp"
#include "core/input_spill.hpp"
#include "integration_helpers.hpp"

#include <array>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr uint64_t kDefaultRingDim = 4096;
constexpr uint64_t kSoakRingDim = 65536;
constexpr std::size_t kChainPrimeCount = 37;
constexpr std::size_t kSpecialPrimeCount = 4;
constexpr unsigned kChainPrimeBits = 40;
constexpr unsigned kSpecialPrimeBits = 60;
constexpr int kSessionCount = 50;
constexpr int kFailedSession = kSessionCount / 2;
// hazeInputGroupName: 64 bytes always holds a name plus its terminator.
constexpr std::size_t kGroupNameBytes = 64;

// The `count` largest primes q < 2^bits with q = 1 mod 2N, descending.
std::vector<uint64_t> ntt_primes_below(uint64_t ring_dim, unsigned bits, std::size_t count) {
    const uint64_t step = 2 * ring_dim;
    std::vector<uint64_t> primes;
    for (uint64_t q = (((uint64_t{1} << bits) - 1) / step * step) + 1; primes.size() < count;
         q -= step) {
        if (haze::is_prime_u64(q))
            primes.push_back(q);
    }
    return primes;
}

// The ring dimension and the two bases every session records over.
struct Shape {
    uint64_t ring_dim;
    std::vector<uint64_t> q;
    std::vector<uint64_t> qp;

    std::size_t bytes() const { return ring_dim * sizeof(uint64_t); }
};

Shape key_switching_shape(uint64_t ring_dim) {
    Shape shape{.ring_dim = ring_dim,
                .q = ntt_primes_below(ring_dim, kChainPrimeBits, kChainPrimeCount),
                .qp = {}};
    shape.qp = shape.q;
    for (uint64_t p : ntt_primes_below(ring_dim, kSpecialPrimeBits, kSpecialPrimeCount))
        shape.qp.push_back(p);
    return shape;
}

std::vector<std::vector<uint64_t>> residues_for(const Shape &shape,
                                                const std::vector<uint64_t> &base, uint64_t seed) {
    std::vector<std::vector<uint64_t>> r(base.size());
    for (std::size_t i = 0; i < base.size(); ++i)
        r[i] = haze::test::make_residue(base[i], seed + i, shape.ring_dim);
    return r;
}

std::vector<std::vector<uint64_t>> download(const Shape &shape, const std::vector<void *> &src,
                                            const std::vector<uint64_t> &base) {
    std::vector<std::vector<uint64_t>> out(base.size(), std::vector<uint64_t>(shape.ring_dim));
    std::vector<void *> dst(base.size());
    for (std::size_t i = 0; i < base.size(); ++i)
        dst[i] = out[i].data();
    const auto src_const = haze::test::to_const(src);
    REQUIRE(hazeMemcpyMrp(dst.data(), src_const.data(), shape.bytes(), HAZE_MEMCPY_DEVICE_TO_HOST,
                          base.data(), base.size()) == HAZE_SUCCESS);
    return out;
}

// One epoch: upload a chain-shaped x over Q and a key-shaped k over QP, record
// x + x over Q and k * k over QP, and tag both results.
struct Epoch {
    std::vector<std::vector<uint64_t>> x;
    std::vector<std::vector<uint64_t>> k;
    std::vector<void *> x_dev;
    std::vector<void *> k_dev;
    std::vector<void *> sum_dev;
    std::vector<void *> product_dev;
};

Epoch record_epoch(const Shape &shape, uint64_t seed) {
    Epoch e;
    e.x = residues_for(shape, shape.q, seed);
    e.k = residues_for(shape, shape.qp, seed + shape.qp.size());
    e.x_dev = haze::test::allocate_and_h2d_residues(e.x, shape.q);
    e.k_dev = haze::test::allocate_and_h2d_residues(e.k, shape.qp);
    e.sum_dev = haze::test::allocate_dst_residues(shape.q.size(), shape.bytes());
    e.product_dev = haze::test::allocate_dst_residues(shape.qp.size(), shape.bytes());
    const auto x = haze::test::to_const(e.x_dev);
    const auto k = haze::test::to_const(e.k_dev);
    REQUIRE(hazeAddMrp(e.sum_dev.data(), x.data(), x.data(), shape.q.data(), shape.q.size(),
                       nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeMulMrp(e.product_dev.data(), k.data(), k.data(), shape.qp.data(), shape.qp.size(),
                       nullptr) == HAZE_SUCCESS);
    for (void *out : e.sum_dev)
        REQUIRE(hazeTagOutput(out) == HAZE_SUCCESS);
    for (void *out : e.product_dev)
        REQUIRE(hazeTagOutput(out) == HAZE_SUCCESS);
    return e;
}

void check_epoch(const Shape &shape, const Epoch &e) {
    const auto sum = download(shape, e.sum_dev, shape.q);
    for (std::size_t i = 0; i < shape.q.size(); ++i) {
        std::vector<uint64_t> expected(shape.ring_dim);
        for (std::size_t j = 0; j < shape.ring_dim; ++j)
            expected[j] = haze::test::add_mod(e.x[i][j], e.x[i][j], shape.q[i]);
        CAPTURE(i);
        REQUIRE(sum[i] == expected);
    }
    const auto product = download(shape, e.product_dev, shape.qp);
    for (std::size_t i = 0; i < shape.qp.size(); ++i) {
        std::vector<uint64_t> expected(shape.ring_dim);
        for (std::size_t j = 0; j < shape.ring_dim; ++j)
            expected[j] = haze::mulmod_u64(e.k[i][j], e.k[i][j], shape.qp[i]);
        CAPTURE(i);
        REQUIRE(product[i] == expected);
    }
}

void free_epoch(const Epoch &e) {
    haze::test::free_all_residues(e.x_dev);
    haze::test::free_all_residues(e.k_dev);
    haze::test::free_all_residues(e.sum_dev);
    haze::test::free_all_residues(e.product_dev);
}

void run_sessions(uint64_t ring_dim) {
    const Shape shape = key_switching_shape(ring_dim);
    const fs::path root = fs::absolute("haze_sequential_sessions");
    fs::remove_all(root);
    for (int session = 0; session < kSessionCount; ++session) {
        CAPTURE(session);
        REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
        const fs::path dir = root / ("session_" + std::to_string(session));
        const std::string dir_string = dir.string();
        const hazeFheParams fhe = {
            .ring_dim = ring_dim, .moduli = shape.qp.data(), .moduli_count = shape.qp.size()};
        const hazeReplayConfig replay = {.target = haze::test::target_from_env(),
                                         .program_directory = dir_string.c_str(),
                                         .reduced_noise = 1};
        REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_SUCCESS);
        REQUIRE(hazeReplayBridgeInitCryptoContext(ring_dim, shape.qp[0]) == HAZE_SUCCESS);

        const auto seed = static_cast<uint64_t>(session) * 0x1000U;
        if (session == kFailedSession) {
            // Retire x's spill binding behind the hook's back, so this flush fails
            // in the post-recording hook exactly as a lost spill file would.
            const Epoch failed = record_epoch(shape, seed);
            std::array<char, kGroupNameBytes> name{};
            REQUIRE(hazeInputGroupName(failed.x_dev[0], name.data(), name.size()) == HAZE_SUCCESS);
            REQUIRE(haze::input_spill().take_named(name.data()).has_value());
            REQUIRE(hazeFlush() == HAZE_ERROR_INTERNAL);
            free_epoch(failed);
        }
        const Epoch e = record_epoch(shape, seed + 1);
        REQUIRE(hazeFlush() == HAZE_SUCCESS);
        check_epoch(shape, e);
        free_epoch(e);
        fs::remove_all(dir); // the replay's own output nests inside it
    }
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    fs::remove_all(root);
}

} // namespace

TEST_CASE("sequential sessions round-trip across a failed flush", "[integration]") {
    run_sessions(kDefaultRingDim);
}

TEST_CASE("sequential sessions round-trip across a failed flush at N=65536", "[.soak]") {
    run_sessions(kSoakRingDim);
}
