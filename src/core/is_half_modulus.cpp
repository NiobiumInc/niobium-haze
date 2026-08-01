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
// hazeIsHalfModulus lowering: the FHETCH ISA has no comparison instruction, so
// the predicate is extracted arithmetically via two centered modulus switches
// through an auxiliary prime.

#include "core/is_half_modulus.hpp"

#include "common/errors.hpp"
#include "common/handle.hpp"
#include "common/mod_arith.hpp"
#include "core/allocator.hpp"
#include "core/centered_switch.hpp"
#include "core/config.hpp"
#include "core/epoch.hpp"
#include "core/mrp_polymap.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <niobium/fhetch_api.h>
#include <numeric>
#include <utility>
#include <vector>

namespace haze {

namespace fhetch = niobium::fhetch;

namespace {

// b = [x > h], h = (q-1)/2: g1 = c(x-h) == x-h (signed), g2 = c(x) == x - q*b,
// so ((g1 - g2 + h) mod p) * q^-1 == b exactly, for any prime p != q.
// Immediates are pre-reduced (replay backends assume scalars below the modulus).
fhetch::Polynomial lower_is_half_modulus(const fhetch::Polynomial &x, uint64_t q, uint64_t p) {
    const uint64_t h = (q - 1) / 2;
    const auto w = fhetch::sr_subps(x, fhetch::Scalar::from_int(h), q);
    const auto g1 = emit_centered_switch(w, q, p);
    const auto g2 = emit_centered_switch(x, q, p);
    fhetch::Polynomial qb = [&] {
        if (h % p != 0) {
            const auto diff = fhetch::sr_subp(g1, g2, p);
            return fhetch::sr_addps(diff, fhetch::Scalar::from_int(h % p), p);
        }
        // p | h zeroes every h-derived immediate; the replay driver's identity
        // folds then expose sub(muli,muli) to scalar_factor_sub, which hoists
        // the sub above the rebase onto unreduced ring-q operands. The +1/-1
        // pair keeps a non-elidable addps in between.
        // TODO(niobium-compiler): drop once scalar_factor_sub gains a
        // cross-ring guard; deployed drivers predate that fix.
        const auto barrier = fhetch::sr_addps(g1, fhetch::Scalar::from_int(1), p);
        const auto diff = fhetch::sr_subp(barrier, g2, p);
        return fhetch::sr_addps(diff, fhetch::Scalar::from_int(p - 1), p);
    }();
    return fhetch::sr_mulps(qb, fhetch::Scalar::from_int(modinv_prime(q, p)), p);
}

} // namespace

std::expected<void, HazeInternalError> is_half_modulus(DevAddr dst, DevAddr src,
                                                       int mod_idx) noexcept {
    if (auto live = allocator().require_allocated(dst); !live)
        return live;
    EpochSession session;
    if (auto rec = epoch().require_recording_locked(); !rec)
        return rec;
    const uint64_t q = fhe_params().modulus(mod_idx);
    if (q == 0) {
        record_internal_error(HazeInternalError::InvalidArgument, "hazeIsHalfModulus: bad mod_idx");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    if (q % 2 == 0) {
        // h = (q-1)/2 centering needs an odd modulus.
        record_internal_error(HazeInternalError::InvalidArgument,
                              "hazeIsHalfModulus: modulus must be odd");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    // The program-wide aux prime generated at configure time (last chain
    // entry); distinct primes, so q and p are coprime by construction. The
    // aux slot itself is not a data modulus.
    const uint64_t p = fhe_params().aux_modulus();
    if (q == p) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "hazeIsHalfModulus: mod_idx names the aux modulus");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    auto x = epoch().lookup_or_create_locked(src);
    if (!x)
        return std::unexpected(x.error());

    epoch().store_compute_result_locked(dst, lower_is_half_modulus(*x, q, p), p);
    return {};
}

std::expected<void, HazeInternalError> is_half_modulus_mrp(void *const *dst, const void *const *src,
                                                           const uint64_t *base,
                                                           std::size_t base_len) noexcept {
    if (auto v = validate_moduli_base(base, base_len); !v)
        return v;
    // The program-wide aux prime generated at configure time (last chain
    // entry); one aux serves every limb.
    const uint64_t aux_modulus = fhe_params().aux_modulus();
    if (aux_modulus == 0) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "hazeIsHalfModulusMrp: no moduli configured");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    for (std::size_t i = 0; i < base_len; ++i) {
        if (base[i] == aux_modulus) {
            record_internal_error(HazeInternalError::InvalidArgument,
                                  "hazeIsHalfModulusMrp: base contains the aux modulus");
            return std::unexpected(HazeInternalError::InvalidArgument);
        }
        if (base[i] % 2 == 0) {
            // h = (q_i-1)/2 centering needs odd limb moduli.
            record_internal_error(HazeInternalError::InvalidArgument,
                                  "hazeIsHalfModulusMrp: base modulus must be odd");
            return std::unexpected(HazeInternalError::InvalidArgument);
        }
        // q_i^-1 mod aux must exist; aux is prime, so this only rejects aux | q_i.
        if (std::gcd(base[i], aux_modulus) != 1) {
            record_internal_error(HazeInternalError::InvalidArgument,
                                  "hazeIsHalfModulusMrp: aux modulus not coprime to base prime");
            return std::unexpected(HazeInternalError::InvalidArgument);
        }
    }
    if (auto live = require_allocated_array(dst, base_len); !live)
        return live;
    EpochSession session;
    if (auto rec = epoch().require_recording_locked(); !rec)
        return rec;
    // Resolve every source before any store so a bad residue fails without
    // half-mutating the dst array (the sibling ops' build/store split).
    std::vector<fhetch::Polynomial> sources;
    sources.reserve(base_len);
    for (std::size_t i = 0; i < base_len; ++i) {
        auto x = epoch().lookup_or_create_locked(to_dev_addr(src[i]));
        if (!x)
            return std::unexpected(x.error());
        sources.push_back(std::move(*x));
    }
    // Outputs stay independent single-residue polynomials: a same-prime MRP
    // grouping would alias residue keys.
    for (std::size_t i = 0; i < base_len; ++i) {
        epoch().store_compute_result_locked(to_dev_addr(dst[i]),
                                            lower_is_half_modulus(sources[i], base[i], aux_modulus),
                                            aux_modulus);
    }
    return {};
}

} // namespace haze
