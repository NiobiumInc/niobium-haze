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
#include "core/graph.hpp"

#include "common/handle.hpp"
#include "common/thread_safety.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace haze {

namespace {
// Never reset: ValueId promises uniqueness for the process lifetime, so
// a stale binding from a previous epoch can never alias a fresh one.
std::atomic<ValueId> g_next_value_id{1};
} // namespace

ValueId new_value_id() noexcept {
    return g_next_value_id.fetch_add(1, std::memory_order_relaxed);
}

void BindingTable::set_slot_bytes(size_t bytes) noexcept {
    // Geometry changes are only reachable pre-freeze (set_ring_dimension
    // before the first compute), i.e. while no binding exists; clearing
    // keeps the invariant explicit rather than assumed.
    slot_bytes_.store(bytes, std::memory_order_relaxed);
    clear();
}

std::atomic<uint64_t> *BindingTable::slot(DevAddr addr, bool create) noexcept {
    const size_t bytes = slot_bytes_.load(std::memory_order_relaxed);
    const uintptr_t raw = to_uintptr(addr);
    if (bytes == 0 || raw < kHbmBase)
        return nullptr;
    const size_t index = (raw - kHbmBase) / bytes;
    const size_t chunk_idx = index / kChunkSlots;
    if (chunk_idx >= kMaxChunks)
        return nullptr;
    Chunk *chunk = spine_[chunk_idx].load(std::memory_order_acquire);
    if (chunk == nullptr) {
        if (!create)
            return nullptr;
        // Install a zeroed chunk; a racing installer wins by CAS and the
        // loser's allocation is dropped. Existing atomics never move.
        auto fresh = std::make_unique<Chunk>();
        Chunk *expected = nullptr;
        if (spine_[chunk_idx].compare_exchange_strong(
                expected, fresh.get(), std::memory_order_acq_rel, std::memory_order_acquire)) {
            chunk = fresh.release();
        } else {
            chunk = expected;
        }
    }
    return &chunk->slots[index % kChunkSlots];
}

const std::atomic<uint64_t> *BindingTable::slot(DevAddr addr) const noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    return const_cast<BindingTable *>(this)->slot(addr, /*create=*/false);
}

ValueId BindingTable::load(DevAddr addr) const noexcept {
    const std::atomic<uint64_t> *s = slot(addr);
    return s == nullptr ? kUnbound : s->load(std::memory_order_acquire);
}

void BindingTable::store(DevAddr addr, ValueId id) noexcept {
    std::atomic<uint64_t> *s = slot(addr, /*create=*/true);
    if (s != nullptr)
        s->store(id, std::memory_order_release);
}

ValueId BindingTable::promote(DevAddr addr, ValueId fresh) noexcept {
    std::atomic<uint64_t> *s = slot(addr, /*create=*/true);
    if (s == nullptr)
        return fresh; // out-of-range addr; caller's validation already failed it
    uint64_t expected = kUnbound;
    if (s->compare_exchange_strong(expected, fresh, std::memory_order_acq_rel,
                                   std::memory_order_acquire)) {
        return fresh;
    }
    return expected;
}

void BindingTable::erase(DevAddr addr) noexcept {
    std::atomic<uint64_t> *s = slot(addr, /*create=*/false);
    if (s != nullptr)
        s->store(kUnbound, std::memory_order_release);
}

void BindingTable::clear() noexcept {
    for (auto &entry : spine_) {
        Chunk *chunk = entry.load(std::memory_order_acquire);
        if (chunk == nullptr)
            continue; // spine fills front-to-back only as slots are touched
        for (auto &word : chunk->slots)
            word.store(kUnbound, std::memory_order_relaxed);
    }
}

void Graph::append(Node &&node) noexcept {
    HazeLockGuard lock(mutex_);
    if (frame_sink_ != nullptr) {
        frame_sink_->push_back(std::move(node));
        return;
    }
    nodes_.push_back(std::move(node));
}

void Graph::install_frame_sink(std::vector<Node> *sink) noexcept {
    HazeLockGuard lock(mutex_);
    frame_sink_ = sink;
}

bool Graph::frame_sink_installed() const noexcept {
    HazeLockGuard lock(mutex_);
    return frame_sink_ != nullptr;
}

size_t Graph::size() const noexcept {
    HazeLockGuard lock(mutex_);
    return nodes_.size();
}

std::vector<Node> Graph::seal() noexcept {
    std::vector<Node> out;
    {
        HazeLockGuard lock(mutex_);
        out.swap(nodes_);
    }
    return out;
}

void Graph::reset() noexcept {
    {
        HazeLockGuard lock(mutex_);
        nodes_.clear();
        frame_sink_ = nullptr; // an open kernel bracket dies with the reset
    }
}

} // namespace haze
