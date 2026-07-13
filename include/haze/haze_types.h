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
#ifndef HAZE_TYPES_H
#define HAZE_TYPES_H

// haze_types.h is a C-compatible public header (consumed by both C
// and C++ TUs); the C-style stdint.h / stddef.h forms must stay.
// NOLINTNEXTLINE(modernize-deprecated-headers)
#include <stddef.h>
// NOLINTNEXTLINE(modernize-deprecated-headers)
#include <stdint.h>

// Symbol visibility.
#ifdef _WIN32
#ifdef HAZE_BUILDING_LIBRARY
#define HAZE_API __declspec(dllexport)
#else
#define HAZE_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define HAZE_API __attribute__((visibility("default")))
#else
#define HAZE_API
#endif

// Marks a return value that must not be silently discarded.
#ifdef __cplusplus
#define HAZE_NODISCARD [[nodiscard]]
#elif defined(__GNUC__) || defined(__clang__)
#define HAZE_NODISCARD __attribute__((warn_unused_result))
#else
#define HAZE_NODISCARD
#endif

// Expands to noexcept in C++, empty in C (for use in public headers).
#ifdef __cplusplus
#define HAZE_NOEXCEPT noexcept
#else
#define HAZE_NOEXCEPT
#endif

// Opaque handle types (distinct pointer types, not void*).
typedef struct haze_stream_s *hazeStream_t;
typedef struct haze_event_s *hazeEvent_t;
typedef struct haze_graph_s *hazeGraph_t;
typedef struct haze_exec_s *hazeGraphExec_t;

// The four typedef-enum types below are the public C ABI: C has no `enum class` and a
// `: uint8_t` underlying type is C23-only, so the checks flagging unscoped enums and oversized
// base types are silenced for the entire ABI block.
// NOLINTBEGIN(cppcoreguidelines-use-enum-class,performance-enum-size)

// Only user-actionable conditions get their own code; anything signalling "haze itself is
// broken" maps to HAZE_ERROR_INTERNAL (loggable, not recoverable), whose rich classification
// lives in the internal `HazeInternalError` enum and surfaces via the HAZE_DEBUG=1 stderr log.
typedef enum {
    // Values are explicit: the catch-all is a single high bit, which is not
    // sequential, so readability-enum-initial-value requires all-or-none.
    HAZE_SUCCESS = 0,
    HAZE_ERROR_INVALID_VALUE = 1,      // argument violated the documented contract
    HAZE_ERROR_OUT_OF_MEMORY = 2,      // allocator could not satisfy the request
    HAZE_ERROR_NOT_SUPPORTED = 3,      // operation is not implemented for this build / target
    HAZE_ERROR_CONFIGERR = 4,          // ring_dim / modulus / target not configured
    HAZE_ERROR_UNKNOWN_ADDRESS = 5,    // DevAddr not in the allocator's table
    HAZE_ERROR_NO_DATA = 6,            // reserved / not normally returned; see SOURCE_UNAVAILABLE
    HAZE_ERROR_SIZE_MISMATCH = 7,      // size != configured polynomial size (ring_dim * 8 bytes)
    HAZE_ERROR_SOURCE_UNAVAILABLE = 8, // compute / D2D read from an address that was never
                                       // written (H2D) and holds no recorded result
    HAZE_ERROR_NOT_FLUSHED = 9, // D2H of an untagged / unflushed address: tag output + hazeFlush
    // User-actionable codes stay sequential below this single high bit (2^10), so
    // `err & HAZE_ERROR_INTERNAL` detects an internal error without enumerating every code.
    HAZE_ERROR_INTERNAL = 1024, // haze invariant broke or backend failed; see HAZE_DEBUG log
} hazeError_t;

// DMA direction: numbering matches CUDA's cudaMemcpyKind value-for-value, so CUDA ports can
// cast cudaMemcpyKind to hazeMemcpyKind without silently swapping directions.
typedef enum {
    HAZE_MEMCPY_HOST_TO_DEVICE = 1,
    HAZE_MEMCPY_DEVICE_TO_HOST = 2,
    HAZE_MEMCPY_DEVICE_TO_DEVICE = 3,
} hazeMemcpyKind;

typedef struct {
    char name[256];
    size_t totalGlobalMem; /* bytes; matches cudaDeviceProp::totalGlobalMem */
    int numRegisters;
    int numSupportedRingDims;          /* HAZE-specific, FHE param */
    int supportedRingDimExponents[32]; /* HAZE-specific */
    int maxCiphertextModuli;           /* HAZE-specific */
    int numHBMBanks;                   /* HAZE-specific */
} hazeDeviceProp;

// Memory-type values stored in hazePointerAttributes::type; numbering matches CUDA's
// cudaMemoryType for portability of CUDA-to-HAZE ports.
typedef enum {
    HAZE_MEMORY_TYPE_UNREGISTERED = 0,
    HAZE_MEMORY_TYPE_HOST = 1,
    HAZE_MEMORY_TYPE_DEVICE = 2,
} hazeMemoryType;
// NOLINTEND(cppcoreguidelines-use-enum-class,performance-enum-size)

typedef struct {
    hazeMemoryType type;
    int device;
    void *devicePointer;
    void *hostPointer;
} hazePointerAttributes;

// One-shot device configuration, filled by the caller and passed to
// hazeConfigureDevice(); haze copies what it needs and retains no pointer.

// FHE-scheme parameters. ring_dim is required (a power of two in the device
// envelope). moduli lists moduli_count ciphertext-modulus primes (contiguous,
// non-zero, unique); twiddle_generators is optional NTT metadata. A *_count of
// 0 permits the matching pointer to be NULL.
typedef struct {
    uint64_t ring_dim;
    const uint64_t *moduli;
    size_t moduli_count;
    const uint64_t *twiddle_generators;
    size_t twiddle_count;
} hazeFheParams;

// Hardware/replay configuration. Every field is optional: a NULL string takes
// its default (target "local"; program "haze" / "0.1" / "HAZE runtime";
// program directory <cwd>/<program_name>) and the int flags default off. Pass
// a NULL hazeReplayConfig* to hazeConfigureDevice to accept all defaults.
typedef struct {
    const char *target;
    const char *program_name;
    const char *program_version;
    const char *program_description;
    const char *program_directory;
    int montgomery;
    int bit_reversal;
    int reduced_noise;
} hazeReplayConfig;

// CRT basis-conversion parameter structs: each describes a multi-residue polynomial (MRP)
// operation by listing its prime moduli plus auxiliary metadata, where every `*_base` array
// holds `uint64_t` primes matching those passed in hazeFheParams::moduli; polynomial
// pointers travel via the matching function's `dst` / `src` arguments, never inside the struct.

// hazeBasisConvert: convert an MRP from src_base to dst_base.
//   src: array of src_base_len input poly pointers.
//   dst: array of dst_base_len output poly pointers.
typedef struct {
    const uint64_t *src_base;
    size_t src_base_len;
    const uint64_t *dst_base;
    size_t dst_base_len;
} hazeBasisConvertParams;

// hazeModDown: rescale by removing rescale_base from src_base.
//   src: array of src_base_len input poly pointers.
//   dst: array of (src_base_len - rescale_base_len) output poly pointers,
//        ordered by src_base \ rescale_base in src_base's original order.
//   rescale_base: a proper subset of src_base.
typedef struct {
    const uint64_t *src_base;
    size_t src_base_len;
    const uint64_t *rescale_base;
    size_t rescale_base_len;
} hazeModDownParams;

// hazeModUp: digit decomposition for hybrid key switching.
//   src: array of src_base_len input poly pointers.
//   dst: flat array of digit_count * (src_base_len + p_base_len) output
//        poly pointers, written digit-major. Each digit covers the union
//        src_base ∪ p_base, in src_base order followed by p_base order.
//   digit_bases: concatenation of digit_count sub-bases.
//   digit_bases_total_len: total length of digit_bases (must equal
//                          digit_base_lens[0] + ... + digit_base_lens[digit_count-1]).
//   digit_base_lens: length of each sub-base.
//   p_base: auxiliary primes appended to every digit's output base.
typedef struct {
    const uint64_t *src_base;
    size_t src_base_len;
    const uint64_t *digit_bases;
    size_t digit_bases_total_len;
    const size_t *digit_base_lens;
    size_t digit_count;
    const uint64_t *p_base;
    size_t p_base_len;
} hazeModUpParams;

#endif /* HAZE_TYPES_H */
