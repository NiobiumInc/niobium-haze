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
// CRT basis-conversion composites: hazeBasisConvert, hazeModDown,
// hazeModUp. Each entry validates its parameter struct, builds an MRP
// from the per-modulus device pointers, dispatches the matching FHETCH
// gadget (fast_base_convert / rescale_fbc / dig_decomp), and stores
// each output residue back into the polymap under its destination
// device pointer.

#include "haze_epoch.hpp"
#include "haze_errors.hpp"
#include "haze_handle.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <niobium/fhetch_api.h>
#include <utility>
#include <vector>

namespace fhetch = niobium::fhetch;

namespace {

// Resolve each src device pointer under the active epoch lock and pair
// it with its modulus, then build an MRP. Caller must hold the
// EpochSession lock. Returns the underlying HazeInternalError on the
// first lookup failure so the caller can map via to_public_error().
std::expected<fhetch::MRP, haze::detail::HazeInternalError>
build_mrp_locked(const void *const *polys, const uint64_t *base, size_t len) {
    using haze::detail::HazeInternalError;

    std::vector<std::pair<fhetch::Polynomial, uint64_t>> pairs;
    pairs.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        const auto addr = haze::detail::to_dev_addr(polys[i]);
        auto poly = haze::detail::epoch().lookup_or_create_locked(addr);
        if (!poly) {
            return std::unexpected(poly.error());
        }
        pairs.emplace_back(std::move(*poly), base[i]);
    }
    return fhetch::MRP::from_pairs(pairs);
}

// Store each residue of `mrp` at the matching dst pointer under the
// active epoch lock. Caller must hold the EpochSession lock. dst aliasing
// any src pointer is safe because all reads complete in build_mrp_locked
// before the first store.
void store_mrp_locked(void *const *dst_polys, const fhetch::MRP &mrp, const uint64_t *base,
                      size_t len) {
    for (size_t i = 0; i < len; ++i) {
        const auto addr = haze::detail::to_dev_addr(dst_polys[i]);
        haze::detail::epoch().store_locked(addr, mrp[base[i]]);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// hazeBasisConvert — fast base conversion
// ---------------------------------------------------------------------------

extern "C" hazeError_t hazeBasisConvert(void * /*dst*/, const void * /*src*/, const void *params,
                                        hazeStream_t /*stream*/) noexcept {
    if (params == nullptr) {
        return set_error(HAZE_ERROR_INVALID_VALUE);
    }
    const auto *p = static_cast<const hazeBasisConvertParams *>(params);
    if (p->src_polys == nullptr || p->src_base == nullptr || p->src_base_len == 0 ||
        p->dst_polys == nullptr || p->dst_base == nullptr || p->dst_base_len == 0) {
        return set_error(HAZE_ERROR_INVALID_VALUE);
    }

    haze::detail::EpochSession session;

    auto src_mrp = build_mrp_locked(p->src_polys, p->src_base, p->src_base_len);
    if (!src_mrp) {
        return set_error(haze::detail::to_public_error(src_mrp.error()));
    }

    const fhetch::ModuliBase target_base(p->dst_base, p->dst_base + p->dst_base_len);
    fhetch::MRP result = fhetch::fast_base_convert(*src_mrp, target_base);
    store_mrp_locked(p->dst_polys, result, p->dst_base, p->dst_base_len);
    return HAZE_SUCCESS;
}

// ---------------------------------------------------------------------------
// hazeModDown — rescale via fast base conversion
// ---------------------------------------------------------------------------

extern "C" hazeError_t hazeModDown(void * /*dst*/, const void * /*src*/, const void *params,
                                   hazeStream_t /*stream*/) noexcept {
    if (params == nullptr) {
        return set_error(HAZE_ERROR_INVALID_VALUE);
    }
    const auto *p = static_cast<const hazeModDownParams *>(params);
    if (p->src_polys == nullptr || p->src_base == nullptr || p->src_base_len == 0 ||
        p->dst_polys == nullptr || p->rescale_base == nullptr || p->rescale_base_len == 0 ||
        p->rescale_base_len > p->src_base_len) {
        return set_error(HAZE_ERROR_INVALID_VALUE);
    }

    // Compute dst_base = src_base \ rescale_base BEFORE opening an
    // EpochSession so a foreign modulus in rescale_base aborts without
    // dirtying the recording (no inputs tagged, no instructions emitted).
    const size_t dst_len = p->src_base_len - p->rescale_base_len;
    std::vector<uint64_t> dst_base;
    dst_base.reserve(dst_len);
    for (size_t i = 0; i < p->src_base_len; ++i) {
        bool removed = false;
        for (size_t j = 0; j < p->rescale_base_len; ++j) {
            if (p->src_base[i] == p->rescale_base[j]) {
                removed = true;
                break;
            }
        }
        if (!removed) {
            dst_base.push_back(p->src_base[i]);
        }
    }
    if (dst_base.size() != dst_len) {
        return set_error(HAZE_ERROR_INVALID_VALUE);
    }

    haze::detail::EpochSession session;

    auto src_mrp = build_mrp_locked(p->src_polys, p->src_base, p->src_base_len);
    if (!src_mrp) {
        return set_error(haze::detail::to_public_error(src_mrp.error()));
    }

    const fhetch::ModuliBase rescale_base(p->rescale_base, p->rescale_base + p->rescale_base_len);
    fhetch::MRP result = fhetch::rescale_fbc(*src_mrp, rescale_base);
    store_mrp_locked(p->dst_polys, result, dst_base.data(), dst_base.size());
    return HAZE_SUCCESS;
}

// ---------------------------------------------------------------------------
// hazeModUp — digit decomposition for hybrid key switching
// ---------------------------------------------------------------------------

extern "C" hazeError_t hazeModUp(void * /*dst*/, const void * /*src*/, const void *params,
                                 hazeStream_t /*stream*/) noexcept {
    if (params == nullptr) {
        return set_error(HAZE_ERROR_INVALID_VALUE);
    }
    const auto *p = static_cast<const hazeModUpParams *>(params);
    if (p->src_polys == nullptr || p->src_base == nullptr || p->src_base_len == 0 ||
        p->digit_bases == nullptr || p->digit_base_lens == nullptr || p->digit_count == 0 ||
        p->p_base == nullptr || p->p_base_len == 0 || p->dst_polys == nullptr) {
        return set_error(HAZE_ERROR_INVALID_VALUE);
    }

    haze::detail::EpochSession session;

    auto src_mrp = build_mrp_locked(p->src_polys, p->src_base, p->src_base_len);
    if (!src_mrp) {
        return set_error(haze::detail::to_public_error(src_mrp.error()));
    }

    // Unflatten digit_bases into a vector<ModuliBase>, one per digit.
    std::vector<fhetch::ModuliBase> digit_bases;
    digit_bases.reserve(p->digit_count);
    size_t offset = 0;
    for (size_t i = 0; i < p->digit_count; ++i) {
        const size_t dlen = p->digit_base_lens[i];
        digit_bases.emplace_back(p->digit_bases + offset, p->digit_bases + offset + dlen);
        offset += dlen;
    }

    const fhetch::ModuliBase p_base(p->p_base, p->p_base + p->p_base_len);
    fhetch::MRPArray result = fhetch::dig_decomp(*src_mrp, digit_bases, p_base);
    if (result.length() != p->digit_count) {
        haze::detail::record_internal_error(haze::detail::HazeInternalError::BackendError,
                                            "hazeModUp: dig_decomp returned wrong length");
        return set_error(
            haze::detail::to_public_error(haze::detail::HazeInternalError::BackendError));
    }

    // Output base for each digit is src_base ∪ p_base. Flatten dst_polys
    // in (src_base order, then p_base order) per digit.
    const size_t per_digit = p->src_base_len + p->p_base_len;
    std::vector<uint64_t> combined_base;
    combined_base.reserve(per_digit);
    combined_base.insert(combined_base.end(), p->src_base, p->src_base + p->src_base_len);
    combined_base.insert(combined_base.end(), p->p_base, p->p_base + p->p_base_len);

    for (size_t d = 0; d < p->digit_count; ++d) {
        store_mrp_locked(p->dst_polys + d * per_digit, result[d], combined_base.data(),
                         combined_base.size());
    }
    return HAZE_SUCCESS;
}
