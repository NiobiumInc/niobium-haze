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
#include "core/config.hpp"

#include "common/errors.hpp"
#include "common/mod_arith.hpp"
#include "core/device.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <haze/haze_types.h>
#include <niobium/checks.h>
#include <optional>
#include <string>
#include <system_error>

namespace haze {

namespace {

// Floor for the generated aux prime: the replay bridge's OpenFHE template
// synthesis rejects tiny trace moduli (LastPrime overflow below ~2^30).
constexpr uint64_t kAuxPrimeFloor = uint64_t{1} << 30U;

// Smallest prime >= kAuxPrimeFloor with p ≡ 1 (mod 2*ring_dim) — NTT-friendly
// for the hardware twiddle tables and readback synthesis — that is not one of
// the user's moduli. Deterministic given (ring_dim, moduli).
// 0 on exhaustion — unreachable for the validated ring_dim envelope (roughly
// one in ln(2^30) ≈ 21 candidates in the progression is prime), but a bounded
// loop turns a hypothetical generator bug into a clean configure error
// instead of a stall.
uint64_t generate_aux_prime(uint64_t ring_dim, const uint64_t *moduli, size_t count) noexcept {
    constexpr uint64_t kMaxCandidates = 100000;
    const uint64_t step = 2 * ring_dim;
    uint64_t candidate = (((kAuxPrimeFloor + step - 1) / step) * step) + 1;
    for (uint64_t tries = 0; tries < kMaxCandidates; ++tries, candidate += step) {
        if (!is_prime_u64(candidate))
            continue;
        bool collides = false;
        for (size_t i = 0; i < count; ++i) {
            if (moduli[i] == candidate) {
                collides = true;
                break;
            }
        }
        if (!collides)
            return candidate;
    }
    return 0;
}

// Power of two within the device envelope; the upper bound also keeps
// n * sizeof(uint64_t) from wrapping.
bool is_supported_ring_dim(uint64_t n) noexcept {
    if (n < (uint64_t{1} << kMinRingDimExponent) || n > (uint64_t{1} << kMaxRingDimExponent))
        return false;
    return (n & (n - 1)) == 0;
}

// True if `dir` is `base` itself, or one of `base`'s ancestor directories,
// walking up parent_path() until it stops changing (the filesystem root).
// Both arguments must already be resolved to the same canonical form (see
// the caller: std::filesystem::canonical() for `dir`, current_path() --
// already canonical, per POSIX getcwd() -- for `base`), so that a symlink
// component in one but not the other cannot make two paths that are
// physically the same compare unequal, or two that are physically
// different compare equal. Used to
// reject a remove_program_directory target that would delete the caller's
// own working directory or something above it; the walk reaches the root
// unconditionally, so `dir` being a root is itself caught as a degenerate
// ancestor of every absolute path, with no separate root check needed.
bool is_self_or_ancestor(const std::filesystem::path &dir,
                         const std::filesystem::path &base) noexcept {
    for (std::filesystem::path cur = base;; cur = cur.parent_path()) {
        if (cur == dir)
            return true;
        if (cur == cur.parent_path()) // reached the root; no further parent
            return false;
    }
}

// Resolves the program_directory of a remove_program_directory=1 config: the
// absolute, canonical directory hazeDeviceReset may delete, or the reason it
// may not. Every rejection records its detail and returns InvalidArgument.
std::expected<std::filesystem::path, HazeInternalError>
resolve_removable_program_directory(const char *program_directory) noexcept {
    // The <cwd>/<program_name> default is too generic a target for a
    // recursive delete to be trusted with; require the caller to name the
    // directory explicitly.
    if (program_directory == nullptr) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "ReplayConfig::create: remove_program_directory requires an explicit "
                              "program_directory");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    // Rejected explicitly, before any filesystem call: canonical() on
    // an empty path is not a usable directory name, and its behavior
    // on one is not something to lean on (it happens to set an error,
    // per experiment, but that is not documented behavior to rely on).
    if (program_directory[0] == '\0') {
        record_internal_error(
            HazeInternalError::InvalidArgument,
            "ReplayConfig::create: remove_program_directory's program_directory must not "
            "be empty");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    // canonical() requires program_directory to already exist and
    // resolves it fully physically: every symlink in every component is
    // followed, and it fails outright (sets canon_ec) if any component
    // is missing -- there is no lexical fallback for a non-existent
    // component the way weakly_canonical has, and so no partially-
    // resolved result that could ever disagree with what a real
    // filesystem call resolves the same string to later. That gap was
    // real: weakly_canonical falls back to pure lexical processing for
    // the ENTIRE remaining suffix once it hits one missing component,
    // so a REAL symlink further down that same suffix (e.g. "uplink" in
    // "missing/../uplink/cwd", with uplink -> "..") never gets
    // resolved during validation, even though the compiler's own
    // (real) file opens do follow it -- letting the validated path and
    // the one actually written to and later deleted diverge again,
    // exactly the bug class this whole check exists to close. Also
    // rejects a path that resolves to something other than a
    // directory (a plain file, say): a `remove_program_directory`
    // target must be a directory to make any sense as one. Both calls
    // use non-throwing (path/dir, error_code&) overloads -- this
    // function is noexcept, and the throwing overloads raise
    // filesystem_error on failure, which would otherwise escape
    // straight to std::terminate (confirmed: an empty string once
    // reached the throwing absolute() this way and aborted the
    // process).
    std::error_code canon_ec;
    std::filesystem::path dir = std::filesystem::canonical(program_directory, canon_ec);
    std::error_code isdir_ec;
    if (canon_ec || !std::filesystem::is_directory(dir, isdir_ec)) {
        record_internal_error(
            HazeInternalError::InvalidArgument,
            "ReplayConfig::create: remove_program_directory's program_directory must "
            "already exist as a directory");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    std::error_code cwd_ec;
    const std::filesystem::path cwd = std::filesystem::current_path(cwd_ec);
    if (cwd_ec) {
        const std::string detail =
            "ReplayConfig::create: remove_program_directory's program_directory cannot be "
            "checked: the current directory cannot be read: " +
            cwd_ec.message();
        record_internal_error(HazeInternalError::InvalidArgument, detail.c_str());
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    if (is_self_or_ancestor(dir, cwd)) {
        record_internal_error(
            HazeInternalError::InvalidArgument,
            "ReplayConfig::create: remove_program_directory's program_directory must not "
            "be a filesystem root, the current directory, or an ancestor of it");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    return dir;
}

} // namespace

std::expected<FheParams, HazeInternalError> FheParams::create(const hazeFheParams &raw) noexcept {
    // Struct well-formedness: a non-zero count with a null array would be
    // dereferenced below.
    if ((raw.moduli_count > 0 && raw.moduli == nullptr) ||
        (raw.twiddle_count > 0 && raw.twiddle_generators == nullptr))
        return std::unexpected(HazeInternalError::InvalidArgument);
    // Per-argument: ring_dim in the device envelope; modulus count within it.
    if (!is_supported_ring_dim(raw.ring_dim))
        return std::unexpected(HazeInternalError::InvalidArgument);
    // Reserve the last chain slot for the generated aux prime.
    if (raw.moduli_count > static_cast<size_t>(kMaxCiphertextModuli) - 1)
        return std::unexpected(HazeInternalError::InvalidArgument);

    FheParams p;
    p.ring_dim_ = raw.ring_dim;
    for (size_t i = 0; i < raw.moduli_count; ++i) {
        const uint64_t qi = raw.moduli[i];
        if (qi == 0) // per-argument: moduli non-zero
            return std::unexpected(HazeInternalError::InvalidArgument);
        if (!is_prime_u64(qi)) { // per-argument: moduli prime
            record_internal_error(HazeInternalError::CompositeModulus,
                                  "FheParams::create: modulus not prime");
            return std::unexpected(HazeInternalError::CompositeModulus);
        }
        for (size_t j = 0; j < i; ++j) // whole-config: moduli unique
            if (raw.moduli[j] == qi) {
                record_internal_error(HazeInternalError::DuplicateModulus,
                                      "FheParams::create: duplicate modulus");
                return std::unexpected(HazeInternalError::DuplicateModulus);
            }
        p.moduli_[i] = qi;
    }
    // Caller-chosen NTT roots: none (the minimal root for every modulus) or one
    // per caller modulus, twiddle_generators[i] pairing with moduli[i].
    if (raw.twiddle_count != 0 && raw.twiddle_count != raw.moduli_count) {
        const std::string detail =
            "FheParams::create: twiddle_count " + std::to_string(raw.twiddle_count) +
            " is neither 0 nor moduli_count " + std::to_string(raw.moduli_count);
        record_internal_error(HazeInternalError::InvalidArgument, detail.c_str());
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    for (size_t i = 0; i < raw.twiddle_count; ++i) {
        const uint64_t root = raw.twiddle_generators[i];
        if (!niobium::is_primitive_2n_root(root, raw.ring_dim, raw.moduli[i])) {
            const std::string detail =
                "FheParams::create: twiddle_generators[" + std::to_string(i) +
                "] = " + std::to_string(root) + " is not a primitive 2N-th root of unity mod " +
                std::to_string(raw.moduli[i]) + " for N = " + std::to_string(raw.ring_dim);
            record_internal_error(HazeInternalError::InvalidArgument, detail.c_str());
            return std::unexpected(HazeInternalError::InvalidArgument);
        }
    }
    // One program-wide aux prime for hazeIsHalfModulus, generated here and
    // appended as the last chain entry; compute calls never derive it.
    if (raw.moduli_count > 0) {
        p.aux_modulus_ = generate_aux_prime(raw.ring_dim, raw.moduli, raw.moduli_count);
        if (p.aux_modulus_ == 0) {
            record_internal_error(HazeInternalError::InvalidArgument,
                                  "FheParams::create: aux prime generation exhausted");
            return std::unexpected(HazeInternalError::InvalidArgument);
        }
        // In bounds: the count cap above reserves this final slot for the aux.
        p.moduli_[raw.moduli_count] = p.aux_modulus_;
    }
    p.moduli_count_ = static_cast<int>(raw.moduli_count) + (raw.moduli_count > 0 ? 1 : 0);
    p.twiddle_generators_.assign(raw.twiddle_generators,
                                 raw.twiddle_generators + raw.twiddle_count);
    return p;
}

std::expected<ReplayConfig, HazeInternalError>
ReplayConfig::create(const hazeReplayConfig *raw) noexcept {
    ReplayConfig rc; // defaults; a NULL struct or NULL field keeps them
    if (raw == nullptr)
        return rc;
    if (raw->opt_level < 0 || raw->opt_level > kMaxOptLevel) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "ReplayConfig::create: opt_level out of range");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    // Unlike reduced_noise (any non-zero int enables it, the CUDA-flag idiom
    // below), this flag gates an irreversible side effect at hazeDeviceReset —
    // recursive deletion of the program directory — so a stray non-0/1 value
    // (e.g. uninitialized memory read as a bool) is rejected rather than
    // silently treated as "on".
    if (raw->remove_program_directory != 0 && raw->remove_program_directory != 1) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "ReplayConfig::create: remove_program_directory must be 0 or 1");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    // Set only when remove_program_directory == 1: the fully resolved
    // directory that will become rc.program_dir_ below, overriding the raw
    // string. Resolving here, once, and pinning the result (rather than the
    // caller's original string) is what keeps the validated, written, and
    // eventually deleted directory identical no matter what happens to the
    // working directory or a symlink component afterward -- see
    // resolve_removable_program_directory for the failure modes this closes.
    std::optional<std::filesystem::path> resolved_program_dir;
    if (raw->remove_program_directory == 1) {
        auto dir = resolve_removable_program_directory(raw->program_directory);
        if (!dir)
            return std::unexpected(dir.error());
        resolved_program_dir = *dir;
    }
    if (raw->target != nullptr)
        rc.target_ = raw->target;
    if (raw->program_name != nullptr)
        rc.program_name_ = raw->program_name;
    if (raw->program_version != nullptr)
        rc.program_version_ = raw->program_version;
    if (raw->program_description != nullptr)
        rc.program_description_ = raw->program_description;
    if (resolved_program_dir) {
        // The resolved, canonical path becomes the config's program
        // directory -- not the caller's original string -- so
        // bootstrap_compiler() pins this exact directory into the vendor
        // compiler. A relative program_directory (or one crossing a
        // symlink) would otherwise be re-resolved wherever/whenever the
        // compiler next reads it back, which may be a different working
        // directory than the one validated above if the caller changes it
        // between this call and bring-up (CompilerBackend::ensure_initialized).
        rc.program_dir_ = resolved_program_dir->string();
        rc.program_dir_set_ = true;
    } else if (raw->program_directory != nullptr) {
        rc.program_dir_ = raw->program_directory;
        rc.program_dir_set_ = true;
    }
    rc.reduced_noise_ = raw->reduced_noise != 0;
    rc.opt_level_ = raw->opt_level;
    rc.remove_program_directory_ = raw->remove_program_directory != 0;
    return rc;
}

} // namespace haze
