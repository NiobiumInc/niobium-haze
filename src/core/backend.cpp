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
// CompilerBackend is the link-time-selected control surface for the
// niobium::compiler() singleton. The linked library is
// niobium-fhetch/libnbfhetch; recording happens locally and replay is
// dispatched to a compiler-side nbcc_fhetch_replay over the FHETCH
// transport when target != "local".
// RYANPR: Where is local used/defined? It is mentioned here but not actually used in the compiler implementation.

#include "core/backend.hpp"

#include "core/config.hpp"
#include "common/errors.hpp"

#include <niobium/compiler.h>

#include <atomic>
#include <mutex>
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
    std::lock_guard lock(init_mutex_);
    if (initialized_.load(std::memory_order_relaxed))
        return true;

    // niobium::compiler() can throw (e.g. std::bad_alloc from internal
    // string handling, or compiler-side configuration errors). Catch
    // here so the C ABI boundary stays exception-clean: a thrown init
    // becomes a recorded BackendError, not a process termination.
    try {
        const std::string program_name = config().program_name();
        const std::string program_version = config().program_version();
        const std::string program_description = config().program_description();
        const std::string target = config().target();

        // niobium::compiler().init() takes (int& argc, char** argv) — we
        // synthesise minimal argv so the compiler's arg parser sees a
        // well-formed invocation. niobium-fhetch's compiler.h does not
        // expose a set_target() setter; the only way to configure the
        // replay target is through init()'s --target= flag (see
        // niobium-fhetch/src/compiler.cpp:153-156). prog_storage_ and
        // target_arg_storage_ own the strings for the duration of init.
        prog_storage_ = program_name;
        target_arg_storage_ = "--target=" + target;
        argv_[0] = prog_storage_.data();
        argv_[1] = target_arg_storage_.data();
        argv_[2] = nullptr;
        // RYANPR: Should this not be 3? You are setting 3 elements. Or two because the last argument is null
        int argc = 2;
        niobium::compiler().init(argc, argv_);
        niobium::compiler().set_program_info(program_name, program_version, program_description);
    } catch (...) {
        record_internal_error(HazeInternalError::BackendError,
                              "CompilerBackend::ensure_initialized");
        return false;
    }

    initialized_.store(true, std::memory_order_release);
    return true;
}

bool CompilerBackend::is_initialized() const noexcept {
    return initialized_.load(std::memory_order_acquire);
}

void CompilerBackend::start_recording() noexcept { niobium::compiler().start(); }

void CompilerBackend::start_epoch() noexcept { niobium::compiler().start_epoch(); }

bool CompilerBackend::stop_epoch() noexcept {
    // Use the canonical stop() rather than stop_epoch(). stop() writes
    // both the .fhetch trace and fhetch_replay.json (via write_replay_json
    // in compiler.cpp:209), which the compiler-side nbcc_fhetch_replay
    // requires to dispatch. stop_epoch() only writes the per-epoch trace
    // and does not produce replay.json — fine for libnbcc's intra-process
    // multi-epoch flow (its stop_epoch does record+replay+reset internally),
    // but breaks the transport hand-off used by libnbfhetch.
    //
    // Haze's recording lifecycle is single-epoch per haze::epoch().reset(),
    // so the canonical stop() is the right semantic match.
    return niobium::compiler().stop();
}

bool CompilerBackend::replay() noexcept {
    // niobium::compiler().replay() can throw on resource-exhaustion paths
    // inside the niobium-fhetch dispatch (e.g. std::system_error from a
    // failed subprocess fork on the transport route). Catching here keeps
    // the haze C ABI exception-clean and surfaces a coherent BackendError.
    try {
        return niobium::compiler().replay();
    } catch (...) {
        record_internal_error(HazeInternalError::BackendError,
                              "CompilerBackend::replay");
        return false;
    }
}

void CompilerBackend::reset() noexcept {
    std::lock_guard lock(init_mutex_);
    initialized_.store(false, std::memory_order_release);
    prog_storage_.clear();
    target_arg_storage_.clear();
    // RYANPR: Not sure why these are variables in the class when we set them in the `ensure_initialized` and then not use them again. The compiler init probably uses the argv and then copies what it needs and doesn't reference the caller's memory (if the init is well written that is).
    argv_[0] = nullptr;
    argv_[1] = nullptr;
    argv_[2] = nullptr;
}

} // namespace haze
