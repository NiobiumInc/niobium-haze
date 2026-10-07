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
#include "common/mod_arith.hpp"
#include "core/config.hpp"
#include "core/device.hpp"
#include "core/epoch.hpp"
#include "core/mrp_polymap.hpp"

#include <cstddef>
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

// OpenFHE's centering threshold floor(q/2) for an odd prime q.
uint64_t half_modulus(uint64_t q) noexcept {
    return (q - 1) / 2;
}

uint64_t add_mod(uint64_t a, uint64_t b, uint64_t m) noexcept {
    const uint64_t s = a + b;
    return (s >= m) ? s - m : s;
}

uint64_t prod_mod(const fhetch::ModuliBase &base, uint64_t m) noexcept {
    uint64_t r = 1 % m;
    for (const uint64_t q : base)
        r = mulmod_u64(r, q % m, m);
    return r;
}

// (Q / base[skip]) mod m, Q = prod(base).
uint64_t prod_mod_except(const fhetch::ModuliBase &base, std::size_t skip, uint64_t m) noexcept {
    uint64_t r = 1 % m;
    for (std::size_t i = 0; i < base.size(); ++i) {
        if (i != skip)
            r = mulmod_u64(r, base[i] % m, m);
    }
    return r;
}

// Pass-through residue: the copy-sentinel ADDI, bound to its real modulus as fhetch's
// copy_residue does so transport replay can probe-serialize it.
fhetch::Polynomial copy_residue(const fhetch::Polynomial &src, uint64_t p) {
    auto z = fhetch::sr_addps(src, fhetch::Scalar::from_int(0), kCopyModulus);
    fhetch::bind_modulus(z, p);
    return z;
}

// Centered (reduced-noise) FBC with the centering hoisted out of the per-term work:
// y_p = sum_i ((s_i + h_i) mod q_i) * c_ip + K_p (mod p), K_p = -sum_i h_i * c_ip mod p, the
// same residue as fhetch's per-term form with one shift per source and one ADDI per target.
fhetch::MRP fast_base_convert_centered(const fhetch::MRP &x,
                                       const fhetch::ModuliBase &target_base) {
    const fhetch::ModuliBase &source_base = x.base();
    std::vector<fhetch::Polynomial> shifted;
    shifted.reserve(source_base.size());
    for (std::size_t i = 0; i < source_base.size(); ++i) {
        const uint64_t q = source_base[i];
        const uint64_t q_hat_inv = modinv_prime(prod_mod_except(source_base, i, q), q);
        const auto scaled = fhetch::sr_mulps(x[q], fhetch::Scalar::from_int(q_hat_inv), q);
        shifted.push_back(fhetch::sr_addps(scaled, fhetch::Scalar::from_int(half_modulus(q)), q));
    }

    const std::unordered_set<uint64_t> source_set(source_base.begin(), source_base.end());
    std::vector<std::pair<fhetch::Polynomial, uint64_t>> pairs;
    pairs.reserve(target_base.size());
    for (const uint64_t p : target_base) {
        if (source_set.contains(p)) {
            pairs.emplace_back(copy_residue(x[p], p), p);
            continue;
        }
        uint64_t h_c_sum = 0;
        std::vector<fhetch::Polynomial> terms;
        terms.reserve(source_base.size());
        for (std::size_t i = 0; i < source_base.size(); ++i) {
            const uint64_t c = prod_mod_except(source_base, i, p);
            h_c_sum = add_mod(h_c_sum, mulmod_u64(half_modulus(source_base[i]) % p, c, p), p);
            terms.push_back(fhetch::sr_mulps(shifted[i], fhetch::Scalar::from_int(c), p));
        }
        fhetch::Polynomial acc = terms[0];
        for (std::size_t i = 1; i < terms.size(); ++i)
            acc = fhetch::sr_addp(acc, terms[i], p);
        const uint64_t k_p = (h_c_sum == 0) ? 0 : p - h_c_sum;
        pairs.emplace_back(fhetch::sr_addps(acc, fhetch::Scalar::from_int(k_p), p), p);
    }
    return fhetch::MRP::from_pairs(pairs);
}

// CKKS ApproxModDown over the factored centered FBC; same composition as fhetch::rescale_fbc.
fhetch::MRP rescale_centered(const fhetch::MRP &x, const fhetch::ModuliBase &rescale_base) {
    const std::unordered_set<uint64_t> rescale_set(rescale_base.begin(), rescale_base.end());
    fhetch::ModuliBase target_base;
    for (const uint64_t q : x.base()) {
        if (!rescale_set.contains(q))
            target_base.push_back(q);
    }
    const fhetch::MRP y =
        fast_base_convert_centered(fhetch::mr_subset(x, rescale_base), target_base);

    std::vector<std::pair<fhetch::Polynomial, uint64_t>> pairs;
    pairs.reserve(target_base.size());
    for (const uint64_t q : target_base) {
        const uint64_t p_inv = modinv_prime(prod_mod(rescale_base, q), q);
        const auto diff = fhetch::sr_subp(x[q], y[q], q);
        pairs.emplace_back(fhetch::sr_mulps(diff, fhetch::Scalar::from_int(p_inv), q), q);
    }
    return fhetch::MRP::from_pairs(pairs);
}

// FBC under the replay config: the factored centered form when reduced_noise is set, fhetch's
// Standard form otherwise.
fhetch::MRP lift(const fhetch::MRP &x, const fhetch::ModuliBase &target_base) {
    if (replay_config().reduced_noise())
        return fast_base_convert_centered(x, target_base);
    return fhetch::fast_base_convert(x, target_base, fhetch::FbcVariant::Standard);
}

fhetch::MRP rescale(const fhetch::MRP &x, const fhetch::ModuliBase &rescale_base) {
    if (replay_config().reduced_noise())
        return rescale_centered(x, rescale_base);
    return fhetch::rescale_fbc(x, rescale_base, fhetch::FbcVariant::Standard);
}

// Validation helpers; each returns InvalidArgument with a debug-log
// breadcrumb on the first failure, keeping the C ABI shim thin.

std::expected<void, HazeInternalError> validate(const hazeBasisConvertParams &p) noexcept {
    if (p.src_base == nullptr || p.src_base_len == 0 || p.dst_base == nullptr ||
        p.dst_base_len == 0) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "hazeBasisConvert: empty or null base");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    // Zero moduli divide the FBC math; duplicates alias residue keys.
    if (auto v = validate_moduli_base(p.src_base, p.src_base_len); !v)
        return v;
    return validate_moduli_base(p.dst_base, p.dst_base_len);
}

std::expected<void, HazeInternalError> validate(const hazeModDownParams &p) noexcept {
    if (p.src_base == nullptr || p.src_base_len == 0 || p.rescale_base == nullptr ||
        p.rescale_base_len == 0) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "hazeModDown: empty or null base");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    // Duplicate rescale primes strip only one unique src prime each — the
    // result would then overrun the caller's dst pointer array.
    if (auto v = validate_moduli_base(p.src_base, p.src_base_len); !v)
        return v;
    if (auto v = validate_moduli_base(p.rescale_base, p.rescale_base_len); !v)
        return v;
    // rescale_base must be a *proper* subset of src_base — equal-length
    // would leave dst empty (upstream fhetch asserts the same).
    if (p.rescale_base_len >= p.src_base_len) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "hazeModDown: rescale_base_len >= src_base_len");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    // Foreign-modulus check: every rescale_base prime must appear in src_base;
    // rejecting HAZE-side beats fhetch's assert (stripped in release).
    std::unordered_set<uint64_t> src_set(p.src_base, p.src_base + p.src_base_len);
    for (size_t j = 0; j < p.rescale_base_len; ++j) {
        if (!src_set.contains(p.rescale_base[j])) {
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
    if (auto v = validate_moduli_base(p.src_base, p.src_base_len); !v)
        return v;
    if (auto v = validate_moduli_base(p.p_base, p.p_base_len); !v)
        return v;
    // Digits lift to src_base ∪ p_base; overlap duplicates a residue key.
    std::unordered_set<uint64_t> src_set(p.src_base, p.src_base + p.src_base_len);
    for (size_t j = 0; j < p.p_base_len; ++j) {
        if (src_set.contains(p.p_base[j])) {
            record_internal_error(HazeInternalError::InvalidArgument,
                                  "hazeModUp: p_base and src_base share a prime");
            return std::unexpected(HazeInternalError::InvalidArgument);
        }
    }
    if (p.digit_count > static_cast<size_t>(kMaxCiphertextModuli)) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "hazeModUp: digit_count above device modulus envelope");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    // Per-digit lengths must sum to digit_bases_total_len, and every digit
    // prime must come from src_base (mr_subset throws on a foreign prime).
    size_t sum = 0;
    size_t offset = 0;
    for (size_t i = 0; i < p.digit_count; ++i) {
        const size_t dlen = p.digit_base_lens[i];
        sum += dlen;
        if (sum > p.digit_bases_total_len) {
            break; // mismatch reported below without reading past digit_bases
        }
        if (auto v = validate_moduli_base(p.digit_bases + offset, dlen); !v)
            return v;
        for (size_t j = 0; j < dlen; ++j) {
            if (!src_set.contains(p.digit_bases[offset + j])) {
                record_internal_error(HazeInternalError::InvalidArgument,
                                      "hazeModUp: digit base prime not in src_base");
                return std::unexpected(HazeInternalError::InvalidArgument);
            }
        }
        offset += dlen;
    }
    if (sum != p.digit_bases_total_len) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "hazeModUp: digit_base_lens do not sum to digit_bases_total_len");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    return {};
}

// hazeBasisConvert's recording, with the conversion itself supplied by the caller.
template <auto ConvertFn>
std::expected<void, HazeInternalError> convert_basis(void *const *dst, const void *const *src,
                                                     const hazeBasisConvertParams &p) noexcept {
    if (auto v = validate(p); !v) {
        return v;
    }

    EpochSession session;
    if (auto rec = epoch().require_recording_locked(); !rec)
        return rec;

    auto src_mrp = build_mrp_locked(src, p.src_base, p.src_base_len);
    if (!src_mrp) {
        return std::unexpected(src_mrp.error());
    }

    const fhetch::ModuliBase target_base(p.dst_base, p.dst_base + p.dst_base_len);
    fhetch::MRP result = ConvertFn(*src_mrp, target_base);
    return store_mrp_locked(dst, result, p.dst_base, p.dst_base_len);
}

} // namespace

std::expected<void, HazeInternalError> basis_convert(void *const *dst, const void *const *src,
                                                     const hazeBasisConvertParams &p) noexcept {
    // The configured lift (matching mod_down/mod_up), so a split keyswitch mod-down composes
    // byte-for-byte against hazeModDown under either variant.
    return convert_basis<lift>(dst, src, p);
}

std::expected<void, HazeInternalError>
basis_convert_centered(void *const *dst, const void *const *src,
                       const hazeBasisConvertParams &p) noexcept {
    return convert_basis<fast_base_convert_centered>(dst, src, p);
}

std::expected<void, HazeInternalError> mod_down(void *const *dst, const void *const *src,
                                                const hazeModDownParams &p) noexcept {
    if (auto v = validate(p); !v) {
        return v;
    }

    EpochSession session;
    if (auto rec = epoch().require_recording_locked(); !rec)
        return rec;

    auto src_mrp = build_mrp_locked(src, p.src_base, p.src_base_len);
    if (!src_mrp) {
        return std::unexpected(src_mrp.error());
    }

    const fhetch::ModuliBase rescale_base(p.rescale_base, p.rescale_base + p.rescale_base_len);
    fhetch::MRP result = rescale(*src_mrp, rescale_base);
    // result.base() == src_base \ rescale_base in original order; use it directly
    // so HAZE-side and backend-side agree on the dst layout.
    const auto &dst_base = result.base();
    return store_mrp_locked(dst, result, dst_base.data(), dst_base.size());
}

std::expected<void, HazeInternalError> mod_up(void *const *dst, const void *const *src,
                                              const hazeModUpParams &p) noexcept {
    if (auto v = validate(p); !v) {
        return v;
    }

    EpochSession session;
    if (auto rec = epoch().require_recording_locked(); !rec)
        return rec;

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

    // Open-code fhetch::dig_decomp, which hardcodes the per-term centered FBC, so each digit
    // takes the configured lift. The per-digit target is src_base + p_base (Q||P), as in
    // dig_decomp.
    const fhetch::MRP &x = *src_mrp;
    fhetch::ModuliBase target_base = x.base();
    target_base.insert(target_base.end(), p_base.begin(), p_base.end());
    for (size_t d = 0; d < p.digit_count; ++d) {
        const fhetch::MRP digit = lift(fhetch::mr_subset(x, digit_bases[d]), target_base);
        const auto &d_base = digit.base();
        auto stored =
            store_mrp_locked(dst + (d * d_base.size()), digit, d_base.data(), d_base.size());
        if (!stored)
            return stored;
    }
    return {};
}

} // namespace haze
