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

#include "common/thread_safety.hpp"

#include <atomic>

namespace haze {

// Control surface for the niobium::compiler() singleton. HAZE records
// FHETCH IR via fhetch::sr_*, fhetch::tag_input, and fhetch::result;
// those route to the linked fhetch "dummy" compiler implicitly. CompilerBackend wraps
// only the *control* operations: init, recording lifecycle, replay.
//
// Single concrete class, no virtual dispatch. Backend swap (e.g. to the
// niobium-fhetch dummy compiler when stable) happens at link time:
// backend.cpp calls into whichever library supplies the
// niobium::compiler() symbol.
class CompilerBackend {
  public:
    static CompilerBackend &instance() noexcept;

    // Idempotent, concurrency-safe compiler init from Config metadata;
    // false on throw or unsupported config, and the compute preludes'
    // require_recording_locked gate then surfaces the failure at the ABI.
    [[nodiscard]] bool ensure_initialized() noexcept;

    // True iff ensure_initialized() has completed successfully.
    bool is_initialized() const noexcept;

    // Mark a new epoch (snapshots the polynomial-ID base); call before
    // start_recording.
    static void start_epoch() noexcept;

    // Begin a new recording (after init or after stop_recording).
    static void start_recording() noexcept;

    // Finalize the recording via upstream stop() — writes the .fhetch
    // trace AND fhetch_replay.json (upstream stop_epoch() writes only the
    // trace); replay() must be invoked separately.
    static bool stop_recording() noexcept;

    // Drop the vendor-side captured input/output registries (mirrors
    // haze's per-epoch clear).
    static void clear_captured() noexcept;

    // Full vendor-compiler reset; hazeDeviceReset teardown only, and last
    // so live queries (bridge program-dir lookup) see pre-reset state.
    static void reset_compiler() noexcept;

    // Trigger replay of the most recently recorded epoch. Behaviour
    // depends on the configured target — see haze.h's hazeSetTarget
    // doc for the two-tier table (local in-process simulator vs HTTP
    // transport). Returns true on success.
    static bool replay() noexcept;

    // Drop cached state so the next call to ensure_initialized() starts
    // fresh. Mainly for tests via hazeDeviceReset().
    void reset() noexcept;

    CompilerBackend(const CompilerBackend &) = delete;
    CompilerBackend &operator=(const CompilerBackend &) = delete;

  private:
    CompilerBackend() = default;

    // Atomic flag enables a lock-free fast path on the hot
    // ensure_initialized check. init_mutex_ serializes the first-call
    // path so concurrent first callers don't all run init.
    std::atomic<bool> initialized_{false};
    HazeMutex init_mutex_;
};

inline CompilerBackend &backend() noexcept {
    return CompilerBackend::instance();
}

} // namespace haze
