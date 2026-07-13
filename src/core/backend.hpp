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

// CompilerBackend wraps only the *control* operations (init, recording
// lifecycle, replay) of the niobium::compiler() singleton; IR (fhetch::sr_*,
// tag_input, result) routes to the linked compiler directly.
class DeviceState;

// Single concrete class, no virtual dispatch; the backend swaps at link time
// via whichever library supplies the niobium::compiler() symbol.
class CompilerBackend {
  public:
    // Idempotent, concurrency-safe compiler init from the replay config;
    // false on throw or unsupported config, and the compute preludes'
    // require_recording_locked gate then surfaces the failure at the ABI.
    [[nodiscard]] bool ensure_initialized() noexcept;

    // True iff ensure_initialized() has completed successfully.
    bool is_initialized() const noexcept;

    // Mark a new epoch (snapshots the polynomial-ID base); call before
    // start_recording.
    [[nodiscard]] static bool start_epoch() noexcept;

    // Begin a new recording (after init or after stop_recording).
    [[nodiscard]] static bool start_recording() noexcept;

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

    // Replay the most recently recorded epoch, dispatched per the configured
    // target (see haze.h's hazeConfigureDevice / hazeReplayConfig::target doc);
    // true on success.
    static bool replay() noexcept;

    // Drop cached state so the next ensure_initialized() starts fresh; mainly for
    // tests via hazeDeviceReset().
    void reset() noexcept;

    CompilerBackend(const CompilerBackend &) = delete;
    CompilerBackend &operator=(const CompilerBackend &) = delete;

  private:
    friend class DeviceState;
    CompilerBackend() = default;

    // Atomic flag drives the lock-free fast path; init_mutex_ serializes the
    // first-call path so concurrent first callers don't all run init.
    std::atomic<bool> initialized_{false};
    HazeMutex init_mutex_;
};

// One-shot vendor-compiler bootstrap (argv from Config, program info, pinned
// program dir) shared by first-compute bring-up and the replay bridge's pre-init.
// Format flags are explicit parameters because vendor init LATCHES them (set-only
// until reset), so callers pass a guard-approved snapshot; not idempotence-guarded
// and may throw, so callers contain both.
void bootstrap_compiler(bool montgomery, bool bit_reversal);

// Defined in device_state.cpp (returns the DeviceState member).
CompilerBackend &backend() noexcept;

} // namespace haze
