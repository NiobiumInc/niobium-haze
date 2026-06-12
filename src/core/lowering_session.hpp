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

#include "core/context_fwd.hpp"

namespace haze {

// The per-flush handle to the fhetch/compiler control surface — the
// SEAM for the planned libnbfhetch context API.
//
// Today libnbfhetch is a process singleton (niobium::compiler(), one
// TraceWriter, one address counter), so every flush binds the same
// global engine and concurrent flushes serialize behind lower.cpp's
// mutex. The planned fhetch change keeps that global as the default
// but adds an opt-out context object (own trace, own address counter,
// own captured-input registries, own simulator + program dir).
//
// This class is where that lands: ONE LoweringSession is constructed
// per finalize(), every control call during lowering goes through it,
// and LowerCtx exposes it to thunks. When fhetch contexts exist, the
// constructor acquires a context (or installs it as the thread's
// current emission scope), the members forward to it instead of the
// singleton, the serializing mutex becomes per-session — and nothing
// outside this class and finalize() changes. Keep new flush-time
// compiler/fhetch control calls HERE, never inline in lower.cpp.
class LoweringSession {
  public:
    // Binds the flush to the context being lowered: backend init reads
    // the context's Config, and the planned per-flush fhetch scrub will
    // re-derive program info / target / ring dim from it.
    explicit LoweringSession(Context &ctx) noexcept : ctx_(&ctx) {}
    LoweringSession(const LoweringSession &) = delete;
    LoweringSession &operator=(const LoweringSession &) = delete;

    // Scrub the process-global fhetch engine and rebuild it from the
    // flushing context's Config: compiler().reset() wipes every sticky
    // global (program info/dir, target, data-format flags, recording
    // registries, captured IO, the bridge hook), then init re-derives
    // all of it from ctx. The global is re-bound on EVERY flush, so no
    // context can observe another's leftovers through fhetch — the
    // closest haze can get to a per-flush engine until libnbfhetch
    // grows a real context object. False = leave the tape untouched
    // and report flush success, the documented failed-init behavior.
    [[nodiscard]] bool ensure_backend() noexcept;

    // Re-install the replay bridge's post-recording hook (dropped by
    // the scrub) from the bridge's stored state. Loud failure: a flush
    // that should synthesize MRP templates but silently can't would
    // corrupt downstream replay.
    [[nodiscard]] bool rebind_bridge() noexcept;

    // Open the recording this lowering emits into. start_epoch()
    // before start memorizes the polynomial-ID base so post-materialize
    // resets snap back to it.
    void begin_epoch() noexcept;

    // Finalize the recording and write the per-epoch .fhetch trace
    // (the replay_bridge post-recording hook runs inside).
    [[nodiscard]] bool write_trace() noexcept;

    // Drain the bridge hook's failure flag (valid once per write_trace).
    [[nodiscard]] bool bridge_hook_failed() noexcept;

    // Dispatch replay per the configured target.
    [[nodiscard]] bool replay() noexcept;

    // Drop captured input/output state so a failed materialize can't
    // leak into the next epoch. Safe to call on every exit path.
    void clear_captured() noexcept;

  private:
    Context *ctx_;
};

} // namespace haze
