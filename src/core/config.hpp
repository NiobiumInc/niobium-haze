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
#include "common/thread_safety.hpp"

#include <cstdint>
#include <expected>
#include <haze/haze_types.h>
#include <string>
#include <string_view>
#include <vector>

namespace haze {

// Canonical target string dispatched through libnbfhetch's compiler.
// kLocalTarget runs the in-process FHETCH instruction-set simulator:
// libnbfhetch loads the .fhetch trace into fhetch_sim::Simulator,
// executes it, and writes <program_dir>/serialized_probes/<name>.ct
// so epoch.cpp's result-population loop fills shadow buffers with
// computed values. No compiler-side binary or HTTP transport needed.
//
// Anything else (e.g. "FUNC_SIM", "FHE_SIM", "FPGA_TRI", "fhetch_sim")
// is forwarded verbatim to nbcc_fhetch_replay over the HTTP transport
// — those strings select a backend on the compiler side. Keep
// kLocalTarget symbolic so a future rename in libnbfhetch flows
// through every comparison site in haze.
constexpr std::string_view kLocalTarget = "local";

class DeviceState;

// Global FHE-context configuration: ring dimension, ciphertext moduli,
// twiddle generators, plus the program/target metadata fed to the compiler
// at recording-init time. A DeviceState member reached via config(); the
// mutex covers writes and reads alike (vector resizes).
class Config {
  public:
    // FHE parameters (existing public API).
    std::expected<void, HazeInternalError> set_ring_dimension(uint64_t n) noexcept
        HAZE_EXCLUDES(mutex_);
    std::expected<void, HazeInternalError> set_modulus(int idx, uint64_t modulus) noexcept
        HAZE_EXCLUDES(mutex_);
    std::expected<void, HazeInternalError> set_twiddle_generator(int idx,
                                                                 uint64_t generator) noexcept
        HAZE_EXCLUDES(mutex_);
    std::expected<void, HazeInternalError> configure_device() noexcept HAZE_EXCLUDES(mutex_);

    uint64_t ring_dim() const noexcept HAZE_EXCLUDES(mutex_);
    uint64_t modulus(int idx) const noexcept HAZE_EXCLUDES(mutex_);

    // Program / target metadata fed to the compiler during init. Defaults:
    // name="haze", version="0.1", description="HAZE runtime", target=kLocalTarget.
    std::expected<void, HazeInternalError> set_program_info(const char *name, const char *version,
                                                            const char *description) noexcept
        HAZE_EXCLUDES(mutex_);
    std::expected<void, HazeInternalError> set_target(const char *target) noexcept
        HAZE_EXCLUDES(mutex_);
    std::string program_name() const noexcept HAZE_EXCLUDES(mutex_);
    std::string program_version() const noexcept HAZE_EXCLUDES(mutex_);
    std::string program_description() const noexcept HAZE_EXCLUDES(mutex_);
    std::string target() const noexcept HAZE_EXCLUDES(mutex_);

    // Data-representation toggles (Montgomery form, bit-reversed order), off by
    // default; the local simulator rejects non-ordinary traces at init.
    void set_montgomery(bool enable) noexcept HAZE_EXCLUDES(mutex_);
    void set_bit_reversal(bool enable) noexcept HAZE_EXCLUDES(mutex_);
    bool montgomery() const noexcept HAZE_EXCLUDES(mutex_);
    bool bit_reversal() const noexcept HAZE_EXCLUDES(mutex_);

    // Centered (reduced-noise) FBC variant matching OpenFHE WITH_REDUCED_NOISE,
    // off by default; the HazeEngine constructor enables it for bit-exact parity.
    void set_reduced_noise(bool enable) noexcept HAZE_EXCLUDES(mutex_);
    bool reduced_noise() const noexcept HAZE_EXCLUDES(mutex_);

    // Optional output-directory override. When set, the backend forwards it to
    // niobium::compiler().set_program_directory() at init, so the program dir
    // (.fhetch + inputs + templates + cryptocontext) lands at this exact path
    // instead of the cwd/<program_name> default. Unset by default.
    std::expected<void, HazeInternalError> set_program_directory(const char *dir) noexcept
        HAZE_EXCLUDES(mutex_);
    bool has_program_directory() const noexcept HAZE_EXCLUDES(mutex_);
    std::string program_directory() const noexcept HAZE_EXCLUDES(mutex_);

    void reset() noexcept HAZE_EXCLUDES(mutex_);

    Config(const Config &) = delete;
    Config &operator=(const Config &) = delete;

  private:
    friend class DeviceState;
    Config() = default;

    mutable HazeMutex mutex_;
    uint64_t ring_dim_ HAZE_GUARDED_BY(mutex_) = 0;
    std::vector<uint64_t> moduli_ HAZE_GUARDED_BY(mutex_);
    std::vector<uint64_t> twiddle_generators_ HAZE_GUARDED_BY(mutex_);
    bool configured_ HAZE_GUARDED_BY(mutex_) = false;

    // Defaults applied lazily on first read.
    std::string program_name_ HAZE_GUARDED_BY(mutex_);
    std::string program_version_ HAZE_GUARDED_BY(mutex_);
    std::string program_description_ HAZE_GUARDED_BY(mutex_);
    std::string target_ HAZE_GUARDED_BY(mutex_);
    std::string program_dir_ HAZE_GUARDED_BY(mutex_);
    bool program_info_set_ HAZE_GUARDED_BY(mutex_) = false;
    bool target_set_ HAZE_GUARDED_BY(mutex_) = false;
    bool program_dir_set_ HAZE_GUARDED_BY(mutex_) = false;
    bool montgomery_ HAZE_GUARDED_BY(mutex_) = false;
    bool bit_reversal_ HAZE_GUARDED_BY(mutex_) = false;
    bool reduced_noise_ HAZE_GUARDED_BY(mutex_) = false;
};

// Defined in device_state.cpp (returns the DeviceState member).
Config &config() noexcept;

} // namespace haze
