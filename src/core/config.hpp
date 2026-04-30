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

#include <haze/haze_types.h>

#include <cstdint>
#include <mutex>
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

// Global FHE-context configuration: ring dimension, ciphertext moduli,
// twiddle generators. Plus the program/target metadata fed to the
// niobium compiler at recording-init time.
//
// Singleton via instance(). Mutex protects mutating writes; reads are
// taken under the same lock for consistency with vector resizes.
class Config {
  public:
    static Config &instance() noexcept;

    // FHE parameters (existing public API).
    hazeError_t set_ring_dimension(uint64_t n) noexcept;
    hazeError_t set_modulus(int idx, uint64_t modulus) noexcept;
    hazeError_t set_twiddle_generator(int idx, uint64_t generator) noexcept;
    hazeError_t configure_device() noexcept;

    uint64_t ring_dim() const noexcept;
    uint64_t modulus(int idx) const noexcept;
    bool is_configured() const noexcept;
    std::vector<uint64_t> moduli_copy() const noexcept;

    // Program / target metadata fed to the compiler during init.
    // Defaults: name="haze", version="0.1", description="HAZE runtime",
    // target=kLocalTarget (overridable by HAZE_TARGET env var unless
    // an explicit hazeSetTarget call has been made).
    hazeError_t set_program_info(const char *name, const char *version,
                                 const char *description) noexcept;
    hazeError_t set_target(const char *target) noexcept;
    std::string program_name() const noexcept;
    std::string program_version() const noexcept;
    std::string program_description() const noexcept;
    std::string target() const noexcept;

    void reset() noexcept;

  private:
    Config() = default;
    Config(const Config &) = delete;
    Config &operator=(const Config &) = delete;

    mutable std::mutex mutex_;
    uint64_t ring_dim_ = 0;
    std::vector<uint64_t> moduli_;
    std::vector<uint64_t> twiddle_generators_;
    bool configured_ = false;

    // Defaults applied lazily on first read.
    std::string program_name_;
    std::string program_version_;
    std::string program_description_;
    std::string target_;
    bool program_info_set_ = false;
    bool target_set_ = false;
};

inline Config &config() noexcept { return Config::instance(); }

} // namespace haze
