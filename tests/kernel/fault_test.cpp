// tests/kernel/fault_test.cpp — what happens when program code throws.
//
// Each check is a bug the kernel used to have (or would have without the
// fault layer), and each was checked against the bug put back.

#include <jaal/core/core_fx.hpp>
#include <jaal/host/headless.hpp>
#include <jaal/kernel/fault.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

using namespace std::chrono_literals;
using jaal::Sink;
using jaal::fault;
using jaal::fault_policy;
using jaal::fault_site;

namespace {

// A program whose update throws on one message.
struct Fragile {
    struct Model { std::vector<std::string> lines{"a", "b", "c"}; };
    struct Add { std::string s; }; struct Boom {};
    using Msg = std::variant<Add, Boom>;
    using Cmd = jaal::CoreCmd<Msg>;
    static Model init() { return {}; }
    static std::pair<Model, Cmd> update(Model m, Msg msg) {
        if (std::holds_alternative<Boom>(msg)) {
            m.lines.push_back("half-done");        // partially mutated...
            throw std::runtime_error("update failed");
        }
        m.lines.push_back(std::get<Add>(msg).s);
        return {std::move(m), Cmd::none()};
    }
};

struct recorded {
    std::vector<fault> faults;
    jaal::fault_handler handler() {
        return [this](const fault& f) { faults.push_back(f); };
    }
};

// ── 1. skip: the model is EXACTLY as before the throwing message ─────────
// update() takes the model by value and moves it in. Without a copy taken
// first, the throw leaves the kernel's model moved-from (measured: a
// 3-line model came back with 0 lines).
int skip_keeps_model() {
    recorded rec;
    jaal::kernel::options opt;
    opt.on_fault = fault_policy::skip;
    opt.faults   = rec.handler();
    jaal::headless<Fragile> h(opt);
    h.send(Fragile::Add{"d"});
    h.send(Fragile::Boom{});
    h.send(Fragile::Add{"e"});                      // keeps going after
    const auto& l = h.model().lines;
    if (l != std::vector<std::string>{"a", "b", "c", "d", "e"}) return 101;
    if (h.quit()) return 102;
    if (rec.faults.size() != 1) return 103;
    if (rec.faults[0].site != fault_site::update) return 104;
    if (!rec.faults[0].model_kept || rec.faults[0].stopping) return 105;
    if (rec.faults[0].what != "update failed") return 106;
    return 0;
}

// ── 2. stop (the default): quits with 70, reported, no crash ────────────
int stop_quits() {
    recorded rec;
    jaal::kernel::options opt;
    opt.faults = rec.handler();                     // on_fault defaults to stop
    jaal::headless<Fragile> h(opt);
    h.send(Fragile::Boom{});
    if (!h.quit()) return 201;
    if (std::move(h).finish() != jaal::fault_exit_code) return 202;
    if (rec.faults.size() != 1 || !rec.faults[0].stopping) return 203;
    return 0;
}

// ── 3. a throwing TASK is reported, not swallowed ───────────────────────
// Before: the kernel built its pool with no error callback, so a task
// that threw vanished and the program waited forever for its result.
struct TaskThrows {
    struct Model { int done = 0; };
    struct Go {}; struct Done {};
    using Msg = std::variant<Go, Done>;
    using Cmd = jaal::CoreCmd<Msg>;
    static Model init() { return {}; }
    static std::pair<Model, Cmd> update(Model m, Msg msg) {
        if (std::holds_alternative<Go>(msg))
            return {m, Cmd::batch(
                Cmd::task([](Sink<Msg>, std::stop_token) { throw std::logic_error("task failed"); }),
                Cmd::task([](Sink<Msg> out, std::stop_token) { out.send(Done{}); }))};
        ++m.done;
        return {m, Cmd::none()};
    }
};

int task_fault_reported() {
    recorded rec;
    jaal::kernel::options opt;
    opt.on_fault = fault_policy::skip;
    opt.faults   = rec.handler();
    jaal::headless<TaskThrows> h(opt);
    h.send(TaskThrows::Go{});
    if (!h.run_until_idle(2s)) return 301;
    if (h.model().done != 1) return 302;            // the good task still landed
    if (rec.faults.size() != 1) return 303;
    if (rec.faults[0].site != fault_site::task) return 304;
    if (rec.faults[0].what != "task failed") return 305;
    return 0;
}

int task_fault_stops_by_default() {
    recorded rec;
    jaal::kernel::options opt;
    opt.faults = rec.handler();
    jaal::headless<TaskThrows> h(opt);
    h.send(TaskThrows::Go{});
    for (int i = 0; i < 200 && !h.quit(); ++i) {
        std::this_thread::sleep_for(5ms);
        h.kernel().step(h.record());
    }
    if (!h.quit()) return 311;
    return 0;
}

// ── 4. a throwing subscribe() keeps the running subscriptions ───────────
struct SubThrows {
    struct Model { int ticks = 0; bool bad = false; };
    struct Tick {}; struct Break {};
    using Msg = std::variant<Tick, Break>;
    using Cmd = jaal::CoreCmd<Msg>;
    using Sub = jaal::Sub<Msg, jaal::core_src>;
    static Model init() { return {}; }
    static std::pair<Model, Cmd> update(Model m, Msg msg) {
        if (std::holds_alternative<Break>(msg)) m.bad = true;
        else ++m.ticks;
        return {m, Cmd::none()};
    }
    static Sub subscribe(const Model& m) {
        if (m.bad) throw std::runtime_error("subscribe failed");
        return Sub::every(10ms, Tick{});
    }
};

int subscribe_fault_keeps_subs() {
    recorded rec;
    jaal::kernel::options opt;
    opt.on_fault = fault_policy::skip;
    opt.faults   = rec.handler();
    jaal::headless<SubThrows> h(opt);
    h.advance(25ms);
    if (h.model().ticks != 2) return 401;
    h.send(SubThrows::Break{});                     // next subscribe() throws
    if (rec.faults.size() != 1 || rec.faults[0].site != fault_site::subscribe) return 402;
    h.advance(30ms);
    // the timer kept running: the last good subscription set survived
    if (h.model().ticks != 5) return 403;
    return 0;
}

// ── 5. shutdown can't hang on a task that ignores its stop token ────────
struct Stubborn {
    struct Model {};
    struct Go {};
    using Msg = std::variant<Go>;
    using Cmd = jaal::CoreCmd<Msg>;
    static Model init() { return {}; }
    static std::pair<Model, Cmd> update(Model m, Msg) {
        return {m, Cmd::task([](Sink<Msg>, std::stop_token) {
            // ignores the stop token: a stuck syscall, a bad loop
            std::this_thread::sleep_for(1500ms);
        })};
    }
};

int shutdown_bounded() {
    recorded rec;
    jaal::kernel::options opt;
    opt.shutdown_grace = 100ms;
    opt.faults         = rec.handler();
    const auto t0 = std::chrono::steady_clock::now();
    {
        jaal::headless<Stubborn> h(opt);
        h.send(Stubborn::Go{});
        std::this_thread::sleep_for(20ms);          // the task is running
        std::move(h).finish();
    }
    const auto took = std::chrono::steady_clock::now() - t0;
    if (took > 800ms) return 501;                   // didn't wait out the 1.5 s task
    if (rec.faults.empty()) return 502;             // the abandonment was reported
    // The abandoned worker still wakes later and returns into memory it
    // co-owns (pool::core). Let it: ASan/TSan watch this.
    std::this_thread::sleep_for(1600ms);
    return 0;
}

// ── 6. a throwing fault handler doesn't take the loop down ──────────────
int throwing_handler() {
    jaal::kernel::options opt;
    opt.on_fault = fault_policy::skip;
    opt.faults   = [](const fault&) { throw std::runtime_error("handler failed"); };
    jaal::headless<Fragile> h(opt);
    h.send(Fragile::Boom{});
    h.send(Fragile::Add{"x"});
    if (h.model().lines.back() != "x") return 601;
    return 0;
}

}  // namespace

int main() {
    int (*const checks[])() = {skip_keeps_model, stop_quits, task_fault_reported,
                               task_fault_stops_by_default, subscribe_fault_keeps_subs,
                               shutdown_bounded, throwing_handler};
    for (auto f : checks)
        if (int r = f()) {
            std::fprintf(stderr, "fault_test: check %d failed\n", r);
            return 1;
        }
    return 0;
}
