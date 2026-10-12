#pragma once
// jaal::JoinFree — does destroying a T wait for another thread?
//
// Every blocking wait in jaal's API takes `const jaal::unlocked&`, so a wait
// under a lock does not compile (core/capability.hpp). A DESTRUCTOR is the
// one call that can't take a capability: it runs wherever its owner dies.
// A pool or worker_group joins its threads when destroyed, so one owned by a
// guarded<T> and replaced or dropped inside a lock body would be exactly the
// banned wait, hidden in a `= {}`.
//
// JoinFree<T> says no part of T is such an object: the walker looks through
// fields, containers, optionals, variants and owning pointers for any type
// marked `joins_on_destroy`. guarded<T> requires it, so joining objects live
// outside locks, where their owner can stop() them with a capability first.
//
// What it can't see: a class that hides a joining member behind a pointer
// to an incomplete type, or a class jaal can't look inside. The second is
// the same limit Sendable has, closed the same way: a class with no visible
// fields is accepted (shallow rule), and its author marks it if it joins.

#include <memory>
#include <type_traits>

#include "../meta/fields.hpp"
#include "capability.hpp"
#include "stdshape.hpp"
#include "walk.hpp"

namespace jaal {

namespace detail {

template <class T> struct join_pointee { using type = void; };
template <class E, class D> struct join_pointee<std::unique_ptr<E, D>> {
    using type = std::remove_cv_t<E>;
};
// shared_ptr may be the last owner, so its pointee's destructor may run here.
// weak_ptr never destroys anything: it is accepted like a raw pointer.
template <class E> struct join_pointee<std::shared_ptr<E>> { using type = std::remove_cv_t<E>; };
template <class E> inline constexpr bool is_weak_ptr = false;
template <class E> inline constexpr bool is_weak_ptr<std::weak_ptr<E>> = true;

struct join_free_policy {
    template <class T>
    using field_list = meta::field_types_t<T>;

    template <class T>
    static consteval auto classify() {
        namespace d = walk::decision;
        namespace s = stdshape;
        using U = std::remove_cv_t<T>;
        using S = s::shape_t<U>;
        using P = typename join_pointee<U>::type;

        if constexpr (joins_on_destroy<U>)                           return d::reject{};
        else if constexpr (is_weak_ptr<U>)                           return d::accept{};
        // An owning pointer destroys its pointee: look through it.
        else if constexpr (!std::is_void_v<P>)                       return d::into<P>{};
        else if constexpr (std::is_reference_v<T> || std::is_pointer_v<U>
                           || std::is_arithmetic_v<U> || std::is_enum_v<U>
                           || std::is_member_pointer_v<U> || std::is_function_v<U>
                           || std::is_union_v<U>)                    return d::accept{};
        else if constexpr (std::is_same_v<S, s::value> || std::is_same_v<S, s::view>
                           || std::is_same_v<S, s::sync_handle>)     return d::accept{};
        else if constexpr (s::is_owns_shape<S> || s::is_value_of_shape<S>)
            return typename s::elements<S>::template as<d::into>{};
        else if constexpr (std::is_class_v<U>)                       return d::structural{};
        else                                                         return d::accept{};
    }
};

template <class T>
using join_free_run = walk::run<join_free_policy, std::remove_cv_t<T>>;

// A class the walker can't look inside is accepted (it has no fields it can
// see; its author marks it if it joins).
template <class T>
consteval bool join_free_value() {
    using U = std::remove_cv_t<T>;
    if constexpr (std::is_class_v<U> && !meta::aggregate_struct<U> && !joins_on_destroy<U>
                  && std::is_same_v<stdshape::shape_t<U>, stdshape::unknown>
                  && std::is_void_v<typename join_pointee<U>::type>)
        return true;
    else
        return join_free_run<T>::ok;
}

}  // namespace detail

/// Destroying a T never waits for another thread.
template <class T>
concept JoinFree = detail::join_free_value<T>();

}  // namespace jaal
