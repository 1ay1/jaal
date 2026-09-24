// tests/core/children_test.cpp — a keyed LIST of child programs, and
// Cmd::send.
//
// What must hold:
//   * messages route to the right child, and only that child folds
//   * a message for a child that's gone is dropped, not a crash
//   * each child's STREAM KEYS are prefixed with its id, so two children
//     subscribing to the same key get two distinct subscriptions instead of
//     silently sharing one (the bug this type exists to prevent)
//   * removing a child drops its subscriptions, so the reconciler stops them
//   * children are walked in a stable order (a UI mustn't jitter)
//   * effects with background work map through map_with, carrying the id by
//     value rather than in a capture
//   * Cmd::send folds in the SAME step, unlike after(0ms)

#include <jaal/core.hpp>
#include <jaal/host/headless.hpp>
#include <jaal/jaal.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <variant>
#include <vector>

using namespace std::chrono_literals;
using jaal::Sink;

#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,   \
                         __LINE__, #c);                                   \
            return 1;                                                     \
        }                                                                 \
    } while (0)

namespace {

// ── a child with its own timer, task and stream ───────────────────────────
struct Tab {
    struct Model {
        int  n       = 0;
        bool loading = false;
        int  ticks   = 0;
    };
    struct Bump {};
    struct Load {};
    struct Loaded { int v; };
    struct Tick {};
    struct Watch {};
    using Msg = std::variant<Bump, Load, Loaded, Tick, Watch>;
    using Cmd = jaal::CoreCmd<Msg>;
    using Sub = jaal::CoreSub<Msg>;

    static Model init() { return {}; }

    static std::pair<Model, Cmd> update(Model m, Msg msg) {
        if (std::holds_alternative<Bump>(msg)) {
            ++m.n;
            return {m, Cmd::none()};
        }
        if (std::holds_alternative<Load>(msg)) {
            m.loading = true;
            // A task: its mapper must be captureless, which is exactly what
            // makes a keyed list hard without map_with.
            return {m, Cmd::task([](Sink<Msg> s, std::stop_token, int v) {
                           s.send(Loaded{v});
                       }, 7)};
        }
        if (auto* l = std::get_if<Loaded>(&msg)) {
            m.loading = false;
            m.n += l->v;
            return {m, Cmd::none()};
        }
        if (std::holds_alternative<Tick>(msg)) {
            ++m.ticks;
            return {m, Cmd::none()};
        }
        return {m, Cmd::none()};              // Watch: only a marker
    }

    // Every tab asks for the SAME stream key. Unprefixed, they'd collide.
    static Sub subscribe(const Model& m) {
        if (!m.loading) return Sub::none();
        return Sub::stream("feed", [](Sink<Msg> s, std::stop_token st) {
            while (!st.stop_requested()) s.send(Tick{});
        });
    }
};

// ── the parent: a list of tabs ───────────────────────────────────────────
struct ToTab { int id; Tab::Msg msg; };
struct NewTab {};
struct CloseTab { int id; };
struct BumpAll {};

struct App {
    using Msg  = std::variant<ToTab, NewTab, CloseTab, BumpAll>;
    using Tabs = jaal::children<Tab, Msg, ToTab>;
    using Cmd  = jaal::CoreCmd<Msg>;
    using Sub  = jaal::CoreSub<Msg>;

    struct Model { Tabs::map tabs; };

    static Model init() { return {}; }

    static std::pair<Model, Cmd> update(Model m, Msg msg) {
        if (auto r = Tabs::match(msg)) return {std::move(m), fold_child(m, *r)};
        if (std::holds_alternative<NewTab>(msg)) {
            auto [id, cmd] = Tabs::add(m.tabs);
            (void)id;
            return {std::move(m), std::move(cmd)};
        }
        if (auto* c = std::get_if<CloseTab>(&msg)) {
            Tabs::remove(m.tabs, c->id);
            return {std::move(m), Cmd::none()};
        }
        // BumpAll: reuse the per-tab path instead of duplicating it. This is
        // what Cmd::send is for.
        std::vector<Cmd> cs;
        for (const auto& [id, model] : m.tabs)
            cs.push_back(Cmd::send(Msg{ToTab{id, Tab::Bump{}}}));
        return {std::move(m), Cmd::batch(std::move(cs))};
    }

    static Sub subscribe(const Model& m) { return Tabs::subscribe(m.tabs); }

private:
    static Cmd fold_child(Model& m, const Tabs::routed& r) {
        return Tabs::update(m.tabs, r);
    }
};

using H = jaal::headless<App>;

int routes_to_the_right_child() {
    H h;
    h.send(NewTab{});                          // id 0
    h.send(NewTab{});                          // id 1
    h.advance(1ms);
    CHECK(h.model().tabs.size() == 2);

    h.send(ToTab{1, Tab::Bump{}});
    h.advance(1ms);
    CHECK(App::Tabs::find(h.model().tabs, 0)->n == 0);   // untouched
    CHECK(App::Tabs::find(h.model().tabs, 1)->n == 1);   // folded
    return 0;
}

int message_for_a_missing_child_is_dropped() {
    H h;
    h.send(NewTab{});
    h.advance(1ms);
    h.send(ToTab{99, Tab::Bump{}});            // no such tab
    h.advance(1ms);
    CHECK(h.model().tabs.size() == 1);
    CHECK(App::Tabs::find(h.model().tabs, 0)->n == 0);
    CHECK(h.kernel().fault_count() == 0);      // dropped, not a fault
    return 0;
}

int stream_keys_are_per_child() {
    // Both tabs load, so both ask for stream key "feed". The parent's Sub
    // must carry two DISTINCT keys: unprefixed, the reconciler (which keys
    // streams by string) would collapse them into one subscription and one
    // tab would never tick. Asserted on the Sub itself, so no threads run.
    App::Model m;
    App::Tabs::add(m.tabs, 0);
    App::Tabs::add(m.tabs, 1);
    m.tabs[0].loading = true;
    m.tabs[1].loading = true;

    auto s = App::subscribe(m);
    std::vector<std::string> keys;
    s.for_each([&]<class X>(const X& x) {
        if constexpr (std::same_as<std::remove_cvref_t<X>,
                                   jaal::payload_t<jaal::fx::stream, App::Msg>>)
            keys.push_back(x.key);
    });
    CHECK(keys.size() == 2);
    CHECK(keys[0] == "0/feed");
    CHECK(keys[1] == "1/feed");

    // One tab stops loading: only the other's stream remains, so the
    // reconciler stops the first by key.
    m.tabs[0].loading = false;
    auto s2 = App::subscribe(m);
    std::vector<std::string> left;
    s2.for_each([&]<class X>(const X& x) {
        if constexpr (std::same_as<std::remove_cvref_t<X>,
                                   jaal::payload_t<jaal::fx::stream, App::Msg>>)
            left.push_back(x.key);
    });
    CHECK(left.size() == 1);
    CHECK(left[0] == "1/feed");
    return 0;
}

int removing_a_child_drops_its_subscriptions() {
    App::Model m;
    App::Tabs::add(m.tabs, 0);
    m.tabs[0].loading = true;
    CHECK(!App::subscribe(m).is_none());

    App::Tabs::remove(m.tabs, 0);
    // Gone from the model, so gone from subscribe(): the reconciler fires
    // its stop_token at the next step. No leaked thread.
    CHECK(App::subscribe(m).is_none());
    CHECK(m.tabs.empty());
    return 0;
}

int order_is_stable() {
    H h;
    for (int i = 0; i < 5; ++i) h.send(NewTab{});
    h.advance(1ms);
    CHECK(h.model().tabs.size() == 5);
    int expect = 0;
    for (const auto& [id, model] : h.model().tabs) {
        CHECK(id == expect);                   // ascending, every time
        ++expect;
    }
    return 0;
}

int task_results_come_back_to_their_own_child() {
    H h;
    h.send(NewTab{});
    h.send(NewTab{});
    h.advance(1ms);
    h.send(ToTab{1, Tab::Load{}});             // only tab 1 loads
    // The task runs on the real pool, so wait for it rather than assuming a
    // step is enough (advance() doesn't wait for another thread).
    CHECK(h.run_until_idle());
    CHECK(App::Tabs::find(h.model().tabs, 0)->n == 0);
    CHECK(App::Tabs::find(h.model().tabs, 1)->n == 7);   // the task's value
    CHECK(!App::Tabs::find(h.model().tabs, 1)->loading);
    return 0;
}

int send_reaches_every_child_in_one_step() {
    H h;
    for (int i = 0; i < 3; ++i) h.send(NewTab{});
    h.advance(1ms);
    h.send(BumpAll{});
    h.advance(1ms);
    for (const auto& [id, model] : h.model().tabs) {
        (void)id;
        CHECK(model.n == 1);                   // every tab folded the Bump
    }
    return 0;
}

// ── Cmd::send is not a timer ─────────────────────────────────────────────
struct Chain {
    struct Model { std::vector<int> seen; };
    struct A {}; struct B {}; struct C {};
    using Msg = std::variant<A, B, C>;
    using Cmd = jaal::CoreCmd<Msg>;

    static Model init() { return {}; }

    static std::pair<Model, Cmd> update(Model m, Msg msg) {
        if (std::holds_alternative<A>(msg)) {
            m.seen.push_back(1);
            return {m, Cmd::batch(Cmd::send(Msg{B{}}), Cmd::send(Msg{C{}}))};
        }
        if (std::holds_alternative<B>(msg)) {
            m.seen.push_back(2);
            return {m, Cmd::none()};
        }
        m.seen.push_back(3);
        return {m, Cmd::none()};
    }
};

int send_folds_in_order_without_the_clock() {
    jaal::headless<Chain> h;
    // send() itself steps, and a sent message needs no timer: one step folds
    // A and then both of its sends, in order, with the clock untouched.
    h.send(Chain::A{});
    CHECK(h.model().seen.size() == 3);
    CHECK(h.model().seen[0] == 1);
    CHECK(h.model().seen[1] == 2);
    CHECK(h.model().seen[2] == 3);
    return 0;
}

}  // namespace

int main() {
    if (int r = routes_to_the_right_child())            return r;
    if (int r = message_for_a_missing_child_is_dropped()) return r;
    if (int r = stream_keys_are_per_child())            return r;
    if (int r = removing_a_child_drops_its_subscriptions()) return r;
    if (int r = order_is_stable())                      return r;
    if (int r = task_results_come_back_to_their_own_child()) return r;
    if (int r = send_reaches_every_child_in_one_step()) return r;
    if (int r = send_folds_in_order_without_the_clock()) return r;
    std::puts("children: ok");
    return 0;
}
