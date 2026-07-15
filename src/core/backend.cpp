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
//
// Control surface for the niobium::compiler() singleton; recording is
// local, replay is dispatched per the configured target. libnbfhetch treats
// "local" as the in-process simulator and any other string as an HTTP target
// for nbcc_fhetch_replay (haze keeps comparisons symbolic via kLocalTarget).

#include "core/backend.hpp"

#include "common/errors.hpp"
#include "common/thread_safety.hpp"
#include "core/config.hpp"

#include <atomic>
#include <expected>
#include <niobium/compiler.h>
#include <niobium/openfhe/probes.h>
#include <string>

namespace haze {

namespace {

// Bring niobium::compiler() up from the frozen ReplayConfig (target, program
// info, pinned directory, format flags), running the vendor init sequence once.
// ensure_initialized is the sole caller and owns the idempotence guard, the
// config-finalized precondition, and the montgomery/local compat check; this may
// throw (vendor init), and that caller contains it.
void bootstrap_compiler() {
    const ReplayConfig &rc = replay_config();
    const std::string &program_name = rc.program_name();
    const std::string &program_version = rc.program_version();
    const std::string &program_description = rc.program_description();

    // Synthesize a minimal argv — compiler().init() exposes no setters; it
    // copies argv during the call, so function-local storage is safe.
    std::string prog_storage = program_name;
    std::string target_arg_storage = "--target=" + rc.target();
    std::string montgomery_arg_storage = "--montgomery";
    std::string bitrev_arg_storage = "--bit_reversal";
    std::string no_ring_check_storage = "--no-ring-dim-check";
    std::string no_prime_check_storage = "--no-prime-check";
    // prog + --target + up to two optional format flags + the two always-on skip
    // flags (--no-ring-dim-check, --no-prime-check) + NULL terminator = 7 slots.
    char *argv[7] = {prog_storage.data(),
                     target_arg_storage.data(),
                     nullptr,
                     nullptr,
                     nullptr,
                     nullptr,
                     nullptr};
    int argc = 2;
    if (rc.montgomery())
        argv[argc++] = montgomery_arg_storage.data();
    if (rc.bit_reversal())
        argv[argc++] = bitrev_arg_storage.data();
    // Hardware ring-dim/prime compatibility is the compiler's job at
    // dispatch; the record/replay bridge always skips those checks.
    argv[argc++] = no_ring_check_storage.data();
    argv[argc++] = no_prime_check_storage.data();
    niobium::compiler().init(argc, argv);
    niobium::compiler().set_program_info(program_name, program_version, program_description);
    // A pinned output dir must be re-applied after set_program_info (which
    // resets it to cwd/<name>) and before the first trace write.
    if (rc.has_program_directory())
        niobium::compiler().set_program_directory(rc.program_directory());
}

} // namespace

// Both bring-up sites — first compute and the replay bridge pre-init — call this,
// so the vendor init runs exactly once per epoch (whichever arrives first); the
// other fast-paths on initialized_.
std::expected<void, HazeInternalError> CompilerBackend::ensure_initialized() noexcept {
    // Lock-free fast path for the common case where init has already
    // completed. The acquire load pairs with the release store below so
    // any reads after this point see the fully-initialized state.
    if (initialized_.load(std::memory_order_acquire))
        return {};

    // Slow path: serialize concurrent first callers so init runs once.
    HazeLockGuard lock(init_mutex_);
    if (initialized_.load(std::memory_order_relaxed))
        return {};

    // Configuration must be finalized by an explicit hazeConfigureDevice()
    // first; bring-up only READS the frozen config — it never finalizes, so the
    // compute path never mutates config state (hence needs no lock for it).
    if (!config_finalized()) {
        record_internal_error(HazeInternalError::NotConfigured,
                              "CompilerBackend::ensure_initialized: hazeConfigureDevice() "
                              "not called before first compute");
        return std::unexpected(HazeInternalError::NotConfigured);
    }

    // The local simulator runs ordinary-form traces only; reject the
    // Montgomery / bit-reversal toggles here so the error names the real cause.
    const ReplayConfig &rc = replay_config();
    if ((rc.montgomery() || rc.bit_reversal()) && rc.target_is_local()) {
        record_internal_error(HazeInternalError::UnsupportedDataFormat,
                              "CompilerBackend::ensure_initialized (montgomery/bit_reversal "
                              "require a transport target such as FUNC_SIM)");
        return std::unexpected(HazeInternalError::UnsupportedDataFormat);
    }

    // niobium::compiler() can throw (bad_alloc, config errors); catch here
    // so a thrown init becomes BackendInitFailed, not a process termination.
    try {
        bootstrap_compiler();
        // haze emits IR via fhetch::sr_* directly, so the OpenFHE-side CPROBE
        // capture path is dead weight; mute it globally. Distinct from
        // openfhe_cprobe_pause_recording, which would also silence sr_*.
        openfhe_suppress_probes(1);
    } catch (...) {
        record_internal_error(HazeInternalError::BackendInitFailed,
                              "CompilerBackend::ensure_initialized");
        return std::unexpected(HazeInternalError::BackendInitFailed);
    }

    initialized_.store(true, std::memory_order_release);
    return {};
}

bool CompilerBackend::is_initialized() const noexcept {
    return initialized_.load(std::memory_order_acquire);
}

bool CompilerBackend::start_epoch() noexcept {
    // Catch so a vendor throw cannot cross this noexcept frame.
    try {
        niobium::compiler().start_epoch();
        return true;
    } catch (...) {
        record_internal_error(HazeInternalError::BackendReplayFailed,
                              "CompilerBackend::start_epoch");
        return false;
    }
}

bool CompilerBackend::start_recording() noexcept {
    try {
        niobium::compiler().start();
        return true;
    } catch (...) {
        record_internal_error(HazeInternalError::BackendReplayFailed,
                              "CompilerBackend::start_recording");
        return false;
    }
}

bool CompilerBackend::stop_recording() noexcept {
    // Upstream stop() (not stop_epoch()) also writes fhetch_replay.json;
    // its filesystem I/O can throw, and this frame is reached through
    // noexcept hazeFlush.
    try {
        return niobium::compiler().stop();
    } catch (...) {
        record_internal_error(HazeInternalError::BackendReplayFailed,
                              "CompilerBackend::stop_recording");
        return false;
    }
}

void CompilerBackend::clear_captured() noexcept {
    try {
        niobium::compiler().clear_captured();
    } catch (...) {
        record_internal_error(HazeInternalError::BackendReplayFailed,
                              "CompilerBackend::clear_captured");
    }
}

void CompilerBackend::reset_compiler() noexcept {
    try {
        niobium::compiler().reset();
    } catch (...) {
        record_internal_error(HazeInternalError::BackendReplayFailed,
                              "CompilerBackend::reset_compiler");
    }
}

bool CompilerBackend::replay() noexcept {
    // replay() can throw on transport-route resource exhaustion (e.g. fork
    // failure); catch so the C ABI surfaces BackendReplayFailed cleanly.
    try {
        return niobium::compiler().replay();
    } catch (...) {
        record_internal_error(HazeInternalError::BackendReplayFailed, "CompilerBackend::replay");
        return false;
    }
}

void CompilerBackend::reset() noexcept {
    HazeLockGuard lock(init_mutex_);
    initialized_.store(false, std::memory_order_release);
}

} // namespace haze
