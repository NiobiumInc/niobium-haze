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
// What each pending output is decoded with (hazeSetOutputMetadata), by the name the program gives
// the output. EpochState writes it and clears it with the pending outputs; the replay bridge's
// post-recording hook reads it into the output's ciphertext template.
#pragma once

#include "common/errors.hpp"
#include "common/thread_safety.hpp"

#include <expected>
#include <haze/haze_types.h>
#include <optional>
#include <string>
#include <unordered_map>

namespace haze {

// A LEAF like InputSpillStore (lock order: common/thread_safety.hpp): its code never calls into
// epoch, allocator or backend.
class OutputMetadataStore {
  public:
    [[nodiscard]] std::expected<void, HazeInternalError>
    set(const std::string &name, const hazeOutputMetadata &metadata) noexcept HAZE_EXCLUDES(mutex_);
    [[nodiscard]] std::optional<hazeOutputMetadata> find(const std::string &name) const noexcept
        HAZE_EXCLUDES(mutex_);
    void clear() noexcept HAZE_EXCLUDES(mutex_);

  private:
    mutable HazeMutex mutex_;
    std::unordered_map<std::string, hazeOutputMetadata> by_name_ HAZE_GUARDED_BY(mutex_);
};

OutputMetadataStore &output_metadata() noexcept;

} // namespace haze
