#pragma once
// jaal::Sendable — may this value be moved to another thread?
//
// Design: CONCURRENCY.md §4.2. A value is Sendable when, after it's moved to
// another thread, it holds nothing that the sending thread can still reach
// or free. So: no raw pointers, no references, no views, no shared mutable
// ownership. Owned values and deep copies are fine.
//
// The check is STRUCTURAL and DEEP:
//   * std containers, optional, variant, tuple, pair, unique_ptr ... are
//     Sendable when their element types are
//   * plain structs are checked field by field (meta::fields, C++26
//     structured binding packs), recursively
//   * recursive types (struct Node { std::vector<Node> kids; }) are handled
//     co-inductively: a type already being checked is assumed fine, so the
//     check terminates and still rejects a bad field anywhere in the cycle
//   * anything jaal can't see into (a class with private fields) is NOT
//     Sendable unless it opts in
//
// Opt-in, for a class that owns everything it holds:
//
//     template <> inline constexpr bool jaal::sendable_opt_in<MyHandle> = true;
//
// Every opt-in is a claim a human made. Keep them rare, grep-able, and
// commented with why the type is safe.
//
// Diagnostics: when T isn't Sendable, sendable_reason<T>() names the
// innermost offending type, so errors point at the field, not the Msg.

#include <array>
#include <chrono>
#include <cstddef>
#include <deque>
#include <expected>
#include <forward_list>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "../meta/diagnose.hpp"
#include "../meta/fields.hpp"
#include "../meta/list.hpp"
#include "../meta/type_name.hpp"

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

// ── the verdict ──────────────────────────────────────────────────────────
//
// verdict<T, Seen>::ok       — the answer
// verdict<T, Seen>::culprit  — the innermost type that made it false (T if ok)
//
// Seen is the list of types currently being checked, for recursion.

namespace detail::send {

template <bool Ok, class Culprit>
struct result {
    static constexpr bool ok = Ok;
    using culprit = Culprit;
};
using yes = result<true, void>;
template <class T> using no = result<false, T>;

template <class T, class Seen> struct verdict;

// All of Ts must pass; report the first one that doesn't.
template <class Seen, class... Ts> struct all_of;
template <class Seen> struct all_of<Seen> : yes {};
template <class Seen, class T, class... Rest>
struct all_of<Seen, T, Rest...>
    : std::conditional_t<verdict<T, Seen>::ok,
                         all_of<Seen, Rest...>,
                         verdict<T, Seen>> {};

template <class Seen, class L> struct all_in;
template <class Seen, class... Ts>
struct all_in<Seen, meta::list<Ts...>> : all_of<Seen, Ts...> {};

template <class T, class Seen> struct push;
template <class T, class... S> struct push<T, meta::list<S...>> {
    using type = meta::list<S..., T>;
};

// ── std library rules ───────────────────────────────────────────────────
// Specialise `std_rule` on the template; `known` marks that a rule exists.
template <class T, class Seen> struct std_rule { static constexpr bool known = false; };

#define JAAL_SEND_ELEMENTWISE(TMPL)                                          \
    template <class... A, class Seen>                                        \
    struct std_rule<TMPL<A...>, Seen> : all_of<Seen, A...> {                 \
        static constexpr bool known = true;                                  \
    };

// Containers and wrappers that own their elements. Allocators, hashers and
// comparators are checked too (they're template arguments like the rest),
// which is right: a stateful allocator holding a pointer is not Sendable.
JAAL_SEND_ELEMENTWISE(std::vector)
JAAL_SEND_ELEMENTWISE(std::deque)
JAAL_SEND_ELEMENTWISE(std::list)
JAAL_SEND_ELEMENTWISE(std::forward_list)
JAAL_SEND_ELEMENTWISE(std::map)
JAAL_SEND_ELEMENTWISE(std::multimap)
JAAL_SEND_ELEMENTWISE(std::set)
JAAL_SEND_ELEMENTWISE(std::multiset)
JAAL_SEND_ELEMENTWISE(std::unordered_map)
JAAL_SEND_ELEMENTWISE(std::unordered_multimap)
JAAL_SEND_ELEMENTWISE(std::unordered_set)
JAAL_SEND_ELEMENTWISE(std::unordered_multiset)
JAAL_SEND_ELEMENTWISE(std::optional)
JAAL_SEND_ELEMENTWISE(std::variant)
JAAL_SEND_ELEMENTWISE(std::tuple)
JAAL_SEND_ELEMENTWISE(std::pair)
JAAL_SEND_ELEMENTWISE(std::expected)
JAAL_SEND_ELEMENTWISE(std::basic_string)
#undef JAAL_SEND_ELEMENTWISE

template <class T, std::size_t N, class Seen>
struct std_rule<std::array<T, N>, Seen> : verdict<T, Seen> {
    static constexpr bool known = true;
};

// Stateless standard pieces used as template arguments above.
template <class T, class Seen> struct std_rule<std::allocator<T>, Seen> : yes {
    static constexpr bool known = true;
};
template <class T, class Seen> struct std_rule<std::char_traits<T>, Seen> : yes {
    static constexpr bool known = true;
};
template <class T, class Seen> struct std_rule<std::less<T>, Seen> : yes {
    static constexpr bool known = true;
};
template <class T, class Seen> struct std_rule<std::hash<T>, Seen> : yes {
    static constexpr bool known = true;
};
template <class T, class Seen> struct std_rule<std::equal_to<T>, Seen> : yes {
    static constexpr bool known = true;
};

// Unique ownership: the pointee moves with it.
template <class T, class D, class Seen>
struct std_rule<std::unique_ptr<T, D>, Seen> : all_of<Seen, T, D> {
    static constexpr bool known = true;
};
template <class T, class Seen> struct std_rule<std::default_delete<T>, Seen> : yes {
    static constexpr bool known = true;
};

// Shared ownership of MUTABLE data is exactly the thing jaal rules out.
// shared_ptr<const T> is only safe when T can't change through const; that
// is Frozen's job (core/frozen.hpp), and jaal::shared<T> is the type to use.
template <class T, class Seen> struct std_rule<std::shared_ptr<T>, Seen> : no<std::shared_ptr<T>> {
    static constexpr bool known = true;
};
template <class T, class Seen> struct std_rule<std::weak_ptr<T>, Seen> : no<std::weak_ptr<T>> {
    static constexpr bool known = true;
};

// Views borrow. Never Sendable.
template <class T> inline constexpr bool is_view_v = false;
template <class C, class Tr> inline constexpr bool is_view_v<std::basic_string_view<C, Tr>> = true;
template <class T, std::size_t E> inline constexpr bool is_view_v<std::span<T, E>> = true;

template <class T> inline constexpr bool is_shared_owner_v = false;
template <class T> inline constexpr bool is_shared_owner_v<std::shared_ptr<T>> = true;
template <class T> inline constexpr bool is_shared_owner_v<std::weak_ptr<T>> = true;

template <class C, class Tr, class Seen>
struct std_rule<std::basic_string_view<C, Tr>, Seen> : no<std::basic_string_view<C, Tr>> {
    static constexpr bool known = true;
};
template <class T, std::size_t E, class Seen>
struct std_rule<std::span<T, E>, Seen> : no<std::span<T, E>> {
    static constexpr bool known = true;
};

// Time values are plain numbers.
template <class R, class P, class Seen>
struct std_rule<std::chrono::duration<R, P>, Seen> : verdict<R, Seen> {
    static constexpr bool known = true;
};
template <class C, class D, class Seen>
struct std_rule<std::chrono::time_point<C, D>, Seen> : verdict<D, Seen> {
    static constexpr bool known = true;
};

// Stop tokens are designed to be shared across threads.
template <class Seen> struct std_rule<std::stop_token, Seen> : yes {
    static constexpr bool known = true;
};

// std::expected<void, E>: the void is "no value", not a field.
template <class E, class Seen>
struct std_rule<std::expected<void, E>, Seen> : verdict<E, Seen> {
    static constexpr bool known = true;
};

template <class Seen> struct std_rule<std::monostate, Seen> : yes {
    static constexpr bool known = true;
};
template <class Seen> struct std_rule<std::nullopt_t, Seen> : yes {
    static constexpr bool known = true;
};
template <class Seen> struct std_rule<std::nullptr_t, Seen> : yes {
    static constexpr bool known = true;
};

// ── the main rule ───────────────────────────────────────────────────────
template <class T, class... S>
struct verdict<T, meta::list<S...>> {
private:
    using Seen = meta::list<S...>;

    static consteval auto decide() {
        using U = std::remove_cv_t<T>;
        if constexpr (sendable_opt_out<U>)                      return no<U>{};
        else if constexpr (sendable_opt_in<U>)                  return yes{};
        else if constexpr (meta::contains_v<Seen, U>)           return yes{};   // co-inductive
        else if constexpr (std::is_reference_v<T>)              return no<T>{};
        else if constexpr (std::is_pointer_v<U>
                           || std::is_member_pointer_v<U>)      return no<U>{};
        else if constexpr (std::is_function_v<U>)              return no<U>{};
        else if constexpr (std::is_arithmetic_v<U>
                           || std::is_enum_v<U>)                return yes{};
        else if constexpr (std::is_same_v<U, std::byte>)       return yes{};
        else if constexpr (std::is_array_v<U>)
            return verdict<std::remove_all_extents_t<U>, Seen>{};
        else if constexpr (std_rule<U, Seen>::known)            return std_rule<U, Seen>{};
        else if constexpr (std::is_union_v<U>)                  return no<U>{};
        else if constexpr (meta::aggregate_struct<U>)
            return all_in<typename push<U, Seen>::type, meta::field_types_t<U>>{};
        else                                                    return no<U>{};
    }
    using r = decltype(decide());

public:
    static constexpr bool ok = r::ok;
    using culprit = typename r::culprit;
};

}  // namespace detail::send

// ── public API ───────────────────────────────────────────────────────────

template <class T>
inline constexpr bool sendable_v =
    detail::send::verdict<T, meta::list<>>::ok;

/// The innermost type that makes T non-Sendable; void if T is Sendable.
template <class T>
using sendable_culprit_t =
    typename detail::send::verdict<T, meta::list<>>::culprit;

template <class T>
concept Sendable =
    std::is_object_v<T>          // no references, no functions, no void
    && std::movable<T>
    && sendable_v<T>;

/// A sentence explaining why T isn't Sendable, for static_assert.
template <class T>
consteval auto sendable_reason() {
    using C = sendable_culprit_t<T>;
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
        else if constexpr (detail::send::is_view_v<C>)
            m += "a view that borrows memory another thread may free";
        else if constexpr (detail::send::is_shared_owner_v<C>)
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
