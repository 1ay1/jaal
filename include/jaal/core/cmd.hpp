#pragma once
// jaal::basic_cmd<Msg, Row> — effects as data, with the allowed set in the type.
//
// Apps don't name this: they write jaal::Cmd<Msg, extra...> (program.hpp),
// which always includes the core effects. basic_cmd takes an exact row and
// is what the kernel, hosts and composition helpers work with.
//
// Row says which effects a Cmd may contain. basic_cmd::inner is a plain
// std::variant over exactly those effects (plus None and Batch), so tests
// and hosts use std::get_if / std::visit on it like on any variant.
//
// Three rules the types enforce:
//
//   1. An effect converts into a Cmd only if it's in the row.
//        jaal::Cmd<Msg> c = Beep{};              // error: beep not in the row
//
//   2. Row widening. A Cmd over row A converts to one over B when A ⊆ B. So
//      a child that only uses core effects fits into a parent that allows
//      more, and never the other way round.
//
//   3. map(f) re-targets every effect at a new Msg type through each
//      effect's fmap. Effects must provide it (Effect concept), so an effect
//      that carries a Msg can't be forgotten by map.
//
// Factory functions come from the effects themselves: if an effect
// descriptor has `ctors<Self, Msg>`, the Cmd inherits it. So Cmd::after(...)
// exists only on a Cmd whose row has `after`, and maya can add
// Cmd::commit_scrollback(...) without jaal knowing it exists.

#include <concepts>
#include <functional>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "../meta/list.hpp"
#include "effect.hpp"
#include "row.hpp"

namespace jaal {

template <class Msg, Row R> class basic_cmd;

namespace detail::cmd {

// Inherit D::ctors<Self, Msg> when D provides it; otherwise nothing.
template <class D, class Self, class Msg> struct ctors_of {};
template <class D, class Self, class Msg>
    requires requires { typename D::template ctors<Self, Msg>; }
struct ctors_of<D, Self, Msg> : D::template ctors<Self, Msg> {};

template <class T> inline constexpr bool is_cmd_v = false;
template <class M, class R> inline constexpr bool is_cmd_v<basic_cmd<M, R>> = true;

}  // namespace detail::cmd

template <class Msg, Effect... Ds>
class basic_cmd<Msg, row<Ds...>>
    : public detail::cmd::ctors_of<Ds, basic_cmd<Msg, row<Ds...>>, Msg>... {
public:
    using msg_type = Msg;
    using row_type = row<Ds...>;

    struct None {};
    struct Batch { std::vector<basic_cmd> cmds; };

    using variant = std::variant<None, Batch, payload_t<Ds, Msg>...>;
    variant inner;

    // ── construction ────────────────────────────────────────────────────
    basic_cmd() noexcept : inner(None{}) {}

    /// Any effect payload in the row.
    template <class E>
        requires meta::member_of<std::remove_cvref_t<E>,
                                 meta::list<payload_t<Ds, Msg>...>>
    basic_cmd(E&& e) : inner(std::forward<E>(e)) {}          // NOLINT: implicit on purpose

    /// An effect payload NOT in the row: rejected with a reason, rather than
    /// "no viable conversion".
    template <class E>
        requires (!meta::member_of<std::remove_cvref_t<E>,
                                   meta::list<payload_t<Ds, Msg>...>>)
              && (!detail::cmd::is_cmd_v<std::remove_cvref_t<E>>)
              && (!std::same_as<std::remove_cvref_t<E>, None>)
              && (!std::same_as<std::remove_cvref_t<E>, Batch>)
#if defined(__cpp_deleted_function) && __cpp_deleted_function >= 202403L
    basic_cmd(E&&) = delete("jaal: this effect is not in the Cmd's row; add it to the row "
                      "(jaal::Cmd<Msg, ..., this_effect>)");
#else
    basic_cmd(E&&) = delete;
#endif

    /// Row widening: a basic_cmd allowed fewer effects fits where more are allowed.
    template <class R>
        requires subrow_of<R, row_type> && (!std::same_as<R, row_type>)
    basic_cmd(basic_cmd<Msg, R> narrow) : inner(None{}) {        // NOLINT: implicit on purpose
        using N = basic_cmd<Msg, R>;
        std::visit([this]<class X>(X&& x) {
            using U = std::remove_cvref_t<X>;
            if constexpr (std::same_as<U, typename N::None>) {
                inner = None{};
            } else if constexpr (std::same_as<U, typename N::Batch>) {
                Batch b;
                b.cmds.reserve(x.cmds.size());
                for (auto& c : x.cmds) b.cmds.emplace_back(basic_cmd(std::move(c)));
                inner = std::move(b);
            } else {
                inner = std::forward<X>(x);          // same payload type in both rows
            }
        }, std::move(narrow.inner));
    }

    /// Narrowing is never implicit.
    template <class R>
        requires (!subrow_of<R, row_type>)
#if defined(__cpp_deleted_function) && __cpp_deleted_function >= 202403L
    basic_cmd(basic_cmd<Msg, R>) = delete("jaal: can't convert to a Cmd with fewer effects "
                              "(only widening is allowed)");
#else
    basic_cmd(basic_cmd<Msg, R>) = delete;
#endif

    basic_cmd(const basic_cmd&)            = default;
    basic_cmd(basic_cmd&&) noexcept        = default;
    basic_cmd& operator=(const basic_cmd&) = default;
    basic_cmd& operator=(basic_cmd&&) noexcept = default;

    // ── building ────────────────────────────────────────────────────────
    [[nodiscard]] static basic_cmd none() noexcept { return basic_cmd{}; }

    /// Batch, flattening nested batches and dropping Nones. Batch of one is
    /// that one; batch of nothing is none().
    [[nodiscard]] static basic_cmd batch(std::vector<basic_cmd> cmds) {
        Batch out;
        for (auto& c : cmds) {
            if (auto* b = std::get_if<Batch>(&c.inner)) {
                for (auto& inner_c : b->cmds) out.cmds.push_back(std::move(inner_c));
            } else if (!std::holds_alternative<None>(c.inner)) {
                out.cmds.push_back(std::move(c));
            }
        }
        if (out.cmds.empty()) return none();
        if (out.cmds.size() == 1) return std::move(out.cmds.front());
        basic_cmd r;
        r.inner = std::move(out);
        return r;
    }

    template <class... Cs>
        requires (sizeof...(Cs) > 0) && (std::convertible_to<Cs, basic_cmd> && ...)
    [[nodiscard]] static basic_cmd batch(Cs&&... cs) {
        std::vector<basic_cmd> v;
        v.reserve(sizeof...(Cs));
        (v.emplace_back(basic_cmd(std::forward<Cs>(cs))), ...);
        return batch(std::move(v));
    }

    // ── functor ─────────────────────────────────────────────────────────
    /// Re-target every effect at another Msg type. F must be callable with
    /// Msg; what it returns is the new Msg type. Consumes the basic_cmd.
    template <std::invocable<Msg> F>
    [[nodiscard]] auto map(F f) && -> basic_cmd<std::invoke_result_t<F, Msg>, row_type> {
        using To = basic_cmd<std::invoke_result_t<F, Msg>, row_type>;
        return std::visit([&]<class X>(X&& x) -> To {
            using U = std::remove_cvref_t<X>;
            if constexpr (std::same_as<U, None>) {
                return To{};
            } else if constexpr (std::same_as<U, Batch>) {
                typename To::Batch b;
                b.cmds.reserve(x.cmds.size());
                for (auto& c : x.cmds) b.cmds.push_back(std::move(c).map(f));
                To t;
                t.inner = std::move(b);
                return t;
            } else {
                return map_payload<To>(f, std::forward<X>(x));
            }
        }, std::move(inner));
    }

    template <std::invocable<Msg> F>
    [[nodiscard]] auto map(F f) const& -> basic_cmd<std::invoke_result_t<F, Msg>, row_type>
        requires std::copyable<basic_cmd>
    {
        return basic_cmd(*this).map(std::move(f));
    }

    /// Re-target every effect with a mapper that also gets an ID: `f(id, msg)`.
    ///
    /// This is what map() can't do for background work. A task's or stream's
    /// mapper runs on another thread, so it must be captureless — which means
    /// a plain map() can never tell the mapper WHICH of several children a
    /// message belongs to. Here the id travels in the effect by value
    /// (Sendable, like any task argument) instead of in a capture, so a keyed
    /// LIST of children works for every effect, tasks and streams included
    /// (core/children.hpp).
    template <class Id, class F>
        requires std::invocable<F, const Id&, Msg>
    [[nodiscard]] auto map_with(Id id, F f) &&
        -> basic_cmd<std::invoke_result_t<F, const Id&, Msg>, row_type> {
        using To = basic_cmd<std::invoke_result_t<F, const Id&, Msg>, row_type>;
        return std::visit([&]<class X>(X&& x) -> To {
            using U = std::remove_cvref_t<X>;
            if constexpr (std::same_as<U, None>) {
                return To{};
            } else if constexpr (std::same_as<U, Batch>) {
                typename To::Batch b;
                b.cmds.reserve(x.cmds.size());
                for (auto& c : x.cmds) b.cmds.push_back(std::move(c).map_with(id, f));
                To t;
                t.inner = std::move(b);
                return t;
            } else {
                return map_payload_with<To>(id, f, std::forward<X>(x));
            }
        }, std::move(inner));
    }

    // ── queries ─────────────────────────────────────────────────────────
    [[nodiscard]] bool is_none() const noexcept {
        return std::holds_alternative<None>(inner);
    }

    /// Does this basic_cmd (including inside batches) contain an effect of kind D?
    template <Effect D>
        requires in_row<D, row_type>
    [[nodiscard]] bool contains() const {
        return std::visit([]<class X>(const X& x) {
            using U = std::remove_cvref_t<X>;
            if constexpr (std::same_as<U, Batch>) {
                for (auto& c : x.cmds)
                    if (c.template contains<D>()) return true;
                return false;
            } else {
                return std::same_as<U, payload_t<D, Msg>>;
            }
        }, inner);
    }

private:
    // Find the ONE descriptor whose payload is U and fmap through it.
    // Payload types are distinct within a row: two effects with the same
    // payload type would make the variant ambiguous, which row validation
    // (payloads_distinct below) rules out.
    template <class To, class F, class P>
    static To map_payload(F& f, P&& p) {
        using U = std::remove_cvref_t<P>;
        To out;
        bool done = false;
        ([&] {
            if constexpr (std::same_as<U, payload_t<Ds, Msg>>) {
                out  = To(Ds::fmap(f, std::forward<P>(p)));
                done = true;
            }
        }(), ...);
        (void)done;
        return out;
    }

    // Same, through fmap_with, so the mapper is handed the id. An effect
    // that carries a Msg must provide fmap_with to be usable here; the
    // static_assert says so instead of failing deep inside the pack.
    template <class To, class Id, class F, class P>
    static To map_payload_with(const Id& id, F& f, P&& p) {
        using U = std::remove_cvref_t<P>;
        To out;
        ([&] {
            if constexpr (std::same_as<U, payload_t<Ds, Msg>>) {
                static_assert(
                    requires { Ds::fmap_with(id, f, std::forward<P>(p)); },
                    "jaal: this effect has no fmap_with, so it can't be mapped with "
                    "an id; add fmap_with(id, f, payload) next to its fmap");
                out = To(Ds::fmap_with(id, f, std::forward<P>(p)));
            }
        }(), ...);
        return out;
    }

    // Each effect's payload must be a distinct type, or inner would hold
    // the same alternative twice and std::get/holds_alternative would be
    // ambiguous.
    static_assert(meta::unique<meta::list<payload_t<Ds, Msg>...>>,
                  "jaal: two effects in this row have the same payload type; "
                  "give each effect its own payload struct");
};

// ── deduce a program's basic_cmd row ───────────────────────────────────────────
template <class C> struct cmd_traits;
template <class M, class R> struct cmd_traits<basic_cmd<M, R>> {
    using msg_type = M;
    using row_type = R;
};

}  // namespace jaal
