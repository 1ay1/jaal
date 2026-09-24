#pragma once
// jaal::detail::stdshape — what each standard library type IS, structurally.
//
// Both Sendable and Frozen need to know, for a std type: does it own its
// elements (vector, optional)? is it a view (string_view, span)? a unique
// owner (unique_ptr)? a shared owner (shared_ptr)? a plain value (duration)?
//
// That's a fact about the std library, not about either property, so it's
// written once here. Each property's policy then maps SHAPE → RULE:
//
//   shape                    Sendable              Frozen
//   ───────────────────────  ────────────────────  ─────────────────────────
//   owns<A...>               all A Sendable        all A Frozen
//   unique_owner<P, D>       P and D Sendable      never: *p is mutable
//   shared_owner<T>          never                 never (jaal::shared is)
//   view                     never (borrows)       never (target can change)
//   value                    yes                   yes
//   value_of<R>              R Sendable            R Frozen
//   sync_handle              yes (thread-safe)     never (state changes)
//   unknown                  (not a std type)      (not a std type)

#include <array>
#include <chrono>
#include <cstddef>
#include <deque>
#include <expected>
#include <forward_list>
#include <functional>
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
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace jaal::detail::stdshape {

template <class... A>          struct owns {};
template <class P, class D>    struct unique_owner {};
template <class T>             struct shared_owner {};
struct view {};
struct value {};
template <class R>             struct value_of {};
struct sync_handle {};         // shared, but every operation is thread-safe
struct unknown {};

template <class T> struct shape_of { using type = unknown; };
template <class T> using shape_t = typename shape_of<T>::type;

// ── owning containers and wrappers ──────────────────────────────────────
// Allocators, hashers and comparators are template arguments like the
// rest, so they are checked too: a stateful allocator holding a pointer is
// neither Sendable nor Frozen.
#define JAAL_STD_OWNS(TMPL) \
    template <class... A> struct shape_of<TMPL<A...>> { using type = owns<A...>; };

JAAL_STD_OWNS(std::vector)
JAAL_STD_OWNS(std::deque)
JAAL_STD_OWNS(std::list)
JAAL_STD_OWNS(std::forward_list)
JAAL_STD_OWNS(std::map)
JAAL_STD_OWNS(std::multimap)
JAAL_STD_OWNS(std::set)
JAAL_STD_OWNS(std::multiset)
JAAL_STD_OWNS(std::unordered_map)
JAAL_STD_OWNS(std::unordered_multimap)
JAAL_STD_OWNS(std::unordered_set)
JAAL_STD_OWNS(std::unordered_multiset)
JAAL_STD_OWNS(std::optional)
JAAL_STD_OWNS(std::variant)
JAAL_STD_OWNS(std::tuple)
JAAL_STD_OWNS(std::pair)
JAAL_STD_OWNS(std::expected)
JAAL_STD_OWNS(std::basic_string)
#undef JAAL_STD_OWNS

template <class T, std::size_t N> struct shape_of<std::array<T, N>> { using type = owns<T>; };
// expected<void, E>: the void is "no value", not an element.
template <class E> struct shape_of<std::expected<void, E>> { using type = owns<E>; };

// ── ownership through a pointer ─────────────────────────────────────────
template <class T, class D> struct shape_of<std::unique_ptr<T, D>> { using type = unique_owner<T, D>; };
template <class T> struct shape_of<std::shared_ptr<T>> { using type = shared_owner<T>; };
template <class T> struct shape_of<std::weak_ptr<T>>   { using type = shared_owner<T>; };

// ── views: borrow memory someone else owns ──────────────────────────────
template <class C, class Tr> struct shape_of<std::basic_string_view<C, Tr>> { using type = view; };
template <class T, std::size_t E> struct shape_of<std::span<T, E>> { using type = view; };

// ── plain values ────────────────────────────────────────────────────────
template <class R, class P> struct shape_of<std::chrono::duration<R, P>> { using type = value_of<R>; };
template <class C, class D> struct shape_of<std::chrono::time_point<C, D>> { using type = value_of<D>; };

template <class T> struct shape_of<std::allocator<T>>      { using type = value; };
template <class T> struct shape_of<std::char_traits<T>>    { using type = value; };
template <class T> struct shape_of<std::less<T>>           { using type = value; };
template <class T> struct shape_of<std::greater<T>>        { using type = value; };
template <class T> struct shape_of<std::hash<T>>           { using type = value; };
template <class T> struct shape_of<std::equal_to<T>>       { using type = value; };
template <class T> struct shape_of<std::default_delete<T>> { using type = value; };
template <> struct shape_of<std::monostate>                { using type = value; };
template <> struct shape_of<std::nullopt_t>                { using type = value; };
template <> struct shape_of<std::nullptr_t>                { using type = value; };

// std::stop_token is a shared handle to a stop state, built to be used from
// many threads at once (its operations are thread-safe by specification).
// So it's Sendable. It is not Frozen: a stop request changes what it reports.
template <> struct shape_of<std::stop_token> { using type = sync_handle; };


// ── shape queries, for policies and diagnostics ───────────────────────────

// The element types a shape is built from, as a walk decision:
//   owns<A...>         → into<A...>
//   unique_owner<P,D>  → into<P, D>
//   value_of<R>        → into<R>
// Only defined for those three.
template <class S> struct elements;
template <class... A> struct elements<owns<A...>> {
    template <template <class...> class Into> using as = Into<A...>;
};
template <class P, class D> struct elements<unique_owner<P, D>> {
    template <template <class...> class Into> using as = Into<P, D>;
};
template <class R> struct elements<value_of<R>> {
    template <template <class...> class Into> using as = Into<R>;
};

template <class S> inline constexpr bool is_owns_shape = false;
template <class... A> inline constexpr bool is_owns_shape<owns<A...>> = true;

template <class S> inline constexpr bool is_value_of_shape = false;
template <class R> inline constexpr bool is_value_of_shape<value_of<R>> = true;

template <class S> inline constexpr bool is_view_shape = false;
template <> inline constexpr bool is_view_shape<view> = true;

template <class S> inline constexpr bool is_shared_shape = false;
template <class T> inline constexpr bool is_shared_shape<shared_owner<T>> = true;

template <class S> inline constexpr bool is_unique_shape = false;
template <class P, class D> inline constexpr bool is_unique_shape<unique_owner<P, D>> = true;

}  // namespace jaal::detail::stdshape
