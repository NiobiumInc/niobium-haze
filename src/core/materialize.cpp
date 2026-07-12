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
#include "common/errors.hpp"
#include "common/handle.hpp"
#include "core/allocator.hpp"
#include "core/backend.hpp"
#include "core/epoch.hpp"
#include "core/polynomial_io.hpp"

#include <cstdint>
#include <expected>
#include <haze/replay_bridge.h>
#include <ios>
#include <niobium/fhetch_api.h>
#include <sstream>
#include <utility>
#include <vector>

namespace haze {

namespace fhetch = niobium::fhetch;

std::expected<void, HazeInternalError> EpochState::materialize_epoch(bool run_replay) {
    // Never clears epoch state on any return path — finalize_locked is the sole
    // clearer, once, after this returns.
    // Step 1: write the trace; the replay-bridge post-recording hook runs
    // inside the vendor stop(), so drain its failure flag right after.
    if (!CompilerBackend::stop_recording()) {
        record_internal_error(HazeInternalError::BackendReplayFailed,
                              "materialize_epoch (stop_recording)");
        return std::unexpected(HazeInternalError::BackendReplayFailed);
    }
    if (hazeReplayBridgeTakeHookHadError() != 0) {
        record_internal_error(
            HazeInternalError::BridgeHookFailed,
            "post_recording_hook reported per-input/output failures (see prior log entries)");
        return std::unexpected(HazeInternalError::BridgeHookFailed);
    }

    // hazeWriteProgram() stops here: the program dir is complete and there
    // is no in-process result to read back.
    if (!run_replay) {
        return {};
    }

    // Step 2: dispatch replay per the configured target.
    if (!CompilerBackend::replay()) {
        record_internal_error(HazeInternalError::BackendReplayFailed, "materialize_epoch (replay)");
        return std::unexpected(HazeInternalError::BackendReplayFailed);
    }

    // Step 3: per-output shadow population; any failure aborts so a stale
    // shadow can't surface as a silent wrong-value D2H.
    for (const auto &[addr, name] : pending_outputs_) {
        fhetch::Polynomial result_poly;
        if (!fhetch::result(name, result_poly)) {
            std::ostringstream body;
            body << "result('" << name << "') unavailable for addr 0x" << std::hex
                 << to_uintptr(addr) << std::dec;
            record_internal_error(HazeInternalError::BackendOutputMissing, body.str().c_str());
            return std::unexpected(HazeInternalError::BackendOutputMissing);
        }
        std::vector<uint64_t> values;
        if (!decode_result_values(result_poly, values)) {
            std::ostringstream body;
            body << "failed to extract values for output '" << name << "' at addr 0x" << std::hex
                 << to_uintptr(addr) << std::dec;
            record_internal_error(HazeInternalError::BackendOutputDecodeFailed, body.str().c_str());
            return std::unexpected(HazeInternalError::BackendOutputDecodeFailed);
        }
        if (auto r = allocator().update_shadow(addr, std::move(values)); !r) {
            return std::unexpected(r.error());
        }
    }

    return {};
}

} // namespace haze
