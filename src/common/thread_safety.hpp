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

// Clang thread-safety analysis annotations. Compile-time, zero runtime cost.
// Enabled by passing -Wthread-safety to clang; expand to nothing on other
// compilers. Documented at:
// https://clang.llvm.org/docs/ThreadSafetyAnalysis.html
//
// Usage in HAZE:
//   - Mark mutex members with HAZE_CAPABILITY("mutex").
//   - Mark fields the mutex protects with HAZE_GUARDED_BY(mutex_).
//   - Mark methods that require the caller to hold the lock with
//     HAZE_REQUIRES(mutex_) — typically the *_locked variants.
//   - Mark methods that take the lock themselves (and so must not be
//     called with the lock already held) with HAZE_EXCLUDES(mutex_).
//   - Mark RAII lock-holders with HAZE_SCOPED_CAPABILITY on the class,
//     HAZE_ACQUIRE(mutex) on the constructor, HAZE_RELEASE() on the
//     destructor.
//   - Mark accessor methods that hand out a reference to the underlying
//     capability with HAZE_RETURN_CAPABILITY(field).
//
// HAZE lock policy — the permitted nesting edges, in full:
//   KernelCache::mutex_ -> Graph::mutex_   (frame sink install, clone appends)
//   Config::mutex_      -> DeviceAllocator::mutex_  (set_ring_dimension)
// Everything else is a leaf: record helpers complete their allocator
// call before Graph::append, and lower.cpp's finalize mutex serializes
// flushes without any haze lock held during lowering. No haze code may
// add a nesting edge without updating this list; the reverse of either
// edge above deadlocks. TSAN is the runtime backstop for the lock-free
// BindingTable, which carries no TSA annotations by design.

// Clang's thread-safety attributes are exposed as GNU-style
// __attribute__((...)), not C++11 [[clang::...]]. The macro names
// here mirror the spellings shown in the clang TSA documentation.
#ifdef __clang__
#define HAZE_CAPABILITY(s) __attribute__((capability(s)))
#define HAZE_SCOPED_CAPABILITY __attribute__((scoped_lockable))
#define HAZE_GUARDED_BY(x) __attribute__((guarded_by(x)))
#define HAZE_REQUIRES(...) __attribute__((requires_capability(__VA_ARGS__)))
#define HAZE_EXCLUDES(...) __attribute__((locks_excluded(__VA_ARGS__)))
#define HAZE_ACQUIRE(...) __attribute__((acquire_capability(__VA_ARGS__)))
#define HAZE_RELEASE(...) __attribute__((release_capability(__VA_ARGS__)))
#define HAZE_RETURN_CAPABILITY(x) __attribute__((lock_returned(x)))
#else
#define HAZE_CAPABILITY(s)
#define HAZE_SCOPED_CAPABILITY
#define HAZE_GUARDED_BY(x)
#define HAZE_REQUIRES(...)
#define HAZE_EXCLUDES(...)
#define HAZE_ACQUIRE(...)
#define HAZE_RELEASE(...)
#define HAZE_RETURN_CAPABILITY(x)
#endif

#include <mutex>

namespace haze {

// Thin wrapper around std::mutex annotated as a clang TSA capability.
// libstdc++'s std::mutex carries no annotations, so without this
// wrapper the TSA attributes cannot identify the underlying mutex as
// the capability they protect. Used by EpochState::mutex_ and
// DeviceAllocator::mutex_.
class HAZE_CAPABILITY("mutex") HazeMutex {
  public:
    HazeMutex() = default;
    HazeMutex(const HazeMutex &) = delete;
    HazeMutex &operator=(const HazeMutex &) = delete;

    void lock() HAZE_ACQUIRE() { impl_.lock(); }
    void unlock() HAZE_RELEASE() { impl_.unlock(); }

  private:
    std::mutex impl_;
};

// RAII scoped lock guard for HazeMutex, equivalent to std::lock_guard
// but with TSA annotations propagated through HazeMutex's lock/unlock.
// libstdc++'s std::lock_guard does not carry capability annotations;
// without this wrapper TSA cannot tell that `std::lock_guard lock(m)`
// holds the capability for the rest of the scope.
class HAZE_SCOPED_CAPABILITY HazeLockGuard {
  public:
    explicit HazeLockGuard(HazeMutex &m) HAZE_ACQUIRE(m) : mutex_(m) { mutex_.lock(); }
    ~HazeLockGuard() HAZE_RELEASE() { mutex_.unlock(); }

    HazeLockGuard(const HazeLockGuard &) = delete;
    HazeLockGuard &operator=(const HazeLockGuard &) = delete;

  private:
    HazeMutex &mutex_;
};

} // namespace haze
