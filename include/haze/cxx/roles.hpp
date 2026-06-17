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
// Parameter-role markers for kernel signatures. Declaring a parameter
// Out<T>/InOut<T> is what makes it an output: haze::cxx::kernel tags
// every Out/InOut buffer after the body runs, so "forgot hazeTagOutput"
// is structurally impossible inside kernels. The wrappers hold
// references to caller-owned handles — kernels never allocate or
// return buffers (a memoized replay skips the body, so caller-owned
// pre-allocated outputs are the only shape that can replay).
#pragma once

namespace haze::cxx::inline v1 {

template <class T> struct In {
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members) — a role IS a reference
    const T &v;
    const T *operator->() const noexcept { return &v; }
    const T &operator*() const noexcept { return v; }
};

template <class T> struct Out {
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members) — a role IS a reference
    T &v;
    T *operator->() const noexcept { return &v; }
    T &operator*() const noexcept { return v; }
};

template <class T> struct InOut {
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members) — a role IS a reference
    T &v;
    T *operator->() const noexcept { return &v; }
    T &operator*() const noexcept { return v; }
};

} // namespace haze::cxx::inline v1
