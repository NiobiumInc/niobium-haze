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
#include "common/thread_safety.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace haze {

// The store owns the on-disk spill of tagged input residues so the recording holds
// metadata only. It is a lock-DAG leaf and must never call into epoch, allocator, or
// backend. Each record file is a header (u32 magic 0x485A5350, u32 version 1, u64
// residue_count, u64 ring_dim) followed by residue_count runs of ring_dim u64 values, all
// host-endian and meaningful only as process-local scratch inside the program dir -- never
// transported. Reclaim happens at take() or clear(); any residue surviving an abnormal exit
// belongs to the program dir's lifecycle, not this store's.
class InputSpillStore {
  public:
    // Directory is created here; the caller guarantees the store holds no records.
    [[nodiscard]] std::expected<void, HazeInternalError>
    activate(std::filesystem::path dir) noexcept HAZE_EXCLUDES(mutex_);

    [[nodiscard]] bool active() const noexcept HAZE_EXCLUDES(mutex_);

    // All residues must be the same non-zero length; the vector must be non-empty.
    [[nodiscard]] std::expected<void, HazeInternalError>
    put(const std::string &name, std::vector<std::vector<uint64_t>> &&residues) noexcept
        HAZE_EXCLUDES(mutex_);

    // Reads dst.size() bytes from the start of residue `residue_idx` of record `name`.
    [[nodiscard]] std::expected<void, HazeInternalError>
    read_residue(const std::string &name, std::size_t residue_idx,
                 std::span<std::byte> dst) const noexcept HAZE_EXCLUDES(mutex_);

    // Returns the whole record and erases it (file removed): the flush-time consumer.
    [[nodiscard]] std::expected<std::vector<std::vector<uint64_t>>, HazeInternalError>
    take(const std::string &name) noexcept HAZE_EXCLUDES(mutex_);

    // Removes every record file and the spill directory; deactivates. Error-path hygiene,
    // so failures are recorded, never propagated.
    void clear() noexcept HAZE_EXCLUDES(mutex_);

  private:
    struct Record {
        std::size_t residue_count;
        std::size_t ring_dim;
    };
    std::filesystem::path record_path_locked(const std::string &name) const HAZE_REQUIRES(mutex_);

    mutable HazeMutex mutex_;
    bool active_ HAZE_GUARDED_BY(mutex_) = false;
    std::filesystem::path dir_ HAZE_GUARDED_BY(mutex_);
    std::unordered_map<std::string, Record> records_ HAZE_GUARDED_BY(mutex_);
};

// Defined in device_state.cpp.
InputSpillStore &input_spill() noexcept;

} // namespace haze
