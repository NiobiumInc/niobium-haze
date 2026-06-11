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
#include "core/kernel_cache.hpp"

#include "common/errors.hpp"
#include "common/handle.hpp"
#include "common/thread_safety.hpp"
#include "core/graph.hpp"
#include "core/mrp_polymap.hpp"
#include "core/record.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <haze/haze_types.h>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace haze {

namespace {

bool env_truthy(const char *var, bool fallback) noexcept {
    const char *v = std::getenv(var); // NOLINT(concurrency-mt-unsafe) — read-only lookup
    if (v == nullptr || v[0] == '\0')
        return fallback;
    return v[0] != '0';
}

// Flatten the C-ABI input/output descriptors to residue addr lists.
std::vector<DevAddr> flatten_inputs(std::span<const hazeKernelInput> inputs) {
    std::vector<DevAddr> addrs;
    for (const hazeKernelInput &in : inputs)
        for (size_t i = 0; i < in.base_len; ++i)
            addrs.push_back(to_dev_addr(in.residues[i]));
    return addrs;
}

std::vector<DevAddr> flatten_outputs(std::span<const hazeKernelOutput> outputs) {
    std::vector<DevAddr> addrs;
    for (const hazeKernelOutput &out : outputs)
        for (size_t i = 0; i < out.base_len; ++i)
            addrs.push_back(to_dev_addr(out.residues[i]));
    return addrs;
}

} // namespace

KernelCache &KernelCache::instance() noexcept {
    static KernelCache inst;
    return inst;
}

bool KernelCache::has_open_frame() const noexcept {
    HazeLockGuard lock(mutex_);
    return frame_ != nullptr;
}

bool KernelCache::memo_enabled_locked() const {
    if (memo_override_ >= 0)
        return memo_override_ != 0;
    return env_truthy("HAZE_KERNEL_MEMO", /*fallback=*/true);
}

bool KernelCache::validate_enabled_locked() const {
    if (validate_override_ >= 0)
        return validate_override_ != 0;
    return env_truthy("HAZE_KERNEL_VALIDATE", /*fallback=*/false);
}

const KernelCache::SubTape *KernelCache::find_locked(uint64_t key_hash,
                                                     std::span<const uint8_t> key_bytes) const {
    auto [first, last] = cache_.equal_range(key_hash);
    for (auto it = first; it != last; ++it) {
        const SubTape &sub = *it->second;
        // Full key bytes are compared on every lookup: a 64-bit hash
        // collision must never alias two kernels into one program.
        if (sub.key_bytes.size() == key_bytes.size() &&
            std::equal(key_bytes.begin(), key_bytes.end(), sub.key_bytes.begin()))
            return &sub;
    }
    return nullptr;
}

std::expected<hazeKernelDisposition, HazeInternalError>
KernelCache::begin(const char *name, uint64_t key_hash, std::span<const uint8_t> key_bytes,
                   std::span<const hazeKernelInput> inputs) noexcept {
    HazeLockGuard lock(mutex_);
    // Nesting is gated in the shim (NOT_SUPPORTED); a frame here with the
    // shim gate passed is a protocol bug.
    if (frame_ != nullptr) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "hazeKernelBegin: bracket already open");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }

    auto frame = std::make_unique<OpenFrame>();
    frame->name = name;
    frame->key_hash = key_hash;
    frame->key_bytes.assign(key_bytes.begin(), key_bytes.end());
    frame->in_addrs = flatten_inputs(inputs);
    frame->in_vids.reserve(frame->in_addrs.size());
    for (DevAddr a : frame->in_addrs) {
        const ValueId vid = bindings().load(a);
        if (vid == kUnbound) {
            // Every kernel input must already be a recorded value (H2D'd
            // or computed); a kernel can't promote foreign shadow bytes.
            record_internal_error(HazeInternalError::SourceUnavailable,
                                  "hazeKernelBegin: input residue is not a recorded value");
            return std::unexpected(HazeInternalError::SourceUnavailable);
        }
        frame->in_vids.push_back(vid);
    }

    if (!memo_enabled_locked()) {
        frame->passthrough = true; // ops record straight onto the main tape
        frame_ = std::move(frame);
        return HAZE_KERNEL_RECORD;
    }

    if (const SubTape *hit = find_locked(key_hash, key_bytes); hit != nullptr) {
        if (validate_enabled_locked()) {
            // Re-trace the body into a scratch frame and diff at End.
            frame->validate_against = hit;
            frame->body = std::make_unique<std::vector<Node>>();
            graph().install_frame_sink(frame->body.get());
            frame_ = std::move(frame);
            return HAZE_KERNEL_RECORD;
        }
        frame->replay = hit;
        frame_ = std::move(frame);
        return HAZE_KERNEL_REPLAY;
    }

    frame->body = std::make_unique<std::vector<Node>>();
    graph().install_frame_sink(frame->body.get());
    frame_ = std::move(frame);
    return HAZE_KERNEL_RECORD;
}

// Member (not static) purely to carry the HAZE_REQUIRES(mutex_) contract.
// NOLINTBEGIN(readability-convert-member-functions-to-static)
std::expected<void, HazeInternalError>
KernelCache::check_closed_body_locked(const OpenFrame &frame,
                                      std::span<const DevAddr> out_addrs) const {
    std::unordered_set<DevAddr> allowed(frame.in_addrs.begin(), frame.in_addrs.end());
    allowed.insert(out_addrs.begin(), out_addrs.end());
    const auto allowed_addr = [&](DevAddr a) { return allowed.contains(a); };

    for (const Node &node : *frame.body) {
        switch (node.kind) {
        case Node::Kind::InputSnapshot:
            // A body promotion means the body computed on a buffer that
            // was not a declared, already-recorded input.
            record_internal_error(HazeInternalError::SourceUnavailable,
                                  "kernel body touched an undeclared input buffer");
            return std::unexpected(HazeInternalError::SourceUnavailable);
        case Node::Kind::H2DInput:
        case Node::Kind::Invalidate:
            // No H2D / memset / free inside a body: a replayed call
            // skips the body, so such effects could never replay.
            record_internal_error(HazeInternalError::SourceUnavailable,
                                  "kernel body performed H2D/memset/free");
            return std::unexpected(HazeInternalError::SourceUnavailable);
        case Node::Kind::Compute:
        case Node::Kind::Copy:
        case Node::Kind::TagOutput:
        case Node::Kind::MrpInputTag:
        case Node::Kind::MrpRegister:
            break;
        }
        if (node.group_addrs.empty()) {
            if (node.dst_vid != kUnbound || node.kind == Node::Kind::TagOutput) {
                if (!allowed_addr(node.addr)) {
                    record_internal_error(HazeInternalError::SourceUnavailable,
                                          "kernel body wrote a buffer outside its formals");
                    return std::unexpected(HazeInternalError::SourceUnavailable);
                }
            }
        } else {
            for (DevAddr a : node.group_addrs) {
                if (!allowed_addr(a)) {
                    record_internal_error(HazeInternalError::SourceUnavailable,
                                          "kernel body used a buffer outside its formals");
                    return std::unexpected(HazeInternalError::SourceUnavailable);
                }
            }
        }
    }
    return {};
}

void KernelCache::instantiate_locked(const SubTape &sub, const OpenFrame &frame,
                                     std::span<const DevAddr> out_addrs) {
    // Positional formal translation, total under the closed-body rule.
    std::unordered_map<DevAddr, DevAddr> addr_map;
    for (size_t i = 0; i < sub.in_addrs.size(); ++i)
        addr_map.emplace(sub.in_addrs[i], frame.in_addrs[i]);
    for (size_t i = 0; i < sub.out_addrs.size(); ++i)
        addr_map.insert_or_assign(sub.out_addrs[i], out_addrs[i]);

    auto vid_map = std::make_shared<std::unordered_map<ValueId, ValueId>>();
    for (size_t i = 0; i < sub.in_vids.size(); ++i)
        vid_map->emplace(sub.in_vids[i], frame.in_vids[i]);
    const auto fresh_vid = [&](ValueId recorded) {
        auto [it, inserted] = vid_map->try_emplace(recorded, kUnbound);
        if (inserted)
            it->second = new_value_id();
        return it->second;
    };
    const auto map_addr = [&](DevAddr a) {
        const auto it = addr_map.find(a);
        return it != addr_map.end() ? it->second : a;
    };

    for (const Node &recorded : sub.body) {
        Node clone = recorded; // shares the immutable thunk; copies metadata
        clone.addr = map_addr(recorded.addr);
        for (auto &a : clone.group_addrs)
            a = map_addr(a);
        if (clone.dst_vid != kUnbound)
            clone.dst_vid = fresh_vid(recorded.dst_vid);
        for (auto &v : clone.src_vids)
            v = fresh_vid(v);
        for (auto &v : clone.group_vids)
            v = fresh_vid(v);
        // Group names are counter-derived at flush from the (translated)
        // leading addr, so instances name exactly like cold recordings —
        // nothing to re-derive here.
        clone.vid_remap = vid_map;
        graph().append(std::move(clone));
    }

    // Bind the outputs to their instance values so subsequent ops (and
    // End's tagging) see them exactly as a cold call would have left them.
    for (size_t i = 0; i < sub.out_addrs.size(); ++i)
        bindings().store(out_addrs[i], fresh_vid(sub.out_vids[i]));
}
// NOLINTEND(readability-convert-member-functions-to-static)

std::expected<void, HazeInternalError>
KernelCache::end(std::span<const hazeKernelOutput> outputs) noexcept {
    std::vector<DevAddr> out_addrs = flatten_outputs(outputs);
    {
        HazeLockGuard lock(mutex_);
        if (frame_ == nullptr) {
            record_internal_error(HazeInternalError::InvalidArgument,
                                  "hazeKernelEnd: no open bracket");
            return std::unexpected(HazeInternalError::InvalidArgument);
        }
        const std::unique_ptr<OpenFrame> frame = std::move(frame_);
        if (frame->body != nullptr)
            graph().install_frame_sink(nullptr);

        if (frame->replay != nullptr) {
            if (out_addrs.size() != frame->replay->out_addrs.size()) {
                record_internal_error(HazeInternalError::InvalidArgument,
                                      "hazeKernelEnd: output shape differs from the recording");
                return std::unexpected(HazeInternalError::InvalidArgument);
            }
            instantiate_locked(*frame->replay, *frame, out_addrs);
        } else if (!frame->passthrough) {
            if (auto closed = check_closed_body_locked(*frame, out_addrs); !closed)
                return closed;

            std::vector<ValueId> out_vids;
            out_vids.reserve(out_addrs.size());
            for (DevAddr a : out_addrs) {
                const ValueId vid = bindings().load(a);
                if (vid == kUnbound) {
                    record_internal_error(HazeInternalError::SourceUnavailable,
                                          "hazeKernelEnd: declared output was never written");
                    return std::unexpected(HazeInternalError::SourceUnavailable);
                }
                out_vids.push_back(vid);
            }

            if (frame->validate_against != nullptr) {
                // Structural diff of the re-trace against the cache:
                // node count, kinds, arities, and moduli must agree.
                const SubTape &ref = *frame->validate_against;
                const std::vector<Node> &scratch = *frame->body;
                bool same = scratch.size() == ref.body.size();
                for (size_t i = 0; same && i < scratch.size(); ++i) {
                    const Node &a = scratch[i];
                    const Node &b = ref.body[i];
                    same = a.kind == b.kind && a.src_vids.size() == b.src_vids.size() &&
                           a.group_addrs.size() == b.group_addrs.size() &&
                           a.group_moduli == b.group_moduli;
                }
                if (!same) {
                    record_internal_error(HazeInternalError::KernelValidationFailed,
                                          "kernel re-trace diverged from the cached sub-tape");
                    return std::unexpected(HazeInternalError::KernelValidationFailed);
                }
                // Equivalent: keep the fresh trace (cold-identical) and
                // leave the cache entry in place.
                for (Node &node : *frame->body)
                    graph().append(std::move(node));
            } else {
                // Fresh recording: memoize a copy, then land the body on
                // the main tape exactly as a cold recording would.
                auto sub = std::make_unique<SubTape>();
                sub->name = frame->name;
                sub->key_bytes = frame->key_bytes;
                sub->in_addrs = frame->in_addrs;
                sub->in_vids = frame->in_vids;
                sub->out_addrs = out_addrs;
                sub->out_vids = out_vids;
                sub->body = *frame->body; // copy: the original lands on the tape
                cache_.emplace(frame->key_hash, std::move(sub));
                for (Node &node : *frame->body)
                    graph().append(std::move(node));
            }
        }
    }

    // Outputs become recording outputs on every path (the typed layer's
    // Out<> contract). Outside the cache lock; record_tag_output appends
    // TagOutput nodes through the normal path.
    for (DevAddr a : out_addrs)
        if (auto tagged = record_tag_output(a); !tagged)
            return tagged;
    return {};
}

std::expected<void, HazeInternalError> KernelCache::abort_frame() noexcept {
    HazeLockGuard lock(mutex_);
    if (frame_ == nullptr) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "hazeKernelAbort: no open bracket");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    if (frame_->body != nullptr)
        graph().install_frame_sink(nullptr);
    frame_.reset(); // recorded nodes die with the frame; nothing reached the tape
    return {};
}

void KernelCache::set_memo_enabled(bool enabled) noexcept {
    HazeLockGuard lock(mutex_);
    memo_override_ = enabled ? 1 : 0;
}

void KernelCache::set_validate(bool enabled) noexcept {
    HazeLockGuard lock(mutex_);
    validate_override_ = enabled ? 1 : 0;
}

void KernelCache::reset() noexcept {
    HazeLockGuard lock(mutex_);
    if (frame_ != nullptr && frame_->body != nullptr)
        graph().install_frame_sink(nullptr);
    frame_.reset();
    cache_.clear();
    // Overrides survive reset (mirrors hollow/multithreaded flags in
    // niobium::compiler().reset()); env fallback is unchanged anyway.
}

} // namespace haze
