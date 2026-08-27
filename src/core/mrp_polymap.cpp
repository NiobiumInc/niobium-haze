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

#include "core/mrp_polymap.hpp"

#include "common/errors.hpp"
#include "common/handle.hpp"
#include "core/allocator.hpp"
#include "core/device.hpp"
#include "core/epoch.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <niobium/fhetch_api.h>
#include <span>
#include <unordered_set>
#include <utility>
#include <vector>

namespace haze {

namespace fhetch = niobium::fhetch;

std::expected<void, HazeInternalError> validate_moduli_base(const uint64_t *base,
                                                            std::size_t len) noexcept {
    if (len == 0 || len > static_cast<std::size_t>(kMaxCiphertextModuli)) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "validate_moduli_base: base length outside [1, maxCiphertextModuli]");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    std::unordered_set<uint64_t> seen;
    seen.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        if (base[i] == 0) {
            record_internal_error(HazeInternalError::InvalidArgument,
                                  "validate_moduli_base: zero modulus");
            return std::unexpected(HazeInternalError::InvalidArgument);
        }
        if (!seen.insert(base[i]).second) {
            record_internal_error(HazeInternalError::InvalidArgument,
                                  "validate_moduli_base: duplicate modulus");
            return std::unexpected(HazeInternalError::InvalidArgument);
        }
    }
    return {};
}

std::expected<void, HazeInternalError> require_allocated_array(void *const *ptrs,
                                                               std::size_t len) noexcept {
    for (std::size_t i = 0; i < len; ++i) {
        if (auto live = allocator().require_allocated(to_dev_addr(ptrs[i])); !live)
            return live;
    }
    return {};
}

std::expected<fhetch::MRP, HazeInternalError>
build_mrp_locked(const void *const *polys, const uint64_t *base, std::size_t len) {
    std::vector<std::pair<fhetch::Polynomial, uint64_t>> pairs;
    pairs.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        DevAddr a = to_dev_addr(polys[i]);
        // A live-in with no declared modulus reached an MRP op through a plain
        // hazeMemcpy, which cannot say which prime the bytes hold. Inputs are
        // declared at upload now, so there is no later moment to recover it:
        // refuse instead of recording a ciphertext residue whose entry would
        // carry modulus 0. Computed sources are exempt: replay reproduces them.
        if (len > 1 && epoch().is_input_locked(a) &&
            epoch().recorded_modulus_locked(a) == kCopyModulus) {
            record_internal_error(HazeInternalError::InvalidArgument,
                                  "build_mrp_locked: MRP operand residue was uploaded without a "
                                  "declared modulus; upload ciphertext residues with "
                                  "hazeMemcpyMrp(HAZE_MEMCPY_HOST_TO_DEVICE, base, base_len)");
            return std::unexpected(HazeInternalError::InvalidArgument);
        }
        auto poly = epoch().lookup_or_create_locked(a);
        if (!poly) {
            return std::unexpected(poly.error());
        }
        pairs.emplace_back(std::move(*poly), base[i]);
    }
    // No input tagging here: hazeMemcpyMrp(H2D) already emitted the one
    // authoritative entry for every uploaded residue.
    return fhetch::MRP::from_pairs(pairs);
}

std::expected<void, HazeInternalError> store_mrp_locked(void *const *dst_polys,
                                                        const fhetch::MRP &mrp,
                                                        const uint64_t *base, std::size_t len) {
    // Guard where the writes happen: basis-convert paths derive their dst
    // layout from params, so the prelude checks don't cover them.
    if (auto live = require_allocated_array(dst_polys, len); !live)
        return live;
    std::vector<DevAddr> addrs;
    addrs.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        DevAddr a = to_dev_addr(dst_polys[i]);
        epoch().store_compute_result_locked(a, mrp[base[i]], base[i]);
        addrs.push_back(a);
    }
    if (len > 1) {
        auto group_name = epoch().mrp_group_name_locked(addrs.front());
        return epoch().record_mrp_group_locked(addrs, std::span(base, len), std::move(group_name));
    }
    return {};
}

std::expected<void, HazeInternalError> copy_h2d_mrp(void *const *dst, const void *const *src,
                                                    std::size_t count, const uint64_t *base,
                                                    std::size_t len) noexcept {
    // Same base contract as the D2D arm: the primes are recorded now, so a
    // malformed base would poison the input entry rather than one instruction.
    if (auto v = validate_moduli_base(base, len); !v)
        return v;
    if (count == 0)
        return {}; // zero-byte copy: success no-op, no shadow / tag side effects
    // Write every residue's shadow first, then tag the group under one session:
    // tagging promotes the just-written bytes to a single fhetch MRP input.
    for (std::size_t i = 0; i < len; ++i)
        if (auto h2d = allocator().copy_h2d(to_dev_addr(dst[i]), src[i], count); !h2d)
            return h2d;
    std::vector<DevAddr> addrs;
    addrs.reserve(len);
    for (std::size_t i = 0; i < len; ++i)
        addrs.push_back(to_dev_addr(dst[i]));
    EpochSession session;
    return epoch().tag_h2d_mrp_input_locked(addrs, std::span(base, len));
}

std::expected<void, HazeInternalError> copy_to_host_mrp(void *const *dst, const void *const *src,
                                                        std::size_t count,
                                                        std::size_t len) noexcept {
    if (len > static_cast<std::size_t>(kMaxCiphertextModuli)) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "copy_to_host_mrp: base_len bound");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    // Per-residue pure shadow read; the caller must have tagged the group and
    // flushed, or each residue errors as OutputNotFlushed.
    for (std::size_t i = 0; i < len; ++i)
        if (auto d2h = copy_to_host(dst[i], to_dev_addr(src[i]), count); !d2h)
            return d2h;
    return {};
}

std::expected<void, HazeInternalError>
copy_device_to_device_mrp(void *const *dst, const void *const *src, std::size_t count,
                          const uint64_t *base, std::size_t len) noexcept {
    // Same contract as SRP D2D: validated base, live dsts, whole-poly count.
    if (auto v = validate_moduli_base(base, len); !v)
        return v;
    if (auto live = require_allocated_array(dst, len); !live)
        return live;
    if (count == 0)
        return {}; // zero-byte copy: validated success no-op, nothing recorded
    const std::size_t poly_bytes = allocator().polynomial_size();
    if (count > poly_bytes) {
        record_internal_error(HazeInternalError::PolySizeMismatch, "copy_device_to_device_mrp");
        return std::unexpected(HazeInternalError::PolySizeMismatch);
    }
    if (count < poly_bytes) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "copy_device_to_device_mrp: partial D2D is not expressible in the "
                              "recorded IR (count must equal the polynomial size)");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    // Per-residue pass-through copy, then register the dst as an MRP output
    // group under the real base[i] so it reads back as an MRP, matching the
    // arithmetic MRP ops.
    EpochSession session;
    if (auto rec = epoch().require_recording_locked(); !rec)
        return rec;
    std::vector<DevAddr> addrs;
    addrs.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        DevAddr d = to_dev_addr(dst[i]);
        if (auto copied = epoch().copy_result_locked(d, to_dev_addr(src[i]), base[i]); !copied)
            return copied;
        addrs.push_back(d);
    }
    if (len > 1) {
        auto group_name = epoch().mrp_group_name_locked(addrs.front());
        return epoch().record_mrp_group_locked(addrs, std::span(base, len), std::move(group_name));
    }
    return {};
}

} // namespace haze
