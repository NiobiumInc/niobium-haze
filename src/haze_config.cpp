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
#include "haze_config.hpp"

#include "haze_allocator.hpp"

#include <haze/haze_types.h>

#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace haze::detail {

namespace {

bool is_supported_ring_dim(uint64_t n) noexcept {
    // TODO: add an explicit upper bound once hardware constraints are known.
    return (n != 0) && ((n & (n - 1)) == 0);
}

} // namespace

Config &Config::instance() noexcept {
    static Config inst;
    return inst;
}

hazeError_t Config::set_ring_dimension(uint64_t n) noexcept {
    if (!is_supported_ring_dim(n))
        return HAZE_ERROR_INVALID_VALUE;
    // Hold the Config lock across the allocator update so no observer
    // ever sees (new ring_dim, old pool poly_bytes) or vice versa. Lock
    // order is Config -> Allocator; the allocator never calls back into
    // Config, so this direction cannot deadlock.
    std::lock_guard lock(mutex_);
    ring_dim_ = n;
    DeviceAllocator::instance().set_polynomial_size(n * sizeof(uint64_t));
    return HAZE_SUCCESS;
}

hazeError_t Config::set_modulus(int idx, uint64_t modulus) noexcept {
    if (idx < 0)
        return HAZE_ERROR_INVALID_VALUE;
    if (modulus == 0)
        return HAZE_ERROR_INVALID_VALUE;
    std::lock_guard lock(mutex_);
    // Reject sparse writes — every index from 0 to idx-1 must already
    // have been set with a non-zero modulus. Without this constraint,
    // skipping an index leaves a 0 in the table which Config::modulus()
    // returns indistinguishably from "out of range," and the compute
    // templates would conflate the two failure modes.
    if (static_cast<int>(moduli_.size()) < idx)
        return HAZE_ERROR_INVALID_VALUE;
    if (static_cast<int>(moduli_.size()) == idx) {
        moduli_.push_back(modulus);
    } else {
        moduli_[static_cast<size_t>(idx)] = modulus;
    }
    return HAZE_SUCCESS;
}

hazeError_t Config::set_twiddle_generator(int idx, uint64_t generator) noexcept {
    if (idx < 0)
        return HAZE_ERROR_INVALID_VALUE;
    std::lock_guard lock(mutex_);
    if (static_cast<int>(twiddle_generators_.size()) <= idx) {
        twiddle_generators_.resize(static_cast<size_t>(idx) + 1, 0);
    }
    twiddle_generators_[static_cast<size_t>(idx)] = generator;
    return HAZE_SUCCESS;
}

hazeError_t Config::configure_device() noexcept {
    std::lock_guard lock(mutex_);
    if (ring_dim_ == 0)
        return HAZE_ERROR_INVALID_VALUE;
    configured_ = true;
    return HAZE_SUCCESS;
}

uint64_t Config::ring_dim() const noexcept {
    std::lock_guard lock(mutex_);
    return ring_dim_;
}

uint64_t Config::modulus(int idx) const noexcept {
    std::lock_guard lock(mutex_);
    if (idx < 0 || static_cast<size_t>(idx) >= moduli_.size())
        return 0;
    return moduli_[static_cast<size_t>(idx)];
}

bool Config::is_configured() const noexcept {
    std::lock_guard lock(mutex_);
    return configured_;
}

std::vector<uint64_t> Config::moduli_copy() const noexcept {
    std::lock_guard lock(mutex_);
    return moduli_;
}

hazeError_t Config::set_program_info(const char *name, const char *version,
                                     const char *description) noexcept {
    if (name == nullptr || version == nullptr || description == nullptr)
        return HAZE_ERROR_INVALID_VALUE;
    std::lock_guard lock(mutex_);
    program_name_ = name;
    program_version_ = version;
    program_description_ = description;
    program_info_set_ = true;
    return HAZE_SUCCESS;
}

hazeError_t Config::set_target(const char *target) noexcept {
    if (target == nullptr)
        return HAZE_ERROR_INVALID_VALUE;
    std::lock_guard lock(mutex_);
    target_ = target;
    target_set_ = true;
    return HAZE_SUCCESS;
}

std::string Config::program_name() const noexcept {
    std::lock_guard lock(mutex_);
    return program_info_set_ ? program_name_ : std::string{"haze"};
}

std::string Config::program_version() const noexcept {
    std::lock_guard lock(mutex_);
    return program_info_set_ ? program_version_ : std::string{"0.1"};
}

std::string Config::program_description() const noexcept {
    std::lock_guard lock(mutex_);
    return program_info_set_ ? program_description_ : std::string{"HAZE runtime"};
}

std::string Config::target() const noexcept {
    {
        std::lock_guard lock(mutex_);
        if (target_set_)
            return target_;
    }
    // Env var fallback used only when no explicit hazeSetTarget call has
    // been made. Default if env var unset: FHE_SIM.
    if (const char *t = std::getenv("HAZE_TARGET"); t != nullptr && t[0] != '\0')
        return std::string{t};
    return std::string{"FHE_SIM"};
}

void Config::reset() noexcept {
    std::lock_guard lock(mutex_);
    ring_dim_ = 0;
    moduli_.clear();
    twiddle_generators_.clear();
    configured_ = false;
    program_name_.clear();
    program_version_.clear();
    program_description_.clear();
    target_.clear();
    program_info_set_ = false;
    target_set_ = false;
}

} // namespace haze::detail
