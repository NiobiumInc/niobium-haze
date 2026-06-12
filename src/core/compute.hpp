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
#pragma once

#include "common/errors.hpp"
#include "common/handle.hpp"
#include "core/config.hpp"
#include "core/epoch.hpp"
#include "core/mrp_polymap.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <niobium/fhetch_api.h>

namespace haze {

// Each compute prelude opens an EpochSession, resolves the modulus, copies
// sources from the polymap, dispatches the FHETCH op, and stores the result.
// Sources are returned by value so in-place ops (dst == src1 [== src2]) stay
// correct.

// Polynomial-polynomial-modulus. Used by hazeAdd, hazeSub, hazeMul.
template <auto OpFn>
std::expected<void, HazeInternalError> binary_pp_op(DevAddr dst, DevAddr src1, DevAddr src2,
                                                    int mod_idx) noexcept {
    EpochSession session;
    const uint64_t q = config().modulus(mod_idx);
    if (q == 0)
        return std::unexpected(HazeInternalError::InvalidArgument);
    auto p1 = epoch().lookup_or_create_locked(src1);
    if (!p1)
        return std::unexpected(p1.error());
    auto p2 = epoch().lookup_or_create_locked(src2);
    if (!p2)
        return std::unexpected(p2.error());
    epoch().store_compute_result_locked(dst, OpFn(*p1, *p2, q), q);
    return {};
}

// Polynomial-scalar-modulus. Used by hazeAddScalar, hazeSubScalar, hazeMulScalar.
template <auto OpFn>
std::expected<void, HazeInternalError> binary_ps_op(DevAddr dst, DevAddr src, uint64_t scalar,
                                                    int mod_idx) noexcept {
    EpochSession session;
    const uint64_t q = config().modulus(mod_idx);
    if (q == 0)
        return std::unexpected(HazeInternalError::InvalidArgument);
    auto p = epoch().lookup_or_create_locked(src);
    if (!p)
        return std::unexpected(p.error());
    epoch().store_compute_result_locked(dst, OpFn(*p, niobium::fhetch::Scalar::from_int(scalar), q),
                                        q);
    return {};
}

// Polynomial-modulus. Used by hazeNTT, hazeINTT.
template <auto OpFn>
std::expected<void, HazeInternalError> unary_pq_op(DevAddr dst, DevAddr src, int mod_idx) noexcept {
    EpochSession session;
    const uint64_t q = config().modulus(mod_idx);
    if (q == 0)
        return std::unexpected(HazeInternalError::InvalidArgument);
    auto p = epoch().lookup_or_create_locked(src);
    if (!p)
        return std::unexpected(p.error());
    epoch().store_compute_result_locked(dst, OpFn(*p, q), q);
    return {};
}

// Polynomial-index. Used by hazeAutomorph. The eval-form automorph is a pure
// slot permutation (value-independent of the modulus), so the op carries the
// COPY sentinel; recover the source's recorded modulus and bind it as
// metadata on source + result so a tagged SRP automorph is probe-serializable
// on transport (matching the MRP automorph, which binds base[i]).
template <auto OpFn>
std::expected<void, HazeInternalError> unary_pi_op(DevAddr dst, DevAddr src,
                                                   uint64_t index) noexcept {
    EpochSession session;
    auto p = epoch().lookup_or_create_locked(src);
    if (!p)
        return std::unexpected(p.error());
    const uint64_t q = epoch().recorded_modulus_locked(src);
    auto result = OpFn(*p, index);
    if (q != kCopyModulus) {
        niobium::fhetch::bind_modulus(*p, q);
        niobium::fhetch::bind_modulus(result, q);
    }
    epoch().store_compute_result_locked(dst, std::move(result), q);
    return {};
}

// MRP compute templates: same shape as the SRP templates, fanned out per
// residue via build_mrp_locked / store_mrp_locked. In-place safety carries
// over per-residue.

// MRP polynomial-polynomial. Used by hazeAddMrp, hazeSubMrp, hazeMulMrp.
template <auto OpFn>
std::expected<void, HazeInternalError>
binary_pp_op_mrp(void *const *dst, const void *const *src1, const void *const *src2,
                 const uint64_t *base, std::size_t base_len) noexcept {
    EpochSession session;
    auto m1 = build_mrp_locked(src1, base, base_len);
    if (!m1)
        return std::unexpected(m1.error());
    auto m2 = build_mrp_locked(src2, base, base_len);
    if (!m2)
        return std::unexpected(m2.error());
    niobium::fhetch::MRP result = OpFn(*m1, *m2);
    auto stored = store_mrp_locked(dst, result, base, base_len);
    if (!stored)
        return std::unexpected(stored.error());
    return {};
}

// MRP polynomial-scalar (scalars[i] pairs with base[i]); used by
// hazeAddScalarMrp, hazeSubScalarMrp, hazeMulScalarMrp.
template <auto OpFn>
std::expected<void, HazeInternalError>
binary_ps_op_mrp(void *const *dst, const void *const *src, const uint64_t *scalars,
                 const uint64_t *base, std::size_t base_len) noexcept {
    EpochSession session;
    auto m = build_mrp_locked(src, base, base_len);
    if (!m)
        return std::unexpected(m.error());
    niobium::fhetch::MRP result = OpFn(*m, build_mrs(scalars, base, base_len));
    auto stored = store_mrp_locked(dst, result, base, base_len);
    if (!stored)
        return std::unexpected(stored.error());
    return {};
}

// MRP polynomial-only: mr_ntt/mr_intt carry their moduli base inside the
// MRP, so this can't reuse unary_pq_op. Used by hazeNTTMrp, hazeINTTMrp.
template <auto OpFn>
std::expected<void, HazeInternalError> unary_p_op_mrp(void *const *dst, const void *const *src,
                                                      const uint64_t *base,
                                                      std::size_t base_len) noexcept {
    EpochSession session;
    auto m = build_mrp_locked(src, base, base_len);
    if (!m)
        return std::unexpected(m.error());
    niobium::fhetch::MRP result = OpFn(*m);
    auto stored = store_mrp_locked(dst, result, base, base_len);
    if (!stored)
        return std::unexpected(stored.error());
    return {};
}

// MRP polynomial-with-index. Used by hazeAutomorphMrp (mr_automorph_eval
// takes an odd integer k in [1, 2N-1] and is otherwise modulus-independent).
template <auto OpFn>
std::expected<void, HazeInternalError> unary_pi_op_mrp(void *const *dst, const void *const *src,
                                                       uint64_t index, const uint64_t *base,
                                                       std::size_t base_len) noexcept {
    EpochSession session;
    auto m = build_mrp_locked(src, base, base_len);
    if (!m)
        return std::unexpected(m.error());
    niobium::fhetch::MRP result = OpFn(*m, index);
    auto stored = store_mrp_locked(dst, result, base, base_len);
    if (!stored)
        return std::unexpected(stored.error());
    return {};
}

} // namespace haze
