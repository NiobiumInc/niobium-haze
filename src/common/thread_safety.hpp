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

// Clang thread-safety analysis annotations: compile-time, zero runtime cost,
// enabled by -Wthread-safety and expanding to nothing on other compilers
// (https://clang.llvm.org/docs/ThreadSafetyAnalysis.html).
//
// Usage: HAZE_CAPABILITY marks a mutex, HAZE_GUARDED_BY the fields it protects,
// HAZE_REQUIRES(mutex_) methods the caller must hold the lock for (the *_locked
// variants), HAZE_EXCLUDES(mutex_) methods that take it themselves (so must not be
// called holding it); HAZE_SCOPED_CAPABILITY + HAZE_ACQUIRE/HAZE_RELEASE annotate
// RAII lock-holders and HAZE_RETURN_CAPABILITY accessors returning the capability.
//
// Lock order (canonical statement — other files reference this one); HAZE's
// mutexes form a DAG, acquire only along its edges:
//
//   EpochState::mutex_ -> DeviceAllocator::mutex_
//
// Config carries no lock. The FHE params and replay config are immutable values
// built by the single explicit hazeConfigureDevice() (from caller-owned structs,
// via transient local builders) and installed into DeviceState; only the control
// plane (hazeConfigureDevice, hazeDeviceReset) writes them. The contract, like
// CUDA's device setup, is that the control plane runs single-threaded and is not
// concurrent with compute: configure happens before the first compute, and
// hazeDeviceReset is not called while compute is in flight. Under that contract
// the compute path only ever READS the frozen config — never finalizes, never
// mutates — so it needs no lock or atomics (CompilerBackend::init_mutex_'s
// acquire/release at bring-up orders the freeze before every compute read).
// init_mutex_ serializes bring-up but acquires no other lock. The allocator is a
// leaf: allocator code must not call into any other component while holding its
// lock; TSAN is the runtime backstop.

// Clang's thread-safety attributes are GNU-style __attribute__((...)), not C++11
// [[clang::...]]; the macro names mirror the clang TSA documentation spellings.
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

// Thin std::mutex wrapper annotated as a clang TSA capability, because libstdc++'s
// std::mutex carries no annotations for TSA to identify; used by EpochState::mutex_
// and DeviceAllocator::mutex_.
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

// RAII scoped lock guard for HazeMutex, like std::lock_guard but propagating TSA
// annotations through HazeMutex's lock/unlock, which libstdc++'s std::lock_guard
// lacks so TSA cannot tell it holds the capability for the scope.
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
