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
#include "core/config.hpp"

#include "common/errors.hpp"
#include "core/context.hpp"

#include <cstdint>
#include <haze/haze.h>
#include <haze/haze_types.h>

extern "C" hazeError_t hazeSetRingDimension(uint64_t n) noexcept {
    return set_internal_result(haze::set_ring_dimension(haze::default_context(), n));
}

extern "C" hazeError_t hazeSetCiphertextModulus(int index, uint64_t modulus) noexcept {
    return set_internal_result(haze::default_context().config.set_modulus(index, modulus));
}

extern "C" hazeError_t hazeSetTwiddleFactors(int index, uint64_t generator) noexcept {
    return set_internal_result(
        haze::default_context().config.set_twiddle_generator(index, generator));
}

extern "C" hazeError_t hazeConfigureDevice() noexcept {
    return set_internal_result(haze::default_context().config.configure_device());
}

extern "C" hazeError_t hazeSetProgramInfo(const char *name, const char *version,
                                          const char *description) noexcept {
    return set_internal_result(
        haze::default_context().config.set_program_info(name, version, description));
}

extern "C" hazeError_t hazeSetTarget(const char *target) noexcept {
    return set_internal_result(haze::default_context().config.set_target(target));
}

extern "C" hazeError_t hazeSetProgramDirectory(const char *dir) noexcept {
    return set_internal_result(haze::default_context().config.set_program_directory(dir));
}

extern "C" hazeError_t hazeSetMontgomery(int enable) noexcept {
    haze::default_context().config.set_montgomery(enable != 0);
    return set_error(HAZE_SUCCESS);
}

extern "C" hazeError_t hazeSetBitReversal(int enable) noexcept {
    haze::default_context().config.set_bit_reversal(enable != 0);
    return set_error(HAZE_SUCCESS);
}
