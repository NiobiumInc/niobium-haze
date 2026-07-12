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

#include <cstdint>
#include <haze/haze_types.h>

// Opaque-handle struct definitions. These match the forward-declared C
// handles in haze_types.h. Allocated via raw new/delete at the C ABI
// boundary per the documented opaque-handle pattern.
struct haze_stream_s {
    uint64_t id;
};

struct haze_event_s {
    uint64_t id;
};

namespace haze {

// Free functions instead of registry classes — state is just two
// counters, not enough invariants to justify a wrapper.

hazeStream_t stream_create() noexcept; // new haze_stream_s, caller owns
void stream_destroy(hazeStream_t s) noexcept;

hazeEvent_t event_create() noexcept;
void event_destroy(hazeEvent_t e) noexcept;
void event_record(hazeEvent_t e) noexcept;

} // namespace haze
