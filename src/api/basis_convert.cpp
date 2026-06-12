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

#include "core/basis_convert.hpp"

#include "common/errors.hpp"
#include "core/context.hpp"

#include <haze/haze.h>
#include <haze/haze_types.h>

namespace {

template <typename ParamsT, auto OpFn>
hazeError_t dispatch(void *const *dst, const void *const *src, const void *params) noexcept {
    if (params == nullptr || src == nullptr || dst == nullptr) {
        return set_error(HAZE_ERROR_INVALID_VALUE);
    }
    auto result = OpFn(haze::default_context(), dst, src, *static_cast<const ParamsT *>(params));
    if (!result) {
        return set_error(haze::to_public_error(result.error()));
    }
    return HAZE_SUCCESS;
}

} // namespace

extern "C" hazeError_t hazeBasisConvert(void *const *dst, const void *const *src,
                                        const void *params, hazeStream_t /*stream*/) noexcept {
    return dispatch<hazeBasisConvertParams, haze::basis_convert>(dst, src, params);
}

extern "C" hazeError_t hazeModDown(void *const *dst, const void *const *src, const void *params,
                                   hazeStream_t /*stream*/) noexcept {
    return dispatch<hazeModDownParams, haze::mod_down>(dst, src, params);
}

extern "C" hazeError_t hazeModUp(void *const *dst, const void *const *src, const void *params,
                                 hazeStream_t /*stream*/) noexcept {
    return dispatch<hazeModUpParams, haze::mod_up>(dst, src, params);
}
