// tests/core/child_test.cpp — embedding one program in another.

#include <jaal/jaal.hpp>
#include <jaal/core/child.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <variant>

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

// A self-contained counter: a task doubles, a timer ticks, a stream feeds.
struct Counter {
    struct Model { int n = 0; bool live = true; bool operator==(const Model&) const = default; };
    struct Inc {}; struct Double {}; struct Set { int n; }; struct Tick {}; struct Fed {};
    struct Stop {};
    using Msg = std::variant<Inc, Double, Set, Tick, Fed, Stop>;
    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg>;

    static Cmd init(Model&) { return Cmd::after(1ms, Msg{Inc{}}); }
    static Cmd update(Model& m, Inc)   { ++m.n; return {}; }
    static Cmd update(Model& m, Double) {
        return Cmd::task([](Sink<Msg> out, std::stop_token, int n) {
                             out.send(Set{n * 2});
                         }, m.n);
    }
    static Cmd update(Model& m, Set s) { m.n = s.n; return {}; }
    static Cmd update(Model& m, Tick)  { m.n += 100; return {}; }
    static Cmd update(Model& m, Fed)   { m.n += 1000; return {}; }
    static Cmd update(Model& m, Stop)  { m.live = false; return {}; }
    static Sub subscribe(const Model& m) {
        if (!m.live) return Sub::none();
        return Sub::batch(Sub::every(10ms, Msg{Tick{}}),
                          Sub::stream("feed", [](Sink<Msg>, std::stop_token) {}));
    }
};

// The parent: two counters and a reset.
struct Pair {
    struct Left  { Counter::Msg msg; };
    struct Right { Counter::Msg msg; };
    struct Reset {};
    using Msg = std::variant<Left, Right, Reset>;
    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg>;
    using L = jaal::child<Counter, Pair, Left>;
    using R = jaal::child<Counter, Pair, Right>;

    struct Model {
        Counter::Model left, right;
        int resets = 0;
    };

    static Cmd init(Model& m) { return Cmd::batch(L::init(m.left), R::init(m.right)); }
    // Routing is just an overload per Wrap case.
    static Cmd update(Model& m, Left l)  { return L::update(m.left, l); }
    static Cmd update(Model& m, Right r) { return R::update(m.right, r); }
    static Cmd update(Model& m, Reset) {
        m.left  = Counter::Model{};
        m.right = Counter::Model{};
        ++m.resets;
        return {};
    }
    static Sub subscribe(const Model& m) {
        return Sub::batch(L::subscribe(m.left, "left"), R::subscribe(m.right, "right"));
    }
};

int routing_with_given() {
    auto t = jaal::given<Pair>();
    // init: each child asked for after(1ms, Inc), wrapped.
    CHECK(t.effects<jaal::fx::after>().size() == 2);
    CHECK(std::holds_alternative<Pair::Left>(t.effects<jaal::fx::after>()[0]->msg));
    CHECK(std::holds_alternative<Pair::Right>(t.effects<jaal::fx::after>()[1]->msg));

    t.when(Pair::Left{Counter::Inc{}}).when(Pair::Left{Counter::Inc{}})
     .when(Pair::Right{Counter::Inc{}});
    t.expect("routed", [](auto& m) { return m.left.n == 2 && m.right.n == 1; });

    // A child's task result comes back wrapped for the same child.
    t.when(Pair::Left{Counter::Double{}}).settle();
    t.expect("task mapped", [](auto& m) { return m.left.n == 4 && m.right.n == 1; });

    t.when(Pair::Reset{});
    t.expect("reset", [](auto& m) { return m.left.n == 0 && m.resets == 1; });
    CHECK(t.ok());
    if (!t.ok()) std::puts(t.report().c_str());
    return 0;
}

int child_api_direct() {
    // init(slot) fills the slot and returns the child's Cmd mapped to the parent.
    Counter::Model slot;
    auto ic = Pair::L::init(slot);
    CHECK(!ic.is_none());
    // update(slot, Wrap) routes the Wrap case straight into the child.
    auto uc = Pair::L::update(slot, Pair::Left{Counter::Inc{}});
    CHECK(slot.n == 1);
    CHECK(uc.is_none());
    // wrap<> builds the parent's Msg from a child Msg.
    Pair::Msg w = Pair::R::wrap<>(Counter::Msg{Counter::Inc{}});
    CHECK(std::holds_alternative<Pair::Right>(w));
    // And the parent's update overload for a Wrap case is the router.
    Pair::Model m;
    (void)jaal::prog::update<Pair>(m, Pair::Msg{Pair::Right{Counter::Inc{}}});
    CHECK(m.left.n == 0 && m.right.n == 1);
    return 0;
}

int subs_are_mapped_and_prefixed() {
    Pair::Model m;
    auto s = Pair::subscribe(m);
    // Two timers and two streams, stream keys don't collide.
    int streams = 0, timers = 0;
    bool left = false, right = false;
    s.for_each([&]<class X>(const X& x) {
        if constexpr (std::same_as<X, jaal::payload_t<jaal::fx::stream, Pair::Msg>>) {
            ++streams;
            left  |= x.key == "left/feed";
            right |= x.key == "right/feed";
        } else if constexpr (std::same_as<X, jaal::payload_t<jaal::fx::every, Pair::Msg>>) {
            ++timers;
        }
    });
    CHECK(streams == 2 && timers == 2);
    CHECK(left && right);
    return 0;
}

int runs_in_the_sim() {
    jaal::sim<Pair> s(3);
    s.at(25ms, Pair::Msg{Pair::Right{Counter::Stop{}}});
    s.at(55ms, Pair::Msg{Pair::Left{Counter::Stop{}}});
    auto r = s.run();
    CHECK(r.end == jaal::sim_end::idle);
    // Each: +1 at 1ms (init's after), then +100 per 10ms tick until stopped.
    CHECK(s.model().right.n == 201);         // ticks at 10, 20
    CHECK(s.model().left.n == 501);          // ticks at 10..50
    return 0;
}

int streams_are_separate() {
    // Stop early: both children subscribed, streams registered, no ticks.
    jaal::sim_options lim;
    lim.max_time = 2ms;
    jaal::sim<Pair> s(4, lim);
    s.run();
    auto left  = s.stream("left/feed");
    auto right = s.stream("right/feed");
    CHECK(left.open() && right.open());
    CHECK(!s.stream("feed").open());          // keys were prefixed
    // The sink takes the PARENT's Msg: in production the mapped body wraps
    // for you; here the test plays the body, so it sends what the body's
    // sink would deliver. Feed the left only.
    CHECK(left.send(Pair::Msg{Pair::Left{Counter::Fed{}}}));
    s.run();
    CHECK(s.model().left.n == 1001);
    CHECK(s.model().right.n == 1);
    return 0;
}

}  // namespace

int main() {
    if (int r = routing_with_given())            return r;
    if (int r = child_api_direct())              return r;
    if (int r = subs_are_mapped_and_prefixed())  return r;
    if (int r = runs_in_the_sim())               return r;
    if (int r = streams_are_separate())          return r;
    std::puts("child: ok");
    return 0;
}
