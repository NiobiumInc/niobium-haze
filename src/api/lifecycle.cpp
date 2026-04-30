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
// Process-wide reset orchestration. Each subsystem owns its own reset();
// reset_all() invokes them in dependency order (epoch first so any
// pending compute is dropped before the backend / allocator state is
// torn down).

#include "core/allocator.hpp"
#include "core/backend.hpp"
#include "core/config.hpp"
#include "core/device.hpp"
#include "core/epoch.hpp"
#include "common/errors.hpp"
#include "core/stream.hpp"

#include <haze/haze.h>
#include <haze/haze_types.h>
#include <niobium/compiler.h>

namespace haze {

void reset_all() noexcept {
    epoch().reset();
    backend().reset();
    allocator().reset();
    config().reset();
    streams_reset();
    events_reset();
    device_reset();
    // Wipe niobium::compiler()'s singleton state too — captured_inputs,
    // captured_outputs, the trace writer's modulus table, and any hooks
    // registered by the replay bridge would otherwise leak across tests
    // (or across distinct programs running in the same process). reset()
    // is generic to libnbfhetch; haze just calls it as part of "start
    // fresh".
    niobium::compiler().reset();
}

} // namespace haze

extern "C" hazeError_t hazeDeviceReset(void) noexcept {
    haze::reset_all();
    // Match cudaDeviceReset: also clear the thread-local last-error so
    // callers can use this as a clean test-isolation point.
    g_last_error = HAZE_SUCCESS;
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeReplay(void) noexcept {
    auto result = haze::epoch().replay_and_populate();
    if (!result)
        return set_error(haze::to_public_error(result.error()));
    return HAZE_SUCCESS;
}
