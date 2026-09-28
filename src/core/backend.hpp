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

#include <atomic>
#include <expected>
#include <filesystem>

namespace haze {

// CompilerBackend wraps only the *control* operations (init, recording
// lifecycle, replay) of the niobium::compiler() singleton; IR (fhetch::sr_*,
// tag_input, result) routes to the linked compiler directly.
class DeviceState;

// Single concrete class, no virtual dispatch; the backend swaps at link time
// via whichever library supplies the niobium::compiler() symbol.
class CompilerBackend {
  public:
    // Idempotent, concurrency-safe compiler bring-up from the frozen replay
    // config: runs the vendor init exactly once (later callers fast-path on the
    // initialized_ flag). Both bring-up sites — first compute and the replay
    // bridge's pre-init — funnel through here, so init happens a single time
    // regardless of their order. Returns the failure reason so callers map it at
    // their boundary; the compute preludes also re-surface it via
    // require_recording_locked.
    [[nodiscard]] std::expected<void, HazeInternalError> ensure_initialized() noexcept
        HAZE_EXCLUDES(init_mutex_);

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
    // tests via hazeDeviceReset(). Returns the absolute, normalized program
    // directory this session's ensure_initialized() recorded at bring-up (see
    // recorded_program_directory_), or an empty path if it never completed
    // successfully. The caller (DeviceState::reset) uses this as the removal
    // target for hazeReplayConfig::remove_program_directory instead of a live
    // re-query, which would answer for whatever directory and working
    // directory are current NOW rather than what this session actually used.
    [[nodiscard]] std::filesystem::path reset() noexcept HAZE_EXCLUDES(init_mutex_);

    // The vendor compiler's active program directory, for callers (the spill store
    // activation site) that need it without including compiler.h themselves. Wraps
    // a vendor call that can throw; a caught throw becomes BackendInitFailed.
    [[nodiscard]] static std::expected<std::filesystem::path, HazeInternalError>
    program_directory() noexcept;

    CompilerBackend(const CompilerBackend &) = delete;
    CompilerBackend &operator=(const CompilerBackend &) = delete;

  private:
    friend class DeviceState;
    CompilerBackend() = default;

    // Atomic flag drives the lock-free fast path; init_mutex_ serializes the
    // first-call path so concurrent first callers don't all run init.
    std::atomic<bool> initialized_{false};
    HazeMutex init_mutex_;

    // Records which program directory THIS SESSION brought up: the exact
    // path config.cpp resolved and pinned into the vendor compiler at
    // configure time (see ensure_initialized(), which just copies it back
    // from the compiler -- no resolution of its own is needed there).
    // Whenever hazeReplayConfig::remove_program_directory was set,
    // ReplayConfig::create already resolved this with
    // std::filesystem::canonical() before bring-up -- absolute, with every
    // symlink in every component fully resolved and no lexical fallback --
    // so there is no working-directory-dependent resolution left for
    // anything downstream to get wrong. A live re-query
    // (niobium::compiler().get_program_directory(), wrapped by
    // program_directory() below) is not used here instead: it can throw
    // when queried before this session's own ensure_initialized() ever ran
    // (program_directory() itself catches that and returns
    // BackendInitFailed), whereas this member is a plain
    // std::filesystem::path -- always valid, empty until a bring-up
    // completes -- with nothing for reset()'s teardown path to catch.
    // reset() hands it back via std::exchange and clears it.
    std::filesystem::path recorded_program_directory_ HAZE_GUARDED_BY(init_mutex_);
};

// Defined in device_state.cpp (returns the DeviceState member).
CompilerBackend &backend() noexcept;

} // namespace haze
