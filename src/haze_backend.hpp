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

#include <atomic>
#include <mutex>
#include <string>

namespace haze::detail {

// Control surface for the niobium::compiler() singleton. HAZE records
// FHETCH IR via fhetch::sr_*, fhetch::tag_input, and fhetch::result;
// those route to the linked compiler implicitly. CompilerBackend wraps
// only the *control* operations: init, recording lifecycle, replay.
//
// Single concrete class, no virtual dispatch. Backend swap (e.g. to the
// niobium-fhetch dummy compiler when stable) happens at link time: the
// implementation file (haze_backend.cpp) calls into whichever library
// supplies the niobium::compiler() symbol.
class CompilerBackend {
  public:
    static CompilerBackend &instance() noexcept;

    // Initialize the underlying compiler with program / target metadata
    // pulled from Config. Idempotent and safe to call concurrently;
    // first caller wins, others no-op once init completes. Returns true
    // on success — callers should propagate the error (returns false if
    // the underlying init throws, in which case the backend stays
    // unusable and subsequent compute calls fail at the recording-start
    // gate rather than crashing inside niobium).
    [[nodiscard]] bool ensure_initialized() noexcept;

    // True iff ensure_initialized() has completed successfully.
    bool is_initialized() const noexcept;

    // Begin a new recording (after init or after stop_epoch).
    void start_recording() noexcept;

    // Mark the start of a functional epoch — anchors poly-ID base on
    // first call, resets to that base on subsequent calls.
    void start_epoch() noexcept;

    // Compile + replay the current epoch, then reset compiler state and
    // resume recording. Returns true on success.
    bool stop_epoch() noexcept;

    // Drop cached state so the next call to ensure_initialized() starts
    // fresh. Mainly for tests via hazeReset().
    void reset() noexcept;

  private:
    CompilerBackend() = default;
    CompilerBackend(const CompilerBackend &) = delete;
    CompilerBackend &operator=(const CompilerBackend &) = delete;

    // Atomic flag enables a lock-free fast path on the hot
    // ensure_initialized check. init_mutex_ serializes the first-call
    // path so concurrent first callers don't all run init.
    std::atomic<bool> initialized_{false};
    std::mutex init_mutex_;
    std::string prog_storage_; // backs argv_[0] across the init() call
    char *argv_[2]{nullptr, nullptr};
};

inline CompilerBackend &backend() noexcept { return CompilerBackend::instance(); }

} // namespace haze::detail
