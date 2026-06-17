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
#include "common/handle.hpp"
#include "common/thread_safety.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace haze {

// The deferred recording tape. Compute entry points append Nodes here at
// record time; hazeFlush seals the tape and lowers it through the fhetch
// API single-threaded (src/core/lower.{hpp,cpp}). No fhetch call happens
// before flush.
//
// THUNK DISCIPLINE: a Node's thunk may capture only plain values —
// ValueId, uint64_t, std::vector<uint64_t>, std::vector<DevAddr>,
// std::string. It must NEVER capture fhetch types (Polynomial, Scalar,
// MRP, MRS, ModuliBase): constructing any of them touches fhetch's
// global polynomial-ID/address allocator, and a record-time construction
// silently shifts every address the lowering pass produces afterwards.
// Construct fhetch objects inside the thunk body, at lowering time.

// SSA-style value identity for one single-residue polynomial in flight.
// 0 is the "unbound" sentinel; real ids start at 1 and are handed out by
// a process-global relaxed counter that is never reset (uniqueness is
// the only contract; per-epoch density is not).
using ValueId = uint64_t;
inline constexpr ValueId kUnbound = 0;

ValueId new_value_id() noexcept;

// Lock-free addr -> ValueId map for the record path ("Tier-2 atomics").
//
// The DeviceAllocator hands out poly_bytes_-sized allocations from a bump
// pointer at kHbmBase (plus a same-size free list), so device addresses
// form a dense slot index: (addr - kHbmBase) / slot_bytes_. Bindings live
// in chunks of atomic words reached through a fixed-capacity spine of
// atomic chunk pointers — growth installs a new chunk by CAS and never
// relocates an existing atomic, so readers are wait-free.
//
// Thread-safety: intentionally NO mutex and NO TSA annotations. Every
// slot is a single atomic word; the protocols are one-line
// (release-store publish, acquire-load consume, CAS first-touch) and the
// cross-thread contract for *meaningful* values is the user's (same-addr
// races are documented-undefined at the API level; the atomics keep them
// memory-safe, nothing more).
class BindingTable {
  public:
    BindingTable() = default; // constructed as a haze_context_s member

    // Configure the slot geometry (allocation size in bytes). Called by
    // Config::set_ring_dimension next to allocator().set_polynomial_size;
    // only reachable while nothing is recorded, so it also clears.
    void set_slot_bytes(size_t bytes) noexcept;

    // Current binding for addr, or kUnbound. Acquire: a non-zero id
    // makes the publishing node's contents visible.
    ValueId load(DevAddr addr) const noexcept;

    // Bind addr -> id (result store, H2D rebind). Release.
    void store(DevAddr addr, ValueId id) noexcept;

    // First-touch promotion: bind addr -> fresh iff currently unbound.
    // Returns the resident id: `fresh` on the winning CAS, the winner's
    // id when another thread (user-undefined but memory-safe) got there
    // first.
    ValueId promote(DevAddr addr, ValueId fresh) noexcept;

    // Drop the binding (hazeFree / hazeMemset invalidate).
    void erase(DevAddr addr) noexcept;

    // Drop every binding (seal, reset). Relaxed: callers order it with
    // their own synchronization (flush's seal swap / hazeDeviceReset).
    void clear() noexcept;

    BindingTable(const BindingTable &) = delete;
    BindingTable &operator=(const BindingTable &) = delete;

  private:
    static constexpr size_t kChunkSlots = 4096; // 32 KiB of atomics per chunk
    static constexpr size_t kMaxChunks = 16384; // 64M slots ceiling
    struct Chunk {
        std::array<std::atomic<uint64_t>, kChunkSlots> slots{};
    };

    // Slot atom for addr, or nullptr when addr is out of range /
    // geometry unset / (when create==false) the chunk was never touched.
    std::atomic<uint64_t> *slot(DevAddr addr, bool create) noexcept;
    const std::atomic<uint64_t> *slot(DevAddr addr) const noexcept;

    std::array<std::atomic<Chunk *>, kMaxChunks> spine_{};
    std::atomic<size_t> slot_bytes_{0};
};

// Default-context accessors (TEMPORARY bridge; defined in context.cpp).
// addr -> recorded modulus (0 = unknown / sentinel-only address).

// Lowering context handed to thunks at flush time; defined in
// core/lower.hpp. Single-threaded by construction.
struct LowerCtx;

using Thunk = std::function<std::expected<void, HazeInternalError>(LowerCtx &)>;

// One tape entry. Metadata-only kinds (TagOutput, MrpRegister,
// Invalidate) carry no thunk: their entire effect is on the derived
// state computed by lower::derive() at flush.
struct Node {
    enum class Kind : uint8_t {
        InputSnapshot, // first-touch shadow promotion -> tag_input at lowering
        H2DInput,      // eager H2D tag; rebinds addr, derive() drops pending tag
        Compute,       // sr_*/mr_*/gadget call producing one or more bindings
        Copy,          // pass-through copy (sentinel modulus + bind_modulus)
        TagOutput,     // hazeTagOutput(addr); named + group-expanded by derive()
        MrpInputTag,   // tag_input(name, MRP) for a multi-residue source group
        MrpRegister,   // multi-residue output group registration
        Invalidate,    // hazeFree/hazeMemset dropped the addr binding
    };

    Kind kind{};
    // Primary address: dst residue / tagged addr / invalidated addr /
    // leading residue of an MRP group.
    DevAddr addr{};
    // Binding produced by this node (binding kinds only). Multi-residue
    // nodes bind group_addrs[i] -> group_vids[i] instead.
    ValueId dst_vid{kUnbound};
    // Values this node's thunk reads (operand lifetimes for the
    // lowering pass's eviction; future DCE rides the same edges).
    std::vector<ValueId> src_vids;
    // MRP group membership (MrpInputTag / MrpRegister) or multi-residue
    // dst binding (Compute): residue addrs in encounter order with
    // base[i] paired per residue and the vid per residue.
    std::vector<DevAddr> group_addrs;
    std::vector<uint64_t> group_moduli;
    std::vector<ValueId> group_vids;
    // Value-translation map for memoized-kernel instances: the thunk
    // captured the RECORDING call's ValueIds; LowerCtx resolves them
    // through this map (nullptr = identity, the common case). Shared by
    // every node cloned from one sub-tape instantiation. Node metadata
    // (dst_vid/src_vids/group_vids) is translated EAGERLY at clone time
    // so derive() never consults this.
    std::shared_ptr<const std::unordered_map<ValueId, ValueId>> vid_remap;
    // Lowering action; empty for metadata-only kinds.
    Thunk thunk;
    // Provenance for flush-time diagnostics: the C-ABI entry point that
    // recorded this node ("hazeAdd", "hazeModUp", ...). String literal.
    const char *entry{""};
};

// The append-only tape (one per context). append() is the only
// record-path lock in haze (a short push under an internal mutex — no
// fhetch work, no allocation of polynomial payloads inside the
// critical section).
class Graph {
  public:
    Graph() = default; // constructed as a haze_context_s member

    void append(Node &&node) noexcept HAZE_EXCLUDES(mutex_);

    // Kernel-recording redirection: while a sink is installed (between
    // hazeKernelBegin and hazeKernelEnd on a RECORD disposition),
    // append() routes nodes into it instead of the tape. Single open
    // bracket at a time; Begin/End must not interleave across threads
    // (documented in haze.h).
    void install_frame_sink(std::vector<Node> *sink) noexcept HAZE_EXCLUDES(mutex_);
    bool frame_sink_installed() const noexcept HAZE_EXCLUDES(mutex_);

    // Number of nodes currently recorded (diagnostics/tests).
    size_t size() const noexcept HAZE_EXCLUDES(mutex_);

    // Swap the tape out for lowering. Callers clear their context's
    // BindingTables alongside (the tape does not know its siblings) so
    // the record path observes a fresh epoch whatever lowering does.
    std::vector<Node> seal() noexcept HAZE_EXCLUDES(mutex_);

    // Discard the tape without lowering: thunks never run, so nothing
    // is ever emitted to fhetch. Callers clear the BindingTables too.
    void reset() noexcept HAZE_EXCLUDES(mutex_);

    Graph(const Graph &) = delete;
    Graph &operator=(const Graph &) = delete;

  private:
    mutable HazeMutex mutex_;
    std::vector<Node> nodes_ HAZE_GUARDED_BY(mutex_);
    std::vector<Node> *frame_sink_ HAZE_GUARDED_BY(mutex_) = nullptr;
};

} // namespace haze
