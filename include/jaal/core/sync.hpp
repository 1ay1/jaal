#pragma once
// jaal::Sync — is it safe for many threads to reach the same T& at once?
//
// Sendable says a value can MOVE to another thread. Sync says one value can
// be USED from several threads at the same time. It's what co_owned<T> (the
// one way jobs share state) requires.
//
// Checked field by field on plain structs, by each field as DECLARED:
//
//   const F                     Sync when F is Frozen: nobody can write it
//   guarded<U>, published<U>    Sync: every access is synchronised
//   std::atomic<arith/enum>     Sync
//   const std::stop_source      Sync (thread-safe by specification)
//   const co_owned<U>           Sync: U is Sync, and the handle can't change
//   const shared_ptr<U>,
//   const unique_ptr<U>         Sync when U is: the pointer can't be reseated
//   nested plain struct         Sync when its fields are
//   anything else               NOT: a plain writable field is a data race
//
// A class jaal can't see inside opts in with sync_opt_in, and that is a
// claim a human made: every member function is safe to call concurrently.
// An opted-in field still has to be const unless the type can't be assigned,
// since assigning the whole field is a write. Keep opt-ins rare and commented.

#include <atomic>
#include <memory>
#include <type_traits>

#include "../meta/diagnose.hpp"
#include "../meta/fields.hpp"
#include "../meta/type_name.hpp"
#include "frozen.hpp"
#include "stdshape.hpp"
#include "walk.hpp"

namespace jaal {

/// Opt a class in: every member function may run concurrently with any other.
template <class T>
inline constexpr bool sync_opt_in = false;

/// Opt a handle in as Sync when it can't be reassigned: a const co_owned<U>
/// reaches a Sync U and nothing else.
template <class T>
inline constexpr bool sync_when_const = false;

namespace detail {

template <class T> inline constexpr bool is_plain_atomic = false;
template <class X>
inline constexpr bool is_plain_atomic<std::atomic<X>> =
    std::is_arithmetic_v<X> || std::is_enum_v<X>;

// What a const owning pointer reaches. The pointer can't be reseated, so the
// field is Sync exactly when the pointee is.
template <class T> struct owned_pointee {};
template <class E> struct owned_pointee<std::shared_ptr<E>> { using type = std::remove_const_t<E>; };
template <class E, class D> struct owned_pointee<std::unique_ptr<E, D>> { using type = std::remove_const_t<E>; };
template <class T>
concept owning_pointer = requires { typename owned_pointee<T>::type; };

// A field that can be assigned is a write, whatever its methods promise:
// swapping a whole stop_source under a reader is a race. So those count only
// when const.
template <class U>
inline constexpr bool reseatable = std::is_copy_assignable_v<U> || std::is_move_assignable_v<U>;

struct sync_policy {
    template <class T>
    using field_list = meta::declared_fields_t<T>;

    template <class T>
    static consteval auto classify() {
        namespace d = walk::decision;
        using U = std::remove_cv_t<T>;
        if constexpr (std::is_reference_v<T>)                  return d::reject{};
        else if constexpr (sync_opt_in<U>
                           || std::is_same_v<stdshape::shape_t<U>, stdshape::sync_handle>)
            return std::conditional_t<std::is_const_v<T> || !reseatable<U>,
                                      d::accept, d::reject>{};
        else if constexpr (std::is_const_v<T> && sync_when_const<U>) return d::accept{};
        else if constexpr (std::is_const_v<T> && owning_pointer<U>)
            return d::into<typename owned_pointee<U>::type>{};
        else if constexpr (std::is_const_v<T>)
            return std::conditional_t<Frozen<U>, d::accept, d::reject>{};
        else if constexpr (is_plain_atomic<U>)                 return d::accept{};
        else if constexpr (std::is_array_v<U> || meta::aggregate_struct<U>)
            return d::structural{};
        else                                                   return d::reject{};
    }
};

template <class T>
using sync_run = walk::run<sync_policy, std::remove_cv_t<T>>;

}  // namespace detail

template <class T>
concept Sync = std::is_object_v<T> && detail::sync_run<T>::ok;

/// The innermost field type that makes T not Sync; void if T is Sync.
template <class T>
using sync_culprit_t = typename detail::sync_run<T>::culprit;

template <class T>
consteval auto sync_reason() {
    using C = sync_culprit_t<T>;
    meta::message<512> m;
    m += "jaal: '";
    m.append_short(meta::type_name<T>());
    m += "' is not Sync";
    if constexpr (!std::is_void_v<C>) {
        if constexpr (!std::is_same_v<std::remove_cv_t<C>, std::remove_cv_t<T>>) {
            m += ": it has a field of type '";
            m.append_short(meta::type_name<C>());
            m += "', ";
        } else {
            m += ": ";
        }
        if constexpr (std::is_const_v<C>)
            m += "which is const but not Frozen (it can still change through const access)";
        else if constexpr (std::is_class_v<C> && !meta::aggregate_struct<C>)
            m += "a class jaal can't see inside; make it a guarded<U>, or if every "
                 "member function is safe to call concurrently, specialise "
                 "jaal::sync_opt_in for it";
        else
            m += "which any thread could write; make it const, a guarded<U> or an atomic";
    }
    return m;
}

template <class T>
consteval void require_sync() {
    if constexpr (!Sync<T>) {
#if defined(__cpp_static_assert) && __cpp_static_assert >= 202306L
        static_assert(Sync<T>, sync_reason<T>());
#else
        static_assert(Sync<T>, "jaal: type is not Sync (see sync_culprit_t)");
#endif
    }
}

}  // namespace jaal
