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
//
// Strong RAII handles over the C ABI's void* device buffers. The typed
// layer never lets a buffer travel without its moduli: Srp carries its
// ModSlot, Mrp carries its prime base. Both are move-only and free
// their allocations on destruction (errors from a post-reset free are
// swallowed, mirroring test/e2e/ops.hpp's Allocs).
#pragma once

#include "haze/cxx/error.hpp"

#include <cstddef>
#include <cstdint>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <span>
#include <utility>
#include <vector>

namespace haze::cxx::inline v1 {

// Owning RAII handle over a hazeContext_t: one recording program with
// its FHE parameters fixed at creation. Buffers allocated from a
// context must not outlive it (mirrors the C ABI contract).
class Context {
  public:
    static Result<Context> create(uint64_t ring_dim, std::span<const uint64_t> moduli) noexcept {
        hazeContext_t raw = nullptr;
        if (const hazeError_t err = hazeContextCreate(&raw, ring_dim, moduli.data(), moduli.size());
            err != HAZE_SUCCESS)
            return Status{err};
        return Context{raw};
    }

    Context(Context &&other) noexcept : raw_(std::exchange(other.raw_, nullptr)) {}
    Context &operator=(Context &&other) noexcept {
        if (this != &other) {
            destroy_now();
            raw_ = std::exchange(other.raw_, nullptr);
        }
        return *this;
    }
    Context(const Context &) = delete;
    Context &operator=(const Context &) = delete;
    ~Context() { destroy_now(); }

    hazeContext_t get() const noexcept { return raw_; }

  private:
    explicit Context(hazeContext_t raw) noexcept : raw_(raw) {}
    void destroy_now() noexcept {
        if (raw_ != nullptr)
            (void)hazeContextDestroy(std::exchange(raw_, nullptr));
    }

    hazeContext_t raw_ = nullptr;
};

// A ciphertext modulus VALUE paired with the config index the
// single-residue C ABI addresses it by. The typed layer always passes
// both together so a kernel can never read an ambient index that
// disagrees with the prime it was keyed on.
struct ModSlot {
    uint64_t value = 0;
    int index = 0;

    friend constexpr bool operator==(const ModSlot &, const ModSlot &) noexcept = default;
};

// One single-residue polynomial allocation.
class Srp {
  public:
    static Result<Srp> allocate(hazeContext_t ctx, ModSlot mod, size_t bytes) noexcept {
        void *ptr = nullptr;
        if (const hazeError_t err = hazeMalloc(ctx, &ptr, bytes); err != HAZE_SUCCESS)
            return Status{err};
        return Srp{ctx, ptr, mod};
    }

    // allocate + H2D in one step.
    static Result<Srp> upload(hazeContext_t ctx, ModSlot mod,
                              std::span<const uint64_t> values) noexcept {
        auto srp = allocate(ctx, mod, values.size_bytes());
        if (!srp)
            return srp;
        if (const hazeError_t err = hazeMemcpy(ctx, srp.value().addr(), values.data(),
                                               values.size_bytes(), HAZE_MEMCPY_HOST_TO_DEVICE);
            err != HAZE_SUCCESS)
            return Status{err};
        return srp;
    }

    Srp(Srp &&other) noexcept
        : ctx_(other.ctx_), addr_(std::exchange(other.addr_, nullptr)), mod_(other.mod_) {}
    Srp &operator=(Srp &&other) noexcept {
        if (this != &other) {
            free_now();
            ctx_ = other.ctx_;
            addr_ = std::exchange(other.addr_, nullptr);
            mod_ = other.mod_;
        }
        return *this;
    }
    Srp(const Srp &) = delete;
    Srp &operator=(const Srp &) = delete;
    ~Srp() { free_now(); }

    hazeContext_t context() const noexcept { return ctx_; }
    void *addr() const noexcept { return addr_; }
    ModSlot mod() const noexcept { return mod_; }

    Status download(std::span<uint64_t> out) const noexcept {
        return Status{
            hazeMemcpy(ctx_, out.data(), addr_, out.size_bytes(), HAZE_MEMCPY_DEVICE_TO_HOST)};
    }

  private:
    Srp(hazeContext_t ctx, void *addr, ModSlot mod) noexcept : ctx_(ctx), addr_(addr), mod_(mod) {}
    void free_now() noexcept {
        if (addr_ != nullptr)
            (void)hazeFree(ctx_, std::exchange(addr_, nullptr)); // post-destroy free is benign
    }

    hazeContext_t ctx_ = nullptr;
    void *addr_ = nullptr;
    ModSlot mod_{};
};

// A multi-residue polynomial: one allocation per residue, prime base
// travelling with the pointers (residues[i] is the polynomial mod
// base[i]).
class Mrp {
  public:
    static Result<Mrp> allocate(hazeContext_t ctx, std::span<const uint64_t> base,
                                size_t poly_bytes) noexcept {
        Mrp mrp;
        mrp.ctx_ = ctx;
        mrp.base_.assign(base.begin(), base.end());
        mrp.ptrs_.reserve(base.size());
        for (size_t i = 0; i < base.size(); ++i) {
            void *ptr = nullptr;
            if (const hazeError_t err = hazeMalloc(ctx, &ptr, poly_bytes); err != HAZE_SUCCESS)
                return Status{err}; // mrp's dtor frees the residues already allocated
            mrp.ptrs_.push_back(ptr);
        }
        return mrp;
    }

    // allocate + per-residue H2D (residues[i] under base[i]).
    static Result<Mrp> upload(hazeContext_t ctx, std::span<const std::vector<uint64_t>> residues,
                              std::span<const uint64_t> base) noexcept {
        if (residues.size() != base.size() || residues.empty())
            return Status{HAZE_ERROR_INVALID_VALUE};
        for (const auto &r : residues)
            if (r.size() != residues[0].size())
                return Status{HAZE_ERROR_INVALID_VALUE}; // ragged towers: refuse loudly
        auto mrp = allocate(ctx, base, residues[0].size() * sizeof(uint64_t));
        if (!mrp)
            return mrp;
        // One hazeMemcpyMrp so the residues register as one MRP upload.
        std::vector<const void *> srcs;
        srcs.reserve(residues.size());
        for (const auto &r : residues)
            srcs.push_back(r.data());
        if (const hazeError_t err = hazeMemcpyMrp(
                ctx, mrp.value().data(), srcs.data(), residues[0].size() * sizeof(uint64_t),
                HAZE_MEMCPY_HOST_TO_DEVICE, base.data(), base.size());
            err != HAZE_SUCCESS)
            return Status{err};
        return mrp;
    }

    // Interop: take ownership of raw C-ABI allocations from `ctx` (they
    // will be hazeFree'd against it by this handle).
    static Mrp adopt(hazeContext_t ctx, std::span<void *const> ptrs,
                     std::span<const uint64_t> base) noexcept {
        Mrp mrp;
        mrp.ctx_ = ctx;
        mrp.ptrs_.assign(ptrs.begin(), ptrs.end());
        mrp.base_.assign(base.begin(), base.end());
        return mrp;
    }

    Mrp(Mrp &&other) noexcept
        : ctx_(other.ctx_), ptrs_(std::exchange(other.ptrs_, {})), base_(std::move(other.base_)) {}
    Mrp &operator=(Mrp &&other) noexcept {
        if (this != &other) {
            free_now();
            ctx_ = other.ctx_;
            ptrs_ = std::move(other.ptrs_);
            base_ = std::move(other.base_);
            other.ptrs_.clear();
        }
        return *this;
    }
    Mrp(const Mrp &) = delete;
    Mrp &operator=(const Mrp &) = delete;
    ~Mrp() { free_now(); }

    hazeContext_t context() const noexcept { return ctx_; }
    void *const *data() const noexcept { return ptrs_.data(); }
    const uint64_t *base() const noexcept { return base_.data(); }
    size_t base_len() const noexcept { return base_.size(); }
    std::span<const uint64_t> base_span() const noexcept { return base_; }
    void *residue(size_t i) const noexcept { return ptrs_[i]; }

    Status download_residue(size_t i, std::span<uint64_t> out) const noexcept {
        if (i >= ptrs_.size())
            return Status{HAZE_ERROR_INVALID_VALUE};
        return Status{
            hazeMemcpy(ctx_, out.data(), ptrs_[i], out.size_bytes(), HAZE_MEMCPY_DEVICE_TO_HOST)};
    }

    // Interop: give the allocations up to the caller (no hazeFree here).
    std::vector<void *> release() noexcept {
        base_.clear();
        return std::exchange(ptrs_, {});
    }

  private:
    Mrp() = default;
    void free_now() noexcept {
        for (void *p : ptrs_)
            if (p != nullptr)
                (void)hazeFree(ctx_, p);
        ptrs_.clear();
    }

    hazeContext_t ctx_ = nullptr;
    std::vector<void *> ptrs_;
    std::vector<uint64_t> base_;
};

} // namespace haze::cxx::inline v1
