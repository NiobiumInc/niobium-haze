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
// haze::cxx::kernel — the typed kernel-authoring entry point.
//
//   inline constexpr auto axpy = haze::cxx::kernel("axpy",
//       [](In<Mrp> a, In<Mrp> b, Out<Mrp> dst, uint64_t k) -> Status {
//           ...ops::mul_scalar / ops::add over the handles...
//       });
//   Status s = axpy(a, b, out, 4);   // records; out is auto-tagged
//
// The wrapper enforces, AT COMPILE TIME, the contract that keeps a
// kernel body replayable when memoization lands:
//   - stateless callable (captureless lambda) — a capture is hidden
//     state a memoized replay would silently go stale against;
//   - concrete, non-generic, non-mutable operator() — the signature is
//     the kernel's interface and must be introspectable;
//   - every parameter is In/Out/InOut<Srp|Mrp> (traced buffer) or a
//     std::regular, param_hash-able value (memo key);
//   - the body returns void or Status — never a buffer (a replayed
//     call skips the body, so outputs must be caller-pre-allocated
//     Out<> parameters).
// Violations fail concept resolution; the deleted kernel() catch-all
// carries the rules in its diagnostic.
//
// In M2 a kernel executes its body on every call (no memoization);
// after the body succeeds, every Out/InOut buffer is tagged as a
// recording output through one funnel (tag_outputs). hazeFlush stays an
// explicit caller decision.
#pragma once

#include "haze/cxx/error.hpp"
#include "haze/cxx/handles.hpp"
#include "haze/cxx/hash.hpp"
#include "haze/cxx/ops.hpp"
#include "haze/cxx/roles.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>

namespace haze::cxx::inline v1 {

namespace detail {

// ---- signature introspection ----
// A generic lambda's operator() is a template: &F::operator() does not
// resolve. A mutable lambda's operator() is non-const: no
// specialization matches. Both therefore fail `introspectable`.
template <class T> struct function_traits; // primary: undefined

template <class C, class R, class... A> struct function_traits<R (C::*)(A...) const> {
    using result = R;
    using args = std::tuple<A...>;
    static constexpr std::size_t arity = sizeof...(A);
};
template <class C, class R, class... A>
struct function_traits<R (C::*)(A...) const noexcept> : function_traits<R (C::*)(A...) const> {};

template <class F>
concept introspectable = requires { typename function_traits<decltype(&F::operator())>::args; };

// ---- parameter-role classification ----
template <class T> struct role_of {
    using buffer = void;
};
template <class T> struct role_of<In<T>> {
    using buffer = T;
    static constexpr bool is_out = false;
};
template <class T> struct role_of<Out<T>> {
    using buffer = T;
    static constexpr bool is_out = true;
};
template <class T> struct role_of<InOut<T>> {
    using buffer = T;
    static constexpr bool is_out = true;
};

template <class T>
concept is_role = !std::is_void_v<typename role_of<std::remove_cvref_t<T>>::buffer>;

} // namespace detail

template <class T>
concept traced_buffer = std::same_as<T, Srp> || std::same_as<T, Mrp>;

// A traced parameter: In/Out/InOut of exactly Srp or Mrp, by value
// (the role wrapper itself is the value; it holds the reference).
template <class T>
concept traced_param =
    detail::is_role<T> && traced_buffer<typename detail::role_of<std::remove_cvref_t<T>>::buffer> &&
    !std::is_reference_v<T>;

// A memo-key parameter: regular + hashable through param_hash. Required
// from day one so kernel signatures survive memoization unchanged.
template <class T>
concept memo_key = std::regular<std::remove_cvref_t<T>> &&
                   requires(const std::remove_cvref_t<T> &value, uint64_t seed) {
                       {
                           param_hash<std::remove_cvref_t<T>>::mix(value, seed)
                       } -> std::same_as<uint64_t>;
                   };

template <class T>
concept kernel_param = traced_param<T> || memo_key<T>;

// Captureless lambdas only: captured state is invisible to a memo key
// and would make a replayed kernel silently stale.
template <class F>
concept stateless = std::is_empty_v<std::remove_cvref_t<F>>;

namespace detail {

template <class R>
concept kernel_result = std::is_void_v<R> || std::same_as<R, Status>;

template <class Tuple>
inline constexpr bool all_kernel_params = []<std::size_t... I>(std::index_sequence<I...>) {
    return (kernel_param<std::tuple_element_t<I, Tuple>> && ...);
}(std::make_index_sequence<std::tuple_size_v<Tuple>>{});

} // namespace detail

template <class F>
concept kernel_compatible =
    stateless<F> && detail::introspectable<F> &&
    detail::all_kernel_params<typename detail::function_traits<decltype(&F::operator())>::args> &&
    detail::kernel_result<typename detail::function_traits<decltype(&F::operator())>::result>;

namespace detail {

// One actual against one formal:
//  - Out/InOut<T> formals need a MUTABLE lvalue of exactly T;
//  - In<T> formals accept any lvalue of T (const ok);
//  - memo keys convert by value.
// Spelled as a disjunction of conjunctions (not a ternary): concept
// conjunctions short-circuit with substitution-failure-as-false, so
// `role_of<Formal>::is_out` never poisons the non-role branch.
template <class Formal, class Actual>
concept binds_as_out =
    is_role<Formal> && role_of<Formal>::is_out &&
    std::same_as<std::remove_cvref_t<Actual>, typename role_of<Formal>::buffer> &&
    std::is_lvalue_reference_v<Actual> && !std::is_const_v<std::remove_reference_t<Actual>>;

template <class Formal, class Actual>
concept binds_as_in = is_role<Formal> && !role_of<Formal>::is_out &&
                      std::same_as<std::remove_cvref_t<Actual>, typename role_of<Formal>::buffer>;

template <class Formal, class Actual>
concept bindable_one = binds_as_out<Formal, Actual> || binds_as_in<Formal, Actual> ||
                       (!is_role<Formal> && std::convertible_to<Actual, Formal>);

template <class Formals, class... Actuals>
concept bindable = sizeof...(Actuals) == std::tuple_size_v<Formals> &&
                   []<std::size_t... I>(std::index_sequence<I...>) {
                       return (bindable_one<std::tuple_element_t<I, Formals>,
                                            std::tuple_element_t<I, std::tuple<Actuals...>>> &&
                               ...);
                   }(std::index_sequence_for<Actuals...>{});

// Wrap an actual into the formal's role (or pass a memo key through).
template <class Formal, class Actual> decltype(auto) bind_arg(Actual &&actual) {
    if constexpr (is_role<Formal>) {
        return Formal{actual};
    } else {
        return std::forward<Actual>(actual);
    }
}

} // namespace detail

template <class F> class Kernel {
    using traits = detail::function_traits<decltype(&F::operator())>;
    using Formals = typename traits::args;

  public:
    constexpr Kernel(const char *name, F body) noexcept : name_(name), body_(body) {}

    constexpr const char *name() const noexcept { return name_; }

    template <class... Actuals>
        requires detail::bindable<Formals, Actuals &&...>
    Status operator()(Actuals &&...actuals) const {
        // M2: trace the body on every call. (M3 swaps this block for the
        // hazeKernelBegin/End record-or-replay protocol; the call shape
        // users see is final.)
        Status body_status =
            invoke(std::index_sequence_for<Actuals...>{}, std::forward<Actuals>(actuals)...);
        if (!body_status.ok())
            return body_status; // body failed: nothing gets tagged
        // Safe after the forward above: role actuals are lvalues bound by
        // reference (never moved), and tag_outputs reads roles only.
        // NOLINTNEXTLINE(bugprone-use-after-move)
        return tag_outputs(std::index_sequence_for<Actuals...>{}, actuals...);
    }

  private:
    template <std::size_t... I, class... Actuals>
    Status invoke(std::index_sequence<I...> /*seq*/, Actuals &&...actuals) const {
        if constexpr (std::is_void_v<typename traits::result>) {
            body_(detail::bind_arg<std::tuple_element_t<I, Formals>>(
                std::forward<Actuals>(actuals))...);
            return Status{};
        } else {
            return body_(detail::bind_arg<std::tuple_element_t<I, Formals>>(
                std::forward<Actuals>(actuals))...);
        }
    }

    // THE tag funnel: every Out/InOut buffer becomes a recording output
    // here, post-body, in parameter order. Keep this the single place
    // outputs are declared — M3 replaces exactly this body with
    // hazeKernelEnd.
    template <std::size_t... I, class... Actuals>
    Status tag_outputs(std::index_sequence<I...> /*seq*/, Actuals &...actuals) const {
        Status status{};
        const auto tag_one = [&status]<class Formal>(auto &actual) {
            using D = std::remove_cvref_t<Formal>;
            if constexpr (detail::is_role<D>) {
                if constexpr (detail::role_of<D>::is_out) {
                    if (status.ok())
                        status = tag_output(actual);
                }
            }
        };
        (tag_one.template operator()<std::tuple_element_t<I, Formals>>(actuals), ...);
        return status;
    }

    const char *name_;
    F body_;
};

template <class F>
    requires kernel_compatible<F>
constexpr Kernel<F> kernel(const char *name, F body) noexcept {
    return Kernel<F>{name, body};
}

// Catch-all so violations get one readable diagnostic instead of a
// concept-resolution wall. The rules, in prose:
//   stateless (captureless) lambda; concrete non-generic non-mutable
//   operator(); parameters are In/Out/InOut<Srp|Mrp> or std::regular
//   param_hash-able values; return type void or Status.
template <class F> constexpr void kernel(const char *name, F body) = delete;

} // namespace haze::cxx::inline v1
