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
#include "common/handle.hpp"
#include "core/config.hpp"
#include "core/graph.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>

namespace haze {

// Record-path entry points for the deferred tape. No fhetch call
// happens here: every helper appends Nodes / updates the BindingTable
// and returns; hazeFlush's lower::finalize() does the emission. The
// only locks on this path are Graph::append's internal mutex and the
// allocator's own mutex (first-touch promotion / H2D snapshot) — and
// they never nest.

// Backend init (idempotent, failure swallowed — parity with the eager
// EpochSession, which surfaced init failure at flush) + parameter
// freeze. Returns the frozen snapshot, or nullptr while ring_dim is
// unset (the caller's own validation then reports the op's failure).
const ConfigSnapshot *record_prelude() noexcept;

// Resolve `addr` to its bound value. On first reference, snapshots the
// shadow bytes NOW (evicting, exactly like the eager engine — the
// bytes' ownership moves into the recording) and appends the
// InputSnapshot node that lowers to fhetch::tag_input.
std::expected<ValueId, HazeInternalError> resolve_operand(DevAddr addr, uint64_t ring_dim) noexcept;

// fhetch's copy sentinel (TraceWriter COPY_MODULUS_VALUE); doubles as
// the "modulus unknown" marker for the addr->modulus tracking.
inline constexpr uint64_t kCopyModulus = 0xFFFFFFFFFFFFFFFFULL;

// Bind `addr` to a fresh value (compute result). Call only after every
// resolve_operand of the op, so in-place dst==src reads the old value.
// `modulus` records the residue's real modulus (kCopyModulus =
// "unknown") so a later pass-through copy / eval-form automorph of this
// address can recover and bind it.
ValueId bind_result(DevAddr addr, uint64_t modulus = kCopyModulus) noexcept;

// Real modulus last recorded for `addr` by a modulus-carrying op, or
// kCopyModulus if none (raw input, or a sentinel-only result).
uint64_t recorded_modulus(DevAddr addr) noexcept;

// Record a pass-through copy dst <- src. The op carries the COPY
// sentinel; the real modulus rides as metadata — pass it when known
// (MRP D2D has base[i]), otherwise it is recovered from the source's
// recorded modulus so a copy of a compute-produced SRP value stays
// probe-serializable on transport.
std::expected<void, HazeInternalError> record_copy(DevAddr dst, DevAddr src, uint64_t ring_dim,
                                                   uint64_t modulus = kCopyModulus) noexcept;

// Eager H2D input registration (port of tag_h2d_input_locked): the
// just-written shadow bytes are snapshotted non-evicting and the addr
// is unconditionally rebound; derive() drops any pending output tag.
std::expected<void, HazeInternalError> record_h2d_input(DevAddr addr) noexcept;

// Declare `addr` an output (backs hazeTagOutput). Requires a live
// binding — tagging with nothing recorded is the caller error the
// eager engine reported as SourceUnavailable.
std::expected<void, HazeInternalError> record_tag_output(DevAddr addr) noexcept;

// Drop addr's binding and append the Invalidate node (hazeFree /
// hazeMemset). No-op while the tape is empty: there is nothing to
// invalidate, and appending would turn a compute-free hazeFlush into a
// backend-initializing one.
void record_invalidate(DevAddr addr) noexcept;

// ---- Free functions the api/ shims call (signatures preserved from
// the deleted epoch.hpp) ----

// Pure shadow read backing hazeMemcpy D2H; unmaterialized bytes read as
// OutputNotFlushed.
std::expected<void, HazeInternalError> copy_to_host(void *dst, DevAddr src, size_t count) noexcept;

// Backs hazeWriteProgram (finalize without replay).
std::expected<void, HazeInternalError> write_program() noexcept;

// Backs hazeTagOutput.
std::expected<void, HazeInternalError> tag_output(DevAddr addr) noexcept;

// Backs hazeFlush (finalize + replay + shadow population).
std::expected<void, HazeInternalError> flush() noexcept;

// D2D as a recorded pass-through copy: promotes `src` if needed and
// binds `dst` to the result.
std::expected<void, HazeInternalError> copy_device_to_device(DevAddr dst, DevAddr src,
                                                             size_t count) noexcept;

// H2D-time eager-tag: register the H2D'd buffer at `addr` as an input.
std::expected<void, HazeInternalError> tag_h2d_input(DevAddr addr) noexcept;

} // namespace haze
