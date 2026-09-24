#pragma once
// jaal::Frozen — can this value change through a const reference?
//
// C++'s `const` is shallow. A `const T&` still lets you change:
//   * `mutable` fields                  (LazyBytes::bytes() const writes two)
//   * whatever a pointer or handle
//     points at                         (const unique_ptr<X> gives a mutable X&)
//   * whatever a reference field
//     refers to
// Frozen<T> means `const` is DEEP for T: through a const T&, nothing
// reachable from T's own fields can change.
//
// It's a general property, not a threading one. It's what makes a value
// safe to cache, hash, use as a key, snapshot, or share. jaal uses it for
// shared<T> (core/shared.hpp): Shareable = Sendable && Frozen.
//
// What it can't see: a class that changes things through a global or a
// static. No type check can. The CI ban-list catches mutable statics.
//
// Rules, by the std shape table (core/stdshape.hpp) and the walker:
//
//   arithmetic, enum, std::byte          frozen
//   owning containers, optional, variant frozen when elements are
//   unique_ptr, shared_ptr, weak_ptr     NOT: the pointee changes through const
//   raw pointers, references             NOT
//   string_view, span                    NOT: what they point at can change
//   stop_token                           NOT: a stop request changes it
//   jaal::shared<T>                      frozen (only exists for frozen T)
//   plain struct                         every field frozen, and NO `mutable`
//   class jaal can't see inside          NOT, unless it opts in

#include <cstddef>
#include <type_traits>

#include "../meta/diagnose.hpp"
#include "../meta/fields.hpp"
#include "../meta/type_name.hpp"
#include "stdshape.hpp"
#include "walk.hpp"

namespace jaal {

/// Opt a class jaal can't look inside into Frozen. Only for types whose
/// every const member function leaves every observable state unchanged,
/// and that expose nothing mutable through const access.
template <class T>
inline constexpr bool frozen_opt_in = false;

namespace detail {

// A field is `mutable` exactly when a CONST binding still reports it
// non-const. meta::fields_t keeps that distinction (field_types_t drops it).
template <class F>
struct mutable_marker {};

template <class T> inline constexpr bool is_mutable_marker_v = false;
template <class F> inline constexpr bool is_mutable_marker_v<mutable_marker<F>> = true;

template <class F>
using frozen_field = std::conditional_t<
    std::is_reference_v<F> || std::is_const_v<F>,
    std::remove_const_t<F>,          // normal field (or reference field: rejected later)
    mutable_marker<F>>;              // a `mutable` field: always rejected

struct frozen_policy {
    template <class T>
    using field_list = meta::transform_t<frozen_field, meta::fields_t<T>>;

    template <class T>
    static consteval auto classify() {
        namespace d = walk::decision;
        namespace s = stdshape;
        using U = std::remove_cv_t<T>;
        using S = s::shape_t<U>;

        if constexpr (is_mutable_marker_v<U>)
            return d::reject{};                                // a `mutable` field
        else if constexpr (frozen_opt_in<U>)                   return d::accept{};
        else if constexpr (std::is_reference_v<T>
                           || std::is_pointer_v<U>
                           || std::is_member_pointer_v<U>
                           || std::is_function_v<U>)           return d::reject{};
        else if constexpr (std::is_arithmetic_v<U>
                           || std::is_enum_v<U>
                           || std::is_same_v<U, std::byte>)    return d::accept{};
        else if constexpr (std::is_union_v<U>)                 return d::reject{};
        // std library, by shape
        else if constexpr (std::is_same_v<S, s::value>)        return d::accept{};
        else if constexpr (std::is_same_v<S, s::view>
                           || std::is_same_v<S, s::sync_handle>
                           || s::is_shared_shape<S>
                           || s::is_unique_shape<S>)           return d::reject{};
        else if constexpr (s::is_owns_shape<S> || s::is_value_of_shape<S>)
            return typename s::elements<S>::template as<d::into>{};
        else                                                   return d::structural{};
    }
};

template <class T>
using frozen_run = walk::run<frozen_policy, std::remove_cv_t<T>>;

template <class C> struct unmark { using type = C; };
template <class F> struct unmark<mutable_marker<F>> { using type = F; };

}  // namespace detail

template <class T>
inline constexpr bool frozen_v =
    std::is_reference_v<T> ? false : detail::frozen_run<T>::ok;

/// The innermost type that makes T non-Frozen; void if T is Frozen.
/// For a `mutable` field, this is the field's type.
template <class T>
using frozen_culprit_t =
    typename detail::unmark<typename detail::frozen_run<T>::culprit>::type;

/// True when T's non-Frozen-ness comes from a `mutable` field somewhere.
template <class T>
inline constexpr bool frozen_fails_on_mutable_v =
    detail::is_mutable_marker_v<typename detail::frozen_run<T>::culprit>;

template <class T>
concept Frozen = std::is_object_v<T> && frozen_v<T>;

/// A sentence explaining why T isn't Frozen, for static_assert.
template <class T>
consteval auto frozen_reason() {
    using C  = frozen_culprit_t<T>;
    using CS = detail::stdshape::shape_t<std::remove_cv_t<C>>;
    meta::message<512> m;
    m += "jaal: '";
    m.append_short(meta::type_name<T>());
    m += "' is not Frozen";
    if constexpr (std::is_void_v<C>) {
        return m;
    } else {
        if constexpr (frozen_fails_on_mutable_v<T>) {
            m += ": it has a `mutable` field of type '";
            m.append_short(meta::type_name<C>());
            m += "', which a const method can still change";
            return m;
        } else {
            if constexpr (!std::is_same_v<C, std::remove_cv_t<T>>) {
                m += ": it contains '";
                m.append_short(meta::type_name<C>());
                m += "', ";
            } else {
                m += ": ";
            }
            if constexpr (detail::stdshape::is_unique_shape<CS>
                          || detail::stdshape::is_shared_shape<CS>)
                m += "a pointer-like owner; what it points at can change "
                     "through a const reference";
            else if constexpr (std::is_pointer_v<C> || std::is_reference_v<C>)
                m += "a pointer; what it points at can change through a const reference";
            else if constexpr (detail::stdshape::is_view_shape<CS>)
                m += "a view; the memory it points at can change";
            else if constexpr (std::is_same_v<CS, detail::stdshape::sync_handle>)
                m += "a handle whose state other threads can change";
            else if constexpr (std::is_union_v<C>)
                m += "a union jaal can't see inside";
            else if constexpr (std::is_class_v<C> && !meta::aggregate_struct<C>)
                m += "a class jaal can't see inside; if const access can never "
                     "change it, specialise jaal::frozen_opt_in for it";
            else
                m += "something that can change through a const reference";
            return m;
        }
    }
}

template <class T>
consteval void require_frozen() {
    if constexpr (!Frozen<T>) {
#if defined(__cpp_static_assert) && __cpp_static_assert >= 202306L
        static_assert(Frozen<T>, frozen_reason<T>());
#else
        static_assert(Frozen<T>, "jaal: type is not Frozen (see frozen_culprit_t)");
#endif
    }
}

}  // namespace jaal
