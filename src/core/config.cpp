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
#include "core/device.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <haze/haze_types.h>

namespace haze {

namespace {

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
    if (raw.moduli_count > static_cast<size_t>(kMaxCiphertextModuli))
        return std::unexpected(HazeInternalError::InvalidArgument);

    FheParams p;
    p.ring_dim_ = raw.ring_dim;
    for (size_t i = 0; i < raw.moduli_count; ++i) {
        const uint64_t qi = raw.moduli[i];
        if (qi == 0) // per-argument: moduli non-zero
            return std::unexpected(HazeInternalError::InvalidArgument);
        for (size_t j = 0; j < i; ++j) // whole-config: moduli unique
            if (raw.moduli[j] == qi) {
                record_internal_error(HazeInternalError::DuplicateModulus,
                                      "FheParams::create: duplicate modulus");
                return std::unexpected(HazeInternalError::DuplicateModulus);
            }
        p.moduli_[i] = qi;
    }
    p.moduli_count_ = static_cast<int>(raw.moduli_count);
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
