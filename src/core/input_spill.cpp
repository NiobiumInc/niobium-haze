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
#include "common/handle.hpp"
#include "common/thread_safety.hpp"

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
constexpr uint32_t kVersion = 2;
constexpr std::size_t kHeaderBytes = (sizeof(uint32_t) * 2) + sizeof(uint64_t);

// Shared file shape for both addr- and name-scoped records: magic, version, ring_dim,
// then ring_dim u64 values. Returns false (path already removed) on any write failure.
bool write_residue_file(const std::filesystem::path &path, const std::vector<uint64_t> &residue) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const uint32_t magic = kMagic;
    const uint32_t version = kVersion;
    const uint64_t ring_dim = residue.size();
    out.write(reinterpret_cast<const char *>(&magic), sizeof(magic));
    out.write(reinterpret_cast<const char *>(&version), sizeof(version));
    out.write(reinterpret_cast<const char *>(&ring_dim), sizeof(ring_dim));
    out.write(reinterpret_cast<const char *>(residue.data()),
              static_cast<std::streamsize>(ring_dim * sizeof(uint64_t)));
    out.flush();
    if (out) {
        return true;
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return false;
}

// Full read-back with header validation; `expected_ring_dim` is the caller's own
// bookkeeping (Record::ring_dim), checked against the file's own header for
// corruption detection.
bool read_residue_file(const std::filesystem::path &path, std::size_t expected_ring_dim,
                       std::vector<uint64_t> &out) {
    std::ifstream in(path, std::ios::binary);
    uint32_t magic = 0;
    uint32_t version = 0;
    uint64_t ring_dim = 0;
    in.read(reinterpret_cast<char *>(&magic), sizeof(magic));
    in.read(reinterpret_cast<char *>(&version), sizeof(version));
    in.read(reinterpret_cast<char *>(&ring_dim), sizeof(ring_dim));
    if (!in || magic != kMagic || version != kVersion || ring_dim != expected_ring_dim) {
        return false;
    }
    std::vector<uint64_t> residue(ring_dim);
    in.read(reinterpret_cast<char *>(residue.data()),
            static_cast<std::streamsize>(ring_dim * sizeof(uint64_t)));
    if (!in) {
        return false;
    }
    out = std::move(residue);
    return true;
}

} // namespace

std::string InputSpillStore::addr_filename(DevAddr addr) {
    std::ostringstream name;
    name << "addr_" << std::hex << to_uintptr(addr) << ".spill";
    return name.str();
}

std::string InputSpillStore::name_filename(const std::string &name, std::size_t index) {
    std::ostringstream file;
    file << "name_" << name << '_' << index << ".spill";
    return file.str();
}

std::filesystem::path InputSpillStore::record_path_locked(DevAddr addr) const {
    return dir_ / addr_filename(addr);
}

std::filesystem::path InputSpillStore::temp_record_path_locked(DevAddr addr) const {
    return dir_ / (addr_filename(addr) + ".tmp");
}

std::filesystem::path InputSpillStore::name_record_path_locked(const std::string &name,
                                                               std::size_t index) const {
    return dir_ / name_filename(name, index);
}

void InputSpillStore::remove_name_snapshots_locked(const std::string &name,
                                                   std::size_t count) const {
    for (std::size_t i = 0; i < count; ++i) {
        std::error_code ec;
        std::filesystem::remove(name_record_path_locked(name, i), ec);
    }
}

std::expected<void, HazeInternalError>
InputSpillStore::activate(std::filesystem::path dir) noexcept {
    try {
        HazeLockGuard lock(mutex_);
        // Records legitimately outlive one recording (address lifetime, not epoch
        // lifetime), so only a root CHANGE while active is refused.
        if (active_ && dir_ != dir) {
            record_internal_error(
                HazeInternalError::SpillIoFailed,
                "InputSpillStore::activate: re-activate with a different root while active");
            return std::unexpected(HazeInternalError::SpillIoFailed);
        }
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

bool InputSpillStore::has(DevAddr addr) const noexcept {
    HazeLockGuard lock(mutex_);
    return records_.contains(addr);
}

std::expected<void, HazeInternalError>
InputSpillStore::put(DevAddr addr, std::vector<uint64_t> &&residue) noexcept {
    std::filesystem::path tmp_path;
    try {
        // Consuming the parameter releases the caller's buffer even on validation-failure
        // returns, and the tidy rvalue-param rule requires the move.
        const std::vector<uint64_t> owned = std::move(residue);
        HazeLockGuard lock(mutex_);
        if (!active_) {
            record_internal_error(HazeInternalError::SpillIoFailed,
                                  "InputSpillStore::put: store not active");
            return std::unexpected(HazeInternalError::SpillIoFailed);
        }
        if (owned.empty()) {
            record_internal_error(HazeInternalError::SpillIoFailed,
                                  "InputSpillStore::put: empty residue");
            return std::unexpected(HazeInternalError::SpillIoFailed);
        }

        // Write to a scratch file, then rename it over the addr's path: the rename is
        // atomic and always plants a FRESH inode there, so an existing hardlinked
        // snapshot keeps referencing the bytes it had before this put.
        tmp_path = temp_record_path_locked(addr);
        if (!write_residue_file(tmp_path, owned)) {
            record_internal_error(HazeInternalError::SpillIoFailed,
                                  "InputSpillStore::put: write failed");
            return std::unexpected(HazeInternalError::SpillIoFailed);
        }
        std::error_code ec;
        std::filesystem::rename(tmp_path, record_path_locked(addr), ec);
        if (ec) {
            std::filesystem::remove(tmp_path, ec);
            record_internal_error(HazeInternalError::SpillIoFailed,
                                  "InputSpillStore::put: rename failed");
            return std::unexpected(HazeInternalError::SpillIoFailed);
        }
        records_.insert_or_assign(addr, Record{.ring_dim = owned.size()});
        return {};
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
        record_internal_error(HazeInternalError::SpillIoFailed, "InputSpillStore::put");
        return std::unexpected(HazeInternalError::SpillIoFailed);
    }
}

std::expected<void, HazeInternalError>
InputSpillStore::read(DevAddr addr, std::span<std::byte> dst) const noexcept {
    try {
        HazeLockGuard lock(mutex_);
        if (!active_) {
            record_internal_error(HazeInternalError::SpillIoFailed,
                                  "InputSpillStore::read: store not active");
            return std::unexpected(HazeInternalError::SpillIoFailed);
        }
        if (dst.empty()) {
            record_internal_error(HazeInternalError::SpillIoFailed,
                                  "InputSpillStore::read: empty destination span");
            return std::unexpected(HazeInternalError::SpillIoFailed);
        }
        auto it = records_.find(addr);
        if (it == records_.end()) {
            record_internal_error(HazeInternalError::SpillRecordMissing,
                                  "InputSpillStore::read: unknown addr");
            return std::unexpected(HazeInternalError::SpillRecordMissing);
        }
        const Record &rec = it->second;
        if (dst.size() > rec.ring_dim * sizeof(uint64_t)) {
            record_internal_error(HazeInternalError::SpillIoFailed,
                                  "InputSpillStore::read: size out of range");
            return std::unexpected(HazeInternalError::SpillIoFailed);
        }
        const std::filesystem::path path = record_path_locked(addr);
        std::ifstream in(path, std::ios::binary);
        in.seekg(static_cast<std::streamoff>(kHeaderBytes), std::ios::beg);
        in.read(reinterpret_cast<char *>(dst.data()), static_cast<std::streamsize>(dst.size()));
        if (!in) {
            record_internal_error(HazeInternalError::SpillIoFailed,
                                  "InputSpillStore::read: read failed");
            return std::unexpected(HazeInternalError::SpillIoFailed);
        }
        return {};
    } catch (...) {
        record_internal_error(HazeInternalError::SpillIoFailed, "InputSpillStore::read");
        return std::unexpected(HazeInternalError::SpillIoFailed);
    }
}

std::expected<void, HazeInternalError> InputSpillStore::erase(DevAddr addr) noexcept {
    try {
        HazeLockGuard lock(mutex_);
        if (!active_) {
            record_internal_error(HazeInternalError::SpillIoFailed,
                                  "InputSpillStore::erase: store not active");
            return std::unexpected(HazeInternalError::SpillIoFailed);
        }
        auto it = records_.find(addr);
        if (it == records_.end()) {
            // Callers erase only an addr they already confirmed has(addr); a miss here
            // means the store's bookkeeping disagrees with the caller's, which is a bug.
            record_internal_error(HazeInternalError::SpillRecordMissing,
                                  "InputSpillStore::erase: unknown addr");
            return std::unexpected(HazeInternalError::SpillRecordMissing);
        }
        const std::filesystem::path path = record_path_locked(addr);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        if (ec) {
            record_internal_error(HazeInternalError::SpillIoFailed,
                                  "InputSpillStore::erase: remove failed");
            return std::unexpected(HazeInternalError::SpillIoFailed);
        }
        records_.erase(it);
        return {};
    } catch (...) {
        record_internal_error(HazeInternalError::SpillIoFailed, "InputSpillStore::erase");
        return std::unexpected(HazeInternalError::SpillIoFailed);
    }
}

std::expected<void, HazeInternalError>
InputSpillStore::bind_name(const std::string &name, std::vector<DevAddr> &&addrs) noexcept {
    try {
        const std::vector<DevAddr> owned = std::move(addrs);
        HazeLockGuard lock(mutex_);
        if (!active_) {
            record_internal_error(HazeInternalError::SpillIoFailed,
                                  "InputSpillStore::bind_name: store not active");
            return std::unexpected(HazeInternalError::SpillIoFailed);
        }
        // Hardlink every addr's CURRENT file into a name-scoped path now: since put()
        // always renames a fresh inode over the addr path, a later put/erase touching
        // that addr leaves this hardlink pointing at the bytes it had at this moment. A
        // failure partway rolls back the links already made and drops the name's stale
        // named_records_ entry, so a rejected bind_name never leaves the map referencing
        // files the rollback just removed.
        std::vector<Record> snapshot_records;
        snapshot_records.reserve(owned.size());
        for (std::size_t i = 0; i < owned.size(); ++i) {
            auto it = records_.find(owned[i]);
            if (it == records_.end()) {
                remove_name_snapshots_locked(name, i);
                named_records_.erase(name);
                std::ostringstream body;
                body << "InputSpillStore::bind_name('" << name << "'): addr 0x" << std::hex
                     << to_uintptr(owned[i]) << " has no record to snapshot";
                record_internal_error(HazeInternalError::SpillIoFailed, body.str().c_str());
                return std::unexpected(HazeInternalError::SpillIoFailed);
            }
            const Record rec = it->second;
            const std::filesystem::path snapshot_path = name_record_path_locked(name, i);
            // A re-registration's snapshot path may already hold a link from a previous
            // bind_name call; clear it first so create_hard_link doesn't fail on an
            // existing path (a miss here is not an error -- nothing to clear).
            std::error_code remove_ec;
            std::filesystem::remove(snapshot_path, remove_ec);
            if (remove_ec) {
                remove_name_snapshots_locked(name, i);
                named_records_.erase(name);
                std::ostringstream body;
                body << "InputSpillStore::bind_name('" << name
                     << "'): could not clear existing snapshot at index " << i;
                record_internal_error(HazeInternalError::SpillIoFailed, body.str().c_str());
                return std::unexpected(HazeInternalError::SpillIoFailed);
            }
            std::error_code link_ec;
            std::filesystem::create_hard_link(record_path_locked(owned[i]), snapshot_path, link_ec);
            if (link_ec) {
                remove_name_snapshots_locked(name, i);
                named_records_.erase(name);
                std::ostringstream body;
                body << "InputSpillStore::bind_name('" << name << "'): hardlink failed for addr 0x"
                     << std::hex << to_uintptr(owned[i]);
                record_internal_error(HazeInternalError::SpillIoFailed, body.str().c_str());
                return std::unexpected(HazeInternalError::SpillIoFailed);
            }
            snapshot_records.push_back(rec);
        }
        // Overwrite: the loop above already refreshed indices [0, owned.size()); a
        // shorter re-registration must also drop the previous registration's TRAILING
        // indices, or they leak as orphaned files nothing references anymore.
        if (auto prev = named_records_.find(name);
            prev != named_records_.end() && prev->second.size() > owned.size()) {
            for (std::size_t i = owned.size(); i < prev->second.size(); ++i) {
                std::error_code ec;
                std::filesystem::remove(name_record_path_locked(name, i), ec);
            }
        }
        named_records_.insert_or_assign(name, std::move(snapshot_records));
        return {};
    } catch (...) {
        record_internal_error(HazeInternalError::SpillIoFailed, "InputSpillStore::bind_name");
        return std::unexpected(HazeInternalError::SpillIoFailed);
    }
}

std::expected<std::vector<std::vector<uint64_t>>, HazeInternalError>
InputSpillStore::take_named(const std::string &name) noexcept {
    try {
        HazeLockGuard lock(mutex_);
        if (!active_) {
            record_internal_error(HazeInternalError::SpillIoFailed,
                                  "InputSpillStore::take_named: store not active");
            return std::unexpected(HazeInternalError::SpillIoFailed);
        }
        auto it = named_records_.find(name);
        if (it == named_records_.end()) {
            std::ostringstream body;
            body << "InputSpillStore::take_named('" << name << "'): unknown name";
            record_internal_error(HazeInternalError::SpillRecordMissing, body.str().c_str());
            return std::unexpected(HazeInternalError::SpillRecordMissing);
        }
        const std::vector<Record> recs = it->second;
        std::vector<std::vector<uint64_t>> residues;
        residues.reserve(recs.size());
        for (std::size_t i = 0; i < recs.size(); ++i) {
            std::vector<uint64_t> residue;
            if (!read_residue_file(name_record_path_locked(name, i), recs[i].ring_dim, residue)) {
                std::ostringstream body;
                body << "InputSpillStore::take_named('" << name << "'): residue " << i
                     << " read failed";
                record_internal_error(HazeInternalError::SpillIoFailed, body.str().c_str());
                return std::unexpected(HazeInternalError::SpillIoFailed);
            }
            residues.push_back(std::move(residue));
        }
        for (std::size_t i = 0; i < recs.size(); ++i) {
            std::error_code ec;
            std::filesystem::remove(name_record_path_locked(name, i), ec);
            if (ec) {
                // A failed remove is an error, so the caller never receives data whose
                // on-disk snapshot we could not retire.
                std::ostringstream body;
                body << "InputSpillStore::take_named('" << name
                     << "'): remove failed for "
                        "residue "
                     << i;
                record_internal_error(HazeInternalError::SpillIoFailed, body.str().c_str());
                return std::unexpected(HazeInternalError::SpillIoFailed);
            }
        }
        named_records_.erase(it);
        return residues;
    } catch (...) {
        record_internal_error(HazeInternalError::SpillIoFailed, "InputSpillStore::take_named");
        return std::unexpected(HazeInternalError::SpillIoFailed);
    }
}

void InputSpillStore::clear_names() noexcept {
    HazeLockGuard lock(mutex_);
    for (const auto &[name, recs] : named_records_) {
        for (std::size_t i = 0; i < recs.size(); ++i) {
            std::error_code ec;
            std::filesystem::remove(name_record_path_locked(name, i), ec);
            if (ec) {
                record_internal_error(HazeInternalError::SpillIoFailed,
                                      "InputSpillStore::clear_names: remove snapshot failed");
            }
        }
    }
    named_records_.clear();
}

void InputSpillStore::clear() noexcept {
    HazeLockGuard lock(mutex_);
    // Snapshot and deactivate before touching the filesystem, so a mid-removal throw still
    // leaves the store empty and inactive.
    auto local_records = std::move(records_);
    auto local_named_records = std::move(named_records_);
    const std::filesystem::path local_dir = dir_;
    active_ = false;
    dir_.clear();
    try {
        // dir_ is already cleared above, so build paths from the local snapshots rather
        // than record_path_locked / name_record_path_locked.
        for (const auto &addr : std::views::keys(local_records)) {
            std::error_code ec;
            std::filesystem::remove(local_dir / addr_filename(addr), ec);
            if (ec) {
                record_internal_error(HazeInternalError::SpillIoFailed,
                                      "InputSpillStore::clear: remove record failed");
            }
        }
        for (const auto &[name, recs] : local_named_records) {
            for (std::size_t i = 0; i < recs.size(); ++i) {
                std::error_code ec;
                std::filesystem::remove(local_dir / name_filename(name, i), ec);
                if (ec) {
                    record_internal_error(HazeInternalError::SpillIoFailed,
                                          "InputSpillStore::clear: remove snapshot failed");
                }
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
