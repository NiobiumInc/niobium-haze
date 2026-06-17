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
// HAZE compute API: extern "C" shims dispatching to the templates in
// core/compute.hpp. Each shim validates pointer arguments, casts to
// DevAddr, and selects the matching FHETCH operation.

#include "core/compute.hpp"

#include "common/errors.hpp"
#include "common/handle.hpp"
#include "core/context.hpp" // IWYU pragma: keep — ctx-> needs the complete type

#include <cstddef>
#include <cstdint>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <niobium/fhetch_api.h>

namespace fhetch = niobium::fhetch;

extern "C" hazeError_t hazeAdd(hazeContext_t ctx, void *dst, const void *src1, const void *src2,
                               int mod_idx, hazeStream_t /*stream*/) noexcept {
    if (ctx == nullptr || dst == nullptr || src1 == nullptr || src2 == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return set_internal_result(haze::binary_pp_op<fhetch::sr_addp>(
        *ctx, haze::to_dev_addr(dst), haze::to_dev_addr(src1), haze::to_dev_addr(src2), mod_idx));
}

extern "C" hazeError_t hazeSub(hazeContext_t ctx, void *dst, const void *src1, const void *src2,
                               int mod_idx, hazeStream_t /*stream*/) noexcept {
    if (ctx == nullptr || dst == nullptr || src1 == nullptr || src2 == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return set_internal_result(haze::binary_pp_op<fhetch::sr_subp>(
        *ctx, haze::to_dev_addr(dst), haze::to_dev_addr(src1), haze::to_dev_addr(src2), mod_idx));
}

extern "C" hazeError_t hazeMul(hazeContext_t ctx, void *dst, const void *src1, const void *src2,
                               int mod_idx, hazeStream_t /*stream*/) noexcept {
    if (ctx == nullptr || dst == nullptr || src1 == nullptr || src2 == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return set_internal_result(haze::binary_pp_op<fhetch::sr_mulp>(
        *ctx, haze::to_dev_addr(dst), haze::to_dev_addr(src1), haze::to_dev_addr(src2), mod_idx));
}

extern "C" hazeError_t hazeAddScalar(hazeContext_t ctx, void *dst, const void *src, uint64_t scalar,
                                     int mod_idx, hazeStream_t /*stream*/) noexcept {
    if (ctx == nullptr || dst == nullptr || src == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return set_internal_result(haze::binary_ps_op<fhetch::sr_addps>(
        *ctx, haze::to_dev_addr(dst), haze::to_dev_addr(src), scalar, mod_idx));
}

extern "C" hazeError_t hazeSubScalar(hazeContext_t ctx, void *dst, const void *src, uint64_t scalar,
                                     int mod_idx, hazeStream_t /*stream*/) noexcept {
    if (ctx == nullptr || dst == nullptr || src == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return set_internal_result(haze::binary_ps_op<fhetch::sr_subps>(
        *ctx, haze::to_dev_addr(dst), haze::to_dev_addr(src), scalar, mod_idx));
}

extern "C" hazeError_t hazeMulScalar(hazeContext_t ctx, void *dst, const void *src, uint64_t scalar,
                                     int mod_idx, hazeStream_t /*stream*/) noexcept {
    if (ctx == nullptr || dst == nullptr || src == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return set_internal_result(haze::binary_ps_op<fhetch::sr_mulps>(
        *ctx, haze::to_dev_addr(dst), haze::to_dev_addr(src), scalar, mod_idx));
}

extern "C" hazeError_t hazeNTT(hazeContext_t ctx, void *dst, const void *src, int mod_idx,
                               hazeStream_t /*stream*/) noexcept {
    if (ctx == nullptr || dst == nullptr || src == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return set_internal_result(haze::unary_pq_op<fhetch::sr_ntt>(*ctx, haze::to_dev_addr(dst),
                                                                 haze::to_dev_addr(src), mod_idx));
}

extern "C" hazeError_t hazeINTT(hazeContext_t ctx, void *dst, const void *src, int mod_idx,
                                hazeStream_t /*stream*/) noexcept {
    if (ctx == nullptr || dst == nullptr || src == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return set_internal_result(haze::unary_pq_op<fhetch::sr_intt>(*ctx, haze::to_dev_addr(dst),
                                                                  haze::to_dev_addr(src), mod_idx));
}

extern "C" hazeError_t hazeAutomorph(hazeContext_t ctx, void *dst, const void *src, uint64_t index,
                                     hazeStream_t /*stream*/) noexcept {
    if (ctx == nullptr || dst == nullptr || src == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    // Explicit function-pointer type picks the modulus-less overload (the SRP
    // automorph carries no base; the MRP variant uses the modulus-carrying one).
    // NOLINTNEXTLINE(clang-analyzer-deadcode.DeadStores) — read as a template argument below.
    constexpr niobium::fhetch::Polynomial (*kAutomorphEval)(const niobium::fhetch::Polynomial &,
                                                            uint64_t) = fhetch::sr_automorph_eval;
    return set_internal_result(haze::unary_pi_op<kAutomorphEval>(*ctx, haze::to_dev_addr(dst),
                                                                 haze::to_dev_addr(src), index));
}

// ---------------------------------------------------------------------------
// Multi-residue polynomial (MRP) variants.
//
// Each shim validates only the top-level array arguments (non-null + non-zero
// base length); the per-residue dev pointers in dst / src* are dereferenced
// inside build_mrp_locked, where bad addresses surface as the standard
// HazeInternalError translated by to_public_error.
// ---------------------------------------------------------------------------

extern "C" hazeError_t hazeAddMrp(hazeContext_t ctx, void *const *dst, const void *const *src1,
                                  const void *const *src2, const uint64_t *base, size_t base_len,
                                  hazeStream_t /*stream*/) noexcept {
    if (ctx == nullptr || dst == nullptr || src1 == nullptr || src2 == nullptr || base == nullptr ||
        base_len == 0)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return set_internal_result(
        haze::binary_pp_op_mrp<fhetch::mr_addp>(*ctx, dst, src1, src2, base, base_len));
}

extern "C" hazeError_t hazeSubMrp(hazeContext_t ctx, void *const *dst, const void *const *src1,
                                  const void *const *src2, const uint64_t *base, size_t base_len,
                                  hazeStream_t /*stream*/) noexcept {
    if (ctx == nullptr || dst == nullptr || src1 == nullptr || src2 == nullptr || base == nullptr ||
        base_len == 0)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return set_internal_result(
        haze::binary_pp_op_mrp<fhetch::mr_subp>(*ctx, dst, src1, src2, base, base_len));
}

extern "C" hazeError_t hazeMulMrp(hazeContext_t ctx, void *const *dst, const void *const *src1,
                                  const void *const *src2, const uint64_t *base, size_t base_len,
                                  hazeStream_t /*stream*/) noexcept {
    if (ctx == nullptr || dst == nullptr || src1 == nullptr || src2 == nullptr || base == nullptr ||
        base_len == 0)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return set_internal_result(
        haze::binary_pp_op_mrp<fhetch::mr_mulp>(*ctx, dst, src1, src2, base, base_len));
}

extern "C" hazeError_t hazeAddScalarMrp(hazeContext_t ctx, void *const *dst, const void *const *src,
                                        const uint64_t *scalars, const uint64_t *base,
                                        size_t base_len, hazeStream_t /*stream*/) noexcept {
    if (ctx == nullptr || dst == nullptr || src == nullptr || scalars == nullptr ||
        base == nullptr || base_len == 0)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return set_internal_result(
        haze::binary_ps_op_mrp<fhetch::mr_addps>(*ctx, dst, src, scalars, base, base_len));
}

extern "C" hazeError_t hazeSubScalarMrp(hazeContext_t ctx, void *const *dst, const void *const *src,
                                        const uint64_t *scalars, const uint64_t *base,
                                        size_t base_len, hazeStream_t /*stream*/) noexcept {
    if (ctx == nullptr || dst == nullptr || src == nullptr || scalars == nullptr ||
        base == nullptr || base_len == 0)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return set_internal_result(
        haze::binary_ps_op_mrp<fhetch::mr_subps>(*ctx, dst, src, scalars, base, base_len));
}

extern "C" hazeError_t hazeMulScalarMrp(hazeContext_t ctx, void *const *dst, const void *const *src,
                                        const uint64_t *scalars, const uint64_t *base,
                                        size_t base_len, hazeStream_t /*stream*/) noexcept {
    if (ctx == nullptr || dst == nullptr || src == nullptr || scalars == nullptr ||
        base == nullptr || base_len == 0)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return set_internal_result(
        haze::binary_ps_op_mrp<fhetch::mr_mulps>(*ctx, dst, src, scalars, base, base_len));
}

extern "C" hazeError_t hazeNTTMrp(hazeContext_t ctx, void *const *dst, const void *const *src,
                                  const uint64_t *base, size_t base_len,
                                  hazeStream_t /*stream*/) noexcept {
    if (ctx == nullptr || dst == nullptr || src == nullptr || base == nullptr || base_len == 0)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return set_internal_result(
        haze::unary_p_op_mrp<fhetch::mr_ntt>(*ctx, dst, src, base, base_len));
}

extern "C" hazeError_t hazeINTTMrp(hazeContext_t ctx, void *const *dst, const void *const *src,
                                   const uint64_t *base, size_t base_len,
                                   hazeStream_t /*stream*/) noexcept {
    if (ctx == nullptr || dst == nullptr || src == nullptr || base == nullptr || base_len == 0)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return set_internal_result(
        haze::unary_p_op_mrp<fhetch::mr_intt>(*ctx, dst, src, base, base_len));
}

extern "C" hazeError_t hazeAutomorphMrp(hazeContext_t ctx, void *const *dst, const void *const *src,
                                        uint64_t index, const uint64_t *base, size_t base_len,
                                        hazeStream_t /*stream*/) noexcept {
    if (ctx == nullptr || dst == nullptr || src == nullptr || base == nullptr || base_len == 0)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return set_internal_result(
        haze::unary_pi_op_mrp<fhetch::mr_automorph_eval>(*ctx, dst, src, index, base, base_len));
}

extern "C" hazeError_t hazeRotAutomorphCoeffMrp(hazeContext_t ctx, void *const *dst,
                                                const void *const *src, uint64_t offset,
                                                const uint64_t *base, size_t base_len,
                                                hazeStream_t /*stream*/) noexcept {
    if (ctx == nullptr || dst == nullptr || src == nullptr || base == nullptr || base_len == 0)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return set_internal_result(haze::unary_pi_op_mrp<fhetch::mr_rot_automorph_coeff>(
        *ctx, dst, src, offset, base, base_len));
}

// CRT basis conversion (hazeBasisConvert / hazeModDown / hazeModUp) is
// implemented in basis_convert.cpp. Graph capture / execution stubs
// (hazeStreamBeginCapture, hazeGraph*) live in graph.cpp.
