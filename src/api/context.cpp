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
// Context-management shims. A hazeContext_t owns one recording program;
// its FHE parameters are fixed at creation (see the contract block in
// include/haze/haze.h).

#include "core/context.hpp"

#include "common/errors.hpp"

#include <cstddef>
#include <cstdint>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <memory>
#include <new>

extern "C" hazeError_t hazeContextCreate(hazeContext_t *ctx, uint64_t ring_dim,
                                         const uint64_t *moduli, size_t n_moduli) noexcept {
    if (ctx == nullptr || (n_moduli != 0 && moduli == nullptr))
        return set_error(HAZE_ERROR_INVALID_VALUE);
    auto fresh = std::unique_ptr<haze_context_s>(new (std::nothrow) haze_context_s);
    if (fresh == nullptr)
        return set_error(HAZE_ERROR_OUT_OF_MEMORY);
    if (auto init = fresh->config.init_params(ring_dim, moduli, n_moduli, fresh->allocator,
                                              fresh->values, fresh->recorded_moduli);
        !init)
        return set_error(haze::to_public_error(init.error()));
    *ctx = fresh.release();
    return set_error(HAZE_SUCCESS);
}

extern "C" hazeError_t hazeContextDestroy(hazeContext_t ctx) noexcept {
    if (ctx == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    // An unflushed tape or open kernel bracket dies with the context:
    // thunks never run, so nothing was ever emitted to fhetch.
    delete ctx;
    return set_error(HAZE_SUCCESS);
}
