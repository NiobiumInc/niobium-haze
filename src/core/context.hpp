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
#include "core/allocator.hpp"
#include "core/config.hpp"
#include "core/context_fwd.hpp" // IWYU pragma: export
#include "core/graph.hpp"
#include "core/kernel_cache.hpp"

#include <cstdint>
#include <expected>

// The haze recording context: one program in flight, with its own
// parameters, device address space, value bindings, tape, and kernel
// cache. This is the C ABI's opaque handle type (hazeContext_t in
// haze_types.h), so it lives in the global namespace.
//
// De-globalization status: every former singleton is now a member here.
// The ONLY process-global haze state left is the flush path's
// serialization around the (for now) global fhetch engine — see
// lowering_session.hpp — plus the content-free ValueId counter.
// `haze::default_context()` is a TEMPORARY bridge that backs the
// parameterless C ABI until the context-first entry points land
// (deleted in the final step of the de-globalization sequence).
struct haze_context_s {
    haze::Config config;
    haze::DeviceAllocator allocator;
    haze::BindingTable values;          // addr -> ValueId
    haze::BindingTable recorded_moduli; // addr -> last real modulus
    haze::Graph tape;
    haze::KernelCache kernels;
};

namespace haze {

// TEMPORARY bridge (de-globalization step C1, removed in C4): the
// process-default context behind the parameterless C ABI. New code
// must take a Context& instead of calling this.
Context &default_context() noexcept;

// Orchestrated ring-dimension set: Config's parameter validation plus
// the sibling geometry pushes (allocator pool size + binding-table slot
// geometry) under the Config lock. TEMPORARY — the de-globalization's
// params-at-create step moves the parameters to hazeContextCreate and
// deletes this.
std::expected<void, HazeInternalError> set_ring_dimension(Context &ctx, uint64_t n) noexcept;

} // namespace haze
