#pragma once
// jaal::meta algorithms — operations that BUILD lists.
//
// Every operation returns a meta::list. Implementations avoid recursive
// template instantiation: concat is a single fold over a binary join, dedup
// and filter are one expansion each (every element maps to list<> or
// list<T>, then the results are concatenated).

#include "list.hpp"

namespace jaal::meta {

// ── concat ───────────────────────────────────────────────────────────────
namespace detail {
// A tag wrapper so operator+ on it can't collide with anything real.
template <class L> struct cat_box {};

template <class... As, class... Bs>
auto operator+(cat_box<list<As...>>, cat_box<list<Bs...>>)
    -> cat_box<list<As..., Bs...>>;

template <class L> auto cat_unbox(cat_box<L>) -> L;

// One binary fold: linear, no recursive instantiation.
template <class... Ls>
using concat_impl = decltype(cat_unbox((cat_box<list<>>{} + ... + cat_box<Ls>{})));
}  // namespace detail

template <type_list... Ls>
using concat_t = detail::concat_impl<Ls...>;

// ── filter ───────────────────────────────────────────────────────────────
// Keep the elements for which Pred<T>::value is true.
template <template <class> class Pred, class L> struct filter;
template <template <class> class Pred, class... Ts>
struct filter<Pred, list<Ts...>> {
    using type = concat_t<std::conditional_t<Pred<Ts>::value, list<Ts>, list<>>...>;
};
template <template <class> class Pred, class L>
using filter_t = typename filter<Pred, L>::type;

// ── transform ────────────────────────────────────────────────────────────
template <template <class> class F, class L> struct transform;
template <template <class> class F, class... Ts>
struct transform<F, list<Ts...>> {
    using type = list<F<Ts>...>;
};
template <template <class> class F, class L>
using transform_t = typename transform<F, L>::type;

// ── dedup ────────────────────────────────────────────────────────────────
// Keep the FIRST occurrence of each type, preserving order.
namespace detail {
template <class L, class Seq> struct dedup_impl;
template <class... Ts, std::size_t... Is>
struct dedup_impl<list<Ts...>, std::index_sequence<Is...>> {
    using type = concat_t<std::conditional_t<
        index_of_v<list<Ts...>, Ts> == Is, list<Ts>, list<>>...>;
};
}  // namespace detail

template <class L> struct dedup;
template <class... Ts>
struct dedup<list<Ts...>> {
    using type = typename detail::dedup_impl<
        list<Ts...>, std::index_sequence_for<Ts...>>::type;
};
template <type_list L>
using dedup_t = typename dedup<L>::type;

// ── minus ────────────────────────────────────────────────────────────────
// Elements of A that are not in B, order of A preserved.
template <class A, class B> struct minus;
template <class... As, class B>
struct minus<list<As...>, B> {
    using type = concat_t<std::conditional_t<contains_v<B, As>, list<>, list<As>>...>;
};
template <type_list A, type_list B>
using minus_t = typename minus<A, B>::type;

// ── union ────────────────────────────────────────────────────────────────
template <type_list... Ls>
using union_t = dedup_t<concat_t<Ls...>>;

}  // namespace jaal::meta
