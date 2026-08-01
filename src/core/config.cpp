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
#include "core/config.hpp"

#include "common/errors.hpp"
#include "common/mod_arith.hpp"
#include "core/device.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <haze/haze_types.h>

namespace haze {

namespace {

// Floor for the generated aux prime: the replay bridge's OpenFHE template
// synthesis rejects tiny trace moduli (LastPrime overflow below ~2^30).
constexpr uint64_t kAuxPrimeFloor = uint64_t{1} << 30U;

// Smallest prime >= kAuxPrimeFloor with p ≡ 1 (mod 2*ring_dim) — NTT-friendly
// for the hardware twiddle tables and readback synthesis — that is not one of
// the user's moduli. Deterministic given (ring_dim, moduli).
uint64_t generate_aux_prime(uint64_t ring_dim, const uint64_t *moduli, size_t count) noexcept {
    const uint64_t step = 2 * ring_dim;
    uint64_t candidate = (((kAuxPrimeFloor + step - 1) / step) * step) + 1;
    for (;; candidate += step) {
        if (!is_prime_u64(candidate))
            continue;
        bool collides = false;
        for (size_t i = 0; i < count; ++i) {
            if (moduli[i] == candidate) {
                collides = true;
                break;
            }
        }
        if (!collides)
            return candidate;
    }
}

// Power of two within the device envelope; the upper bound also keeps
// n * sizeof(uint64_t) from wrapping.
bool is_supported_ring_dim(uint64_t n) noexcept {
    if (n < (uint64_t{1} << kMinRingDimExponent) || n > (uint64_t{1} << kMaxRingDimExponent))
        return false;
    return (n & (n - 1)) == 0;
}

} // namespace

std::expected<FheParams, HazeInternalError> FheParams::create(const hazeFheParams &raw) noexcept {
    // Struct well-formedness: a non-zero count with a null array would be
    // dereferenced below.
    if ((raw.moduli_count > 0 && raw.moduli == nullptr) ||
        (raw.twiddle_count > 0 && raw.twiddle_generators == nullptr))
        return std::unexpected(HazeInternalError::InvalidArgument);
    // Per-argument: ring_dim in the device envelope; modulus count within it.
    if (!is_supported_ring_dim(raw.ring_dim))
        return std::unexpected(HazeInternalError::InvalidArgument);
    // Reserve the last chain slot for the generated aux prime.
    if (raw.moduli_count > static_cast<size_t>(kMaxCiphertextModuli) - 1)
        return std::unexpected(HazeInternalError::InvalidArgument);

    FheParams p;
    p.ring_dim_ = raw.ring_dim;
    for (size_t i = 0; i < raw.moduli_count; ++i) {
        const uint64_t qi = raw.moduli[i];
        if (qi == 0) // per-argument: moduli non-zero
            return std::unexpected(HazeInternalError::InvalidArgument);
        if (!is_prime_u64(qi)) { // per-argument: moduli prime
            record_internal_error(HazeInternalError::CompositeModulus,
                                  "FheParams::create: modulus not prime");
            return std::unexpected(HazeInternalError::CompositeModulus);
        }
        for (size_t j = 0; j < i; ++j) // whole-config: moduli unique
            if (raw.moduli[j] == qi) {
                record_internal_error(HazeInternalError::DuplicateModulus,
                                      "FheParams::create: duplicate modulus");
                return std::unexpected(HazeInternalError::DuplicateModulus);
            }
        p.moduli_[i] = qi;
    }
    // One program-wide aux prime for hazeIsHalfModulus, generated here and
    // appended as the last chain entry; compute calls never derive it.
    if (raw.moduli_count > 0) {
        p.aux_modulus_ = generate_aux_prime(raw.ring_dim, raw.moduli, raw.moduli_count);
        p.moduli_[raw.moduli_count] = p.aux_modulus_;
    }
    p.moduli_count_ = static_cast<int>(raw.moduli_count) + (raw.moduli_count > 0 ? 1 : 0);
    p.twiddle_generators_.assign(raw.twiddle_generators,
                                 raw.twiddle_generators + raw.twiddle_count);
    return p;
}

ReplayConfig ReplayConfig::create(const hazeReplayConfig *raw) noexcept {
    ReplayConfig rc; // defaults; a NULL struct or NULL field keeps them
    if (raw == nullptr)
        return rc;
    if (raw->target != nullptr)
        rc.target_ = raw->target;
    if (raw->program_name != nullptr)
        rc.program_name_ = raw->program_name;
    if (raw->program_version != nullptr)
        rc.program_version_ = raw->program_version;
    if (raw->program_description != nullptr)
        rc.program_description_ = raw->program_description;
    if (raw->program_directory != nullptr) {
        rc.program_dir_ = raw->program_directory;
        rc.program_dir_set_ = true;
    }
    rc.montgomery_ = raw->montgomery != 0;
    rc.bit_reversal_ = raw->bit_reversal != 0;
    rc.reduced_noise_ = raw->reduced_noise != 0;
    return rc;
}

} // namespace haze
