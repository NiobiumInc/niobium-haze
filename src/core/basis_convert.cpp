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
#include "common/handle.hpp"
#include "common/thread_safety.hpp"
#include "core/epoch.hpp"

#include <cstdint>
#include <expected>
#include <haze/haze_types.h>
#include <niobium/fhetch_api.h>
#include <unordered_set>
#include <utility>
#include <vector>

namespace haze {

namespace fhetch = niobium::fhetch;

namespace {

// Build an MRP from per-modulus device pointers + their primes.
// Caller holds the EpochSession lock. Failures propagate the underlying
// HazeInternalError from lookup_or_create_locked (UnknownAddress / NoData
// / NotConfigured / AllocTooSmall).
std::expected<fhetch::MRP, HazeInternalError> build_mrp_locked(const void *const *polys,
                                                               const uint64_t *base, size_t len)
    HAZE_REQUIRES(epoch().mutex()) {
    std::vector<std::pair<fhetch::Polynomial, uint64_t>> pairs;
    pairs.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        auto poly = epoch().lookup_or_create_locked(to_dev_addr(polys[i]));
        if (!poly) {
            return std::unexpected(poly.error());
        }
        pairs.emplace_back(std::move(*poly), base[i]);
    }
    return fhetch::MRP::from_pairs(pairs);
}

// Store each residue of `mrp` at the matching dst pointer. The base
// argument names which moduli to read from the MRP (in dst_polys' order).
// Caller holds the EpochSession lock.
void store_mrp_locked(void *const *dst_polys, const fhetch::MRP &mrp, const uint64_t *base,
                      size_t len) HAZE_REQUIRES(epoch().mutex()) {
    for (size_t i = 0; i < len; ++i) {
        epoch().store_compute_result_locked(to_dev_addr(dst_polys[i]), mrp[base[i]]);
    }
}

// Validation helpers. Each returns InvalidArgument on the first failure
// and records a debug-log breadcrumb. Pre-flight checks live here so
// the C ABI shim stays a thin wrapper.

std::expected<void, HazeInternalError> validate(const hazeBasisConvertParams &p) noexcept {
    if (p.src_base == nullptr || p.src_base_len == 0 || p.dst_base == nullptr ||
        p.dst_base_len == 0) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "hazeBasisConvert: empty or null base");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    return {};
}

std::expected<void, HazeInternalError> validate(const hazeModDownParams &p) noexcept {
    if (p.src_base == nullptr || p.src_base_len == 0 || p.rescale_base == nullptr ||
        p.rescale_base_len == 0) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "hazeModDown: empty or null base");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    // rescale_base must be a *proper* subset of src_base — equal-length
    // would leave dst empty (FhetchApi.cpp:1660 asserts the same).
    if (p.rescale_base_len >= p.src_base_len) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "hazeModDown: rescale_base_len >= src_base_len");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    // Foreign-modulus check: every prime in rescale_base must appear in
    // src_base. Doing this HAZE-side rejects bad calls before fhetch's
    // assert (which strips in release).
    std::unordered_set<uint64_t> src_set(p.src_base, p.src_base + p.src_base_len);
    for (size_t j = 0; j < p.rescale_base_len; ++j) {
        if (src_set.count(p.rescale_base[j]) == 0) {
            record_internal_error(HazeInternalError::InvalidArgument,
                                  "hazeModDown: rescale_base not subset of src_base");
            return std::unexpected(HazeInternalError::InvalidArgument);
        }
    }
    return {};
}

std::expected<void, HazeInternalError> validate(const hazeModUpParams &p) noexcept {
    if (p.src_base == nullptr || p.src_base_len == 0 || p.digit_bases == nullptr ||
        p.digit_base_lens == nullptr || p.digit_count == 0 || p.p_base == nullptr ||
        p.p_base_len == 0) {
        record_internal_error(HazeInternalError::InvalidArgument, "hazeModUp: empty or null base");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    // digit_bases is a flat concatenation; the per-digit lengths must sum
    // to digit_bases_total_len. Catch caller miscounts before slicing
    // out-of-bounds.
    size_t sum = 0;
    for (size_t i = 0; i < p.digit_count; ++i) {
        sum += p.digit_base_lens[i];
    }
    if (sum != p.digit_bases_total_len) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "hazeModUp: digit_base_lens do not sum to digit_bases_total_len");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    return {};
}

} // namespace

std::expected<void, HazeInternalError> basis_convert(void *const *dst, const void *const *src,
                                                     const hazeBasisConvertParams &p) noexcept {
    if (auto v = validate(p); !v) {
        return v;
    }

    EpochSession session;

    auto src_mrp = build_mrp_locked(src, p.src_base, p.src_base_len);
    if (!src_mrp) {
        return std::unexpected(src_mrp.error());
    }

    const fhetch::ModuliBase target_base(p.dst_base, p.dst_base + p.dst_base_len);
    fhetch::MRP result = fhetch::fast_base_convert(*src_mrp, target_base);
    store_mrp_locked(dst, result, p.dst_base, p.dst_base_len);
    return {};
}

std::expected<void, HazeInternalError> mod_down(void *const *dst, const void *const *src,
                                                const hazeModDownParams &p) noexcept {
    if (auto v = validate(p); !v) {
        return v;
    }

    EpochSession session;

    auto src_mrp = build_mrp_locked(src, p.src_base, p.src_base_len);
    if (!src_mrp) {
        return std::unexpected(src_mrp.error());
    }

    const fhetch::ModuliBase rescale_base(p.rescale_base, p.rescale_base + p.rescale_base_len);
    fhetch::MRP result = fhetch::rescale_fbc(*src_mrp, rescale_base);
    // result.base() == src_base \ rescale_base in src_base's original order
    // (FhetchApi.cpp:1606-1617). Use it directly so HAZE-side and
    // backend-side never disagree on the dst layout.
    const auto &dst_base = result.base();
    store_mrp_locked(dst, result, dst_base.data(), dst_base.size());
    return {};
}

std::expected<void, HazeInternalError> mod_up(void *const *dst, const void *const *src,
                                              const hazeModUpParams &p) noexcept {
    if (auto v = validate(p); !v) {
        return v;
    }

    EpochSession session;

    auto src_mrp = build_mrp_locked(src, p.src_base, p.src_base_len);
    if (!src_mrp) {
        return std::unexpected(src_mrp.error());
    }

    std::vector<fhetch::ModuliBase> digit_bases;
    digit_bases.reserve(p.digit_count);
    size_t offset = 0;
    for (size_t i = 0; i < p.digit_count; ++i) {
        const size_t dlen = p.digit_base_lens[i];
        digit_bases.emplace_back(p.digit_bases + offset, p.digit_bases + offset + dlen);
        offset += dlen;
    }

    const fhetch::ModuliBase p_base(p.p_base, p.p_base + p.p_base_len);
    fhetch::MRPArray result = fhetch::dig_decomp(*src_mrp, digit_bases, p_base);
    if (result.length() != p.digit_count) {
        record_internal_error(HazeInternalError::BackendError,
                              "hazeModUp: dig_decomp returned wrong length");
        return std::unexpected(HazeInternalError::BackendError);
    }

    // Each result[d].base() == src_base + p_base (FhetchApi.cpp:1704-1705)
    // — same size and order across all digits. Use it directly to flatten
    // dst writes.
    for (size_t d = 0; d < p.digit_count; ++d) {
        const auto &d_base = result[d].base();
        store_mrp_locked(dst + d * d_base.size(), result[d], d_base.data(), d_base.size());
    }
    return {};
}

} // namespace haze
