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

#include "core/config.hpp"
#include "core/epoch.hpp"
#include "common/errors.hpp"
#include "common/handle.hpp"

#include <haze/haze_types.h>

#include <niobium/fhetch_api.h>

#include <cstdint>
#include <expected>

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
    epoch().store_compute_result_locked(dst, OpFn(*p, niobium::fhetch::Scalar::from_int(scalar), q));
    return HAZE_SUCCESS;
}

// Polynomial-modulus. Used by hazeNTT, hazeINTT.
template <auto OpFn>
hazeError_t unary_pq_op(DevAddr dst, DevAddr src, int mod_idx) noexcept {
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
template <auto OpFn>
hazeError_t unary_pi_op(DevAddr dst, DevAddr src, uint64_t index) noexcept {
    EpochSession session;
    auto p = epoch().lookup_or_create_locked(src);
    if (!p)
        return set_error(to_public_error(p.error()));
    epoch().store_compute_result_locked(dst, OpFn(*p, index));
    return HAZE_SUCCESS;
}

} // namespace haze
