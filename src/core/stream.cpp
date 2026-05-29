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
#include "core/stream.hpp"

#include <atomic>
#include <cstdint>
#include <haze/haze_types.h>
#include <new>

namespace haze {

namespace {
std::atomic<uint64_t> g_next_stream_id{1};
std::atomic<uint64_t> g_next_event_id{1};
} // namespace

hazeStream_t stream_create() noexcept {
    return new (std::nothrow)
        haze_stream_s{.id = g_next_stream_id.fetch_add(1, std::memory_order_relaxed)};
}

void stream_destroy(hazeStream_t s) noexcept {
    delete s; // delete nullptr is well-defined and a no-op
}

void streams_reset() noexcept {
    g_next_stream_id.store(1, std::memory_order_relaxed);
}

hazeEvent_t event_create() noexcept {
    return new (std::nothrow)
        haze_event_s{.id = g_next_event_id.fetch_add(1, std::memory_order_relaxed)};
}

void event_destroy(hazeEvent_t e) noexcept {
    delete e;
}

void event_record(hazeEvent_t /*e*/) noexcept {
    // Events do not model ordering in this runtime (CUDA-shape parity
    // only); recording is a no-op.
}

void events_reset() noexcept {
    g_next_event_id.store(1, std::memory_order_relaxed);
}

} // namespace haze
