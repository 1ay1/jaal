#pragma once
// jaal::detail::walk — one structural type walker, many properties.
//
// Sendable and Frozen ask different questions about the same thing: "what
// is this value made of, all the way down?". They share this walker and
// differ only in their RULES. Each property is a policy type:
//
//   struct my_policy {
//       // decide about T before the walker looks inside it. Return:
//       //   decision::accept      — T is fine, don't look inside
//       //   decision::reject      — T is the culprit
//       //   decision::into<L>     — T is fine iff every type in L is
//       //   decision::structural  — let the walker look inside T
//       template <class T> static consteval auto classify();
//   };
//
// The walker supplies everything the two policies would otherwise both
// have to write:
//   * recursion into aggregate structs, field by field (meta::fields)
//   * arrays
//   * recursion guard: a type already on the stack is assumed fine
//     (co-inductive), so recursive types terminate and a bad field
//     anywhere in the cycle is still found
//   * the culprit: the innermost type that failed, for diagnostics

#include <type_traits>

#include "../meta/fields.hpp"
#include "../meta/list.hpp"

namespace jaal::detail::walk {

// ── what a policy can say ────────────────────────────────────────────────
namespace decision {
struct accept {};
struct reject {};
struct structural {};
template <class... Ts> struct into {};
}  // namespace decision

// ── the result ───────────────────────────────────────────────────────────
template <bool Ok, class Culprit>
struct result {
    static constexpr bool ok = Ok;
    using culprit = Culprit;
};
using yes = result<true, void>;
template <class T> using no = result<false, T>;

template <class Policy, class T, class Seen> struct check;

// All must pass; the first failure is reported.
template <class Policy, class Seen, class... Ts> struct all_of;
template <class Policy, class Seen> struct all_of<Policy, Seen> : yes {};
template <class Policy, class Seen, class T, class... Rest>
struct all_of<Policy, Seen, T, Rest...>
    : std::conditional_t<check<Policy, T, Seen>::ok,
                         all_of<Policy, Seen, Rest...>,
                         check<Policy, T, Seen>> {};

template <class Policy, class Seen, class L> struct all_in;
template <class Policy, class Seen, class... Ts>
struct all_in<Policy, Seen, meta::list<Ts...>> : all_of<Policy, Seen, Ts...> {};

template <class T, class Seen> struct push;
template <class T, class... S> struct push<T, meta::list<S...>> {
    using type = meta::list<S..., T>;
};

// Turn a policy decision into a result.
template <class Policy, class T, class Seen, class D> struct apply;
template <class Policy, class T, class Seen>
struct apply<Policy, T, Seen, decision::accept> : yes {};
template <class Policy, class T, class Seen>
struct apply<Policy, T, Seen, decision::reject> : no<T> {};
template <class Policy, class T, class Seen, class... Ts>
struct apply<Policy, T, Seen, decision::into<Ts...>>
    : all_of<Policy, typename push<T, Seen>::type, Ts...> {};
template <class Policy, class T, class Seen>
struct apply<Policy, T, Seen, decision::structural> {
private:
    static consteval auto go() {
        if constexpr (std::is_array_v<T>)
            return check<Policy, std::remove_all_extents_t<T>, Seen>{};
        else if constexpr (meta::aggregate_struct<T>)
            return all_in<Policy, typename push<T, Seen>::type,
                          typename Policy::template field_list<T>>{};
        else
            return no<T>{};          // can't see inside: not proven fine
    }
    using r = decltype(go());
public:
    static constexpr bool ok = r::ok;
    using culprit = typename r::culprit;
};

template <class Policy, class T, class... S>
struct check<Policy, T, meta::list<S...>> {
private:
    using Seen = meta::list<S...>;
    static consteval auto go() {
        if constexpr (meta::contains_v<Seen, T>)
            return yes{};                                        // co-inductive
        else
            return apply<Policy, T, Seen,
                         decltype(Policy::template classify<T>())>{};
    }
    using r = decltype(go());
public:
    static constexpr bool ok = r::ok;
    using culprit = typename r::culprit;
};

template <class Policy, class T>
using run = check<Policy, T, meta::list<>>;

}  // namespace jaal::detail::walk
