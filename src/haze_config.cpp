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

#include <vector>

// ---------------------------------------------------------------------------
// Configuration state
// ---------------------------------------------------------------------------

struct NbConfig {
    uint64_t ring_dimension = 0;
    std::vector<uint64_t> moduli;
    std::vector<uint64_t> twiddle_generators;
    bool configured = false;
};

static NbConfig g_config;  // NOLINT

// TODO: add an explicit upper bound once hardware constraints are known.
static bool is_supported_ring_dim(uint64_t n) noexcept {
    return (n != 0) && ((n & (n - 1)) == 0);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

extern "C" hazeError_t hazeSetRingDimension(uint64_t n) noexcept {
    if (!is_supported_ring_dim(n)) return set_error(HAZE_ERROR_INVALID_VALUE);
    g_config.ring_dimension = n;
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeSetCiphertextModulus(int index,
                                                  uint64_t modulus) noexcept {
    if (index < 0) return set_error(HAZE_ERROR_INVALID_VALUE);
    if (modulus == 0) return set_error(HAZE_ERROR_INVALID_VALUE);
    if (static_cast<int>(g_config.moduli.size()) <= index) {
        g_config.moduli.resize(static_cast<size_t>(index) + 1, 0);
    }
    g_config.moduli[static_cast<size_t>(index)] = modulus;
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeSetTwiddleFactors(int index,
                                               uint64_t generator) noexcept {
    if (index < 0) return set_error(HAZE_ERROR_INVALID_VALUE);
    if (static_cast<int>(g_config.twiddle_generators.size()) <= index) {
        g_config.twiddle_generators.resize(static_cast<size_t>(index) + 1, 0);
    }
    g_config.twiddle_generators[static_cast<size_t>(index)] = generator;
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeConfigureDevice() noexcept {
    if (g_config.ring_dimension == 0) return set_error(HAZE_ERROR_INVALID_VALUE);
    g_config.configured = true;
    return HAZE_SUCCESS;
}

// ---------------------------------------------------------------------------
// Internal accessors (used by haze_compute.cpp, haze_materialize.cpp)
// ---------------------------------------------------------------------------

uint64_t haze_config_ring_dim() noexcept {
    return g_config.ring_dimension;
}

uint64_t haze_config_modulus(int index) noexcept {
    if (index < 0 || static_cast<size_t>(index) >= g_config.moduli.size())
        return 0;
    return g_config.moduli[static_cast<size_t>(index)];
}

bool haze_config_is_configured() noexcept {
    return g_config.configured;
}

std::vector<uint64_t> haze_config_moduli_copy() noexcept {
    return g_config.moduli;
}
