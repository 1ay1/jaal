#pragma once
// jaal::Sub<Msg, Row> — what a program wants to hear about, as data.
//
// subscribe(model) returns a Sub. The kernel diffs it against what's
// running and starts/stops things to match. Like Cmd, the Row says what
// kinds of subscription are allowed, and a host must support them.
//
// Two kinds, kept apart because they have different lifetimes:
//
//   ROUTERS  are stateless filters over the host's input events: "on a key,
//            maybe produce this Msg". They run ON THE LOOP THREAD, inside
//            dispatch, and are rebuilt every time subscribe() runs. So they
//            may capture freely: nothing they capture can outlive the call
//            that uses it. (This is maya's on_key / on_mouse.)
//            Descriptor: route(payload, event) -> optional<Msg>.
//
//   SOURCES  have a lifetime: started, kept across model changes, stopped.
//            A timer is one; a file watcher or a socket reader would be
//            another. Each has a KEY. The reconciler keeps a running source
//            as long as a source with the same key keeps being returned,
//            so a timer keeps its phase.
//            Descriptor: key(payload) -> key_type (regular, hashable).
//
// Keys are compared per source kind: the running set is keyed by
// (descriptor, key), a sum type. Two different kinds of source never
// collide even if their key values are equal.
//
// A source that runs on another thread (a watcher, a reader) follows the
// TASK rules, not the router rules: captureless, Sendable arguments. That's
// the source descriptor's job; jaal's own `every` runs on the kernel's
// timer heap and doesn't need a thread.

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "../meta/list.hpp"
#include "effect.hpp"
#include "router.hpp"
#include "row.hpp"

namespace jaal {

// ── descriptor concepts ─────────────────────────────────────────────────
// A subscription descriptor is an Effect (name, type<Msg>, fmap) plus
// either a router or a source interface.

/// Routes events of type D::event_type into an optional Msg.
template <class D>
concept RouterDescriptor = Effect<D> && requires {
    typename D::event_type;
} && requires(const payload_t<D, detail::probe::probe_msg>& p,
              const typename D::event_type& ev) {
    { D::route(p, ev) } -> std::same_as<std::optional<detail::probe::probe_msg>>;
};

/// Has a stable identity the reconciler can key on.
template <class D>
concept SourceDescriptor = Effect<D> && requires {
    typename D::key_type;
} && std::regular<typename D::key_type>
  && requires(const payload_t<D, detail::probe::probe_msg>& p,
              const typename D::key_type& k) {
    { D::key(p) } -> std::same_as<typename D::key_type>;
    { std::hash<typename D::key_type>{}(k) } -> std::convertible_to<std::size_t>;
};

template <class D>
concept SubDescriptor = RouterDescriptor<D> || SourceDescriptor<D>;

// ── Sub ─────────────────────────────────────────────────────────────────
template <class Msg, Row R> class Sub;

namespace detail::sub {
template <class D, class Self, class Msg> struct ctors_of {};
template <class D, class Self, class Msg>
    requires requires { typename D::template ctors<Self, Msg>; }
          && (!requires { requires !D::inherit_ctors; })
struct ctors_of<D, Self, Msg> : D::template ctors<Self, Msg> {};

template <class T> inline constexpr bool is_sub_v = false;
template <class M, class R> inline constexpr bool is_sub_v<Sub<M, R>> = true;
}  // namespace detail::sub

template <class Msg, SubDescriptor... Ds>
class Sub<Msg, row<Ds...>>
    : public detail::sub::ctors_of<Ds, Sub<Msg, row<Ds...>>, Msg>... {
public:
    using msg_type = Msg;
    using row_type = row<Ds...>;

    struct None {};
    struct Batch { std::vector<Sub> subs; };

    using variant = std::variant<None, Batch, payload_t<Ds, Msg>...>;
    variant inner;

    Sub() noexcept : inner(None{}) {}

    template <class E>
        requires meta::member_of<std::remove_cvref_t<E>,
                                 meta::list<payload_t<Ds, Msg>...>>
    Sub(E&& e) : inner(std::forward<E>(e)) {}           // NOLINT: implicit on purpose

    template <class E>
        requires (!meta::member_of<std::remove_cvref_t<E>,
                                   meta::list<payload_t<Ds, Msg>...>>)
              && (!detail::sub::is_sub_v<std::remove_cvref_t<E>>)
              && (!std::same_as<std::remove_cvref_t<E>, None>)
              && (!std::same_as<std::remove_cvref_t<E>, Batch>)
#if defined(__cpp_deleted_function) && __cpp_deleted_function >= 202403L
    Sub(E&&) = delete("jaal: this subscription is not in the Sub's row");
#else
    Sub(E&&) = delete;
#endif

    /// Row widening, like Cmd.
    template <class R>
        requires subrow_of<R, row_type> && (!std::same_as<R, row_type>)
    Sub(Sub<Msg, R> narrow) : inner(None{}) {           // NOLINT: implicit on purpose
        using N = Sub<Msg, R>;
        std::visit([this]<class X>(X&& x) {
            using U = std::remove_cvref_t<X>;
            if constexpr (std::same_as<U, typename N::None>) {
                inner = None{};
            } else if constexpr (std::same_as<U, typename N::Batch>) {
                Batch b;
                b.subs.reserve(x.subs.size());
                for (auto& s : x.subs) b.subs.emplace_back(Sub(std::move(s)));
                inner = std::move(b);
            } else {
                inner = std::forward<X>(x);
            }
        }, std::move(narrow.inner));
    }

    template <class R>
        requires (!subrow_of<R, row_type>)
#if defined(__cpp_deleted_function) && __cpp_deleted_function >= 202403L
    Sub(Sub<Msg, R>) = delete("jaal: can't convert to a Sub with fewer kinds "
                              "(only widening is allowed)");
#else
    Sub(Sub<Msg, R>) = delete;
#endif

    Sub(const Sub&)                = default;
    Sub(Sub&&) noexcept            = default;
    Sub& operator=(const Sub&)     = default;
    Sub& operator=(Sub&&) noexcept = default;

    [[nodiscard]] static Sub none() noexcept { return Sub{}; }

    /// Sub::on(tag{}, f): subscribe to the router kind named by `tag` (a
    /// jaal::router<Event, "name"> in this Sub's row). One name for every
    /// router kind, so two routers never collide on a factory name.
    template <class Tag, class F>
        requires meta::member_of<Tag, meta::list<Ds...>>
              && requires { typename Tag::template ctors<Sub, Msg>; }
    [[nodiscard]] static Sub on(Tag t, F&& f) {
        return Tag::template ctors<Sub, Msg>::on(t, std::forward<F>(f));
    }
    template <class Tag, class F>
        requires (!meta::member_of<Tag, meta::list<Ds...>>)
    static Sub on(Tag, F&&) {
        static_assert(meta::member_of<Tag, meta::list<Ds...>>,
                      "jaal: Sub::on(tag, f): this router isn't in the Sub's row; "
                      "add it with make_row / row_union");
        return Sub{};
    }

    [[nodiscard]] static Sub batch(std::vector<Sub> subs) {
        Batch out;
        for (auto& s : subs) {
            if (auto* b = std::get_if<Batch>(&s.inner)) {
                for (auto& x : b->subs) out.subs.push_back(std::move(x));
            } else if (!std::holds_alternative<None>(s.inner)) {
                out.subs.push_back(std::move(s));
            }
        }
        if (out.subs.empty()) return none();
        if (out.subs.size() == 1) return std::move(out.subs.front());
        Sub r;
        r.inner = std::move(out);
        return r;
    }

    template <class... Ss>
        requires (sizeof...(Ss) > 0) && (std::convertible_to<Ss, Sub> && ...)
    [[nodiscard]] static Sub batch(Ss&&... ss) {
        std::vector<Sub> v;
        v.reserve(sizeof...(Ss));
        (v.emplace_back(Sub(std::forward<Ss>(ss))), ...);
        return batch(std::move(v));
    }

    template <std::invocable<Msg> F>
    [[nodiscard]] auto map(F f) && -> Sub<std::invoke_result_t<F, Msg>, row_type> {
        using To = Sub<std::invoke_result_t<F, Msg>, row_type>;
        return std::visit([&]<class X>(X&& x) -> To {
            using U = std::remove_cvref_t<X>;
            if constexpr (std::same_as<U, None>) {
                return To{};
            } else if constexpr (std::same_as<U, Batch>) {
                typename To::Batch b;
                b.subs.reserve(x.subs.size());
                for (auto& s : x.subs) b.subs.push_back(std::move(s).map(f));
                To t;
                t.inner = std::move(b);
                return t;
            } else {
                To out;
                ([&] {
                    if constexpr (std::same_as<U, payload_t<Ds, Msg>>)
                        out = To(Ds::fmap(f, std::forward<X>(x)));
                }(), ...);
                return out;
            }
        }, std::move(inner));
    }

    template <std::invocable<Msg> F>
    [[nodiscard]] auto map(F f) const& -> Sub<std::invoke_result_t<F, Msg>, row_type>
        requires std::copyable<Sub>
    {
        return Sub(*this).map(std::move(f));
    }

    /// Re-target with a mapper that also gets an ID: `f(id, msg)`. The Sub
    /// counterpart of Cmd::map_with — a stream's mapper runs on the stream's
    /// own thread and so can't capture, but it can carry a Sendable id by
    /// value. That's what lets a keyed list of children subscribe
    /// (core/children.hpp).
    template <class Id, class F>
        requires std::invocable<F, const Id&, Msg>
    [[nodiscard]] auto map_with(Id id, F f) &&
        -> Sub<std::invoke_result_t<F, const Id&, Msg>, row_type> {
        using To = Sub<std::invoke_result_t<F, const Id&, Msg>, row_type>;
        return std::visit([&]<class X>(X&& x) -> To {
            using U = std::remove_cvref_t<X>;
            if constexpr (std::same_as<U, None>) {
                return To{};
            } else if constexpr (std::same_as<U, Batch>) {
                typename To::Batch b;
                b.subs.reserve(x.subs.size());
                for (auto& s : x.subs) b.subs.push_back(std::move(s).map_with(id, f));
                To t;
                t.inner = std::move(b);
                return t;
            } else {
                To out;
                ([&] {
                    if constexpr (std::same_as<U, payload_t<Ds, Msg>>) {
                        static_assert(
                            requires { Ds::fmap_with(id, f, std::forward<X>(x)); },
                            "jaal: this source has no fmap_with, so it can't be mapped "
                            "with an id; add fmap_with(id, f, payload) next to its fmap");
                        out = To(Ds::fmap_with(id, f, std::forward<X>(x)));
                    }
                }(), ...);
                return out;
            }
        }, std::move(inner));
    }

    [[nodiscard]] bool is_none() const noexcept {
        return std::holds_alternative<None>(inner);
    }

    /// Visit every leaf subscription (batches flattened), in order.
    template <class F>
    void for_each(F&& f) const {
        std::visit([&]<class X>(const X& x) {
            using U = std::remove_cvref_t<X>;
            if constexpr (std::same_as<U, None>) {
            } else if constexpr (std::same_as<U, Batch>) {
                for (auto& s : x.subs) s.for_each(f);
            } else {
                f(x);
            }
        }, inner);
    }

private:
    static_assert(meta::unique<meta::list<payload_t<Ds, Msg>...>>,
                  "jaal: two subscription kinds in this row have the same "
                  "payload type; give each its own payload struct");
};

// ── the reconciler's key space ───────────────────────────────────────────
/// A running source's identity: which kind it is, plus its key. A sum over
/// the source kinds in a row, so different kinds never collide.
template <class D>
    requires SourceDescriptor<D>
struct tagged_key {
    typename D::key_type key;
    bool operator==(const tagged_key&) const = default;
};

namespace detail::sub {
// A row with NO source kinds (no subscribe, or routers only) still needs a
// key type for the reconciler's maps. std::variant<> is ill-formed, so it
// gets this instead: a type with no values. Nothing can construct one, so
// the "a source of this kind" code paths are unreachable by type, not by
// luck.
struct no_source_key {
    no_source_key() = delete;
    bool operator==(const no_source_key&) const = default;
};

template <class L> struct key_variant;
template <> struct key_variant<meta::list<>> {
    using type = std::variant<no_source_key>;
};
template <class D, class... Ds> struct key_variant<meta::list<D, Ds...>> {
    using type = std::variant<tagged_key<D>, tagged_key<Ds>...>;
};
template <class D> struct is_source : std::bool_constant<SourceDescriptor<D>> {};
}  // namespace detail::sub

template <Row R>
using source_key_t = typename detail::sub::key_variant<
    meta::filter_t<detail::sub::is_source, typename R::effects>>::type;

// ── core source: every ───────────────────────────────────────────────────
namespace fx {

/// Deliver msg every interval, on the kernel's timer heap.
///
/// Key: (interval, ordinal). The ordinal is this timer's position among
/// same-interval `every`s in one subscribe() result. It lets two timers with
/// the same interval coexist, and a timer keep its phase while the model
/// changes. maya's old interval-only key silently starved every
/// same-interval timer after the first.
///
/// The ordinal isn't something the program writes. A source that sets
/// `numbered = true` gets its ordinal filled in by the reconciler, which
/// numbers keys with the same base in the order they appear.
struct every {
    static constexpr std::string_view name = "every";
    template <class Msg> struct type {
        std::chrono::milliseconds interval;
        Msg msg;
    };
    template <class F, class M>
    static auto fmap(F&& f, type<M> e) -> type<std::invoke_result_t<F, M>> {
        return {e.interval, std::invoke(std::forward<F>(f), std::move(e.msg))};
    }
    template <class Id, class F, class M>
    static auto fmap_with(const Id& id, F&& f, type<M> e)
        -> type<std::invoke_result_t<F, const Id&, M>> {
        return {e.interval, std::invoke(std::forward<F>(f), id, std::move(e.msg))};
    }

    struct key_type {
        std::int64_t  interval_ms = 0;
        std::uint32_t ordinal     = 0;
        bool operator==(const key_type&) const = default;
    };
    /// The base key; the reconciler fills in ordinal.
    template <class M>
    static key_type key(const type<M>& e) { return {e.interval.count(), 0}; }

    /// Tells the reconciler to number same-base keys (see above).
    static constexpr bool numbered = true;

    template <class Self, class Msg> struct ctors {
        [[nodiscard]] static Self every(std::chrono::milliseconds i, Msg m) {
            return Self(type<Msg>{i, std::move(m)});
        }
    };
};

}  // namespace fx

// core_src (every + stream) is declared in core_fx.hpp, which can see
// fx::stream; stream.hpp depends on the task machinery in fx.hpp.

}  // namespace jaal

template <>
struct std::hash<jaal::fx::every::key_type> {
    std::size_t operator()(const jaal::fx::every::key_type& k) const noexcept {
        const auto a = static_cast<std::uint64_t>(k.interval_ms);
        return static_cast<std::size_t>(a * 0x9E3779B97F4A7C15ull ^ k.ordinal);
    }
};
