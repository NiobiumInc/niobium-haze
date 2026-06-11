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
#include "core/graph.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <haze/haze_types.h>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace haze {

// Kernel memoization (backs hazeKernelBegin / hazeKernelEnd).
//
// A RECORD bracket redirects the record path's node appends into a
// frame (Graph::install_frame_sink); End closes the frame, enforces the
// closed-body rule, caches the sub-tape under its key, and appends the
// body to the main tape. A REPLAY bracket skips the body entirely: End
// instantiates the cached sub-tape against the call's actual
// inputs/outputs — cloned nodes with eagerly-translated metadata and a
// shared vid_remap for the (immutable, reused) thunks.
//
// Lock order: cache mutex_ -> Graph append mutex (sink install / clone
// appends). The one-way edge is the single permitted haze lock nesting;
// Graph never calls back into the cache.
class KernelCache {
  public:
    static KernelCache &instance() noexcept;

    // True while a Begin bracket is open (nesting gate for the shim).
    bool has_open_frame() const noexcept HAZE_EXCLUDES(mutex_);

    std::expected<hazeKernelDisposition, HazeInternalError>
    begin(const char *name, uint64_t key_hash, std::span<const uint8_t> key_bytes,
          std::span<const hazeKernelInput> inputs) noexcept HAZE_EXCLUDES(mutex_);

    std::expected<void, HazeInternalError> end(std::span<const hazeKernelOutput> outputs) noexcept
        HAZE_EXCLUDES(mutex_);

    // Discard the open frame after a body error (no memo entry, no
    // nodes reach the main tape).
    std::expected<void, HazeInternalError> abort_frame() noexcept HAZE_EXCLUDES(mutex_);

    // Toggles; unset means "consult the env" (HAZE_KERNEL_MEMO,
    // HAZE_KERNEL_VALIDATE), matching HAZE_TARGET's resolution style.
    void set_memo_enabled(bool enabled) noexcept HAZE_EXCLUDES(mutex_);
    void set_validate(bool enabled) noexcept HAZE_EXCLUDES(mutex_);

    // hazeDeviceReset: drop the cache and any open bracket (the sink is
    // cleared by Graph::reset; call this first).
    void reset() noexcept HAZE_EXCLUDES(mutex_);

    KernelCache(const KernelCache &) = delete;
    KernelCache &operator=(const KernelCache &) = delete;

  private:
    KernelCache() = default;

    struct SubTape {
        std::string name;
        std::vector<uint8_t> key_bytes;
        std::vector<DevAddr> in_addrs; // flattened input residues at record time
        std::vector<ValueId> in_vids;
        std::vector<DevAddr> out_addrs; // flattened outputs bound at End
        std::vector<ValueId> out_vids;
        std::vector<Node> body; // recorded nodes (recording call's vids/addrs)
    };

    struct OpenFrame {
        std::string name;
        uint64_t key_hash = 0;
        std::vector<uint8_t> key_bytes;
        std::vector<DevAddr> in_addrs;
        std::vector<ValueId> in_vids;
        std::unique_ptr<std::vector<Node>> body;   // sink target (stable address)
        const SubTape *replay = nullptr;           // REPLAY: instantiate this at End
        const SubTape *validate_against = nullptr; // validation re-trace target
        bool passthrough = false;                  // memo disabled: tag-only End
    };

    bool memo_enabled_locked() const HAZE_REQUIRES(mutex_);
    bool validate_enabled_locked() const HAZE_REQUIRES(mutex_);
    const SubTape *find_locked(uint64_t key_hash, std::span<const uint8_t> key_bytes) const
        HAZE_REQUIRES(mutex_);
    std::expected<void, HazeInternalError>
    check_closed_body_locked(const OpenFrame &frame, std::span<const DevAddr> out_addrs) const
        HAZE_REQUIRES(mutex_);
    void instantiate_locked(const SubTape &sub, const OpenFrame &frame,
                            std::span<const DevAddr> out_addrs) HAZE_REQUIRES(mutex_);

    mutable HazeMutex mutex_;
    std::unordered_multimap<uint64_t, std::unique_ptr<SubTape>> cache_ HAZE_GUARDED_BY(mutex_);
    std::unique_ptr<OpenFrame> frame_ HAZE_GUARDED_BY(mutex_);
    // -1 = unset (env decides), else 0/1.
    int memo_override_ HAZE_GUARDED_BY(mutex_) = -1;
    int validate_override_ HAZE_GUARDED_BY(mutex_) = -1;
};

inline KernelCache &kernel_cache() noexcept {
    return KernelCache::instance();
}

} // namespace haze
