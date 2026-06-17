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
#include "core/graph.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <new>
#include <string>
#include <vector>

namespace haze {

namespace {

bool is_supported_ring_dim(uint64_t n) noexcept {
    return (n != 0) && ((n & (n - 1)) == 0);
}

} // namespace

bool env_flag(const char *name, bool fallback) noexcept {
    const char *v = std::getenv(name); // NOLINT(concurrency-mt-unsafe) — read-only lookup
    if (v == nullptr)
        return fallback;
    return (v[0] == '1' && v[1] == '\0') || std::string_view{v} == "true";
}

bool replay_isolated() noexcept {
    return env_flag("HAZE_REPLAY_ISOLATED", /*fallback=*/false);
}

std::expected<void, HazeInternalError> Config::init_params(uint64_t ring_dim,
                                                           const uint64_t *moduli, size_t n_moduli,
                                                           DeviceAllocator &alloc,
                                                           BindingTable &values,
                                                           BindingTable &recorded_moduli) noexcept {
    if (!is_supported_ring_dim(ring_dim))
        return std::unexpected(HazeInternalError::InvalidArgument);
    for (size_t i = 0; i < n_moduli; ++i)
        if (moduli[i] == 0)
            return std::unexpected(HazeInternalError::InvalidArgument);
    HazeLockGuard lock(mutex_);
    if (configured_ || ring_dim_ != 0)
        return std::unexpected(HazeInternalError::NotConfigured);
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory) — owned by snapshot_, freed by reset()
    const auto *snap = new (std::nothrow)
        ConfigSnapshot{.ring_dim = ring_dim, .moduli = {moduli, moduli + n_moduli}};
    if (snap == nullptr)
        return std::unexpected(HazeInternalError::InvalidArgument);
    ring_dim_ = ring_dim;
    moduli_.assign(moduli, moduli + n_moduli);
    configured_ = true;
    alloc.set_polynomial_size(ring_dim * sizeof(uint64_t));
    values.set_slot_bytes(ring_dim * sizeof(uint64_t));
    recorded_moduli.set_slot_bytes(ring_dim * sizeof(uint64_t));
    snapshot_.store(snap, std::memory_order_release);
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
    {
        HazeLockGuard lock(mutex_);
        if (target_set_)
            return target_;
    }
    // Env var fallback used only when no explicit hazeSetTarget call has
    // been made. Default if env var unset: kLocalTarget — runs the
    // in-process FHETCH simulator, so hazeMemcpy(D2H) returns
    // simulator-computed values out of the box without requiring
    // nbcc_fhetch_replay.
    if (const char *t = std::getenv("HAZE_TARGET"); t != nullptr && t[0] != '\0')
        return std::string{t};
    return std::string{kLocalTarget};
}

void Config::set_montgomery(bool enable) noexcept {
    HazeLockGuard lock(mutex_);
    montgomery_ = enable;
    montgomery_set_ = true;
}

void Config::set_bit_reversal(bool enable) noexcept {
    HazeLockGuard lock(mutex_);
    bit_reversal_ = enable;
    bit_reversal_set_ = true;
}

bool Config::montgomery() const noexcept {
    {
        HazeLockGuard lock(mutex_);
        if (montgomery_set_)
            return montgomery_;
    }
    // Env fallback mirrors target(): consulted only when no explicit setter
    // call has been made.
    return env_flag("HAZE_MONTGOMERY", false);
}

bool Config::bit_reversal() const noexcept {
    {
        HazeLockGuard lock(mutex_);
        if (bit_reversal_set_)
            return bit_reversal_;
    }
    return env_flag("HAZE_BIT_REVERSAL", false);
}

} // namespace haze
