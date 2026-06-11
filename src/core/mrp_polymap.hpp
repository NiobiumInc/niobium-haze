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
#include "core/graph.hpp"
#include "core/lower.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <niobium/fhetch_api.h>
#include <span>
#include <utility>
#include <vector>

namespace haze {

// MRP record-path glue: record_mrp_sources resolves K per-addr residues
// to tape values (appending the dedup'd MRP-input-tag node);
// record_mrp_dests binds K result residues; record_mrp_out_group
// appends the output-group registration that the flush-time derivation
// expands when any residue is tagged.

// Resolve each residue (first touch promotes shadow bytes to an input
// node) and, for multi-residue groups, append the MrpInputTag node so
// the bridge can synthesize a multi-tower input ciphertext. Returns the
// residues' values in base order.
std::expected<std::vector<ValueId>, HazeInternalError>
record_mrp_sources(const void *const *polys, const uint64_t *base, std::size_t len,
                   uint64_t ring_dim) noexcept;

// Fresh result bindings for the K dst residues. Call AFTER every
// record_mrp_sources of the op (in-place safety).
struct MrpDests {
    std::vector<DevAddr> addrs;
    std::vector<ValueId> vids;
};
MrpDests record_mrp_dests(void *const *dst_polys, const uint64_t *base, std::size_t len) noexcept;

// Register the dst residues as an MRP output group (no-op for len == 1;
// the addr-derived name dedups re-registration of the same op).
std::expected<void, HazeInternalError> record_mrp_out_group(std::span<const DevAddr> addrs,
                                                            const uint64_t *base,
                                                            std::size_t len) noexcept;

// Derive an MRP group name from the leading residue addr
// (16-hex-encoded). Exposed for the kernel cache, which re-derives
// names when instantiating a memoized sub-tape against new addresses.
std::string mrp_group_name(std::string_view prefix, DevAddr leading_addr);

// Lowering-side helper for thunks: assemble the residues' materialized
// polynomials into an fhetch MRP (the from_pairs the eager build_mrp
// did at record time).
std::expected<niobium::fhetch::MRP, HazeInternalError>
build_lowered_mrp(const LowerCtx &ctx, const std::vector<ValueId> &vids,
                  const std::vector<uint64_t> &base);

// Per-residue fan-out backing hazeMemcpyMrp; signatures unchanged from
// the eager engine.
std::expected<void, HazeInternalError> copy_h2d_mrp(void *const *dst, const void *const *src,
                                                    std::size_t count, std::size_t len) noexcept;

std::expected<void, HazeInternalError> copy_to_host_mrp(void *const *dst, const void *const *src,
                                                        std::size_t count,
                                                        std::size_t len) noexcept;

std::expected<void, HazeInternalError> copy_device_to_device_mrp(void *const *dst,
                                                                 const void *const *src,
                                                                 const uint64_t *base,
                                                                 std::size_t len) noexcept;

// Build an MRS from per-modulus uint64_t scalars + their primes.
// Pure-data helper used INSIDE thunks at lowering time (fhetch Scalar
// construction must never happen at record time).
inline niobium::fhetch::MRS build_mrs(const uint64_t *scalars, const uint64_t *base,
                                      std::size_t len) {
    std::vector<std::pair<niobium::fhetch::Scalar, uint64_t>> pairs;
    pairs.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        pairs.emplace_back(niobium::fhetch::Scalar::from_int(scalars[i]), base[i]);
    }
    return niobium::fhetch::MRS::from_pairs(pairs);
}

} // namespace haze
