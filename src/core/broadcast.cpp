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
// hazeBroadcast*Mrp lowering: lift one single-residue operand into every base
// prime via the centered switch, then apply the pointwise op per limb.

#include "core/broadcast.hpp"

#include "common/errors.hpp"
#include "common/handle.hpp"
#include "core/centered_switch.hpp"
#include "core/epoch.hpp"
#include "core/mrp_polymap.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <niobium/fhetch_api.h>
#include <utility>
#include <vector>

namespace haze {

namespace fhetch = niobium::fhetch;

namespace {

// dst[i] = src[i] op lift(operand) per limb (kReversed swaps the operands).
// The lift is skipped when the limb already lives in the operand's ring, or when
// the caller sets operand_in_range, which makes the recording ordinary-form-target
// only (see the hazeBroadcastAddMrp contract in haze.h).
template <auto OpFn, bool kReversed = false>
std::expected<void, HazeInternalError>
broadcast_op(void *const *dst, const void *const *src, const void *operand, bool operand_in_range,
             const uint64_t *base, std::size_t base_len) noexcept {
    if (auto v = validate_moduli_base(base, base_len); !v)
        return v;
    if (auto live = require_allocated_array(dst, base_len); !live)
        return live;
    EpochSession session;
    if (auto rec = epoch().require_recording_locked(); !rec)
        return rec;
    // The operand's ring is the modulus haze recorded for the op that produced
    // it (e.g. the hazeIsHalfModulus aux prime); a raw H2D operand has none —
    // record one first (any modulus-carrying op).
    const uint64_t operand_modulus = epoch().recorded_modulus_locked(to_dev_addr(operand));
    if (operand_modulus == kCopyModulus) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "hazeBroadcastMrp: operand has no recorded modulus");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    if (operand_modulus % 2 == 0) {
        // h = (p-1)/2 centering needs an odd operand ring.
        record_internal_error(HazeInternalError::InvalidArgument,
                              "hazeBroadcastMrp: operand modulus must be odd");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    auto x = build_mrp_locked(src, base, base_len);
    if (!x)
        return std::unexpected(x.error());
    auto m = epoch().lookup_or_create_locked(to_dev_addr(operand));
    if (!m)
        return std::unexpected(m.error());

    const bool direct = operand_in_range;
    std::vector<std::pair<fhetch::Polynomial, uint64_t>> pairs;
    pairs.reserve(base_len);
    for (std::size_t i = 0; i < base_len; ++i) {
        const uint64_t q = base[i];
        const fhetch::Polynomial lifted =
            (direct || q == operand_modulus) ? *m : emit_centered_switch(*m, operand_modulus, q);
        const fhetch::Polynomial &limb = (*x)[q];
        pairs.emplace_back(kReversed ? OpFn(lifted, limb, q) : OpFn(limb, lifted, q), q);
    }
    const fhetch::MRP result = fhetch::MRP::from_pairs(pairs);
    return store_mrp_locked(dst, result, base, base_len);
}

} // namespace

std::expected<void, HazeInternalError> broadcast_add(void *const *dst, const void *const *src,
                                                     const void *operand, bool operand_in_range,
                                                     const uint64_t *base,
                                                     std::size_t base_len) noexcept {
    return broadcast_op<fhetch::sr_addp>(dst, src, operand, operand_in_range, base, base_len);
}

std::expected<void, HazeInternalError> broadcast_sub(void *const *dst, const void *const *src,
                                                     const void *operand, bool operand_in_range,
                                                     const uint64_t *base,
                                                     std::size_t base_len) noexcept {
    return broadcast_op<fhetch::sr_subp>(dst, src, operand, operand_in_range, base, base_len);
}

std::expected<void, HazeInternalError> broadcast_rsub(void *const *dst, const void *const *src,
                                                      const void *operand, bool operand_in_range,
                                                      const uint64_t *base,
                                                      std::size_t base_len) noexcept {
    return broadcast_op<fhetch::sr_subp, /*kReversed=*/true>(dst, src, operand, operand_in_range,
                                                             base, base_len);
}

std::expected<void, HazeInternalError> broadcast_mul(void *const *dst, const void *const *src,
                                                     const void *operand, bool operand_in_range,
                                                     const uint64_t *base,
                                                     std::size_t base_len) noexcept {
    return broadcast_op<fhetch::sr_mulp>(dst, src, operand, operand_in_range, base, base_len);
}

} // namespace haze
