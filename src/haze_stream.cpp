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
#include "haze_internal.hpp"

#include <atomic>
#include <mutex>

// ---------------------------------------------------------------------------
// Opaque struct definitions (match forward-declared C handles)
// ---------------------------------------------------------------------------

struct haze_stream_s {
    uint64_t id;
    bool is_default;
};

struct haze_event_s {
    uint64_t id;
    bool recorded;
};

static std::atomic<uint64_t> g_next_stream_id{1};  // NOLINT
static std::atomic<uint64_t> g_next_event_id{1};   // NOLINT

// Default stream: created lazily, never destroyed.
static std::mutex g_default_stream_mutex;               // NOLINT
static haze_stream_s* g_default_stream = nullptr;       // NOLINT

static haze_stream_s* get_default_stream() noexcept {
    std::lock_guard lock(g_default_stream_mutex);
    if (!g_default_stream) {
        g_default_stream = new haze_stream_s{0, true};  // id=0 for default stream
    }
    return g_default_stream;
}

// ---------------------------------------------------------------------------
// Stream API
// ---------------------------------------------------------------------------

extern "C" hazeError_t hazeStreamCreate(hazeStream_t* stream) noexcept {
    // Mirrors CUDA's cudaStreamCreate contract: allocate a handle and write
    // it through the out-pointer on success. Fails with INVALID_VALUE on a
    // null out-pointer and OUT_OF_MEMORY if allocation throws.
    if (!stream) return set_error(HAZE_ERROR_INVALID_VALUE);
    try {
        *stream = new haze_stream_s{g_next_stream_id.fetch_add(1, std::memory_order_relaxed),
                                    false};
    } catch (...) {
        return set_error(HAZE_ERROR_OUT_OF_MEMORY);
    }
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeStreamCreateWithPriority(hazeStream_t* stream,
                                                      unsigned int /*flags*/,
                                                      int /*priority*/) noexcept {
    return hazeStreamCreate(stream);
}

extern "C" hazeError_t hazeStreamDestroy(hazeStream_t stream) noexcept {
    if (!stream) return HAZE_SUCCESS;
    if (stream->is_default) return HAZE_SUCCESS;  // never destroy the default stream
    delete stream;
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeStreamSynchronize(hazeStream_t /*stream*/) noexcept {
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeStreamWaitEvent(hazeStream_t /*stream*/,
                                             hazeEvent_t /*event*/,
                                             unsigned int /*flags*/) noexcept {
    return HAZE_SUCCESS;
}

// ---------------------------------------------------------------------------
// Event API
// ---------------------------------------------------------------------------

extern "C" hazeError_t hazeEventCreate(hazeEvent_t* event) noexcept {
    if (!event) return set_error(HAZE_ERROR_INVALID_VALUE);
    try {
        *event = new haze_event_s{g_next_event_id.fetch_add(1, std::memory_order_relaxed),
                                   false};
    } catch (...) {
        return set_error(HAZE_ERROR_OUT_OF_MEMORY);
    }
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeEventCreateWithFlags(hazeEvent_t* event,
                                                  unsigned int /*flags*/) noexcept {
    return hazeEventCreate(event);
}

extern "C" hazeError_t hazeEventDestroy(hazeEvent_t event) noexcept {
    delete event;
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeEventRecord(hazeEvent_t event,
                                        hazeStream_t /*stream*/) noexcept {
    if (event) event->recorded = true;
    return HAZE_SUCCESS;
}

// ---------------------------------------------------------------------------
// Internal accessor
// ---------------------------------------------------------------------------

hazeStream_t haze_default_stream() noexcept {
    return get_default_stream();
}
