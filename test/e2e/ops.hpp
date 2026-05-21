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

    // Free the trailing pointers beyond `new_size`, resize the internal
    // vector. No-op when new_size >= current size.
    void truncate(std::size_t new_size) noexcept;

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

struct CtxParams {
    lbcrypto::ScalingTechnique mode;
    std::uint32_t mult_depth;
    std::uint32_t scaling_mod_size;
    std::uint32_t batch_size;
    bool with_relin_key = false;
    // Slot rotation indices (OpenFHE convention: positive = left).
    std::vector<std::int32_t> rotate_indices;
    // Explicit ring dimension. 0 = let OpenFHE auto-derive.
    std::uint32_t ring_dim = 0;
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

// Deep-copy via per-tower D2D. Needed because Ct is move-only and the
// Chebyshev recursion references the same input across multiple paths.
Ct clone_ct(const OpCtx &ctx, const Ct &src);

// Drop `levels` trailing towers from `ct` — pure metadata reduction, no
// ModDown arithmetic, no allocation. Mirrors OpenFHE's LevelReduceInPlace.
// Distinct from ops::rescale, which does INTT/ModDown/NTT and consumes a
// multiplicative level. Use this for tower-count alignment before ops
// that require matching towers (add, sub, mult_pt rows).
Ct level_reduce(const OpCtx &ctx, Ct ct, std::size_t levels);

Ct add(const OpCtx &ctx, const Ct &a, const Ct &b);
Ct sub(const OpCtx &ctx, const Ct &a, const Ct &b);

// CKKS-broadcast plaintext (constant across slots within each tower).
Ct mult_scalar(const OpCtx &ctx, const Ct &a, const lbcrypto::Plaintext &pt);

// Multiply by a per-tower polynomial plaintext. Used by linear-transform
// rows where mult_scalar's constant-broadcast contract doesn't apply.
Ct mult_pt(const OpCtx &ctx, const Ct &a, const Allocs &pt_chain);

// Tensor + hybrid-keyswitch relin. FLEXIBLEAUTOEXT pre-rescales both inputs.
Ct mult(const OpCtx &ctx, const Ct &a, const Ct &b);

// FIXEDMANUAL only; other modes' RescaleInPlace is a no-op and callers
// should branch on ctx.mode rather than rely on a forced-copy here.
Ct rescale(const OpCtx &ctx, const Ct &a);

// slot_index must be in CtxParams::rotate_indices.
Ct rotate(const OpCtx &ctx, const Ct &a, std::int32_t slot_index);

// Rotate by an explicit automorphism index, bypassing the slot-index map.
Ct rotate_with_key(const OpCtx &ctx, const Ct &a, const RotationKeyEntry &entry);

// Complex conjugation: automorphism at index 2N-1.
Ct conjugate(const OpCtx &ctx, const Ct &a, const haze::HybridKeyswitchLimbs &conj_key);

} // namespace haze::test::ops
