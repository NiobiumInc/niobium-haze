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

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Symbol visibility
// ---------------------------------------------------------------------------

#if defined(_WIN32)
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
#if defined(__cplusplus)
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
// Hardware status register bit mappings: dmemerr, imemerr, instrerr,
// configerr, iseqerr.
// ---------------------------------------------------------------------------

typedef enum {
    HAZE_SUCCESS = 0,
    HAZE_ERROR_INVALID_HANDLE,
    HAZE_ERROR_INVALID_VALUE,
    HAZE_ERROR_OUT_OF_MEMORY,
    HAZE_ERROR_NOT_SUPPORTED,
    HAZE_ERROR_NOT_READY,
    HAZE_ERROR_LAUNCH_FAILURE,
    HAZE_ERROR_DMEMERR,
    HAZE_ERROR_IMEMERR,
    HAZE_ERROR_INSTRERR,
    HAZE_ERROR_CONFIGERR,
    HAZE_ERROR_ISEQERR,
} hazeError_t;

// ---------------------------------------------------------------------------
// Memory-compute overlap capability flags
// ---------------------------------------------------------------------------

typedef enum {
    HAZE_OVERLAP_NONE = 0,
    HAZE_OVERLAP_LOAD = 1u << 0,
    HAZE_OVERLAP_STORE = 1u << 1,
    HAZE_OVERLAP_FULL = (1u << 0) | (1u << 1),
} hazeOverlapFlags;

// ---------------------------------------------------------------------------
// DMA direction
// Numbering matches CUDA's cudaMemcpyKind value-for-value so callers
// porting from CUDA can cast cudaMemcpyKind to hazeMemcpyKind without
// silently swapping directions.
// ---------------------------------------------------------------------------

typedef enum {
    HAZE_MEMCPY_HOST_TO_HOST = 0,
    HAZE_MEMCPY_HOST_TO_DEVICE = 1,
    HAZE_MEMCPY_DEVICE_TO_HOST = 2,
    HAZE_MEMCPY_DEVICE_TO_DEVICE = 3,
    HAZE_MEMCPY_DEFAULT = 4,
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
    hazeOverlapFlags overlapCaps;      /* HAZE-specific */
    int instructionFIFODepth;          /* HAZE-specific */
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
    HAZE_MEMORY_TYPE_MANAGED = 4
} hazeMemoryType;

typedef struct {
    hazeMemoryType type;
    int device;
    void *devicePointer;
    void *hostPointer;
} hazePointerAttributes;

// RYANPR: Follow the comment structure from task 3: no banners, no definitions of sections since the public repo won't have access to our planning docs.
// ---------------------------------------------------------------------------
// CRT basis conversion parameters (Section 5.4)
// ---------------------------------------------------------------------------
// Each struct describes the moduli bases of a multi-residue polynomial
// (MRP) operation. Polynomial pointers are passed via the matching
// public function's `dst` / `src` arguments; the params struct carries
// scalar / array metadata only.

// Ideally use consistent `//` comment syntax throughout.
/* `src` is an array of length `src_base_len`, one poly pointer per src
 * modulus. `dst` is an array of length `dst_base_len`, one poly pointer
 * per dst modulus. */
typedef struct {
    // RYANPR: What is this base pointer (both source and destination) on all of these structs for when we pass in src and destination in the function call? I assume these are the primes.
    const uint64_t *src_base;
    size_t src_base_len;
    const uint64_t *dst_base;
    size_t dst_base_len;
    uint64_t ring_dim;
} hazeBasisConvertParams;

/* `src` is an array of length `src_base_len`. `dst` is an array of
 * length `src_base_len - rescale_base_len`, ordered by `src_base \
 * rescale_base` in src_base's original order. */
typedef struct {
    const uint64_t *src_base;
    size_t src_base_len;
    const uint64_t *rescale_base; /* subset of src_base, in src_base's order */
    size_t rescale_base_len;
    uint64_t ring_dim;
} hazeModDownParams;

/* `src` is an array of length `src_base_len`. `dst` is a flat array of
 * length `digit_count * (src_base_len + p_base_len)`, written
 * digit-major: digit 0's polys (in src_base order, then p_base order),
 * then digit 1's polys, etc. `digit_bases` is a concatenation of
 * `digit_count` sub-bases; `digit_base_lens[i]` gives the length of the
 * i-th sub-base. hazeModUp emits dig_decomp, producing an MRPArray of
 * length digit_count where each element has base (src_base ∪ p_base). */
typedef struct {
    const uint64_t *src_base;
    size_t src_base_len;
    const uint64_t *digit_bases;
    const size_t *digit_base_lens;
    size_t digit_count;
    const uint64_t *p_base;
    size_t p_base_len;
    uint64_t ring_dim;
} hazeModUpParams;

#endif /* HAZE_TYPES_H */
