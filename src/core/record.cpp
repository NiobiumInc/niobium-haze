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
#include "core/record.hpp"

#include "common/errors.hpp"
#include "common/handle.hpp"
#include "core/allocator.hpp"
#include "core/backend.hpp"
#include "core/config.hpp"
#include "core/graph.hpp"
#include "core/lower.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <niobium/fhetch_api.h>
#include <utility>
#include <vector>

namespace haze {

namespace fhetch = niobium::fhetch;


const ConfigSnapshot *record_prelude() noexcept {
    // Init before anything else so first-call side effects (program-dir
    // resolution, probe suppression) keep their eager-engine timing;
    // failure surfaces at flush, not here.
    [[maybe_unused]] const bool initialized = backend().ensure_initialized();
    return config().freeze();
}

std::expected<ValueId, HazeInternalError> resolve_operand(DevAddr addr,
                                                          uint64_t ring_dim) noexcept {
    if (const ValueId existing = bindings().load(addr); existing != kUnbound)
        return existing;

    // Evicting read: the caller now owns the bytes via the recording;
    // the shadow is freed mid-program exactly as the eager engine did.
    auto components = allocator().extract_polynomial_components(addr, ring_dim);
    if (!components) {
        // Compute / D2D on an addr with neither shadow data nor a
        // binding is undefined under the record-and-replay model.
        if (components.error() == HazeInternalError::NoData) {
            record_internal_error(HazeInternalError::SourceUnavailable,
                                  "resolve_operand: no shadow and no binding");
            return std::unexpected(HazeInternalError::SourceUnavailable);
        }
        return std::unexpected(components.error());
    }

    const ValueId fresh = new_value_id();
    if (const ValueId resident = bindings().promote(addr, fresh); resident != fresh) {
        // Another thread first-touched the same addr (user-undefined,
        // memory-safe): adopt the winner and drop our snapshot.
        return resident;
    }

    Node node{};
    node.kind = Node::Kind::InputSnapshot;
    node.addr = addr;
    node.dst_vid = fresh;
    node.entry = "haze input promotion";
    node.thunk = [bytes = std::move(*components), ring_dim,
                  fresh](LowerCtx &ctx) mutable -> std::expected<void, HazeInternalError> {
        fhetch::Polynomial poly =
            fhetch::Polynomial::from_data(std::move(bytes), ring_dim, fhetch::Format::Evaluation);
        fhetch::tag_input(ctx.node_name(), poly);
        ctx.bind(fresh, std::move(poly));
        return {};
    };
    graph().append(std::move(node));
    return fresh;
}

ValueId bind_result(DevAddr addr, uint64_t modulus) noexcept {
    const ValueId fresh = new_value_id();
    bindings().store(addr, fresh);
    // A no-modulus (kCopyModulus) result drops any stale entry so a
    // later copy/automorph can't recover a previous occupant's modulus.
    if (modulus != kCopyModulus)
        recorded_moduli().store(addr, modulus);
    else
        recorded_moduli().erase(addr);
    return fresh;
}

uint64_t recorded_modulus(DevAddr addr) noexcept {
    const uint64_t q = recorded_moduli().load(addr);
    return q == 0 ? kCopyModulus : q;
}

std::expected<void, HazeInternalError> record_copy(DevAddr dst, DevAddr src, uint64_t ring_dim,
                                                   uint64_t modulus) noexcept {
    const auto src_vid = resolve_operand(src, ring_dim);
    if (!src_vid)
        return std::unexpected(src_vid.error());
    // The op carries the COPY sentinel (the executor lowers ADDI imm=0
    // at modulus-table index 0 as a register copy); the real modulus
    // rides as metadata. Recover it from the source when the caller
    // passed none, and record it back onto src to match the binding.
    if (modulus == kCopyModulus)
        modulus = recorded_modulus(src);
    if (modulus != kCopyModulus)
        recorded_moduli().store(src, modulus);
    const ValueId dst_vid = bind_result(dst, modulus);

    Node node{};
    node.kind = Node::Kind::Copy;
    node.addr = dst;
    node.dst_vid = dst_vid;
    node.src_vids = {*src_vid};
    node.entry = "hazeMemcpy D2D";
    node.thunk = [s = *src_vid, d = dst_vid,
                  modulus](LowerCtx &ctx) -> std::expected<void, HazeInternalError> {
        const auto src_poly = ctx.poly(s);
        if (!src_poly)
            return std::unexpected(src_poly.error());
        auto copy = fhetch::sr_addps(**src_poly, fhetch::Scalar::from_int(0), kCopyModulus);
        if (modulus != kCopyModulus) {
            // Both ends: a source only ever touched by copies would
            // otherwise keep the sentinel in its captured input record.
            fhetch::bind_modulus(**src_poly, modulus);
            fhetch::bind_modulus(copy, modulus);
        }
        ctx.bind(d, std::move(copy));
        return {};
    };
    graph().append(std::move(node));
    return {};
}

std::expected<void, HazeInternalError> record_h2d_input(DevAddr addr) noexcept {
    // Both guards cover invariants the H2D entry point already enforced
    // (copy_h2d requires alloc_set_ membership and a configured
    // poly_bytes_); hits are broken-haze internal errors so the input
    // tag is never silently dropped.
    const ConfigSnapshot *cfg = config().freeze();
    const uint64_t ring_dim = cfg != nullptr ? cfg->ring_dim : 0;
    if (ring_dim == 0) {
        record_internal_error(HazeInternalError::NotConfigured,
                              "record_h2d_input: ring_dim == 0 after copy_h2d");
        return std::unexpected(HazeInternalError::NotConfigured);
    }
    // Non-evicting: a subsequent compute-free D2H still reads the
    // original H2D bytes from the shadow.
    auto components = allocator().read_polynomial_components(addr, ring_dim);
    if (!components)
        return std::unexpected(components.error());

    const ValueId fresh = new_value_id();
    Node node{};
    node.kind = Node::Kind::H2DInput;
    node.addr = addr;
    node.dst_vid = fresh;
    node.entry = "hazeMemcpy H2D";
    node.thunk = [bytes = std::move(*components), ring_dim,
                  fresh](LowerCtx &ctx) mutable -> std::expected<void, HazeInternalError> {
        fhetch::Polynomial poly =
            fhetch::Polynomial::from_data(std::move(bytes), ring_dim, fhetch::Format::Evaluation);
        fhetch::tag_input(ctx.node_name(), poly);
        ctx.bind(fresh, std::move(poly));
        return {};
    };
    graph().append(std::move(node));
    // New H2D bytes are the truth at addr: overwrite any binding.
    bindings().store(addr, fresh);
    return {};
}

std::expected<void, HazeInternalError> record_tag_output(DevAddr addr) noexcept {
    if (bindings().load(addr) == kUnbound) {
        record_internal_error(HazeInternalError::SourceUnavailable,
                              "record_tag_output: addr not bound");
        return std::unexpected(HazeInternalError::SourceUnavailable);
    }
    Node node{};
    node.kind = Node::Kind::TagOutput;
    node.addr = addr;
    node.entry = "hazeTagOutput";
    graph().append(std::move(node));
    return {};
}

void record_invalidate(DevAddr addr) noexcept {
    bindings().erase(addr);
    recorded_moduli().erase(addr);
    // Empty tape ⇒ no binding/group can reference addr, and a metadata
    // node would make a compute-free hazeFlush initialize the backend.
    if (graph().size() == 0)
        return;
    Node node{};
    node.kind = Node::Kind::Invalidate;
    node.addr = addr;
    node.entry = "hazeFree/hazeMemset";
    graph().append(std::move(node));
}

std::expected<void, HazeInternalError> copy_to_host(void *dst, DevAddr src, size_t count) noexcept {
    return allocator().copy_to_host(dst, src, count);
}

std::expected<void, HazeInternalError> write_program() noexcept {
    return finalize(/*run_replay=*/false);
}

std::expected<void, HazeInternalError> tag_output(DevAddr addr) noexcept {
    return record_tag_output(addr);
}

std::expected<void, HazeInternalError> flush() noexcept {
    // Montgomery / bit-reversed traces can't execute on the in-process
    // simulator; surface the failure at the flush call instead of a
    // silent no-op followed by OutputNotFlushed on the next D2H.
    if ((config().montgomery() || config().bit_reversal()) && config().target() == kLocalTarget) {
        record_internal_error(HazeInternalError::UnsupportedDataFormat,
                              "haze::flush (montgomery/bit_reversal require a transport "
                              "target such as FUNC_SIM)");
        return std::unexpected(HazeInternalError::UnsupportedDataFormat);
    }
    return finalize(/*run_replay=*/true);
}

std::expected<void, HazeInternalError> copy_device_to_device(DevAddr dst, DevAddr src,
                                                             size_t /*count*/) noexcept {
    const ConfigSnapshot *cfg = record_prelude();
    return record_copy(dst, src, cfg != nullptr ? cfg->ring_dim : 0);
}

std::expected<void, HazeInternalError> tag_h2d_input(DevAddr addr) noexcept {
    [[maybe_unused]] const bool initialized = backend().ensure_initialized();
    return record_h2d_input(addr);
}

} // namespace haze
