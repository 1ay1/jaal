#pragma once
// jaal::children<Child, Parent, Wrap> — a keyed LIST of child programs.
//
// child<> (core/child.hpp) embeds ONE child in a fixed slot. Real apps hold
// a variable number: agentty's sessions, a tab bar, an editor's open
// buffers, a file tree's expanded nodes. Each needs its own Model, its own
// timers and streams, and messages routed back to the right one.
//
// The parent's Msg carries an id alongside the child's Msg:
//
//   struct ToTab { int id; Tab::Msg msg; };          // Wrap
//   struct NewTab {};
//   struct CloseTab { int id; };
//   using Msg  = std::variant<ToTab, NewTab, CloseTab>;
//   using Tabs = jaal::children<Tab, App, ToTab>;    // id type read from Wrap
//
//   struct Model { Tabs::map tabs; };                // id -> Tab::Model
//
//   static Cmd update(Model& m, ToTab t)   { return Tabs::update(m.tabs, t); }
//   static Cmd update(Model& m, NewTab)    { return Tabs::add(m.tabs).second; }
//   static Cmd update(Model& m, CloseTab c) {
//       Tabs::remove(m.tabs, c.id);                  // its subs stop; see below
//       return {};
//   }
//
//   static Sub subscribe(const Model& m) { return Tabs::subscribe(m.tabs); }
//
// Like child<>, `Parent` is only used for its Msg inside member functions,
// so the alias may appear inside the parent struct.
//
// What this gets right, and hand-rolled code usually doesn't:
//
//   * STREAM KEYS ARE PER CHILD. Two tabs both subscribing to stream "fetch"
//     would collide into one subscription (reconcile keys by string), so one
//     tab's fetch would feed the other. children<> prefixes every child's
//     stream key with its id, so they're distinct, and reconcile starts and
//     stops them per child.
//   * REMOVING A CHILD STOPS ITS WORK. Drop it from the map and its streams
//     disappear from the next subscribe(), so the reconciler fires their
//     stop_tokens. No leaked thread, no message from a tab that's gone.
//   * ONE ID TYPE. The id is read from Wrap, so the parent can't route with
//     an int while the map is keyed by string.
//   * BACKGROUND WORK STAYS SENDABLE. Effects are mapped with map_with, so
//     the id travels by value into tasks and streams instead of in a capture
//     that would have to cross a thread (cmd.hpp map_with).
//
// The id must be Sendable (it reaches worker threads), copyable, hashable
// and printable into a stream key: an integer, a string, or an enum. A
// monotonically increasing integer is the usual choice, and `add()` hands
// one out.

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "child.hpp"
#include "cmd.hpp"
#include "program.hpp"
#include "sendable.hpp"
#include "sub.hpp"

namespace jaal {

namespace detail::kids {

// The id and Msg fields of a Wrap like `struct ToTab { int id; Tab::Msg msg; }`.
// Read structurally, so a Wrap is a plain aggregate with no jaal in it.
template <class W>
concept keyed_wrap = requires(W w) {
    { w.id };
    { w.msg };
};

template <class W> using id_of  = std::remove_cvref_t<decltype(std::declval<W&>().id)>;
template <class W> using msg_of = std::remove_cvref_t<decltype(std::declval<W&>().msg)>;

/// An id as a stream-key prefix. Integers and enums go through to_string;
/// a string id is used as it is. Anything else needs a `to_key` overload
/// findable by ADL.
template <class Id>
std::string key_of(const Id& id) {
    if constexpr (requires { to_key(id); })            return std::string(to_key(id));
    else if constexpr (std::is_convertible_v<Id, std::string>) return std::string(id);
    else if constexpr (std::is_enum_v<Id>)
        return std::to_string(static_cast<std::underlying_type_t<Id>>(id));
    else if constexpr (requires { std::to_string(id); }) return std::to_string(id);
    else {
        static_assert(false,
            "jaal::children: the id type can't be turned into a stream-key prefix; "
            "give it `std::string to_key(const Id&)` findable by ADL");
    }
}

}  // namespace detail::kids

/// A keyed list of child programs. See the header comment.
///
/// `From`, when given, is a case of Parent::Msg with two fields, `id` and
/// `out`: `struct FromTab { int id; Tab::Out out; };`. Every
/// `Cmd::report(...)` a child returns arrives as From, carrying WHICH child
/// reported, folded into the parent in the same step (D41). A child whose Cmd
/// can report must be given a From.
template <Program Child, class Parent, class Wrap, class From = detail::childx::no_from>
    requires detail::kids::keyed_wrap<Wrap>
class children {
public:
    using msg_type   = detail::kids::msg_of<Wrap>;
    using id_type    = detail::kids::id_of<Wrap>;
    using model_type = typename Child::Model;

    static constexpr bool reports =
        !std::is_void_v<detail::childx::report_of<typename Child::Cmd>>;
    static_assert(!reports || !std::is_same_v<From, detail::childx::no_from>,
                  "jaal::children: these children can report (their Cmd has fx::report), "
                  "so give children<> a fourth argument: the case of Parent::Msg their "
                  "reports arrive as, e.g. `struct FromTab { int id; Tab::Out out; };`");

    static_assert(std::same_as<msg_type, typename Child::Msg>,
                  "jaal::children: Wrap::msg must be the child's Msg type");
    static_assert(Sendable<id_type>,
                  "jaal::children: the id reaches worker threads (tasks, streams), "
                  "so it must be Sendable: use an integer, a string or an enum");
    static_assert(std::copyable<id_type>,
                  "jaal::children: the id is copied into each mapped effect");

    /// The parent's field: id -> child model. Ordered, so subscribe() and
    /// any view walk children in a stable order (a hash map would reshuffle
    /// them between frames, and a UI would jitter).
    using map = std::map<id_type, model_type>;

    /// Child::Msg from this child → Parent::Msg. Captureless (it takes the id
    /// as a value), so it may run on a worker thread.
    template <class PM = typename Parent::Msg>
    static PM wrap(const id_type& id, msg_type m) {
        static_assert(detail::childx::is_alt<PM, Wrap>::value,
                      "jaal::children<Child, Parent, Wrap>: Wrap must be a case of Parent::Msg");
        return PM{Wrap{id, std::move(m)}};
    }

private:
    /// A child's Cmd as the parent's: each Msg wrapped with the id, then each
    /// report turned into a send of From{id, out}. The id is captured by
    /// value into the resolver, which runs here, on the loop thread, before
    /// the Cmd goes anywhere; nothing that reaches a worker holds it.
    template <class C>
    static auto lift(const id_type& id, C c) {
        auto mapped = std::move(c).map_with(id, &wrap<>);
        if constexpr (reports) {
            using PM = typename Parent::Msg;
            static_assert(detail::childx::is_alt<PM, From>::value,
                          "jaal::children<..., From>: From must be a case of Parent::Msg");
            return detail::childx::resolve_reports(std::move(mapped), [&id](auto out) {
                return PM{From{id, std::move(out)}};
            });
        } else {
            return mapped;
        }
    }

public:

    // ── the list ────────────────────────────────────────────────────────
    /// Add a child under `id`, running its init(). Replaces any child
    /// already there (and so drops that one's subscriptions).
    static auto add(map& ms, id_type id) {
        auto [model, cmd] = prog::init<Child>();
        ms.insert_or_assign(id, std::move(model));
        return lift(id, std::move(cmd));
    }

    /// Add under the next free integer id, and hand it back. Only for an
    /// integral id: `auto [id, cmd] = Tabs::add(m.tabs);`
    static auto add(map& ms)
        requires std::is_integral_v<id_type>
    {
        const id_type id = ms.empty() ? id_type{0}
                                      : static_cast<id_type>(ms.rbegin()->first + 1);
        return std::pair{id, add(ms, id)};
    }

    /// Drop a child. Its streams stop at the next subscribe(), and its
    /// pending timers are dropped with it. True when there was one.
    static bool remove(map& ms, const id_type& id) { return ms.erase(id) != 0; }

    /// Drop every child.
    static void clear(map& ms) { ms.clear(); }

    [[nodiscard]] static bool has(const map& ms, const id_type& id) {
        return ms.find(id) != ms.end();
    }

    /// The child's model, or null.
    [[nodiscard]] static const model_type* find(const map& ms, const id_type& id) {
        auto it = ms.find(id);
        return it == ms.end() ? nullptr : &it->second;
    }

    // ── the fold ────────────────────────────────────────────────────────
    /// Fold one wrapped message into its child's model, in place, and return
    /// the child's Cmd mapped into the parent. A message for a child that
    /// isn't there is dropped (it was closed while its task was in flight —
    /// normal, not an error).
    template <class W>
        requires std::same_as<std::remove_cvref_t<W>, Wrap>
    static auto update(map& ms, W&& w) {
        using out = decltype(lift(w.id, prog::update<Child>(std::declval<model_type&>(), w.msg)));
        auto it = ms.find(w.id);
        if (it == ms.end()) return out::none();
        const id_type id = w.id;
        return lift(id, prog::update<Child>(it->second, std::forward<W>(w).msg));
    }

    /// Every child's subscriptions, batched, each keyed under its own id so
    /// two children asking for the same stream key don't collide. A child
    /// with no subscribe() yields an empty Sub.
    [[nodiscard]] static auto subscribe(const map& ms) {
        using sub_type = decltype(prog::subscribe<Child>(std::declval<const model_type&>())
                                      .map_with(std::declval<id_type>(), &wrap<>));
        std::vector<sub_type> subs;
        subs.reserve(ms.size());
        for (const auto& [id, model] : ms) {
            auto s = prog::subscribe<Child>(model);
            if (s.is_none()) continue;
            std::string prefix = detail::kids::key_of(id);
            prefix += '/';
            detail::childx::prefix_streams(s, prefix);
            subs.push_back(std::move(s).map_with(id, &wrap<>));
        }
        if (subs.empty()) return sub_type::none();
        return sub_type::batch(std::move(subs));
    }
};

}  // namespace jaal
