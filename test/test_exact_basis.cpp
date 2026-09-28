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
// Exact CRT base conversion (core/exact_basis.hpp): the raw-lift gadget is
// recorded white-box through haze_objs and checked against direct integer
// arithmetic; hazeModUpExact and hazeModDownExact are checked against an
// OpenFHE BigInteger CRT reference.

#include "common/handle.hpp"
#include "common/mod_arith.hpp"
#include "core/epoch.hpp"
#include "core/exact_basis.hpp"
#include "integration_helpers.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <math/math-hal.h>
#include <random>
#include <utility>
#include <vector>

namespace {

constexpr uint64_t kRingDim = 2048;
constexpr std::size_t kBytes = kRingDim * sizeof(uint64_t);
// Prime sizes of a key-switching shape: ciphertext-chain primes just below
// 2^40, special primes just below 2^60.
constexpr unsigned kChainPrimeBits = 40;
constexpr unsigned kSpecialPrimeBits = 60;
constexpr uint64_t kSeed = 20260928;

// The `count` largest primes below 2^bits that are 1 mod 2N, NTT-friendly for
// kRingDim, in descending order.
std::vector<uint64_t> ntt_primes_below(unsigned bits, std::size_t count) {
    constexpr uint64_t kStep = 2 * kRingDim;
    std::vector<uint64_t> primes;
    for (uint64_t c = (uint64_t{1} << bits) - kStep + 1; primes.size() < count; c -= kStep) {
        if (haze::is_prime_u64(c))
            primes.push_back(c);
    }
    return primes;
}

// Reset, then configure every prime the case records under.
void configure(const std::vector<uint64_t> &moduli, int reduced_noise) {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const hazeFheParams fhe = {
        .ring_dim = kRingDim, .moduli = moduli.data(), .moduli_count = moduli.size()};
    const hazeReplayConfig replay = {.target = haze::test::target_from_env(),
                                     .reduced_noise = reduced_noise};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_SUCCESS);
    REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, moduli.front()) == HAZE_SUCCESS);
}

// Residues mod q: the boundary values 0, 1, (q-1)/2, (q+1)/2 and q-1, then
// uniform ones.
std::vector<uint64_t> boundary_then_uniform(uint64_t q, std::mt19937_64 &rng) {
    std::vector<uint64_t> v = {0, 1, (q - 1) / 2, (q + 1) / 2, q - 1};
    std::uniform_int_distribution<uint64_t> uniform(0, q - 1);
    while (v.size() < kRingDim)
        v.push_back(uniform(rng));
    return v;
}

// D2H `dst` and compare it with `want` slot by slot.
void require_residues(void *dst, const std::vector<uint64_t> &want, uint64_t p) {
    std::vector<uint64_t> got(kRingDim);
    REQUIRE(hazeMemcpy(got.data(), dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    for (std::size_t k = 0; k < kRingDim; ++k) {
        INFO("mod " << p << " slot " << k);
        REQUIRE(got[k] == want[k]);
    }
}

// Records raw_lift of the residue at src (mod q) into dst[i] (mod dst_base[i]).
void record_raw_lift(void *src, uint64_t q, const std::vector<uint64_t> &dst_base,
                     const std::vector<void *> &dst) {
    haze::EpochSession session;
    REQUIRE(haze::epoch().require_recording_locked().has_value());
    auto v = haze::epoch().lookup_or_create_locked(haze::to_dev_addr(src));
    REQUIRE(v.has_value());
    const auto lifted = haze::raw_lift(*v, q, dst_base);
    REQUIRE(lifted.size() == dst_base.size());
    for (std::size_t i = 0; i < dst.size(); ++i) {
        haze::epoch().store_compute_result_locked(haze::to_dev_addr(dst[i]), lifted[i],
                                                  dst_base[i]);
    }
}

using lbcrypto::BigInteger;

// The 41-prime key-switching shape: 37 ciphertext-chain primes and 4 special
// primes, the chain split into one single-prime digit and runs of four.
constexpr std::size_t kChainCount = 37;
constexpr std::size_t kSpecialCount = 4;
constexpr std::size_t kDigitLen = 4;

uint64_t mod_u64(const BigInteger &x, uint64_t q) {
    return x.Mod(BigInteger(q)).ConvertToInt<uint64_t>();
}

std::size_t index_of(const std::vector<uint64_t> &base, uint64_t q) {
    const auto it = std::ranges::find(base, q);
    REQUIRE(it != base.end());
    return static_cast<std::size_t>(it - base.begin());
}

// CRT over one base in OpenFHE BigInteger arithmetic, independent of the
// recorded gadgets.
class Crt {
  public:
    explicit Crt(std::vector<uint64_t> base) : base_(std::move(base)) {
        for (const uint64_t q : base_)
            product_ = product_ * BigInteger(q);
        for (const uint64_t q : base_) {
            const BigInteger cofactor = product_ / BigInteger(q);
            weights_.push_back(cofactor * cofactor.Mod(BigInteger(q)).ModInverse(BigInteger(q)));
        }
    }

    const BigInteger &product() const { return product_; }

    // The integer in [0, product()) whose residue mod base[i] is residues[i].
    BigInteger integer(const std::vector<uint64_t> &residues) const {
        BigInteger x(0);
        for (std::size_t i = 0; i < base_.size(); ++i)
            x = x + (BigInteger(residues[i]) * weights_[i]);
        return x.Mod(product_);
    }

  private:
    std::vector<uint64_t> base_;
    BigInteger product_{1};
    std::vector<BigInteger> weights_;
};

// One single-prime digit, then runs of kDigitLen; the last run is listed in
// reverse, so the output layout is seen to follow src_base, not digit order.
std::vector<std::vector<uint64_t>> key_switching_digits(const std::vector<uint64_t> &chain) {
    REQUIRE((chain.size() - 1) % kDigitLen == 0);
    std::vector<std::vector<uint64_t>> digits = {{chain.front()}};
    for (std::size_t i = 1; i < chain.size(); i += kDigitLen) {
        const auto first = chain.begin() + static_cast<std::ptrdiff_t>(i);
        digits.emplace_back(first, first + static_cast<std::ptrdiff_t>(kDigitLen));
    }
    std::ranges::reverse(digits.back());
    return digits;
}

// Residues over `base`, by prime then slot. On every digit, with P its
// product, slots 0-4 hold the integers 0, 1, (P-1)/2, (P+1)/2 and P-1; slots
// 5 and 6 hold (q-1)/2 and (q+1)/2 at every prime q; the rest are uniform.
std::vector<std::vector<uint64_t>>
digit_boundary_inputs(const std::vector<uint64_t> &base,
                      const std::vector<std::vector<uint64_t>> &digits, std::mt19937_64 &rng) {
    constexpr std::size_t kHalfBelowSlot = 5;
    constexpr std::size_t kHalfAboveSlot = 6;
    std::vector<std::vector<uint64_t>> residues(base.size(), std::vector<uint64_t>(kRingDim));
    for (std::size_t i = 0; i < base.size(); ++i) {
        std::uniform_int_distribution<uint64_t> uniform(0, base[i] - 1);
        for (auto &r : residues[i])
            r = uniform(rng);
        residues[i][kHalfBelowSlot] = (base[i] - 1) / 2;
        residues[i][kHalfAboveSlot] = (base[i] + 1) / 2;
    }
    const BigInteger one(1);
    const BigInteger two(2);
    for (const auto &digit : digits) {
        const BigInteger product = Crt(digit).product();
        const std::array<BigInteger, 5> integers = {BigInteger(0), one, (product - one) / two,
                                                    (product + one) / two, product - one};
        for (std::size_t slot = 0; slot < integers.size(); ++slot) {
            for (const uint64_t q : digit)
                residues[index_of(base, q)][slot] = mod_u64(integers[slot], q);
        }
    }
    return residues;
}

// Reference exact extension of one digit: the integer with the inputs'
// residues on `digit`, reduced mod every prime of `target`, by prime then slot.
std::vector<std::vector<uint64_t>> exact_extension(const std::vector<std::vector<uint64_t>> &inputs,
                                                   const std::vector<uint64_t> &base,
                                                   const std::vector<uint64_t> &digit,
                                                   const std::vector<uint64_t> &target) {
    const Crt crt(digit);
    std::vector<std::vector<uint64_t>> out(target.size(), std::vector<uint64_t>(kRingDim));
    std::vector<uint64_t> residues(digit.size());
    for (std::size_t k = 0; k < kRingDim; ++k) {
        for (std::size_t i = 0; i < digit.size(); ++i)
            residues[i] = inputs[index_of(base, digit[i])][k];
        const BigInteger x = crt.integer(residues);
        for (std::size_t j = 0; j < target.size(); ++j)
            out[j][k] = mod_u64(x, target[j]);
    }
    return out;
}

// Sets slots first_slot onward, on every prime of `base`, to the residues of
// `integers`.
void set_integers(std::vector<std::vector<uint64_t>> &residues, const std::vector<uint64_t> &base,
                  std::size_t first_slot, const std::vector<BigInteger> &integers) {
    for (std::size_t s = 0; s < integers.size(); ++s) {
        for (std::size_t i = 0; i < base.size(); ++i)
            residues[i][first_slot + s] = mod_u64(integers[s], base[i]);
    }
}

// Reference exact mod-down: floor(X / P) mod q for each q of `kept`, by prime
// then slot, where X has the inputs' residues on `base` and P = prod rescale.
// It divides only the exact multiple X - (X mod P): OpenFHE's dynamic
// BigInteger division returns 1 whenever both operands share their MSB and top
// limb (bigintdyn/ubintdyn.cpp DividedBy), which is wrong for P-1 over P but
// right for an exact multiple.
std::vector<std::vector<uint64_t>> exact_floor(const std::vector<std::vector<uint64_t>> &inputs,
                                               const std::vector<uint64_t> &base,
                                               const std::vector<uint64_t> &rescale,
                                               const std::vector<uint64_t> &kept) {
    const Crt crt(base);
    const BigInteger product = Crt(rescale).product();
    std::vector<std::vector<uint64_t>> out(kept.size(), std::vector<uint64_t>(kRingDim));
    std::vector<uint64_t> residues(base.size());
    for (std::size_t k = 0; k < kRingDim; ++k) {
        for (std::size_t i = 0; i < base.size(); ++i)
            residues[i] = inputs[i][k];
        const BigInteger x = crt.integer(residues);
        const BigInteger quotient = (x - x.Mod(product)) / product;
        for (std::size_t j = 0; j < kept.size(); ++j)
            out[j][k] = mod_u64(quotient, kept[j]);
    }
    return out;
}

} // namespace

TEST_CASE("raw_lift reproduces the integer at every destination prime", "[integration]") {
    const auto chain = ntt_primes_below(kChainPrimeBits, 3);
    const auto special = ntt_primes_below(kSpecialPrimeBits, 3);
    std::vector<uint64_t> moduli = chain;
    moduli.insert(moduli.end(), special.begin(), special.end());
    // Each source lifts into much smaller, slightly smaller and larger primes.
    struct Lift {
        uint64_t q;
        std::vector<uint64_t> dst_base;
    };
    const std::vector<Lift> lifts = {
        {.q = special[1], .dst_base = {chain[0], chain[1], chain[2], special[0], special[2]}},
        {.q = chain[1], .dst_base = {chain[0], chain[2], special[0], special[1], special[2]}},
    };

    // The lift must not depend on the FBC variant the config selects.
    for (const int reduced_noise : {0, 1}) {
        INFO("reduced_noise " << reduced_noise);
        configure(moduli, reduced_noise);
        std::mt19937_64 rng(kSeed);
        std::vector<std::vector<uint64_t>> inputs;
        std::vector<void *> sources;
        std::vector<std::vector<void *>> outputs;
        for (const auto &lift : lifts) {
            inputs.push_back(boundary_then_uniform(lift.q, rng));
            sources.push_back(
                haze::test::allocate_and_h2d_residues({inputs.back()}, {lift.q}).front());
            outputs.push_back(haze::test::allocate_dst_residues(lift.dst_base.size(), kBytes));
            record_raw_lift(sources.back(), lift.q, lift.dst_base, outputs.back());
        }
        for (const auto &out : outputs) {
            for (void *p : out)
                REQUIRE(hazeTagOutput(p) == HAZE_SUCCESS);
        }
        REQUIRE(hazeFlush() == HAZE_SUCCESS);

        for (std::size_t l = 0; l < lifts.size(); ++l) {
            INFO("source mod " << lifts[l].q);
            for (std::size_t i = 0; i < lifts[l].dst_base.size(); ++i) {
                const uint64_t p = lifts[l].dst_base[i];
                std::vector<uint64_t> want(kRingDim);
                for (std::size_t k = 0; k < kRingDim; ++k)
                    want[k] = inputs[l][k] % p;
                require_residues(outputs[l][i], want, p);
            }
            haze::test::free_all_residues(outputs[l]);
        }
        haze::test::free_all_residues(sources);
    }
}

TEST_CASE("hazeModUpExact rejects null params", "[unit]") {
    configure(ntt_primes_below(kChainPrimeBits, 2), 1);
    REQUIRE(hazeModUpExact(nullptr, nullptr, nullptr, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("hazeModUpExact applies hazeModUp's validation", "[unit]") {
    const auto primes = ntt_primes_below(kChainPrimeBits, 3);
    configure(primes, 1);
    const uint64_t src_base[] = {primes[0], primes[1]};
    const std::size_t digit_base_lens[] = {1};
    const void *src[] = {nullptr, nullptr};
    std::array<void *, 3> dst{};

    SECTION("a digit prime outside src_base") {
        const uint64_t digit_bases[] = {primes[2]};
        const uint64_t p_base[] = {primes[2]};
        const hazeModUpParams p = {.src_base = src_base,
                                   .src_base_len = 2,
                                   .digit_bases = digit_bases,
                                   .digit_bases_total_len = 1,
                                   .digit_base_lens = digit_base_lens,
                                   .digit_count = 1,
                                   .p_base = p_base,
                                   .p_base_len = 1};
        REQUIRE(hazeModUp(dst.data(), src, &p, nullptr) == HAZE_ERROR_INVALID_VALUE);
        REQUIRE(hazeModUpExact(dst.data(), src, &p, nullptr) == HAZE_ERROR_INVALID_VALUE);
    }
    SECTION("a p_base prime shared with src_base") {
        const uint64_t digit_bases[] = {primes[0]};
        const uint64_t p_base[] = {primes[1]};
        const hazeModUpParams p = {.src_base = src_base,
                                   .src_base_len = 2,
                                   .digit_bases = digit_bases,
                                   .digit_bases_total_len = 1,
                                   .digit_base_lens = digit_base_lens,
                                   .digit_count = 1,
                                   .p_base = p_base,
                                   .p_base_len = 1};
        REQUIRE(hazeModUp(dst.data(), src, &p, nullptr) == HAZE_ERROR_INVALID_VALUE);
        REQUIRE(hazeModUpExact(dst.data(), src, &p, nullptr) == HAZE_ERROR_INVALID_VALUE);
    }
    hazeGetLastError();
}

TEST_CASE("hazeModUpExact matches the exact CRT extension on a 41-prime shape", "[integration]") {
    const auto chain = ntt_primes_below(kChainPrimeBits, kChainCount);
    const auto special = ntt_primes_below(kSpecialPrimeBits, kSpecialCount);
    const auto digits = key_switching_digits(chain);
    // src_base ∪ p_base: every digit's output base, in output order.
    std::vector<uint64_t> target = chain;
    target.insert(target.end(), special.begin(), special.end());
    std::vector<uint64_t> digit_bases;
    std::vector<std::size_t> digit_base_lens;
    for (const auto &digit : digits) {
        digit_bases.insert(digit_bases.end(), digit.begin(), digit.end());
        digit_base_lens.push_back(digit.size());
    }
    const hazeModUpParams params = {.src_base = chain.data(),
                                    .src_base_len = chain.size(),
                                    .digit_bases = digit_bases.data(),
                                    .digit_bases_total_len = digit_bases.size(),
                                    .digit_base_lens = digit_base_lens.data(),
                                    .digit_count = digits.size(),
                                    .p_base = special.data(),
                                    .p_base_len = special.size()};

    // The extension must not depend on the FBC variant the config selects.
    for (const int reduced_noise : {0, 1}) {
        INFO("reduced_noise " << reduced_noise);
        configure(target, reduced_noise);
        std::mt19937_64 rng(kSeed);
        const auto inputs = digit_boundary_inputs(chain, digits, rng);
        const auto src = haze::test::allocate_and_h2d_residues(inputs, chain);
        const auto dst = haze::test::allocate_dst_residues(digits.size() * target.size(), kBytes);
        REQUIRE(hazeModUpExact(dst.data(), haze::test::to_const(src).data(), &params, nullptr) ==
                HAZE_SUCCESS);
        for (void *p : dst)
            REQUIRE(hazeTagOutput(p) == HAZE_SUCCESS);
        REQUIRE(hazeFlush() == HAZE_SUCCESS);

        for (std::size_t d = 0; d < digits.size(); ++d) {
            INFO("digit " << d);
            const auto want = exact_extension(inputs, chain, digits[d], target);
            for (std::size_t j = 0; j < target.size(); ++j)
                require_residues(dst[(d * target.size()) + j], want[j], target[j]);
        }
        haze::test::free_all_residues(src);
        haze::test::free_all_residues(dst);
    }
}

TEST_CASE("hazeModDownExact rejects null params", "[unit]") {
    configure(ntt_primes_below(kChainPrimeBits, 2), 1);
    REQUIRE(hazeModDownExact(nullptr, nullptr, nullptr, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("hazeModDownExact applies hazeModDown's validation", "[unit]") {
    const auto primes = ntt_primes_below(kChainPrimeBits, 3);
    configure(primes, 1);
    const uint64_t src_base[] = {primes[0], primes[1]};
    const void *src[] = {nullptr, nullptr};
    std::array<void *, 2> dst{};

    SECTION("a rescale prime outside src_base") {
        const uint64_t rescale_base[] = {primes[2]};
        const hazeModDownParams p = {.src_base = src_base,
                                     .src_base_len = 2,
                                     .rescale_base = rescale_base,
                                     .rescale_base_len = 1};
        REQUIRE(hazeModDown(dst.data(), src, &p, nullptr) == HAZE_ERROR_INVALID_VALUE);
        REQUIRE(hazeModDownExact(dst.data(), src, &p, nullptr) == HAZE_ERROR_INVALID_VALUE);
    }
    SECTION("a rescale_base that is not a proper subset") {
        const hazeModDownParams p = {.src_base = src_base,
                                     .src_base_len = 2,
                                     .rescale_base = src_base,
                                     .rescale_base_len = 2};
        REQUIRE(hazeModDown(dst.data(), src, &p, nullptr) == HAZE_ERROR_INVALID_VALUE);
        REQUIRE(hazeModDownExact(dst.data(), src, &p, nullptr) == HAZE_ERROR_INVALID_VALUE);
    }
    hazeGetLastError();
}

TEST_CASE("hazeModDownExact matches floor(X / P) on a 41-prime shape", "[integration]") {
    const auto chain = ntt_primes_below(kChainPrimeBits, kChainCount);
    const auto special = ntt_primes_below(kSpecialPrimeBits, kSpecialCount);
    std::vector<uint64_t> src_base = chain;
    src_base.insert(src_base.end(), special.begin(), special.end());
    // The key-switching mod-down drops the special primes. The second drops
    // primes from the middle, listed out of src_base order, so the output is
    // seen to keep src_base order.
    const std::vector<std::vector<uint64_t>> rescale_bases = {special,
                                                              {special[2], chain[5], chain[0]}};
    // Slots 0-6 are digit_boundary_inputs' boundaries over the whole base; the
    // next five straddle the floor's first and last steps.
    constexpr std::size_t kFloorStepSlot = 7;
    const BigInteger one(1);
    const BigInteger src_product = Crt(src_base).product();

    for (const auto &rescale : rescale_bases) {
        std::vector<uint64_t> kept;
        for (const uint64_t q : src_base) {
            if (std::ranges::find(rescale, q) == rescale.end())
                kept.push_back(q);
        }
        const BigInteger p = Crt(rescale).product();
        const hazeModDownParams params = {.src_base = src_base.data(),
                                          .src_base_len = src_base.size(),
                                          .rescale_base = rescale.data(),
                                          .rescale_base_len = rescale.size()};

        // The floor division must not depend on the FBC variant the config selects.
        for (const int reduced_noise : {0, 1}) {
            INFO("rescale_base_len " << rescale.size() << ", reduced_noise " << reduced_noise);
            configure(src_base, reduced_noise);
            std::mt19937_64 rng(kSeed);
            auto inputs = digit_boundary_inputs(src_base, {src_base}, rng);
            set_integers(inputs, src_base, kFloorStepSlot,
                         {p - one, p, p + one, src_product - p - one, src_product - p});
            const auto src = haze::test::allocate_and_h2d_residues(inputs, src_base);
            const auto dst = haze::test::allocate_dst_residues(kept.size(), kBytes);
            REQUIRE(hazeModDownExact(dst.data(), haze::test::to_const(src).data(), &params,
                                     nullptr) == HAZE_SUCCESS);
            for (void *d : dst)
                REQUIRE(hazeTagOutput(d) == HAZE_SUCCESS);
            REQUIRE(hazeFlush() == HAZE_SUCCESS);

            const auto want = exact_floor(inputs, src_base, rescale, kept);
            for (std::size_t j = 0; j < kept.size(); ++j)
                require_residues(dst[j], want[j], kept[j]);
            haze::test::free_all_residues(src);
            haze::test::free_all_residues(dst);
        }
    }
}
