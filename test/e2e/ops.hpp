// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Public-API CKKS operation primitives on top of haze's MRP ops, runtime-
// dispatched on the scaling technique. Per-mode pipeline shape lives in
// one place so tests and the FIDESlib `simple.cpp`-style capstone can
// chain the same calls across all four scaling modes.

#pragma once

#include <cstddef>
#include <cstdint>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge_cc.hpp>
#include <map>
#include <openfhe.h>
#include <optional>
#include <utility>
#include <vector>

namespace haze::test::ops {

// RAII move-only vector of haze polynomial allocations.
class Allocs {
  public:
    Allocs() = default;
    Allocs(std::size_t count, std::size_t bytes);
    explicit Allocs(const std::vector<std::vector<uint64_t>> &residues);

    ~Allocs();
    Allocs(Allocs &&other) noexcept;
    Allocs &operator=(Allocs &&other) noexcept;
    Allocs(const Allocs &) = delete;
    Allocs &operator=(const Allocs &) = delete;

    void *const *data() const noexcept { return ptrs_.data(); }
    void **data() noexcept { return ptrs_.data(); }
    std::size_t size() const noexcept { return ptrs_.size(); }
    bool empty() const noexcept { return ptrs_.empty(); }
    void *operator[](std::size_t i) const noexcept { return ptrs_[i]; }

    std::vector<const void *> as_const() const;

  private:
    void free_all() noexcept;
    std::vector<void *> ptrs_;
};

// RAII move-only degree-1 ciphertext (c0, c1) at a known tower count.
class Ct {
  public:
    Ct(Allocs c0, Allocs c1, std::size_t towers, std::uint32_t noise_scale_deg)
        : c0_(std::move(c0)), c1_(std::move(c1)), towers_(towers),
          noise_scale_deg_(noise_scale_deg) {}

    Ct(Ct &&) noexcept = default;
    Ct &operator=(Ct &&) noexcept = default;
    Ct(const Ct &) = delete;
    Ct &operator=(const Ct &) = delete;

    const Allocs &c0() const noexcept { return c0_; }
    const Allocs &c1() const noexcept { return c1_; }
    Allocs &c0() noexcept { return c0_; }
    Allocs &c1() noexcept { return c1_; }
    std::size_t towers() const noexcept { return towers_; }
    std::uint32_t noise_scale_deg() const noexcept { return noise_scale_deg_; }

    // OpenFHE-style level — cross-check against ct->GetLevel().
    std::uint32_t openfhe_level(std::size_t q_full) const noexcept {
        return static_cast<std::uint32_t>(q_full - towers_);
    }

  private:
    Allocs c0_;
    Allocs c1_;
    std::size_t towers_;
    std::uint32_t noise_scale_deg_;
};

struct RotationKeyEntry {
    std::uint32_t auto_index{};
    haze::HybridKeyswitchLimbs limbs;
};

struct OpCtx {
    lbcrypto::CryptoContext<lbcrypto::DCRTPoly> cc;
    lbcrypto::KeyPair<lbcrypto::DCRTPoly> keys;
    haze::HybridKeyswitchLimbs relin_key;
    std::map<std::int32_t, RotationKeyEntry> rotation_keys;
    std::vector<uint64_t> q_base;
    std::vector<uint64_t> p_base;
    std::uint64_t ring_dim{};
    std::size_t poly_bytes{};
    lbcrypto::ScalingTechnique mode{};
    bool with_relin_key{};
};

// No default ctor — `make_ctx` callers must spell out OpenFHEDerives()
// or Pinned(N), making the choice visible at the call site instead of
// hidden in a sentinel.
class RingDimChoice {
  public:
    RingDimChoice() = delete;
    // OpenFHE picks N from mult_depth + scaling_mod_size + the configured
    // HEStd security level.
    static RingDimChoice OpenFHEDerives() noexcept { return RingDimChoice{std::nullopt}; }
    // Pin N and drop the security level to HEStd_NotSet. Test fixtures
    // only (e.g. N=2048 profiling) — never production parameters.
    static RingDimChoice Pinned(std::uint32_t n) noexcept { return RingDimChoice{n}; }

    // nullopt = OpenFHE picks; value = pinned N.
    [[nodiscard]] const std::optional<std::uint32_t> &as_optional() const noexcept { return pin_; }

  private:
    explicit RingDimChoice(std::optional<std::uint32_t> pin) noexcept : pin_(pin) {}
    std::optional<std::uint32_t> pin_;
};

struct CtxParams {
    lbcrypto::ScalingTechnique mode;
    std::uint32_t mult_depth;
    std::uint32_t scaling_mod_size;
    std::uint32_t batch_size;
    bool with_relin_key = false;
    // Slot rotation indices (OpenFHE convention: positive = left).
    std::vector<std::int32_t> rotate_indices;
    RingDimChoice ring_dim;
};

// Construct CC + keys + bridge state. Caller must hazeDeviceReset first.
OpCtx make_ctx(const CtxParams &params);

// ---------------------------------------------------------------------------
// Ciphertext <-> haze transfer
// ---------------------------------------------------------------------------

Ct h2d_ct(const OpCtx &ctx, const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &src);

struct CtBytes {
    std::vector<std::vector<uint64_t>> c0;
    std::vector<std::vector<uint64_t>> c1;
};

// First hazeMemcpy here flushes the epoch and dispatches replay.
// All OpenFHE reference compute must run BEFORE the epoch opens or AFTER
// this call closes it; CPROBES emits fhetch IR while recording is active.
CtBytes d2h_ct(const OpCtx &ctx, const Ct &src);

void inject_ct(const OpCtx &ctx, const CtBytes &src,
               lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &shell);

// ---------------------------------------------------------------------------
// Public-API CKKS operations
// ---------------------------------------------------------------------------

Ct add(const OpCtx &ctx, const Ct &a, const Ct &b);
Ct sub(const OpCtx &ctx, const Ct &a, const Ct &b);

// CKKS-broadcast plaintext (constant across slots within each tower).
Ct mult_scalar(const OpCtx &ctx, const Ct &a, const lbcrypto::Plaintext &pt);

// Tensor + hybrid-keyswitch relin. FLEXIBLEAUTOEXT pre-rescales both inputs.
Ct mult(const OpCtx &ctx, const Ct &a, const Ct &b);

// FIXEDMANUAL only; other modes' RescaleInPlace is a no-op and callers
// should branch on ctx.mode rather than rely on a forced-copy here.
Ct rescale(const OpCtx &ctx, const Ct &a);

// slot_index must be in CtxParams::rotate_indices.
Ct rotate(const OpCtx &ctx, const Ct &a, std::int32_t slot_index);

} // namespace haze::test::ops
