// tests/kernel/given_test.cpp — given/when/expect: update tested as data.

#include <jaal/jaal.hpp>
#include <jaal/host/given.hpp>

#include <chrono>
#include <cstdio>
#include <stdexcept>
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

struct Search {
    struct Model {
        int query = 0, shown = 0, fetches = 0;
        long stamp = 0;
        bool closing = false;
    };
    struct Type { int q; };
    struct Results { int q; };
    struct Stamp { long t; };
    struct Close {};
    struct Boom {};
    using Msg = std::variant<Type, Results, Stamp, Close, Boom>;
    using Cmd = jaal::CoreCmd<Msg>;

    static std::pair<Model, Cmd> init() {
        return {Model{}, Cmd::now([](jaal::fx::now::time_point t) -> Msg {
                    return Stamp{static_cast<long>(t.time_since_epoch().count())};
                })};
    }
    static std::pair<Model, Cmd> update(Model m, Msg msg) {
        return std::visit(jaal::overload{
            [&](Type t) -> std::pair<Model, Cmd> {
                m.query = t.q;
                ++m.fetches;
                return {m, Cmd::task([](Sink<Msg> out, std::stop_token, int q) {
                                         out.send(Results{q * 10});
                                     }, t.q)};
            },
            [&](Results r) -> std::pair<Model, Cmd> {
                if (r.q == m.query * 10) m.shown = r.q;
                return {m, Cmd::none()};
            },
            [&](Stamp s) -> std::pair<Model, Cmd> { m.stamp = s.t; return {m, Cmd::none()}; },
            [&](Close) -> std::pair<Model, Cmd> {
                m.closing = true;
                return {m, Cmd::batch(Cmd::after(500ms, Msg{Close{}}), Cmd::quit(4))};
            },
            [&](Boom) -> std::pair<Model, Cmd> { throw std::runtime_error("kaboom"); },
        }, std::move(msg));
    }
};

int basics() {
    auto t = jaal::given<Search>();
    t.expect_effect<jaal::fx::now>(1);                        // init asked for the time
    t.at_time(jaal::fx::now::time_point{} + 42ns).settle();
    t.expect("stamped", [](auto& m) { return m.stamp == 42; });

    t.when(Search::Type{1}).when(Search::Type{2});
    t.expect("latest query", [](auto& m) { return m.query == 2 && m.fetches == 2; })
     .expect_effect<jaal::fx::task>(1);                       // the last update's task only
    t.settle();
    t.expect("results for the latest query", [](auto& m) { return m.shown == 20; })
     .expect_no_effects();
    CHECK(t.ok());
    return 0;
}

int timers_and_quit() {
    auto t = jaal::given<Search>(Search::Model{});
    t.when(Search::Close{});
    CHECK(t.after().size() == 1);
    CHECK(t.after()[0].delay == 500ms);
    CHECK(std::holds_alternative<Search::Close>(t.after()[0].msg));
    CHECK(t.quit() == 4);
    t.settle();                                               // after/quit are never run
    CHECK(t.folds() == 1);
    CHECK(t.ok());
    return 0;
}

int failures_are_collected() {
    auto t = jaal::given<Search>(Search::Model{});
    t.when(Search::Type{3});
    t.expect("wrong on purpose", [](auto& m) { return m.query == 99; })
     .expect_effect<jaal::fx::task>(2);
    CHECK(!t.ok());
    CHECK(t.failures().size() == 2);
    const auto r = t.report();
    // The model failure shows what the last message changed.
    CHECK(r.find("wrong on purpose") != std::string::npos);
    CHECK(r.find(".0: 0 -> 3") != std::string::npos);
    CHECK(r.find("expected 2 'task' effect(s), got 1") != std::string::npos);
    return 0;
}

int update_throws() {
    auto t = jaal::given<Search>(Search::Model{});
    t.when(Search::Boom{}).when(Search::Type{1});             // second when is a no-op
    CHECK(t.threw());
    CHECK(!t.ok());
    CHECK(t.report().find("update threw: kaboom") != std::string::npos);
    return 0;
}

// A task that always starts another is caught by the round limit.
struct Forever {
    struct Model { int n = 0; };
    struct Go {};
    using Msg = std::variant<Go>;
    using Cmd = jaal::CoreCmd<Msg>;
    static Model init() { return {}; }
    static std::pair<Model, Cmd> update(Model m, Msg) {
        ++m.n;
        return {m, Cmd::task([](Sink<Msg> out, std::stop_token) { out.send(Go{}); })};
    }
};

int settle_is_bounded() {
    auto t = jaal::given<Forever>();
    t.when(Forever::Go{}).settle(50);
    CHECK(!t.ok());
    CHECK(t.model().n == 51);
    CHECK(t.report().find("max_rounds") != std::string::npos);
    return 0;
}

}  // namespace

int main() {
    if (int r = basics())                 return r;
    if (int r = timers_and_quit())        return r;
    if (int r = failures_are_collected()) return r;
    if (int r = update_throws())          return r;
    if (int r = settle_is_bounded())      return r;
    std::puts("given: ok");
    return 0;
}
