#pragma once
// jaal rows — the set of effects a Cmd may contain, as a type.
//
//   using core_fx = make_row<fx::quit, fx::after, fx::task, fx::now>;
//
// A row is a SET: order and duplicates don't matter. make_row<...> puts the
// effects in a canonical order (by name) and removes duplicates, so
//
//   make_row<fx::after, fx::quit>  and  make_row<fx::quit, fx::after, fx::quit>
//
// are the SAME type. That keeps Cmd<Msg, R> types, their variants and their
// error messages stable no matter how a row was spelled or combined. Build
// rows with make_row / row_union, not row<> directly: raw row<> is the
// canonical form and requires its effects already sorted and unique.
//
// Subtyping: a row A is a subrow of B when every effect in A is in B.
// Cmd<Msg, A> converts to Cmd<Msg, B> exactly then (row widening).

#include <array>
#include <cstddef>
#include <string_view>
#include <type_traits>
#include <utility>

#include "../meta/algo.hpp"
#include "../meta/list.hpp"
#include "effect.hpp"

namespace jaal {

namespace detail::rows {

// Effect names are the sort key. Two different effects with the same name
// would make canonical order ambiguous, so that's rejected (see row<>).
template <class... Ds>
consteval bool sorted_unique_names() {
    constexpr std::array<std::string_view, sizeof...(Ds)> n{Ds::name...};
    for (std::size_t i = 1; i < n.size(); ++i)
        if (!(n[i - 1] < n[i])) return false;
    return true;
}

template <class... Ds>
consteval auto sort_order() {
    std::array<std::string_view, sizeof...(Ds)> n{Ds::name...};
    std::array<std::size_t, sizeof...(Ds)> idx{};
    for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    for (std::size_t i = 1; i < idx.size(); ++i)          // insertion sort; rows are small
        for (std::size_t j = i; j > 0 && n[idx[j]] < n[idx[j - 1]]; --j)
            std::swap(idx[j], idx[j - 1]);
    return idx;
}

template <class L, class Seq> struct sorted;
template <class... Ds, std::size_t... Is>
struct sorted<meta::list<Ds...>, std::index_sequence<Is...>> {
    static constexpr auto order = sort_order<Ds...>();
    using type = meta::list<meta::at_t<meta::list<Ds...>, order[Is]>...>;
};

}  // namespace detail::rows

// ── the canonical row ────────────────────────────────────────────────────
template <Effect... Ds>
    requires (detail::rows::sorted_unique_names<Ds...>())
struct row {
    using effects = meta::list<Ds...>;
    static constexpr std::size_t size = sizeof...(Ds);
};

template <class T> inline constexpr bool is_row_v = false;
template <class... Ds> inline constexpr bool is_row_v<row<Ds...>> = true;
template <class T> concept Row = is_row_v<T>;

// ── building rows ────────────────────────────────────────────────────────
namespace detail::rows {
template <class L> struct to_row;
template <class... Ds> struct to_row<meta::list<Ds...>> {
    using deduped = meta::dedup_t<meta::list<Ds...>>;
    using ordered = typename sorted<deduped, std::make_index_sequence<deduped::size>>::type;
    template <class L2> struct wrap;
    template <class... Es> struct wrap<meta::list<Es...>> { using type = row<Es...>; };
    using type = typename wrap<ordered>::type;
};
}  // namespace detail::rows

/// Any effects, any order, duplicates allowed → the canonical row.
template <Effect... Ds>
using make_row = typename detail::rows::to_row<meta::list<Ds...>>::type;

template <Row... Rs>
using row_union = typename detail::rows::to_row<
    meta::concat_t<typename Rs::effects...>>::type;

template <Row A, Row B>
using row_minus = typename detail::rows::to_row<
    meta::minus_t<typename A::effects, typename B::effects>>::type;

template <class A, class B>
concept subrow_of = Row<A> && Row<B>
    && meta::subset_of<typename A::effects, typename B::effects>;

template <class D, class R>
concept in_row = Row<R> && meta::member_of<D, typename R::effects>;

using empty_row = row<>;

}  // namespace jaal
