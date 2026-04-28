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
#include "common/handle.hpp"
#include "common/errors.hpp"

#include <haze/haze.h>
#include <haze/haze_types.h>

#include <niobium/fhetch_api.h>

#include <cstdint>

namespace fhetch = niobium::fhetch;

extern "C" hazeError_t hazeAdd(void *dst, const void *src1, const void *src2, int mod_idx,
                               hazeStream_t /*stream*/) noexcept {
    if (dst == nullptr || src1 == nullptr || src2 == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return haze::binary_pp_op<fhetch::sr_addp>(haze::to_dev_addr(dst),
                                                       haze::to_dev_addr(src1),
                                                       haze::to_dev_addr(src2), mod_idx);
}

extern "C" hazeError_t hazeSub(void *dst, const void *src1, const void *src2, int mod_idx,
                               hazeStream_t /*stream*/) noexcept {
    if (dst == nullptr || src1 == nullptr || src2 == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return haze::binary_pp_op<fhetch::sr_subp>(haze::to_dev_addr(dst),
                                                       haze::to_dev_addr(src1),
                                                       haze::to_dev_addr(src2), mod_idx);
}

extern "C" hazeError_t hazeMul(void *dst, const void *src1, const void *src2, int mod_idx,
                               hazeStream_t /*stream*/) noexcept {
    if (dst == nullptr || src1 == nullptr || src2 == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return haze::binary_pp_op<fhetch::sr_mulp>(haze::to_dev_addr(dst),
                                                       haze::to_dev_addr(src1),
                                                       haze::to_dev_addr(src2), mod_idx);
}

extern "C" hazeError_t hazeAddScalar(void *dst, const void *src, uint64_t scalar, int mod_idx,
                                     hazeStream_t /*stream*/) noexcept {
    if (dst == nullptr || src == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return haze::binary_ps_op<fhetch::sr_addps>(
        haze::to_dev_addr(dst), haze::to_dev_addr(src), scalar, mod_idx);
}

extern "C" hazeError_t hazeSubScalar(void *dst, const void *src, uint64_t scalar, int mod_idx,
                                     hazeStream_t /*stream*/) noexcept {
    if (dst == nullptr || src == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return haze::binary_ps_op<fhetch::sr_subps>(
        haze::to_dev_addr(dst), haze::to_dev_addr(src), scalar, mod_idx);
}

extern "C" hazeError_t hazeMulScalar(void *dst, const void *src, uint64_t scalar, int mod_idx,
                                     hazeStream_t /*stream*/) noexcept {
    if (dst == nullptr || src == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return haze::binary_ps_op<fhetch::sr_mulps>(
        haze::to_dev_addr(dst), haze::to_dev_addr(src), scalar, mod_idx);
}

extern "C" hazeError_t hazeNTT(void *dst, const void *src, int mod_idx,
                               hazeStream_t /*stream*/) noexcept {
    if (dst == nullptr || src == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return haze::unary_pq_op<fhetch::sr_ntt>(haze::to_dev_addr(dst),
                                                     haze::to_dev_addr(src), mod_idx);
}

extern "C" hazeError_t hazeINTT(void *dst, const void *src, int mod_idx,
                                hazeStream_t /*stream*/) noexcept {
    if (dst == nullptr || src == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return haze::unary_pq_op<fhetch::sr_intt>(haze::to_dev_addr(dst),
                                                      haze::to_dev_addr(src), mod_idx);
}

extern "C" hazeError_t hazeAutomorph(void *dst, const void *src, uint64_t index,
                                     hazeStream_t /*stream*/) noexcept {
    if (dst == nullptr || src == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return haze::unary_pi_op<fhetch::sr_automorph_eval>(
        haze::to_dev_addr(dst), haze::to_dev_addr(src), index);
}

// CRT basis conversion (hazeBasisConvert / hazeModDown / hazeModUp) is
// implemented in haze_basis_convert.cpp.

// Graph capture / execution stubs (CUDA-shape names) — not supported yet.

extern "C" hazeError_t hazeStreamBeginCapture(hazeStream_t /*stream*/) noexcept {
    return set_error(HAZE_ERROR_NOT_SUPPORTED);
}

extern "C" hazeError_t hazeStreamEndCapture(hazeStream_t /*stream*/,
                                            hazeGraph_t *graph) noexcept {
    if (graph != nullptr)
        *graph = nullptr;
    return set_error(HAZE_ERROR_NOT_SUPPORTED);
}

extern "C" hazeError_t hazeGraphInstantiate(hazeGraphExec_t *exec,
                                            hazeGraph_t /*graph*/) noexcept {
    if (exec != nullptr)
        *exec = nullptr;
    return set_error(HAZE_ERROR_NOT_SUPPORTED);
}

extern "C" hazeError_t hazeGraphLaunch(hazeGraphExec_t /*exec*/,
                                       hazeStream_t /*stream*/) noexcept {
    return set_error(HAZE_ERROR_NOT_SUPPORTED);
}

extern "C" hazeError_t hazeGraphExecUpdate(hazeGraphExec_t /*exec*/,
                                           hazeGraph_t /*graph*/) noexcept {
    return set_error(HAZE_ERROR_NOT_SUPPORTED);
}

extern "C" hazeError_t hazeGraphExecDestroy(hazeGraphExec_t /*exec*/) noexcept {
    return set_error(HAZE_ERROR_NOT_SUPPORTED);
}

extern "C" hazeError_t hazeGraphDestroy(hazeGraph_t /*graph*/) noexcept {
    return set_error(HAZE_ERROR_NOT_SUPPORTED);
}
