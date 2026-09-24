#pragma once
// jaal::Sendable — may this value be moved to another thread?
//
// Design: CONCURRENCY.md §4.2. A value is Sendable when, after it's moved to
// another thread, it holds nothing that the sending thread can still reach
// or free. So: no raw pointers, no references, no views, no shared mutable
// ownership. Owned values and deep copies are fine.
//
// Sendable is one POLICY over the shared structural walker (core/walk.hpp)
// and the std-library shape table (core/stdshape.hpp); Frozen is another.
// This file only says what Sendable accepts:
//
//   * std types by shape: owning containers if their elements are,
//     unique_ptr if its pointee is (ownership moves with it), views and
//     shared_ptr/weak_ptr never, stop_token yes (built for threads)
//   * plain structs field by field, recursively, recursive types included
//   * anything jaal can't see inside (private fields, unions): no, unless
//     it opts in
//
// Opt-in, for a class that owns everything it holds:
//
//     template <> inline constexpr bool jaal::sendable_opt_in<MyHandle> = true;
//
// Every opt-in is a claim a human made. Keep them rare, grep-able, and
// commented with why the type is safe.

#include <concepts>
#include <cstddef>
#include <type_traits>

#include "../meta/diagnose.hpp"
#include "../meta/fields.hpp"
#include "../meta/type_name.hpp"
#include "stdshape.hpp"
#include "walk.hpp"

namespace jaal {

// ── user extension points ────────────────────────────────────────────────

/// Opt a class jaal can't look inside into Sendable. Use only when the
/// type owns everything it holds and a moved-from instance shares nothing
/// with the moved-to one.
template <class T>
inline constexpr bool sendable_opt_in = false;

/// Opt a type OUT, even if its structure looks fine. For a struct whose
/// fields are all owned values but whose meaning is thread-bound (a handle
/// valid only on the thread that made it).
template <class T>
inline constexpr bool sendable_opt_out = false;

// ── the policy ───────────────────────────────────────────────────────────
namespace detail {

struct sendable_policy {
    // What a move carries: every field's type, cv stripped. A `mutable`
    // field is still owned, so it's fine to MOVE (sharing is Frozen's job).
    template <class T>
    using field_list = meta::field_types_t<T>;

    template <class T>
    static consteval auto classify() {
        namespace d = walk::decision;
        namespace s = stdshape;
        using U = std::remove_cv_t<T>;
        using S = s::shape_t<U>;

        if constexpr (sendable_opt_out<U>)                     return d::reject{};
        else if constexpr (sendable_opt_in<U>)                 return d::accept{};
        else if constexpr (std::is_reference_v<T>)             return d::reject{};
        else if constexpr (std::is_pointer_v<U>
                           || std::is_member_pointer_v<U>
                           || std::is_function_v<U>)           return d::reject{};
        else if constexpr (std::is_arithmetic_v<U>
                           || std::is_enum_v<U>
                           || std::is_same_v<U, std::byte>)    return d::accept{};
        else if constexpr (std::is_union_v<U>)                 return d::reject{};
        // std library, by shape
        else if constexpr (std::is_same_v<S, s::value>
                           || std::is_same_v<S, s::sync_handle>) return d::accept{};
        else if constexpr (std::is_same_v<S, s::view>)         return d::reject{};
        else if constexpr (s::is_shared_shape<S>)              return d::reject{};
        else if constexpr (s::is_owns_shape<S> || s::is_unique_shape<S>
                           || s::is_value_of_shape<S>)
            return typename s::elements<S>::template as<d::into>{};
        // user types: look inside if we can, otherwise not proven
        else                                                   return d::structural{};
    }
};

template <class T>
using sendable_run = walk::run<sendable_policy, std::remove_cv_t<T>>;

}  // namespace detail

// ── public API ───────────────────────────────────────────────────────────

template <class T>
inline constexpr bool sendable_v =
    std::is_reference_v<T> ? false : detail::sendable_run<T>::ok;

/// The innermost type that makes T non-Sendable; void if T is Sendable.
template <class T>
using sendable_culprit_t = typename detail::sendable_run<T>::culprit;

template <class T>
concept Sendable =
    std::is_object_v<T>          // no references, no functions, no void
    && std::movable<T>
    && sendable_v<T>;

/// A sentence explaining why T isn't Sendable, for static_assert.
template <class T>
consteval auto sendable_reason() {
    using C = sendable_culprit_t<T>;
    using CS = detail::stdshape::shape_t<std::remove_cv_t<C>>;
    constexpr bool self = std::is_same_v<C, std::remove_cv_t<T>>;
    meta::message<512> m;
    m += "jaal: '";
    m.append_short(meta::type_name<T>());
    m += "' is not Sendable";
    if constexpr (std::is_void_v<C>) {
        if constexpr (!std::movable<T>) m += ": it can't be moved";
        return m;
    } else {
        // "it contains 'X', <why>"  or, when T is the culprit,  ": <why>"
        if constexpr (!self) {
            m += ": it contains '";
            m.append_short(meta::type_name<C>());
            m += "', ";
        } else {
            m += ": ";
        }
        if constexpr (std::is_pointer_v<C> || std::is_reference_v<C>)
            m += "a pointer, which may point at memory another thread frees";
        else if constexpr (detail::stdshape::is_view_shape<CS>)
            m += "a view that borrows memory another thread may free";
        else if constexpr (detail::stdshape::is_shared_shape<CS>)
            m += "shared ownership of data another thread can change; "
                 "use jaal::shared<T> for immutable sharing";
        else if constexpr (std::is_union_v<C>)
            m += "a union jaal can't see inside (use std::variant)";
        else if constexpr (sendable_opt_out<std::remove_cv_t<C>>)
            m += "a type marked thread-bound (sendable_opt_out)";
        else if constexpr (std::is_class_v<C> && !meta::aggregate_struct<C>)
            m += "a class jaal can't see inside; if it owns everything it "
                 "holds, specialise jaal::sendable_opt_in for it";
        else
            m += "not safe to move to another thread";
        return m;
    }
}

/// Checks T, with a readable error. Use at the outermost API boundary.
template <class T>
consteval void require_sendable() {
    if constexpr (!Sendable<T>) {
#if defined(__cpp_static_assert) && __cpp_static_assert >= 202306L
        static_assert(Sendable<T>, sendable_reason<T>());
#else
        static_assert(Sendable<T>, "jaal: type is not Sendable (see sendable_culprit_t)");
#endif
    }
}

}  // namespace jaal
