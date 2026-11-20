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
#include "core/output_metadata.hpp"

#include "common/errors.hpp"
#include "common/thread_safety.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <expected>
#include <haze/haze_types.h>
#include <optional>
#include <string>

namespace haze {

namespace {

bool in_range(const hazeOutputMetadata &metadata, uint64_t ring_dim) noexcept {
    return std::isfinite(metadata.scaling_factor) && metadata.scaling_factor > 0.0 &&
           metadata.noise_scale_deg != 0 && std::has_single_bit(metadata.slots) &&
           metadata.slots <= ring_dim / 2;
}

} // namespace

std::expected<void, HazeInternalError> OutputMetadataStore::set(const std::string &name,
                                                                const hazeOutputMetadata &metadata,
                                                                uint64_t ring_dim) noexcept {
    if (!in_range(metadata, ring_dim)) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "set_output_metadata: a value outside hazeOutputMetadata's ranges");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    HazeLockGuard lock(mutex_);
    try {
        by_name_.insert_or_assign(name, metadata);
        return {};
    } catch (...) {
        record_internal_error(HazeInternalError::BackendReplayFailed, "OutputMetadataStore::set");
        return std::unexpected(HazeInternalError::BackendReplayFailed);
    }
}

std::optional<hazeOutputMetadata>
OutputMetadataStore::find(const std::string &name) const noexcept {
    HazeLockGuard lock(mutex_);
    if (auto it = by_name_.find(name); it != by_name_.end())
        return it->second;
    return std::nullopt;
}

void OutputMetadataStore::clear() noexcept {
    HazeLockGuard lock(mutex_);
    by_name_.clear();
}

} // namespace haze
