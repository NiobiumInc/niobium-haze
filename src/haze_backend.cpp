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
// niobium::compiler() singleton. Today the linked library is
// niobium-compiler/libnbcc; future builds may swap to the niobium-fhetch
// dummy compiler when its bugs are sorted. HAZE source is unchanged
// either way — the backend only routes through niobium::compiler() which
// resolves at link time.

#include "haze_backend.hpp"

#include "haze_config.hpp"
#include "haze_errors.hpp"

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
        // synthesise minimal argv (the program name) so the compiler's
        // arg parser sees a well-formed invocation. prog_storage_ owns
        // the string for the duration of init.
        prog_storage_ = program_name;
        argv_[0] = prog_storage_.data();
        argv_[1] = nullptr;
        int argc = 1;
        niobium::compiler().init(argc, argv_);
        niobium::compiler().set_program_info(program_name, program_version, program_description);
        niobium::compiler().set_target(target);
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

bool CompilerBackend::stop_epoch() noexcept { return niobium::compiler().stop_epoch(); }

void CompilerBackend::reset() noexcept {
    std::lock_guard lock(init_mutex_);
    initialized_.store(false, std::memory_order_release);
    prog_storage_.clear();
    argv_[0] = nullptr;
    argv_[1] = nullptr;
}

} // namespace haze
