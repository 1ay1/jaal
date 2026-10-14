// tests/core/sub_test.cpp — Sub, sources, and the reconciler.

#include <jaal/core/core_fx.hpp>
#include <jaal/core/sub.hpp>
#include <jaal/kernel/reconcile.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <variant>
#include <vector>

using namespace std::chrono_literals;
using jaal::basic_sub;
using jaal::make_row;
namespace fx = jaal::fx;

// ── a test source keyed by a path ────────────────────────────────────────
struct watch {
    static constexpr std::string_view name = "watch";
    template <class Msg> struct type { std::string path; Msg msg; };
    template <class F, class M>
    static auto fmap(F&& f, type<M> e) -> type<std::invoke_result_t<F, M>> {
        return {std::move(e.path), std::invoke(std::forward<F>(f), std::move(e.msg))};
    }
    using key_type = std::string;
    template <class M>
    static key_type key(const type<M>& e) { return e.path; }
};

static_assert(jaal::SourceDescriptor<fx::every>);
static_assert(jaal::SourceDescriptor<watch>);

struct Msg { int v; bool operator==(const Msg&) const = default; };
using R = make_row<fx::every, watch>;
using S = basic_sub<Msg, R>;

// keys of different source kinds live in a sum type
using K = jaal::source_key_t<R>;
static_assert(std::variant_size_v<K> == 2);

// widening / narrowing, like Cmd
static_assert(std::is_convertible_v<basic_sub<Msg, make_row<fx::every>>, S>);
static_assert(!std::is_constructible_v<basic_sub<Msg, make_row<fx::every>>, S>);
// core_src (every + stream) is NOT a subrow of R: R has no stream
static_assert(!std::is_convertible_v<basic_sub<Msg, jaal::core_src>, S>);

template <class X> concept has_every = requires { X::every(1ms, Msg{0}); };
static_assert(has_every<S>);
static_assert(!has_every<basic_sub<Msg, make_row<watch>>>);

// the program-facing alias always unions in core_src
static_assert(std::is_same_v<jaal::Sub<Msg>, basic_sub<Msg, jaal::core_src>>);
static_assert(std::is_same_v<jaal::Sub<Msg, watch>,
                             basic_sub<Msg, jaal::row_union<jaal::core_src, make_row<watch>>>>);

// ── runtime ──────────────────────────────────────────────────────────────
using jaal::running_sources;
using ek = jaal::tagged_key<fx::every>;
using wk = jaal::tagged_key<watch>;

int main() {
    running_sources<Msg, R> run;

    // 1. first subscribe: everything starts
    auto s1 = S::batch(S::every(1000ms, Msg{1}), S(watch::type<Msg>{"/a", Msg{2}}));
    auto p1 = run.reconcile(s1);
    if (p1.start.size() != 2 || !p1.keep.empty() || !p1.stop.empty()) return 1;

    // 2. same subscription again: nothing starts or stops (timer keeps phase)
    auto p2 = run.reconcile(s1);
    if (p2.changed() || p2.keep.size() != 2) return 5;

    // 3. two timers with the SAME interval: two keys (ordinal 0 and 1).
    //    The first keeps running; only the second starts.
    auto s3 = S::batch(S::every(1000ms, Msg{1}), S::every(1000ms, Msg{3}),
                       S(watch::type<Msg>{"/a", Msg{2}}));
    auto p3 = run.reconcile(s3);
    if (p3.start.size() != 1 || p3.stop.size() != 0 || p3.keep.size() != 2) return 6;
    if (!run.contains(K{ek{{1000, 0}}}) || !run.contains(K{ek{{1000, 1}}})) return 7;

    // 4. a changed Msg on a kept timer: kept, with the new payload
    auto s4 = S::batch(S::every(1000ms, Msg{42}), S::every(1000ms, Msg{3}),
                       S(watch::type<Msg>{"/a", Msg{2}}));
    auto p4 = run.reconcile(s4);
    if (p4.changed()) return 8;
    bool saw42 = false;
    for (auto& u : p4.keep)
        if (auto* e = std::get_if<fx::every::type<Msg>>(&u.p))
            if (e->msg.v == 42) saw42 = true;
    if (!saw42) return 9;

    // 5. drop the watcher: it stops, the timers stay
    auto s5 = S::batch(S::every(1000ms, Msg{42}), S::every(1000ms, Msg{3}));
    auto p5 = run.reconcile(s5);
    if (p5.stop.size() != 1 || !std::holds_alternative<wk>(p5.stop[0].k)) return 10;
    if (run.size() != 2) return 11;

    // 6. a duplicate key in one result is REPORTED, and runs once
    auto s6 = S::batch(S(watch::type<Msg>{"/b", Msg{1}}), S(watch::type<Msg>{"/b", Msg{2}}));
    auto p6 = run.reconcile(s6);
    if (p6.duplicates.size() != 1 || run.size() != 1) return 12;

    // 7. an every and a watch never collide, even though both keys exist
    auto s7 = S::batch(S::every(5ms, Msg{1}), S(watch::type<Msg>{"/c", Msg{1}}));
    auto p7 = run.reconcile(s7);
    if (run.size() != 2 || p7.duplicates.size() != 0) return 13;

    // 8. none stops everything
    auto p8 = run.reconcile(S::none());
    if (p8.stop.size() != 2 || run.size() != 0) return 14;

    // 9. map keeps keys: a mapped every has the same key as before
    auto mapped = S::every(250ms, Msg{1}).map([](Msg m) { return Msg{m.v + 1}; });
    bool ok9 = false;
    mapped.for_each([&]<class X>(const X& x) {
        if constexpr (std::same_as<X, fx::every::type<Msg>>)
            ok9 = x.msg.v == 2 && fx::every::key(x).interval_ms == 250;
    });
    if (!ok9) return 15;

    return 0;
}
