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
#include <niobium/compiler.h>
#include <niobium/openfhe/probes.h>
#include <string>

namespace haze {

CompilerBackend &CompilerBackend::instance() noexcept {
    static CompilerBackend inst;
    return inst;
}

bool CompilerBackend::ensure_initialized() noexcept {
    // Lock-free fast path for the common case where init has already
    // completed. The acquire load pairs with the release store below so
    // any reads after this point see the fully-initialized state.
    if (initialized_.load(std::memory_order_acquire))
        return true;

    // Slow path: serialize concurrent first callers so init runs once.
    HazeLockGuard lock(init_mutex_);
    if (initialized_.load(std::memory_order_relaxed))
        return true;

    // The local simulator runs ordinary-form traces only; reject the
    // Montgomery / bit-reversal toggles here so the error names the real cause.
    const bool montgomery = config().montgomery();
    const bool bit_reversal = config().bit_reversal();
    if ((montgomery || bit_reversal) && config().target() == kLocalTarget) {
        record_internal_error(HazeInternalError::UnsupportedDataFormat,
                              "CompilerBackend::ensure_initialized (montgomery/bit_reversal "
                              "require a transport target such as FUNC_SIM)");
        return false;
    }

    // niobium::compiler() can throw (bad_alloc, config errors); catch here
    // so a thrown init becomes BackendInitFailed, not a process termination.
    try {
        const std::string program_name = config().program_name();
        const std::string program_version = config().program_version();
        const std::string program_description = config().program_description();
        const std::string target = config().target();

        // Synthesize a minimal argv to pass --target= (and the Montgomery /
        // bit-reversal flags) to compiler().init() — no setters are exposed.
        // init copies argv during the call, so function-local storage is safe.
        std::string prog_storage = program_name;
        std::string target_arg_storage = "--target=" + target;
        std::string montgomery_arg_storage = "--montgomery";
        std::string bitrev_arg_storage = "--bit_reversal";
        char *argv[5] = {prog_storage.data(), target_arg_storage.data(), nullptr, nullptr, nullptr};
        int argc = 2;
        if (montgomery)
            argv[argc++] = montgomery_arg_storage.data();
        if (bit_reversal)
            argv[argc++] = bitrev_arg_storage.data();
        niobium::compiler().init(argc, argv);
        niobium::compiler().set_program_info(program_name, program_version, program_description);
        // Optional explicit output dir: when set via hazeSetProgramDirectory,
        // the project (.fhetch + inputs + templates + cryptocontext) lands
        // here instead of cwd/<program_name>. Must precede the first compute
        // op so it's in effect when stop_epoch() writes the trace.
        if (config().has_program_directory())
            niobium::compiler().set_program_directory(config().program_directory());
        // haze emits IR via fhetch::sr_* directly, so the OpenFHE-side CPROBE
        // capture path is dead weight; mute it globally. Distinct from
        // openfhe_cprobe_pause_recording, which would also silence sr_*.
        openfhe_suppress_probes(1);
    } catch (...) {
        record_internal_error(HazeInternalError::BackendInitFailed,
                              "CompilerBackend::ensure_initialized");
        return false;
    }

    initialized_.store(true, std::memory_order_release);
    return true;
}

bool CompilerBackend::is_initialized() const noexcept {
    return initialized_.load(std::memory_order_acquire);
}

void CompilerBackend::start_recording() noexcept {
    niobium::compiler().start();
}

bool CompilerBackend::stop_epoch() noexcept {
    // Use stop() (not stop_epoch()): stop() writes both the .fhetch trace
    // and fhetch_replay.json that nbcc_fhetch_replay needs for HTTP dispatch.
    // Haze is single-epoch per epoch().reset(), so stop() also matches
    // semantically.
    return niobium::compiler().stop();
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
