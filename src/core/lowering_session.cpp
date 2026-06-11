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
#include "core/lowering_session.hpp"

#include "core/backend.hpp"

#include <haze/replay_bridge.h>
#include <niobium/compiler.h>

namespace haze {

// Every member forwards to the process-global engine today; this file
// is the only one that may name niobium::compiler() / CompilerBackend
// on the flush path (see the seam note in the header).
//
// The members are instance methods BY DESIGN even though nothing reads
// member state yet: they become forwards to the session's owned fhetch
// context, and the call sites must not churn when that lands.
// NOLINTBEGIN(readability-convert-member-functions-to-static)

bool LoweringSession::ensure_backend() noexcept {
    return backend().ensure_initialized();
}

void LoweringSession::begin_epoch() noexcept {
    niobium::compiler().start_epoch();
    CompilerBackend::start_recording();
}

bool LoweringSession::write_trace() noexcept {
    return CompilerBackend::stop_epoch();
}

bool LoweringSession::bridge_hook_failed() noexcept {
    return hazeReplayBridgeTakeHookHadError() != 0;
}

bool LoweringSession::replay() noexcept {
    return CompilerBackend::replay();
}

void LoweringSession::clear_captured() noexcept {
    niobium::compiler().clear_captured();
}
// NOLINTEND(readability-convert-member-functions-to-static)

} // namespace haze
