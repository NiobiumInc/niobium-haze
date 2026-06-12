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
// reset_all() invokes them in dependency order (the tape first so any
// pending compute is dropped — thunks never run, nothing reaches fhetch
// — before the backend / allocator state is torn down).

#include "common/errors.hpp"
#include "common/handle.hpp"
#include "core/backend.hpp"
#include "core/context.hpp"
#include "core/device.hpp"
#include "core/record.hpp"
#include "core/stream.hpp"

#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <niobium/compiler.h>

namespace haze {

void reset_all(Context &ctx) noexcept {
    // Clear all internal state first, since we may depend on external object state when clearing
    // otherwise.
    ctx.kernels.reset(ctx); // open bracket + memo entries die first
    ctx.tape.reset();
    ctx.values.clear();
    ctx.recorded_moduli.clear();
    backend().reset();
    ctx.allocator.reset();
    ctx.config.reset(ctx.values);
    streams_reset();
    events_reset();
    device_reset();
    hazeReplayBridgeReset();

    // Clear any external state next
    niobium::compiler().reset();
}

} // namespace haze

extern "C" hazeError_t hazeDeviceReset(void) noexcept {
    haze::reset_all(haze::default_context());
    // Match cudaDeviceReset: also clear the thread-local last-error so
    // callers can use this as a clean test-isolation point.
    g_last_error = HAZE_SUCCESS;
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeWriteProgram(void) noexcept {
    return set_internal_result(haze::write_program(haze::default_context()));
}

extern "C" hazeError_t hazeTagOutput(void *ptr) noexcept {
    if (ptr == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return set_internal_result(haze::tag_output(haze::default_context(), haze::to_dev_addr(ptr)));
}

extern "C" hazeError_t hazeFlush(void) noexcept {
    return set_internal_result(haze::flush(haze::default_context()));
}
