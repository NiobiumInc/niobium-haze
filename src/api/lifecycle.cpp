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
// Process-wide reset orchestration. Engine state lives in hazeContext_t
// objects (destroyed individually); hazeDeviceReset clears only what is
// still process-global: the compiler backend, streams/events, the
// active device, the replay bridge, the fhetch engine, and the
// thread-local last-error flag.

#include "common/errors.hpp"
#include "common/handle.hpp"
#include "core/backend.hpp"
#include "core/context.hpp" // IWYU pragma: keep — ctx-> needs the complete type
#include "core/device.hpp"
#include "core/record.hpp"
#include "core/stream.hpp"

#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <niobium/compiler.h>

namespace haze {

void reset_process_globals() noexcept {
    backend().reset();
    streams_reset();
    events_reset();
    device_reset();
    hazeReplayBridgeReset();
    // External engine last: it may be queried by the bridge reset above.
    niobium::compiler().reset();
}

} // namespace haze

extern "C" hazeError_t hazeDeviceReset(void) noexcept {
    haze::reset_process_globals();
    // Match cudaDeviceReset: also clear the thread-local last-error so
    // callers can use this as a clean test-isolation point.
    g_last_error = HAZE_SUCCESS;
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeWriteProgram(hazeContext_t ctx) noexcept {
    if (ctx == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return set_internal_result(haze::write_program(*ctx));
}

extern "C" hazeError_t hazeTagOutput(hazeContext_t ctx, void *ptr) noexcept {
    if (ctx == nullptr || ptr == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return set_internal_result(haze::tag_output(*ctx, haze::to_dev_addr(ptr)));
}

extern "C" hazeError_t hazeFlush(hazeContext_t ctx) noexcept {
    if (ctx == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return set_internal_result(haze::flush(*ctx));
}
