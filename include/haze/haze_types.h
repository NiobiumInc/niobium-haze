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

// ---------------------------------------------------------------------------
// Symbol visibility
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Opaque handle types (distinct pointer types, not void*)
// ---------------------------------------------------------------------------

typedef struct haze_stream_s *hazeStream_t;
typedef struct haze_event_s *hazeEvent_t;
typedef struct haze_graph_s *hazeGraph_t;
typedef struct haze_exec_s *hazeGraphExec_t;

// ---------------------------------------------------------------------------
// Error codes
// ---------------------------------------------------------------------------

// The four typedef-enum types below are the public C ABI. C does not
// have `enum class`, and adding a `: uint8_t` underlying type is C23-
// only and not portable, so the C++ Core Guidelines / performance
// recommendations that flag unscoped enums and oversized base types
// are silenced for the entire ABI block.
// NOLINTBEGIN(cppcoreguidelines-use-enum-class,performance-enum-size)

// Only user-actionable conditions get their own code. Anything that
// signals "haze itself is broken" maps to HAZE_ERROR_INTERNAL — callers
// can log it but can't recover; the rich classification lives in
// haze's internal `HazeInternalError` enum and surfaces via the
// HAZE_DEBUG=1 stderr log.
typedef enum {
    HAZE_SUCCESS = 0,
    HAZE_ERROR_INVALID_VALUE,      // argument violated the documented contract
    HAZE_ERROR_OUT_OF_MEMORY,      // allocator could not satisfy the request
    HAZE_ERROR_NOT_SUPPORTED,      // operation is not implemented for this build / target
    HAZE_ERROR_CONFIGERR,          // ring_dim / modulus / target not configured
    HAZE_ERROR_UNKNOWN_ADDRESS,    // DevAddr not in the allocator's table
    HAZE_ERROR_NO_DATA,            // address allocated but never written
    HAZE_ERROR_ALLOC_TOO_SMALL,    // allocation size < polynomial size
    HAZE_ERROR_SOURCE_UNAVAILABLE, // compute / D2D source has no shadow + no poly_map_
    HAZE_ERROR_NOT_FLUSHED,        // D2H of an untagged / unflushed address: tag output + hazeFlush
    HAZE_ERROR_INTERNAL,           // haze invariant broke or backend failed; see HAZE_DEBUG log
} hazeError_t;

// ---------------------------------------------------------------------------
// DMA direction
// Numbering matches CUDA's cudaMemcpyKind value-for-value so callers
// porting from CUDA can cast cudaMemcpyKind to hazeMemcpyKind without
// silently swapping directions.
// ---------------------------------------------------------------------------

typedef enum {
    HAZE_MEMCPY_HOST_TO_DEVICE = 1,
    HAZE_MEMCPY_DEVICE_TO_HOST = 2,
    HAZE_MEMCPY_DEVICE_TO_DEVICE = 3,
} hazeMemcpyKind;

// ---------------------------------------------------------------------------
// Device properties.
// ---------------------------------------------------------------------------

typedef struct {
    char name[256];
    size_t totalGlobalMem; /* bytes; matches cudaDeviceProp::totalGlobalMem */
    int numRegisters;
    int numSupportedRingDims;          /* HAZE-specific, FHE param */
    int supportedRingDimExponents[32]; /* HAZE-specific */
    int maxCiphertextModuli;           /* HAZE-specific */
    int numHBMBanks;                   /* HAZE-specific */
} hazeDeviceProp;

// ---------------------------------------------------------------------------
// Pointer attribute query result
// ---------------------------------------------------------------------------

// Memory-type values stored in hazePointerAttributes::type. Numbering
// matches CUDA's cudaMemoryType for portability of CUDA-to-HAZE ports.
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

// CRT basis-conversion parameter structs.
//
// Each struct describes a multi-residue polynomial (MRP) operation by
// listing its prime moduli (the CRT base) and any auxiliary metadata.
// All `*_base` arrays are arrays of `uint64_t` prime values that match
// the primes passed to hazeSetCiphertextModulus.
//
// Polynomial pointers are passed via the matching public function's
// `dst` / `src` arguments — never inside the params struct.

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
