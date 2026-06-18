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
#include "common/thread_safety.hpp"
#include "core/allocator.hpp"

#include <cstdint>
#include <cstdlib>
#include <expected>
#include <string>
#include <utility>
#include <vector>

namespace haze {

namespace {

bool is_supported_ring_dim(uint64_t n) noexcept {
    return (n != 0) && ((n & (n - 1)) == 0);
}

} // namespace

Config &Config::instance() noexcept {
    static Config inst;
    return inst;
}

std::expected<void, HazeInternalError> Config::set_ring_dimension(uint64_t n) noexcept {
    if (!is_supported_ring_dim(n))
        return std::unexpected(HazeInternalError::InvalidArgument);
    // Hold the Config lock across the allocator update so no observer
    // ever sees (new ring_dim, old pool poly_bytes) or vice versa. Lock
    // order is Config -> Allocator; the allocator never calls back into
    // Config, so this direction cannot deadlock.
    HazeLockGuard lock(mutex_);
    if (configured_ && ring_dim_ != n)
        return std::unexpected(HazeInternalError::NotConfigured);
    ring_dim_ = n;
    DeviceAllocator::instance().set_polynomial_size(n * sizeof(uint64_t));
    return {};
}

std::expected<void, HazeInternalError> Config::set_modulus(int idx, uint64_t modulus) noexcept {
    if (idx < 0)
        return std::unexpected(HazeInternalError::InvalidArgument);
    if (modulus == 0)
        return std::unexpected(HazeInternalError::InvalidArgument);
    HazeLockGuard lock(mutex_);
    if (configured_) {
        const auto i = static_cast<size_t>(idx);
        if (i >= moduli_.size() || moduli_[i] != modulus)
            return std::unexpected(HazeInternalError::NotConfigured);
        return {};
    }
    // Reject sparse writes — every index from 0 to idx-1 must already
    // have been set with a non-zero modulus. Without this constraint,
    // skipping an index leaves a 0 in the table which Config::modulus()
    // returns indistinguishably from "out of range," and the compute
    // templates would conflate the two failure modes.
    if (std::cmp_less(moduli_.size(), idx))
        return std::unexpected(HazeInternalError::InvalidArgument);
    if (std::cmp_equal(moduli_.size(), idx)) {
        moduli_.push_back(modulus);
    } else {
        moduli_[static_cast<size_t>(idx)] = modulus;
    }
    return {};
}

std::expected<void, HazeInternalError> Config::set_twiddle_generator(int idx,
                                                                     uint64_t generator) noexcept {
    if (idx < 0)
        return std::unexpected(HazeInternalError::InvalidArgument);
    HazeLockGuard lock(mutex_);
    if (configured_) {
        const auto i = static_cast<size_t>(idx);
        if (i >= twiddle_generators_.size() || twiddle_generators_[i] != generator)
            return std::unexpected(HazeInternalError::NotConfigured);
        return {};
    }
    if (std::cmp_less_equal(twiddle_generators_.size(), idx)) {
        twiddle_generators_.resize(static_cast<size_t>(idx) + 1, 0);
    }
    twiddle_generators_[static_cast<size_t>(idx)] = generator;
    return {};
}

std::expected<void, HazeInternalError> Config::configure_device() noexcept {
    HazeLockGuard lock(mutex_);
    if (ring_dim_ == 0)
        return std::unexpected(HazeInternalError::NotConfigured);
    configured_ = true;
    return {};
}

uint64_t Config::ring_dim() const noexcept {
    HazeLockGuard lock(mutex_);
    return ring_dim_;
}

uint64_t Config::modulus(int idx) const noexcept {
    HazeLockGuard lock(mutex_);
    if (idx < 0 || static_cast<size_t>(idx) >= moduli_.size())
        return 0;
    return moduli_[static_cast<size_t>(idx)];
}

std::expected<void, HazeInternalError>
Config::set_program_info(const char *name, const char *version, const char *description) noexcept {
    if (name == nullptr || version == nullptr || description == nullptr)
        return std::unexpected(HazeInternalError::InvalidArgument);
    HazeLockGuard lock(mutex_);
    program_name_ = name;
    program_version_ = version;
    program_description_ = description;
    program_info_set_ = true;
    return {};
}

std::expected<void, HazeInternalError> Config::set_target(const char *target) noexcept {
    if (target == nullptr)
        return std::unexpected(HazeInternalError::InvalidArgument);
    HazeLockGuard lock(mutex_);
    target_ = target;
    target_set_ = true;
    return {};
}

std::expected<void, HazeInternalError> Config::set_program_directory(const char *dir) noexcept {
    if (dir == nullptr)
        return std::unexpected(HazeInternalError::InvalidArgument);
    HazeLockGuard lock(mutex_);
    program_dir_ = dir;
    program_dir_set_ = true;
    return {};
}

bool Config::has_program_directory() const noexcept {
    HazeLockGuard lock(mutex_);
    return program_dir_set_;
}

std::string Config::program_directory() const noexcept {
    HazeLockGuard lock(mutex_);
    return program_dir_;
}

std::string Config::program_name() const noexcept {
    HazeLockGuard lock(mutex_);
    return program_info_set_ ? program_name_ : std::string{"haze"};
}

std::string Config::program_version() const noexcept {
    HazeLockGuard lock(mutex_);
    return program_info_set_ ? program_version_ : std::string{"0.1"};
}

std::string Config::program_description() const noexcept {
    HazeLockGuard lock(mutex_);
    return program_info_set_ ? program_description_ : std::string{"HAZE runtime"};
}

std::string Config::target() const noexcept {
    // Default kLocalTarget runs the in-process FHETCH simulator.
    HazeLockGuard lock(mutex_);
    return target_set_ ? target_ : std::string{kLocalTarget};
}

void Config::set_montgomery(bool enable) noexcept {
    HazeLockGuard lock(mutex_);
    montgomery_ = enable;
}

void Config::set_bit_reversal(bool enable) noexcept {
    HazeLockGuard lock(mutex_);
    bit_reversal_ = enable;
}

bool Config::montgomery() const noexcept {
    HazeLockGuard lock(mutex_);
    return montgomery_;
}

bool Config::bit_reversal() const noexcept {
    HazeLockGuard lock(mutex_);
    return bit_reversal_;
}

void Config::set_reduced_noise(bool enable) noexcept {
    HazeLockGuard lock(mutex_);
    reduced_noise_ = enable;
}

bool Config::reduced_noise() const noexcept {
    HazeLockGuard lock(mutex_);
    return reduced_noise_;
}

void Config::reset() noexcept {
    HazeLockGuard lock(mutex_);
    ring_dim_ = 0;
    moduli_.clear();
    twiddle_generators_.clear();
    configured_ = false;
    program_name_.clear();
    program_version_.clear();
    program_description_.clear();
    target_.clear();
    program_dir_.clear();
    program_info_set_ = false;
    target_set_ = false;
    program_dir_set_ = false;
    montgomery_ = false;
    bit_reversal_ = false;
    reduced_noise_ = false;
}

} // namespace haze
