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
#include "core/input_spill.hpp"

#include "common/errors.hpp"
#include "common/thread_safety.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace haze {

namespace {

constexpr uint32_t kMagic = 0x485A5350;
constexpr uint32_t kVersion = 1;
constexpr std::size_t kHeaderBytes = (sizeof(uint32_t) * 2) + (sizeof(uint64_t) * 2);

} // namespace

std::filesystem::path InputSpillStore::record_path_locked(const std::string &name) const {
    return dir_ / (name + ".spill");
}

std::expected<void, HazeInternalError>
InputSpillStore::activate(std::filesystem::path dir) noexcept {
    try {
        HazeLockGuard lock(mutex_);
        // Caller contract: a new recording activates only after the previous epoch's clear().
        assert(records_.empty());
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            record_internal_error(HazeInternalError::SpillIoFailed,
                                  "InputSpillStore::activate: create_directories failed");
            return std::unexpected(HazeInternalError::SpillIoFailed);
        }
        dir_ = std::move(dir);
        active_ = true;
        return {};
    } catch (...) {
        record_internal_error(HazeInternalError::SpillIoFailed, "InputSpillStore::activate");
        return std::unexpected(HazeInternalError::SpillIoFailed);
    }
}

bool InputSpillStore::active() const noexcept {
    HazeLockGuard lock(mutex_);
    return active_;
}

std::expected<void, HazeInternalError>
InputSpillStore::put(const std::string &name,
                     std::vector<std::vector<uint64_t>> &&residues) noexcept {
    std::filesystem::path path;
    try {
        // Consuming the parameter releases the caller's buffer even on validation-failure
        // returns, and the tidy rvalue-param rule requires the move.
        const std::vector<std::vector<uint64_t>> owned = std::move(residues);
        HazeLockGuard lock(mutex_);
        if (!active_) {
            record_internal_error(HazeInternalError::SpillIoFailed,
                                  "InputSpillStore::put: store not active");
            return std::unexpected(HazeInternalError::SpillIoFailed);
        }
        if (owned.empty()) {
            std::ostringstream body;
            body << "InputSpillStore::put('" << name << "'): empty residue vector";
            record_internal_error(HazeInternalError::SpillIoFailed, body.str().c_str());
            return std::unexpected(HazeInternalError::SpillIoFailed);
        }
        const std::size_t ring_dim = owned.front().size();
        if (ring_dim == 0) {
            std::ostringstream body;
            body << "InputSpillStore::put('" << name << "'): zero-length residue";
            record_internal_error(HazeInternalError::SpillIoFailed, body.str().c_str());
            return std::unexpected(HazeInternalError::SpillIoFailed);
        }
        for (const auto &residue : owned) {
            if (residue.size() != ring_dim) {
                std::ostringstream body;
                body << "InputSpillStore::put('" << name << "'): residue length mismatch";
                record_internal_error(HazeInternalError::SpillIoFailed, body.str().c_str());
                return std::unexpected(HazeInternalError::SpillIoFailed);
            }
        }

        path = record_path_locked(name);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        const uint32_t magic = kMagic;
        const uint32_t version = kVersion;
        const uint64_t residue_count = owned.size();
        const uint64_t ring_dim_field = ring_dim;
        out.write(reinterpret_cast<const char *>(&magic), sizeof(magic));
        out.write(reinterpret_cast<const char *>(&version), sizeof(version));
        out.write(reinterpret_cast<const char *>(&residue_count), sizeof(residue_count));
        out.write(reinterpret_cast<const char *>(&ring_dim_field), sizeof(ring_dim_field));
        for (const auto &residue : owned) {
            out.write(reinterpret_cast<const char *>(residue.data()),
                      static_cast<std::streamsize>(ring_dim * sizeof(uint64_t)));
        }
        out.flush();
        if (!out) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
            std::ostringstream body;
            body << "InputSpillStore::put('" << name << "'): write failed";
            record_internal_error(HazeInternalError::SpillIoFailed, body.str().c_str());
            return std::unexpected(HazeInternalError::SpillIoFailed);
        }
        records_.insert_or_assign(name,
                                  Record{.residue_count = owned.size(), .ring_dim = ring_dim});
        return {};
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        record_internal_error(HazeInternalError::SpillIoFailed, "InputSpillStore::put");
        return std::unexpected(HazeInternalError::SpillIoFailed);
    }
}

std::expected<void, HazeInternalError>
InputSpillStore::read_residue(const std::string &name, std::size_t residue_idx,
                              std::span<std::byte> dst) const noexcept {
    try {
        HazeLockGuard lock(mutex_);
        if (!active_) {
            record_internal_error(HazeInternalError::SpillIoFailed,
                                  "InputSpillStore::read_residue: store not active");
            return std::unexpected(HazeInternalError::SpillIoFailed);
        }
        if (dst.empty()) {
            std::ostringstream body;
            body << "InputSpillStore::read_residue('" << name << "', residue " << residue_idx
                 << "): empty destination span";
            record_internal_error(HazeInternalError::SpillIoFailed, body.str().c_str());
            return std::unexpected(HazeInternalError::SpillIoFailed);
        }
        auto it = records_.find(name);
        if (it == records_.end()) {
            std::ostringstream body;
            body << "InputSpillStore::read_residue('" << name << "', residue " << residue_idx
                 << "): unknown record";
            record_internal_error(HazeInternalError::SpillRecordMissing, body.str().c_str());
            return std::unexpected(HazeInternalError::SpillRecordMissing);
        }
        const Record &rec = it->second;
        if (residue_idx >= rec.residue_count || dst.size() > rec.ring_dim * sizeof(uint64_t)) {
            std::ostringstream body;
            body << "InputSpillStore::read_residue('" << name << "', residue " << residue_idx
                 << "): index or size out of range";
            record_internal_error(HazeInternalError::SpillIoFailed, body.str().c_str());
            return std::unexpected(HazeInternalError::SpillIoFailed);
        }
        const std::filesystem::path path = record_path_locked(name);
        std::ifstream in(path, std::ios::binary);
        const auto offset = static_cast<std::streamoff>(
            kHeaderBytes + (residue_idx * rec.ring_dim * sizeof(uint64_t)));
        in.seekg(offset, std::ios::beg);
        in.read(reinterpret_cast<char *>(dst.data()), static_cast<std::streamsize>(dst.size()));
        if (!in) {
            std::ostringstream body;
            body << "InputSpillStore::read_residue('" << name << "', residue " << residue_idx
                 << "): read failed";
            record_internal_error(HazeInternalError::SpillIoFailed, body.str().c_str());
            return std::unexpected(HazeInternalError::SpillIoFailed);
        }
        return {};
    } catch (...) {
        record_internal_error(HazeInternalError::SpillIoFailed, "InputSpillStore::read_residue");
        return std::unexpected(HazeInternalError::SpillIoFailed);
    }
}

std::expected<std::vector<std::vector<uint64_t>>, HazeInternalError>
InputSpillStore::take(const std::string &name) noexcept {
    try {
        HazeLockGuard lock(mutex_);
        if (!active_) {
            record_internal_error(HazeInternalError::SpillIoFailed,
                                  "InputSpillStore::take: store not active");
            return std::unexpected(HazeInternalError::SpillIoFailed);
        }
        auto it = records_.find(name);
        if (it == records_.end()) {
            std::ostringstream body;
            body << "InputSpillStore::take('" << name << "'): unknown record";
            record_internal_error(HazeInternalError::SpillRecordMissing, body.str().c_str());
            return std::unexpected(HazeInternalError::SpillRecordMissing);
        }
        const Record rec = it->second;
        const std::filesystem::path path = record_path_locked(name);
        std::ifstream in(path, std::ios::binary);
        uint32_t magic = 0;
        uint32_t version = 0;
        uint64_t residue_count = 0;
        uint64_t ring_dim = 0;
        in.read(reinterpret_cast<char *>(&magic), sizeof(magic));
        in.read(reinterpret_cast<char *>(&version), sizeof(version));
        in.read(reinterpret_cast<char *>(&residue_count), sizeof(residue_count));
        in.read(reinterpret_cast<char *>(&ring_dim), sizeof(ring_dim));
        if (!in || magic != kMagic || version != kVersion || residue_count != rec.residue_count ||
            ring_dim != rec.ring_dim) {
            std::ostringstream body;
            body << "InputSpillStore::take('" << name << "'): header mismatch";
            record_internal_error(HazeInternalError::SpillIoFailed, body.str().c_str());
            return std::unexpected(HazeInternalError::SpillIoFailed);
        }
        std::vector<std::vector<uint64_t>> residues;
        residues.reserve(residue_count);
        for (uint64_t i = 0; i < residue_count; ++i) {
            std::vector<uint64_t> residue(ring_dim);
            in.read(reinterpret_cast<char *>(residue.data()),
                    static_cast<std::streamsize>(ring_dim * sizeof(uint64_t)));
            if (!in) {
                std::ostringstream body;
                body << "InputSpillStore::take('" << name << "'): residue read failed";
                record_internal_error(HazeInternalError::SpillIoFailed, body.str().c_str());
                return std::unexpected(HazeInternalError::SpillIoFailed);
            }
            residues.push_back(std::move(residue));
        }
        in.close();

        std::error_code ec;
        std::filesystem::remove(path, ec);
        if (ec) {
            // A failed remove is an error, so the caller never receives data whose
            // on-disk record we could not retire.
            std::ostringstream body;
            body << "InputSpillStore::take('" << name << "'): remove failed";
            record_internal_error(HazeInternalError::SpillIoFailed, body.str().c_str());
            return std::unexpected(HazeInternalError::SpillIoFailed);
        }
        records_.erase(it);
        return residues;
    } catch (...) {
        record_internal_error(HazeInternalError::SpillIoFailed, "InputSpillStore::take");
        return std::unexpected(HazeInternalError::SpillIoFailed);
    }
}

void InputSpillStore::clear() noexcept {
    HazeLockGuard lock(mutex_);
    // Snapshot and deactivate before touching the filesystem, so a mid-removal throw still
    // leaves the store empty and inactive.
    auto local_records = std::move(records_);
    const std::filesystem::path local_dir = dir_;
    active_ = false;
    dir_.clear();
    try {
        // dir_ is already cleared above, so build paths from the local snapshot rather than
        // record_path_locked.
        for (const auto &name : std::views::keys(local_records)) {
            std::error_code ec;
            std::filesystem::remove(local_dir / (name + ".spill"), ec);
            if (ec) {
                record_internal_error(HazeInternalError::SpillIoFailed,
                                      "InputSpillStore::clear: remove record failed");
            }
        }
        if (!local_dir.empty()) {
            std::error_code dir_ec;
            std::filesystem::remove(local_dir, dir_ec);
            if (dir_ec) {
                record_internal_error(HazeInternalError::SpillIoFailed,
                                      "InputSpillStore::clear: remove dir failed");
            }
        }
    } catch (...) {
        record_internal_error(HazeInternalError::SpillIoFailed, "InputSpillStore::clear");
    }
}

} // namespace haze
