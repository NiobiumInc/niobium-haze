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
#pragma once

#include <haze/haze.h>

#include <cstdint>
#include <mutex>
#include <vector>

// ---------------------------------------------------------------------------
// Thread-local error state (defined in haze_error.cpp)
// ---------------------------------------------------------------------------

extern thread_local hazeError_t g_last_error;

[[nodiscard]] inline hazeError_t set_error(hazeError_t err) noexcept {
    g_last_error = err;
    return err;
}

// ---------------------------------------------------------------------------
// Memory manager accessors (defined in haze_memory.cpp)
// ---------------------------------------------------------------------------

std::mutex& haze_alloc_mutex() noexcept;
std::vector<uint8_t>* haze_shadow_buffer(uintptr_t dev_addr) noexcept;
bool haze_shadow_has_data(uintptr_t dev_addr) noexcept;
size_t haze_alloc_size(uintptr_t dev_addr) noexcept;
void haze_shadow_update(uintptr_t dev_addr, const std::vector<uint8_t>& data) noexcept;

// ---------------------------------------------------------------------------
// Stream accessor (defined in haze_stream.cpp)
// ---------------------------------------------------------------------------

hazeStream_t haze_default_stream() noexcept;

// ---------------------------------------------------------------------------
// Materialization hook (defined in haze_materialize.cpp, task 03).
// Returns HAZE_SUCCESS as a no-op stub until task 03 is implemented.
// ---------------------------------------------------------------------------

hazeError_t haze_materialize_d2h(uintptr_t dev_addr) noexcept;
