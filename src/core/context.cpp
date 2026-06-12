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
#include "core/context.hpp"

#include "common/errors.hpp"
#include "core/allocator.hpp"
#include "core/config.hpp"
#include "core/graph.hpp"
#include "core/kernel_cache.hpp"

#include <cstdint>
#include <expected>

namespace haze {

Context &default_context() noexcept {
    static Context ctx;
    return ctx;
}

// TEMPORARY bridge accessors backing the parameterless C ABI (deleted
// with the bridge in de-globalization step C4). Everything here is the
// process-default context; new code takes a Context& instead.

Config &config() noexcept {
    return default_context().config;
}

DeviceAllocator &allocator() noexcept {
    return default_context().allocator;
}

BindingTable &bindings() noexcept {
    return default_context().values;
}

BindingTable &recorded_moduli() noexcept {
    return default_context().recorded_moduli;
}

Graph &graph() noexcept {
    return default_context().tape;
}

KernelCache &kernel_cache() noexcept {
    return default_context().kernels;
}

std::expected<void, HazeInternalError> set_ring_dimension(Context &ctx, uint64_t n) noexcept {
    return ctx.config.set_ring_dimension(n, ctx.allocator, ctx.values, ctx.recorded_moduli);
}

} // namespace haze
