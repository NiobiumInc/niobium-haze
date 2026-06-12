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

#include <atomic>
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

// Immutable snapshot of the FHE parameters the record path reads on
// every compute call. Published once by Config::freeze() and then read
// lock-free; valid until Config::reset() (hazeDeviceReset concurrent
// with compute is documented-undefined, so readers never outlive it).
struct ConfigSnapshot {
    uint64_t ring_dim = 0;
    std::vector<uint64_t> moduli;

    // 0 = unset / out of range, mirroring Config::modulus().
    uint64_t modulus(int idx) const noexcept {
        if (idx < 0 || static_cast<size_t>(idx) >= moduli.size())
            return 0;
        return moduli[static_cast<size_t>(idx)];
    }
};

// Global FHE-context configuration: ring dimension, ciphertext moduli,
// twiddle generators. Plus the program/target metadata fed to the
// compiler at recording-init time.
//
// Singleton via instance(). Mutex protects mutating writes; reads are
// taken under the same lock for consistency with vector resizes.
class Config {
  public:
    static Config &instance() noexcept;

    // FHE parameters (existing public API).
    std::expected<void, HazeInternalError> set_ring_dimension(uint64_t n) noexcept;
    std::expected<void, HazeInternalError> set_modulus(int idx, uint64_t modulus) noexcept;
    std::expected<void, HazeInternalError> set_twiddle_generator(int idx,
                                                                 uint64_t generator) noexcept;
    std::expected<void, HazeInternalError> configure_device() noexcept;

    uint64_t ring_dim() const noexcept;
    uint64_t modulus(int idx) const noexcept;

    // Freeze the FHE parameters and publish the lock-free snapshot. The
    // record path calls this on every compute entry: the fast path is a
    // single acquire load. Returns nullptr (and freezes nothing) while
    // ring_dim is still unset, so a failed early compute does not lock
    // the user out of configuring afterwards. Once frozen, parameter
    // mutators behave exactly as after configure_device(): identical
    // re-sets succeed, changes are rejected. Cleared by reset().
    const ConfigSnapshot *freeze() noexcept;
    bool frozen() const noexcept;

    // Program / target metadata fed to the compiler during init.
    // Defaults: name="haze", version="0.1", description="HAZE runtime",
    // target=kLocalTarget (overridable by HAZE_TARGET env var unless
    // an explicit hazeSetTarget call has been made).
    std::expected<void, HazeInternalError> set_program_info(const char *name, const char *version,
                                                            const char *description) noexcept;
    std::expected<void, HazeInternalError> set_target(const char *target) noexcept;
    std::string program_name() const noexcept;
    std::string program_version() const noexcept;
    std::string program_description() const noexcept;
    std::string target() const noexcept;

    // Data-representation toggles: Montgomery form and bit-reversed coefficient
    // order, independent. Per flag: explicit setter > env (HAZE_MONTGOMERY /
    // HAZE_BIT_REVERSAL, "1"/"true") > off. The local simulator runs only
    // ordinary-form traces; the backend rejects these at init.
    void set_montgomery(bool enable) noexcept;
    void set_bit_reversal(bool enable) noexcept;
    bool montgomery() const noexcept;
    bool bit_reversal() const noexcept;

    // Optional output-directory override. When set, the backend forwards it to
    // niobium::compiler().set_program_directory() at init, so the project dir
    // (.fhetch + inputs + templates + cryptocontext) lands at this exact path
    // instead of the cwd/<program_name> default. Unset by default.
    std::expected<void, HazeInternalError> set_program_directory(const char *dir) noexcept;
    bool has_program_directory() const noexcept;
    std::string program_directory() const noexcept;

    void reset() noexcept;

    Config(const Config &) = delete;
    Config &operator=(const Config &) = delete;

  private:
    Config() = default;

    // Published-once snapshot; written under mutex_, read lock-free.
    std::atomic<const ConfigSnapshot *> snapshot_{nullptr};

    mutable HazeMutex mutex_;
    uint64_t ring_dim_ = 0;
    std::vector<uint64_t> moduli_;
    std::vector<uint64_t> twiddle_generators_;
    bool configured_ = false;

    // Defaults applied lazily on first read.
    std::string program_name_;
    std::string program_version_;
    std::string program_description_;
    std::string target_;
    std::string program_dir_;
    bool program_info_set_ = false;
    bool target_set_ = false;
    bool program_dir_set_ = false;
    bool montgomery_ = false;
    bool bit_reversal_ = false;
    bool montgomery_set_ = false;
    bool bit_reversal_set_ = false;
};

inline Config &config() noexcept {
    return Config::instance();
}

// Shared truthy env-var read ("1" or "true"; anything else — including
// unset — yields `fallback`). The single truthiness definition for
// every HAZE_* boolean toggle.
bool env_flag(const char *name, bool fallback) noexcept;

} // namespace haze
