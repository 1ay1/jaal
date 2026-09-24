// tests/meta/list_test.cpp — static tests for jaal::meta.
//
// Every check is a static_assert: if this file compiles, the tests passed.
// main() exists only so ctest has something to run.

#include <jaal/meta.hpp>

#include <string_view>
#include <variant>

namespace m = jaal::meta;

struct A {}; struct B {}; struct C {}; struct D {};

// ── list / size ──────────────────────────────────────────────────────────
static_assert(m::type_list<m::list<>>);
static_assert(m::type_list<m::list<A, B>>);
static_assert(!m::type_list<A>);
static_assert(!m::type_list<std::variant<A>>);
static_assert(m::size_v<m::list<>> == 0);
static_assert(m::size_v<m::list<A, B, C>> == 3);

// ── member_of / contains ────────────────────────────────────────────────
static_assert(m::member_of<A, m::list<A, B>>);
static_assert(m::member_of<B, m::list<A, B>>);
static_assert(!m::member_of<C, m::list<A, B>>);
static_assert(!m::member_of<A, m::list<>>);
static_assert(!m::member_of<A, A>);                  // not a list: false, not an error
static_assert(!m::member_of<const A, m::list<A>>);   // exact match, no decay

// ── count / unique ───────────────────────────────────────────────────────
static_assert(m::count_v<m::list<A, B, A>, A> == 2);
static_assert(m::count_v<m::list<A, B, A>, C> == 0);
static_assert(m::unique<m::list<>>);
static_assert(m::unique<m::list<A, B, C>>);
static_assert(!m::unique<m::list<A, B, A>>);

// ── subset_of / same_set ─────────────────────────────────────────────────
static_assert(m::subset_of<m::list<>, m::list<>>);
static_assert(m::subset_of<m::list<>, m::list<A>>);
static_assert(m::subset_of<m::list<B, A>, m::list<A, B, C>>);
static_assert(!m::subset_of<m::list<A, D>, m::list<A, B, C>>);
static_assert(m::same_set<m::list<A, B>, m::list<B, A>>);
static_assert(m::same_set<m::list<A, A, B>, m::list<B, A>>);
static_assert(!m::same_set<m::list<A>, m::list<A, B>>);

// ── index_of / at ────────────────────────────────────────────────────────
static_assert(m::index_of_v<m::list<A, B, C>, A> == 0);
static_assert(m::index_of_v<m::list<A, B, C>, C> == 2);
static_assert(m::index_of_v<m::list<A, B, A>, A> == 0);     // first occurrence
static_assert(m::index_of_v<m::list<A, B>, C> == 2);        // absent = size
static_assert(m::index_of_v<m::list<>, A> == 0);
static_assert(std::is_same_v<m::at_t<m::list<A, B, C>, 0>, A>);
static_assert(std::is_same_v<m::at_t<m::list<A, B, C>, 2>, C>);

// at<> out of range is rejected by its constraint, not a hard error.
template <class L, std::size_t I>
concept can_at = requires { typename m::at_t<L, I>; };
static_assert(can_at<m::list<A>, 0>);
static_assert(!can_at<m::list<A>, 1>);
static_assert(!can_at<m::list<>, 0>);

// ── apply ────────────────────────────────────────────────────────────────
static_assert(std::is_same_v<m::apply_t<std::variant, m::list<A, B>>, std::variant<A, B>>);

// ── concat ───────────────────────────────────────────────────────────────
static_assert(std::is_same_v<m::concat_t<>, m::list<>>);
static_assert(std::is_same_v<m::concat_t<m::list<A>>, m::list<A>>);
static_assert(std::is_same_v<m::concat_t<m::list<A>, m::list<>, m::list<B, C>>,
                             m::list<A, B, C>>);
static_assert(std::is_same_v<m::concat_t<m::list<A>, m::list<A>>, m::list<A, A>>);

// ── filter / transform ───────────────────────────────────────────────────
template <class T> struct not_b : std::bool_constant<!std::is_same_v<T, B>> {};
static_assert(std::is_same_v<m::filter_t<not_b, m::list<A, B, C, B>>, m::list<A, C>>);
static_assert(std::is_same_v<m::filter_t<not_b, m::list<>>, m::list<>>);
static_assert(std::is_same_v<m::transform_t<std::add_const_t, m::list<A, B>>,
                             m::list<const A, const B>>);

// ── dedup / minus / union ────────────────────────────────────────────────
static_assert(std::is_same_v<m::dedup_t<m::list<>>, m::list<>>);
static_assert(std::is_same_v<m::dedup_t<m::list<A, B, A, C, B>>, m::list<A, B, C>>);
static_assert(std::is_same_v<m::minus_t<m::list<A, B, C>, m::list<B>>, m::list<A, C>>);
static_assert(std::is_same_v<m::minus_t<m::list<A, B>, m::list<A, B>>, m::list<>>);
static_assert(std::is_same_v<m::minus_t<m::list<>, m::list<A>>, m::list<>>);
static_assert(std::is_same_v<m::union_t<m::list<A, B>, m::list<B, C>>, m::list<A, B, C>>);
static_assert(m::unique<m::union_t<m::list<A, A>, m::list<A>>>);

// ── scale ────────────────────────────────────────────────────────────────
// Linear operations run far past GCC's default template depth (900): proof
// that nothing recurses. Pairwise operations (unique, dedup, minus) are
// O(n²) comparisons by nature, so they're checked at a realistic width:
// real effect rows are ~10-30 entries.
template <std::size_t I> struct N {};
template <class Seq> struct many;
template <std::size_t... Is> struct many<std::index_sequence<Is...>> {
    using type = m::list<N<Is>...>;
};

using deep = typename many<std::make_index_sequence<2000>>::type;
static_assert(m::size_v<deep> == 2000);
static_assert(m::index_of_v<deep, N<1999>> == 1999);
static_assert(m::member_of<N<1999>, deep>);
static_assert(std::is_same_v<m::at_t<deep, 1999>, N<1999>>);
static_assert(m::size_v<m::concat_t<deep, deep, deep>> == 6000);

using wide = typename many<std::make_index_sequence<64>>::type;
static_assert(m::unique<wide>);
static_assert(std::is_same_v<m::dedup_t<m::concat_t<wide, wide>>, wide>);
static_assert(std::is_same_v<m::minus_t<wide, wide>, m::list<>>);
static_assert(m::same_set<wide, m::dedup_t<m::concat_t<wide, wide>>>);

// ── fixed_string ─────────────────────────────────────────────────────────
template <m::fixed_string S> struct named { static constexpr std::string_view name = S; };
static_assert(named<"beep">::name == "beep");
static_assert(named<"">::name.empty());
static_assert(m::fixed_string("ab") == m::fixed_string("ab"));
static_assert(!(m::fixed_string("ab") == m::fixed_string("abc")));
static_assert(!std::is_same_v<named<"a">, named<"b">>);

// ── diagnose ─────────────────────────────────────────────────────────────
constexpr auto msg = m::cat("cannot run '", named<"beep">::name, "'");
static_assert(std::string_view(msg.data(), msg.size()) == "cannot run 'beep'");
JAAL_REQUIRE(true, "fixed text form compiles");
JAAL_REQUIRE_MSG(true, m::cat("computed ", "form"), "fallback form");

int main() { return 0; }
