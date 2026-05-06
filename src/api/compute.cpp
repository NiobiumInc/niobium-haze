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
// haze_compute.hpp. Each shim validates pointer arguments, casts to
// DevAddr, and selects the matching FHETCH operation.

#include "core/compute.hpp"

#include "common/errors.hpp"
#include "common/handle.hpp"

#include <cstddef>
#include <cstdint>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <niobium/fhetch_api.h>

namespace fhetch = niobium::fhetch;

extern "C" hazeError_t hazeAdd(void *dst, const void *src1, const void *src2, int mod_idx,
                               hazeStream_t /*stream*/) noexcept {
    if (dst == nullptr || src1 == nullptr || src2 == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return haze::binary_pp_op<fhetch::sr_addp>(haze::to_dev_addr(dst), haze::to_dev_addr(src1),
                                               haze::to_dev_addr(src2), mod_idx);
}

extern "C" hazeError_t hazeSub(void *dst, const void *src1, const void *src2, int mod_idx,
                               hazeStream_t /*stream*/) noexcept {
    if (dst == nullptr || src1 == nullptr || src2 == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return haze::binary_pp_op<fhetch::sr_subp>(haze::to_dev_addr(dst), haze::to_dev_addr(src1),
                                               haze::to_dev_addr(src2), mod_idx);
}

extern "C" hazeError_t hazeMul(void *dst, const void *src1, const void *src2, int mod_idx,
                               hazeStream_t /*stream*/) noexcept {
    if (dst == nullptr || src1 == nullptr || src2 == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return haze::binary_pp_op<fhetch::sr_mulp>(haze::to_dev_addr(dst), haze::to_dev_addr(src1),
                                               haze::to_dev_addr(src2), mod_idx);
}

extern "C" hazeError_t hazeAddScalar(void *dst, const void *src, uint64_t scalar, int mod_idx,
                                     hazeStream_t /*stream*/) noexcept {
    if (dst == nullptr || src == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return haze::binary_ps_op<fhetch::sr_addps>(haze::to_dev_addr(dst), haze::to_dev_addr(src),
                                                scalar, mod_idx);
}

extern "C" hazeError_t hazeSubScalar(void *dst, const void *src, uint64_t scalar, int mod_idx,
                                     hazeStream_t /*stream*/) noexcept {
    if (dst == nullptr || src == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return haze::binary_ps_op<fhetch::sr_subps>(haze::to_dev_addr(dst), haze::to_dev_addr(src),
                                                scalar, mod_idx);
}

extern "C" hazeError_t hazeMulScalar(void *dst, const void *src, uint64_t scalar, int mod_idx,
                                     hazeStream_t /*stream*/) noexcept {
    if (dst == nullptr || src == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return haze::binary_ps_op<fhetch::sr_mulps>(haze::to_dev_addr(dst), haze::to_dev_addr(src),
                                                scalar, mod_idx);
}

extern "C" hazeError_t hazeNTT(void *dst, const void *src, int mod_idx,
                               hazeStream_t /*stream*/) noexcept {
    if (dst == nullptr || src == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return haze::unary_pq_op<fhetch::sr_ntt>(haze::to_dev_addr(dst), haze::to_dev_addr(src),
                                             mod_idx);
}

extern "C" hazeError_t hazeINTT(void *dst, const void *src, int mod_idx,
                                hazeStream_t /*stream*/) noexcept {
    if (dst == nullptr || src == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return haze::unary_pq_op<fhetch::sr_intt>(haze::to_dev_addr(dst), haze::to_dev_addr(src),
                                              mod_idx);
}

extern "C" hazeError_t hazeAutomorph(void *dst, const void *src, uint64_t index,
                                     hazeStream_t /*stream*/) noexcept {
    if (dst == nullptr || src == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return haze::unary_pi_op<fhetch::sr_automorph_eval>(haze::to_dev_addr(dst),
                                                        haze::to_dev_addr(src), index);
}

// ---------------------------------------------------------------------------
// Multi-residue polynomial (MRP) variants.
//
// Each shim validates only the top-level array arguments (non-null + non-zero
// base length); the per-residue dev pointers in dst / src* are dereferenced
// inside build_mrp_locked, where bad addresses surface as the standard
// HazeInternalError translated by to_public_error.
// ---------------------------------------------------------------------------

extern "C" hazeError_t hazeAddMrp(void *const *dst, const void *const *src1,
                                  const void *const *src2, const uint64_t *base, size_t base_len,
                                  hazeStream_t /*stream*/) noexcept {
    if (dst == nullptr || src1 == nullptr || src2 == nullptr || base == nullptr || base_len == 0)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return haze::binary_pp_op_mrp<fhetch::mr_addp>(dst, src1, src2, base, base_len);
}

extern "C" hazeError_t hazeSubMrp(void *const *dst, const void *const *src1,
                                  const void *const *src2, const uint64_t *base, size_t base_len,
                                  hazeStream_t /*stream*/) noexcept {
    if (dst == nullptr || src1 == nullptr || src2 == nullptr || base == nullptr || base_len == 0)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return haze::binary_pp_op_mrp<fhetch::mr_subp>(dst, src1, src2, base, base_len);
}

extern "C" hazeError_t hazeMulMrp(void *const *dst, const void *const *src1,
                                  const void *const *src2, const uint64_t *base, size_t base_len,
                                  hazeStream_t /*stream*/) noexcept {
    if (dst == nullptr || src1 == nullptr || src2 == nullptr || base == nullptr || base_len == 0)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return haze::binary_pp_op_mrp<fhetch::mr_mulp>(dst, src1, src2, base, base_len);
}

extern "C" hazeError_t hazeAddScalarMrp(void *const *dst, const void *const *src,
                                        const uint64_t *scalars, const uint64_t *base,
                                        size_t base_len, hazeStream_t /*stream*/) noexcept {
    if (dst == nullptr || src == nullptr || scalars == nullptr || base == nullptr || base_len == 0)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return haze::binary_ps_op_mrp<fhetch::mr_addps>(dst, src, scalars, base, base_len);
}

extern "C" hazeError_t hazeSubScalarMrp(void *const *dst, const void *const *src,
                                        const uint64_t *scalars, const uint64_t *base,
                                        size_t base_len, hazeStream_t /*stream*/) noexcept {
    if (dst == nullptr || src == nullptr || scalars == nullptr || base == nullptr || base_len == 0)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return haze::binary_ps_op_mrp<fhetch::mr_subps>(dst, src, scalars, base, base_len);
}

extern "C" hazeError_t hazeMulScalarMrp(void *const *dst, const void *const *src,
                                        const uint64_t *scalars, const uint64_t *base,
                                        size_t base_len, hazeStream_t /*stream*/) noexcept {
    if (dst == nullptr || src == nullptr || scalars == nullptr || base == nullptr || base_len == 0)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return haze::binary_ps_op_mrp<fhetch::mr_mulps>(dst, src, scalars, base, base_len);
}

extern "C" hazeError_t hazeNTTMrp(void *const *dst, const void *const *src, const uint64_t *base,
                                  size_t base_len, hazeStream_t /*stream*/) noexcept {
    if (dst == nullptr || src == nullptr || base == nullptr || base_len == 0)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return haze::unary_p_op_mrp<fhetch::mr_ntt>(dst, src, base, base_len);
}

extern "C" hazeError_t hazeINTTMrp(void *const *dst, const void *const *src, const uint64_t *base,
                                   size_t base_len, hazeStream_t /*stream*/) noexcept {
    if (dst == nullptr || src == nullptr || base == nullptr || base_len == 0)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return haze::unary_p_op_mrp<fhetch::mr_intt>(dst, src, base, base_len);
}

extern "C" hazeError_t hazeAutomorphMrp(void *const *dst, const void *const *src, uint64_t index,
                                        const uint64_t *base, size_t base_len,
                                        hazeStream_t /*stream*/) noexcept {
    if (dst == nullptr || src == nullptr || base == nullptr || base_len == 0)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return haze::unary_pi_op_mrp<fhetch::mr_automorph_eval>(dst, src, index, base, base_len);
}

extern "C" hazeError_t hazeRotAutomorphCoeffMrp(void *const *dst, const void *const *src,
                                                uint64_t offset, const uint64_t *base,
                                                size_t base_len,
                                                hazeStream_t /*stream*/) noexcept {
    if (dst == nullptr || src == nullptr || base == nullptr || base_len == 0)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return haze::unary_pi_op_mrp<fhetch::mr_rot_automorph_coeff>(dst, src, offset, base, base_len);
}

// CRT basis conversion (hazeBasisConvert / hazeModDown / hazeModUp) is
// implemented in basis_convert.cpp. Graph capture / execution stubs
// (hazeStreamBeginCapture, hazeGraph*) live in graph.cpp.
