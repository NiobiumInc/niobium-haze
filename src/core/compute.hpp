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
#include <haze/haze_types.h>
#include <niobium/fhetch_api.h>

namespace haze {

// Each compute entry point shares the same prelude: open an EpochSession
// (lock + ensure_recording), resolve the modulus, copy each source poly
// out of the polymap, dispatch to the FHETCH operation, store the
// result. The four templates below capture this shape parametrised by
// the FHETCH function. Source polys are returned by value so in-place
// operations (dst == src1 [== src2]) stay correct.

// Polynomial-polynomial-modulus. Used by hazeAdd, hazeSub, hazeMul.
template <auto OpFn>
hazeError_t binary_pp_op(DevAddr dst, DevAddr src1, DevAddr src2, int mod_idx) noexcept {
    EpochSession session;
    const uint64_t q = config().modulus(mod_idx);
    if (q == 0)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    auto p1 = epoch().lookup_or_create_locked(src1);
    if (!p1)
        return set_error(to_public_error(p1.error()));
    auto p2 = epoch().lookup_or_create_locked(src2);
    if (!p2)
        return set_error(to_public_error(p2.error()));
    epoch().store_compute_result_locked(dst, OpFn(*p1, *p2, q));
    return HAZE_SUCCESS;
}

// Polynomial-scalar-modulus. Used by hazeAddScalar, hazeSubScalar, hazeMulScalar.
template <auto OpFn>
hazeError_t binary_ps_op(DevAddr dst, DevAddr src, uint64_t scalar, int mod_idx) noexcept {
    EpochSession session;
    const uint64_t q = config().modulus(mod_idx);
    if (q == 0)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    auto p = epoch().lookup_or_create_locked(src);
    if (!p)
        return set_error(to_public_error(p.error()));
    epoch().store_compute_result_locked(dst,
                                        OpFn(*p, niobium::fhetch::Scalar::from_int(scalar), q));
    return HAZE_SUCCESS;
}

// Polynomial-modulus. Used by hazeNTT, hazeINTT.
template <auto OpFn> hazeError_t unary_pq_op(DevAddr dst, DevAddr src, int mod_idx) noexcept {
    EpochSession session;
    const uint64_t q = config().modulus(mod_idx);
    if (q == 0)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    auto p = epoch().lookup_or_create_locked(src);
    if (!p)
        return set_error(to_public_error(p.error()));
    epoch().store_compute_result_locked(dst, OpFn(*p, q));
    return HAZE_SUCCESS;
}

// Polynomial-index. Used by hazeAutomorph.
template <auto OpFn> hazeError_t unary_pi_op(DevAddr dst, DevAddr src, uint64_t index) noexcept {
    EpochSession session;
    auto p = epoch().lookup_or_create_locked(src);
    if (!p)
        return set_error(to_public_error(p.error()));
    epoch().store_compute_result_locked(dst, OpFn(*p, index));
    return HAZE_SUCCESS;
}

// MRP compute templates. Same shape as the SRP templates above but
// fan out across one DevAddr per residue via build_mrp_locked /
// store_mrp_locked from core/mrp_polymap.hpp. In-place safety carries
// over per-residue (the helpers' copy semantics are documented there).

// MRP polynomial-polynomial. Used by hazeAddMrp, hazeSubMrp, hazeMulMrp.
template <auto OpFn>
hazeError_t binary_pp_op_mrp(void *const *dst, const void *const *src1, const void *const *src2,
                             const uint64_t *base, std::size_t base_len) noexcept {
    EpochSession session;
    auto m1 = build_mrp_locked(src1, base, base_len);
    if (!m1)
        return set_error(to_public_error(m1.error()));
    auto m2 = build_mrp_locked(src2, base, base_len);
    if (!m2)
        return set_error(to_public_error(m2.error()));
    niobium::fhetch::MRP result = OpFn(*m1, *m2);
    store_mrp_locked(dst, result, base, base_len);
    return HAZE_SUCCESS;
}

// MRP polynomial-scalar. `scalars[i]` pairs with `base[i]`.
// Used by hazeAddScalarMrp, hazeSubScalarMrp, hazeMulScalarMrp.
template <auto OpFn>
hazeError_t binary_ps_op_mrp(void *const *dst, const void *const *src, const uint64_t *scalars,
                             const uint64_t *base, std::size_t base_len) noexcept {
    EpochSession session;
    auto m = build_mrp_locked(src, base, base_len);
    if (!m)
        return set_error(to_public_error(m.error()));
    niobium::fhetch::MRP result = OpFn(*m, build_mrs(scalars, base, base_len));
    store_mrp_locked(dst, result, base, base_len);
    return HAZE_SUCCESS;
}

// MRP polynomial-only (no per-modulus argument). The fhetch `mr_ntt` /
// `mr_intt` ops carry their moduli base inside the MRP itself and take
// no extra modulus parameter — so this shape cannot reuse `unary_pq_op`.
// Used by hazeNTTMrp, hazeINTTMrp.
template <auto OpFn>
hazeError_t unary_p_op_mrp(void *const *dst, const void *const *src, const uint64_t *base,
                           std::size_t base_len) noexcept {
    EpochSession session;
    auto m = build_mrp_locked(src, base, base_len);
    if (!m)
        return set_error(to_public_error(m.error()));
    niobium::fhetch::MRP result = OpFn(*m);
    store_mrp_locked(dst, result, base, base_len);
    return HAZE_SUCCESS;
}

// MRP polynomial-with-index. Used by hazeAutomorphMrp (mr_automorph_eval
// takes an odd integer k in [1, 2N-1] and is otherwise modulus-independent).
template <auto OpFn>
hazeError_t unary_pi_op_mrp(void *const *dst, const void *const *src, uint64_t index,
                            const uint64_t *base, std::size_t base_len) noexcept {
    EpochSession session;
    auto m = build_mrp_locked(src, base, base_len);
    if (!m)
        return set_error(to_public_error(m.error()));
    niobium::fhetch::MRP result = OpFn(*m, index);
    store_mrp_locked(dst, result, base, base_len);
    return HAZE_SUCCESS;
}

} // namespace haze
