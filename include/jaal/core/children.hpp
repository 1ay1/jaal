#pragma once
// jaal::children<Child, ParentMsg, Wrap> — a keyed LIST of child programs.
//
// child<> (core/child.hpp) embeds ONE child in a fixed slot. Real apps hold
// a variable number: agentty's sessions, a tab bar, an editor's open
// buffers, a file tree's expanded nodes. Each needs its own Model, its own
// timers and streams, and messages routed back to the right one.
//
// The parent's Msg carries an id alongside the child's Msg:
//
//   struct ToTab { int id; Tab::Msg msg; };          // Wrap
//   using Msg  = std::variant<ToTab, NewTab, CloseTab>;
//   using Tabs = jaal::children<Tab, Msg, ToTab>;    // id type read from Wrap
//
//   struct Model { Tabs::map tabs; };                // id -> Tab::Model
//
//   static std::pair<Model, Cmd> update(Model m, Msg msg) {
//       if (auto* c = Tabs::match(msg))              // {id, &child_msg}
//           return {m, Tabs::update(m.tabs, *c)};    // routes to that child
//       if (std::holds_alternative<NewTab>(msg)) {
//           auto [id, cmd] = Tabs::add(m.tabs);      // init() for the new one
//           return {m, cmd};
//       }
//       if (auto* c = std::get_if<CloseTab>(&msg)) {
//           Tabs::remove(m.tabs, c->id);             // its subs stop; see below
//           return {m, Cmd::none()};
//       }
//       ...
//   }
//
//   static Sub subscribe(const Model& m) { return Tabs::subscribe(m.tabs); }
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
template <Program Child, class ParentMsg, class Wrap>
    requires detail::kids::keyed_wrap<Wrap>
class children {
public:
    using msg_type   = detail::kids::msg_of<Wrap>;
    using id_type    = detail::kids::id_of<Wrap>;
    using model_type = typename Child::Model;

    static_assert(std::same_as<msg_type, typename Child::Msg>,
                  "jaal::children: Wrap::msg must be the child's Msg type");
    static_assert(Sendable<id_type>,
                  "jaal::children: the id reaches worker threads (tasks, streams), "
                  "so it must be Sendable: use an integer, a string or an enum");
    static_assert(std::copyable<id_type>,
                  "jaal::children: the id is copied into each mapped effect");

    /// The Cmd type this returns: the child's row, re-targeted at the
    /// parent's Msg. Deduced, so the parent never repeats its row and can't
    /// get it wrong. (The Sub type is deduced per call instead: a child with
    /// no subscribe() has no Sub type to name.)
    using cmd_type = decltype(std::declval<cmd_of<Child>>().map_with(
        std::declval<id_type>(), std::declval<ParentMsg (*)(const id_type&, msg_type)>()));

    /// The parent's field: id -> child model. Ordered, so subscribe() and
    /// any view walk children in a stable order (a hash map would reshuffle
    /// them between frames, and a UI would jitter).
    using map = std::map<id_type, model_type>;

    /// One routed message: which child, and what for it.
    struct routed {
        id_type         id;
        const msg_type* msg;
    };

    // ── routing ─────────────────────────────────────────────────────────
    /// The child message inside `m`, with its id, or null when `m` isn't for
    /// a child. The returned pointer borrows from `m`.
    [[nodiscard]] static std::optional<routed> match(const ParentMsg& m) noexcept {
        if (const auto* w = std::get_if<Wrap>(&m)) return routed{w->id, &w->msg};
        return std::nullopt;
    }

    /// Child::Msg from this child → ParentMsg. Captureless (it takes the id
    /// as a value), so it may run on a worker thread.
    static ParentMsg wrap(const id_type& id, msg_type m) {
        return ParentMsg{Wrap{id, std::move(m)}};
    }

    // ── the list ────────────────────────────────────────────────────────
    /// Add a child under `id`, running its init(). Replaces any child
    /// already there (and so drops that one's subscriptions).
    static cmd_type add(map& ms, id_type id) {
        auto [model, cmd] = run_init<Child>();
        ms.insert_or_assign(id, std::move(model));
        return std::move(cmd).map_with(id, &wrap);
    }

    /// Add under the next free integer id, and hand it back. Only for an
    /// integral id: `auto [id, cmd] = Tabs::add(m.tabs);`
    static std::pair<id_type, cmd_type> add(map& ms)
        requires std::is_integral_v<id_type>
    {
        const id_type id = ms.empty() ? id_type{0}
                                      : static_cast<id_type>(ms.rbegin()->first + 1);
        return {id, add(ms, id)};
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
    /// Fold one child's message into its model, in place, and return its Cmd
    /// mapped into the parent. A message for a child that isn't there is
    /// dropped (it was closed while its task was in flight — normal, not an
    /// error).
    static cmd_type update(map& ms, const routed& r) {
        auto it = ms.find(r.id);
        if (it == ms.end()) return cmd_type::none();
        auto [next, cmd] = detail::prog::split(Child::update(std::move(it->second), *r.msg));
        it->second = std::move(next);
        return std::move(cmd).map_with(r.id, &wrap);
    }

    /// Every child's subscriptions, batched, each keyed under its own id so
    /// two children asking for the same stream key don't collide. A child
    /// with no subscribe() yields an empty Sub, as run_subscribe defines.
    [[nodiscard]] static auto subscribe(const map& ms) {
        using sub_type = decltype(run_subscribe<Child>(std::declval<const model_type&>())
                                      .map_with(std::declval<id_type>(), &wrap));
        std::vector<sub_type> subs;
        subs.reserve(ms.size());
        for (const auto& [id, model] : ms) {
            auto s = run_subscribe<Child>(model);
            if (s.is_none()) continue;
            std::string prefix = detail::kids::key_of(id);
            prefix += '/';
            detail::childx::prefix_streams(s, prefix);
            subs.push_back(std::move(s).map_with(id, &wrap));
        }
        if (subs.empty()) return sub_type::none();
        return sub_type::batch(std::move(subs));
    }
};

}  // namespace jaal
