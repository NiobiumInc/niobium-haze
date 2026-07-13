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

#include "common/errors.hpp"
#include "core/device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <haze/haze_types.h>
#include <string>
#include <string_view>
#include <vector>

namespace haze {

// Canonical target string dispatched through libnbfhetch's compiler:
// kLocalTarget runs the in-process FHETCH simulator; anything else is
// forwarded verbatim to nbcc_fhetch_replay.
constexpr std::string_view kLocalTarget = "local";

// Immutable FHE-scheme parameters: a thin owning wrapper over the caller's
// hazeFheParams, deep-copied and validated by create() (the sole non-empty
// construction path — no separate builder). Read lock-free during compute: it
// never changes after configure, and the backend/epoch acquire fence orders the
// copy before every read, so no lock or atomics are needed.
class FheParams {
  public:
    // Validate the caller's struct and deep-copy it into an immutable FheParams.
    // Enforces per-argument invariants (ring_dim in the device envelope; moduli
    // non-zero and within the modulus envelope), the whole-config invariant
    // (moduli unique), and struct well-formedness (a NULL array with a non-zero
    // count is InvalidArgument). Nothing partial escapes — on any failure it
    // returns the error and no object.
    static std::expected<FheParams, HazeInternalError> create(const hazeFheParams &raw) noexcept;

    FheParams() = default; // empty pre-configure placeholder (ring_dim 0 = unconfigured)

    uint64_t ring_dim() const noexcept { return ring_dim_; }
    // 0 means "no such configured slot" (outside the contiguous [0, count) range).
    uint64_t modulus(int idx) const noexcept {
        if (idx < 0 || idx >= moduli_count_)
            return 0;
        return moduli_[static_cast<size_t>(idx)];
    }

  private:
    uint64_t ring_dim_ = 0;
    std::array<uint64_t, static_cast<size_t>(kMaxCiphertextModuli)> moduli_{};
    int moduli_count_ = 0;
    std::vector<uint64_t> twiddle_generators_; // stored for the trace; no reader yet
};

// Immutable hardware/replay configuration (target + program metadata + the
// data-format toggles): a thin owning wrapper over the caller's hazeReplayConfig,
// copied by create(). Read at backend bring-up, replay dispatch, and compute.
class ReplayConfig {
  public:
    // Copy the caller's struct into an immutable config; a NULL field keeps its
    // default and a NULL struct yields all defaults. Infallible — montgomery/
    // bit-reversal on a local target is an execution-compatibility concern checked
    // at bring-up against the built config, not a config invariant.
    static ReplayConfig create(const hazeReplayConfig *raw) noexcept;

    ReplayConfig() = default; // defaults (target "local", program "haze"/"0.1"/...)

    const std::string &target() const noexcept { return target_; }
    bool target_is_local() const noexcept { return target_ == kLocalTarget; }
    const std::string &program_name() const noexcept { return program_name_; }
    const std::string &program_version() const noexcept { return program_version_; }
    const std::string &program_description() const noexcept { return program_description_; }
    bool has_program_directory() const noexcept { return program_dir_set_; }
    const std::string &program_directory() const noexcept { return program_dir_; }
    bool montgomery() const noexcept { return montgomery_; }
    bool bit_reversal() const noexcept { return bit_reversal_; }
    bool reduced_noise() const noexcept { return reduced_noise_; }

  private:
    std::string target_ = std::string(kLocalTarget);
    std::string program_name_ = "haze";
    std::string program_version_ = "0.1";
    std::string program_description_ = "HAZE runtime";
    std::string program_dir_;
    bool program_dir_set_ = false;
    bool montgomery_ = false;
    bool bit_reversal_ = false;
    bool reduced_noise_ = false;
};

// One-shot configuration entry point (impl in config.cpp): validate + deep-copy
// the caller's structs into the two immutable configs (FheParams::create /
// ReplayConfig::create) and install them into DeviceState. `replay` may be null
// (accept all defaults). Only the frozen configs are stored, and nothing is
// installed on failure. Called solely by hazeConfigureDevice.
std::expected<void, HazeInternalError> configure_device(const hazeFheParams &fhe,
                                                        const hazeReplayConfig *replay) noexcept;

// Read side (defined in device_state.cpp): the frozen configs — plain
// always-valid members (default until configured), so reads never fault.
// Meaningful once config_finalized() is true; callers gate on it.
const FheParams &fhe_params() noexcept;
const ReplayConfig &replay_config() noexcept;
// True once hazeConfigureDevice has installed the configs. Compute/bring-up/
// bridge paths REQUIRE this; they only read the frozen config and never
// configure themselves, so the read path never mutates config state.
bool config_finalized() noexcept;

} // namespace haze
