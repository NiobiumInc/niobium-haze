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
#include "common/handle.hpp"
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

// The store replaces the RAM shadow for tagged inputs one-for-one on disk: each live
// input address's residue lives under its own "addr_<hex>.spill" file (header: u32
// magic 0x485A5350, u32 version 2, u64 ring_dim; then ring_dim u64 values, host-endian,
// process-local scratch -- never transported) for as long as the address itself is live
// with that value, mirroring hazeFree/overwrite/memset the same way the allocator's
// shadow map used to. put() writes a scratch file and renames it over the addr's path, so
// every put lands on a FRESH inode there.
//
// bind_name is a SNAPSHOT, not a copy: it hard-links the addr's CURRENT file into its own
// name-scoped path at call time, at zero bytes copied. Because put() always renames a
// fresh inode over the addr path, a name's hardlink keeps referencing the OLD inode after
// any later put/erase touches that addr, so a still-pending tag can never see bytes from
// a compute result, hazeFree, memset, or re-upload reusing that same address. take_named
// reads and erases only the name-scoped links; the addr-scoped records above are
// untouched by either call. A bind_name that fails partway -- including a rejected
// re-registration -- drops the name's named_records_ entry entirely and removes every
// snapshot file up to whichever is larger, this attempt's progress or the previous
// registration's length, rather than leaving either referenced by nothing. It is a
// lock-DAG leaf and must never call into epoch, allocator, or backend. Names passed to
// bind_name/take_named must be a single path component (no '/').
class InputSpillStore {
  public:
    // Directory is created here. Idempotent while already active with the SAME root
    // (records persist across recordings' activate calls); re-activating with a
    // DIFFERENT root while active is a hard error.
    [[nodiscard]] std::expected<void, HazeInternalError>
    activate(std::filesystem::path dir) noexcept HAZE_EXCLUDES(mutex_);

    // Membership: the D2H mode discriminator (deterministic, not a fallback probe).
    [[nodiscard]] bool has(DevAddr addr) const noexcept HAZE_EXCLUDES(mutex_);

    // One polynomial per address; overwrite allowed (a re-upload replaces the file).
    [[nodiscard]] std::expected<void, HazeInternalError>
    put(DevAddr addr, std::vector<uint64_t> &&residue) noexcept HAZE_EXCLUDES(mutex_);

    // Seekable partial read from the start of addr's residue; dst.size() bytes.
    [[nodiscard]] std::expected<void, HazeInternalError>
    read(DevAddr addr, std::span<std::byte> dst) const noexcept HAZE_EXCLUDES(mutex_);

    // Removes addr's file and record. Missing addr is a hard error: callers erase only
    // what they know is present (check has(addr) first for an addr that may be foreign).
    [[nodiscard]] std::expected<void, HazeInternalError> erase(DevAddr addr) noexcept
        HAZE_EXCLUDES(mutex_);

    // Per-recording manifest for the post-recording hook: hardlinks each addr's CURRENT
    // residue file into a name-scoped snapshot (overwrite allowed, zero bytes copied);
    // every addr must already have a record (SpillIoFailed otherwise -- callers always
    // put() first).
    [[nodiscard]] std::expected<void, HazeInternalError>
    bind_name(const std::string &name, std::vector<DevAddr> &&addrs) noexcept HAZE_EXCLUDES(mutex_);

    // Reads the name's snapshotted residues IN ORDER and erases them (the name-scoped
    // copies only -- addr-scoped records are untouched). Unknown name is
    // SpillRecordMissing. A read failure leaves the record intact and retriable; once every
    // read succeeds the record is retired unconditionally, even if deleting a snapshot file
    // then fails -- a retry sees SpillRecordMissing, never a half-deleted record.
    [[nodiscard]] std::expected<std::vector<std::vector<uint64_t>>, HazeInternalError>
    take_named(const std::string &name) noexcept HAZE_EXCLUDES(mutex_);

    // Drops every name's snapshotted copies, leaving addr records (and their bytes)
    // untouched.
    void clear_names() noexcept HAZE_EXCLUDES(mutex_);

    // Removes every record file (addr- and name-scoped), the spill directory, and every
    // manifest; deactivates. Device teardown only. Error-path hygiene: failures are
    // recorded, never propagated.
    void clear() noexcept HAZE_EXCLUDES(mutex_);

  private:
    struct Record {
        std::size_t ring_dim;
    };
    // Removes name's snapshot files [0, count) -- bind_name's rollback on a partial
    // failure, and reused by overwrite/clear paths that retire a name's snapshots.
    void remove_name_snapshots_locked(const std::string &name, std::size_t count) const
        HAZE_REQUIRES(mutex_);
    static std::string addr_filename(DevAddr addr);
    static std::string name_filename(const std::string &name, std::size_t index);
    std::filesystem::path record_path_locked(DevAddr addr) const HAZE_REQUIRES(mutex_);
    // Scratch path for put()'s write-then-rename: same dir, addr's filename plus ".tmp".
    std::filesystem::path temp_record_path_locked(DevAddr addr) const HAZE_REQUIRES(mutex_);
    std::filesystem::path name_record_path_locked(const std::string &name, std::size_t index) const
        HAZE_REQUIRES(mutex_);

    mutable HazeMutex mutex_;
    bool active_ HAZE_GUARDED_BY(mutex_) = false;
    std::filesystem::path dir_ HAZE_GUARDED_BY(mutex_);
    std::unordered_map<DevAddr, Record> records_ HAZE_GUARDED_BY(mutex_);
    // Name -> its snapshotted residues' Records, in bind_name's addrs order.
    std::unordered_map<std::string, std::vector<Record>> named_records_ HAZE_GUARDED_BY(mutex_);
};

// Defined in device_state.cpp.
InputSpillStore &input_spill() noexcept;

} // namespace haze
