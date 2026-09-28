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

#include "core/exact_basis.hpp"

#include "common/mod_arith.hpp"
#include "core/centered_switch.hpp"
#include "core/epoch.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <niobium/fhetch_api.h>
#include <span>
#include <unordered_set>
#include <utility>
#include <vector>

namespace haze {

namespace fhetch = niobium::fhetch;

namespace {

// A prime whose mixed-radix sum is still open: the sum of the digits so far
// mod prime, and the next digit's radix M_t mod prime.
struct OpenSum {
    uint64_t prime;
    fhetch::Polynomial sum;
    uint64_t radix;
};

} // namespace

std::vector<fhetch::Polynomial> raw_lift(const fhetch::Polynomial &v, uint64_t q,
                                         std::span<const uint64_t> dst_base) noexcept {
    const uint64_t half_q = (q - 1) / 2;
    // One shift feeds every destination's chain; it is the chain's input, not
    // one of the intermediates the driver's SwitchModulus match needs single-use.
    const auto shifted = fhetch::sr_subps(v, fhetch::Scalar::from_int(half_q), q);
    std::vector<fhetch::Polynomial> lifted;
    lifted.reserve(dst_base.size());
    for (const uint64_t p : dst_base) {
        const auto centered = emit_centered_switch(shifted, q, p);
        lifted.push_back(fhetch::sr_addps(centered, fhetch::Scalar::from_int(half_q % p), p));
    }
    return lifted;
}

fhetch::MRP exact_base_convert(const fhetch::MRP &x,
                               const fhetch::ModuliBase &target_base) noexcept {
    const fhetch::ModuliBase &digits = x.base();
    const std::unordered_set<uint64_t> shared(digits.begin(), digits.end());
    // Every target prime outside x stays open to the end; each later digit prime
    // stays open until its own digit is solved from its sum.
    std::vector<uint64_t> open_primes;
    for (const uint64_t r : target_base) {
        if (!shared.contains(r))
            open_primes.push_back(r);
    }
    const size_t target_only = open_primes.size();
    open_primes.insert(open_primes.end(), digits.begin() + 1, digits.end());

    const uint64_t b0 = digits.front();
    const auto first = raw_lift(x[b0], b0, open_primes);
    std::vector<OpenSum> open;
    open.reserve(open_primes.size());
    for (size_t i = 0; i < open_primes.size(); ++i)
        open.push_back({.prime = open_primes[i], .sum = first[i], .radix = b0 % open_primes[i]});

    for (size_t t = 1; t < digits.size(); ++t) {
        const uint64_t b = digits[t];
        // Digit primes follow the target-only ones in digit order and leave as
        // they are solved, so b's sum is always the first after target_only.
        const OpenSum own = open[target_only];
        open.erase(open.begin() + static_cast<std::ptrdiff_t>(target_only));
        // y_t = (x mod b - sum) * M_t^-1 mod b.
        const auto diff = fhetch::sr_subp(x[b], own.sum, b);
        const auto digit =
            fhetch::sr_mulps(diff, fhetch::Scalar::from_int(modinv_prime(own.radix, b)), b);
        std::vector<uint64_t> primes;
        primes.reserve(open.size());
        for (const auto &o : open)
            primes.push_back(o.prime);
        const auto lifted = raw_lift(digit, b, primes);
        for (size_t i = 0; i < open.size(); ++i) {
            OpenSum &o = open[i];
            const auto term =
                fhetch::sr_mulps(lifted[i], fhetch::Scalar::from_int(o.radix), o.prime);
            o.sum = fhetch::sr_addp(o.sum, term, o.prime);
            o.radix = mulmod_u64(o.radix, b % o.prime, o.prime);
        }
    }

    // Only the target-only sums remain, in target order.
    std::vector<std::pair<fhetch::Polynomial, uint64_t>> pairs;
    pairs.reserve(target_base.size());
    size_t next = 0;
    for (const uint64_t r : target_base) {
        if (shared.contains(r))
            pairs.emplace_back(emit_copy(x[r], r), r);
        else
            pairs.emplace_back(open[next++].sum, r);
    }
    return fhetch::MRP::from_pairs(pairs);
}

fhetch::MRP exact_rescale(const fhetch::MRP &x, const fhetch::ModuliBase &rescale_base) noexcept {
    std::vector<uint64_t> primes = x.base();
    std::vector<fhetch::Polynomial> residues;
    residues.reserve(primes.size());
    for (const uint64_t q : primes)
        residues.push_back(x[q]);

    for (const uint64_t r : rescale_base) {
        const auto at = std::ranges::find(primes, r) - primes.begin();
        const fhetch::Polynomial top = residues[static_cast<size_t>(at)];
        primes.erase(primes.begin() + at);
        residues.erase(residues.begin() + at);
        const auto lifted = raw_lift(top, r, primes);
        for (size_t i = 0; i < primes.size(); ++i) {
            const uint64_t q = primes[i];
            const auto diff = fhetch::sr_subp(residues[i], lifted[i], q);
            residues[i] = fhetch::sr_mulps(diff, fhetch::Scalar::from_int(modinv_prime(r, q)), q);
        }
    }

    std::vector<std::pair<fhetch::Polynomial, uint64_t>> pairs;
    pairs.reserve(primes.size());
    for (size_t i = 0; i < primes.size(); ++i)
        pairs.emplace_back(residues[i], primes[i]);
    return fhetch::MRP::from_pairs(pairs);
}

} // namespace haze
