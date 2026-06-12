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
// Per-context program metadata / target shims. The FHE parameters (ring
// dimension, modulus chain) are fixed by hazeContextCreate and have no
// setters.

#include "core/config.hpp"

#include "common/errors.hpp"
#include "core/context.hpp"

#include <haze/haze.h>
#include <haze/haze_types.h>

extern "C" hazeError_t hazeSetProgramInfo(hazeContext_t ctx, const char *name, const char *version,
                                          const char *description) noexcept {
    if (ctx == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return set_internal_result(ctx->config.set_program_info(name, version, description));
}

extern "C" hazeError_t hazeSetTarget(hazeContext_t ctx, const char *target) noexcept {
    if (ctx == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return set_internal_result(ctx->config.set_target(target));
}

extern "C" hazeError_t hazeSetProgramDirectory(hazeContext_t ctx, const char *dir) noexcept {
    if (ctx == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return set_internal_result(ctx->config.set_program_directory(dir));
}

extern "C" hazeError_t hazeSetMontgomery(hazeContext_t ctx, int enable) noexcept {
    if (ctx == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    ctx->config.set_montgomery(enable != 0);
    return set_error(HAZE_SUCCESS);
}

extern "C" hazeError_t hazeSetBitReversal(hazeContext_t ctx, int enable) noexcept {
    if (ctx == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    ctx->config.set_bit_reversal(enable != 0);
    return set_error(HAZE_SUCCESS);
}
