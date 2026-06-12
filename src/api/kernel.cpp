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
// Kernel-memoization shims. Thin extern "C" layer over
// haze::KernelCache; see the contract block above hazeKernelBegin in
// include/haze/haze.h.

#include "common/errors.hpp"
#include "core/context.hpp"
#include "core/kernel_cache.hpp"
#include "core/record.hpp"

#include <cstddef>
#include <cstdint>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <span>

namespace {

// Validate one traced-value descriptor (shared by inputs and outputs).
template <typename Desc> bool well_formed(const Desc *list, size_t n) noexcept {
    if (n != 0 && list == nullptr)
        return false;
    for (size_t i = 0; i < n; ++i) {
        if (list[i].residues == nullptr || list[i].base == nullptr || list[i].base_len == 0)
            return false;
        for (size_t r = 0; r < list[i].base_len; ++r)
            if (list[i].residues[r] == nullptr)
                return false;
    }
    return true;
}

} // namespace

extern "C" hazeError_t hazeKernelBegin(const char *name, uint64_t key_hash,
                                       const uint8_t *key_bytes, size_t key_bytes_len,
                                       const hazeKernelInput *inputs, size_t n_inputs,
                                       hazeKernelDisposition *disposition) noexcept {
    if (name == nullptr || disposition == nullptr || (key_bytes_len != 0 && key_bytes == nullptr) ||
        !well_formed(inputs, n_inputs))
        return set_error(HAZE_ERROR_INVALID_VALUE);
    haze::Context &ctx = haze::default_context();
    // Nesting a kernel inside an open bracket is reserved future work.
    if (ctx.kernels.has_open_frame())
        return set_error(HAZE_ERROR_NOT_SUPPORTED);
    // Bring the backend up / freeze parameters with first-compute timing
    // even when the body is about to be skipped.
    (void)haze::record_prelude(ctx);

    auto result =
        ctx.kernels.begin(ctx, name, key_hash, {key_bytes, key_bytes_len}, {inputs, n_inputs});
    if (!result)
        return set_error(haze::to_public_error(result.error()));
    *disposition = *result;
    return set_error(HAZE_SUCCESS);
}

extern "C" hazeError_t hazeKernelEnd(const hazeKernelOutput *outputs, size_t n_outputs) noexcept {
    if (!well_formed(outputs, n_outputs))
        return set_error(HAZE_ERROR_INVALID_VALUE);
    haze::Context &ctx = haze::default_context();
    return set_internal_result(ctx.kernels.end(ctx, {outputs, n_outputs}));
}

extern "C" hazeError_t hazeKernelAbort(void) noexcept {
    haze::Context &ctx = haze::default_context();
    return set_internal_result(ctx.kernels.abort_frame(ctx));
}

extern "C" hazeError_t hazeSetKernelMemo(int enable) noexcept {
    haze::default_context().kernels.set_memo_enabled(enable != 0);
    return set_error(HAZE_SUCCESS);
}

extern "C" hazeError_t hazeSetKernelValidate(int enable) noexcept {
    haze::default_context().kernels.set_validate(enable != 0);
    return set_error(HAZE_SUCCESS);
}
