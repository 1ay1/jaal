// tests/kernel/kernel_parts_test.cpp — mailbox, timers, pool, loop_bound.
// Runtime behaviour; the tricky bits get their own check.

#include <jaal/core/program.hpp>
#include <jaal/kernel/loop.hpp>
#include <jaal/kernel/mailbox.hpp>
#include <jaal/kernel/pool.hpp>
#include <jaal/kernel/timer_heap.hpp>
#include <jaal/platform/clock.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
namespace k = jaal::kernel;
using jaal::platform::sim_clock;

struct Msg { int v; };

// ── loop_bound: a token can't be copied, moved, or made without the key ──
static_assert(!std::is_copy_constructible_v<k::loop_token>);
static_assert(!std::is_move_constructible_v<k::loop_token>);
static_assert(!std::is_default_constructible_v<k::loop_token>);
static_assert(std::is_constructible_v<k::loop_token, k::loop_key>);
// loop_key's ctor is explicit, so a token can't appear from {} at a call
// site: you have to name loop_key, which is grep-able.
static_assert(!std::is_convertible_v<decltype(""), k::loop_key>);
static_assert(!std::is_constructible_v<k::loop_token>);
// There is no static accessor: a captureless task body can call any
// function, so a loop_token::current() would hand a worker a token.
template <class T> concept has_current = requires { T::current(); };
static_assert(!has_current<k::loop_token>);

static int mailbox_tests() {
    // wake fires only on empty → non-empty
    std::atomic<int> wakes{0};
    k::inbox<Msg> in([&] { ++wakes; });
    auto sink = in.sink();
    if (!sink.send(Msg{1})) return 1;
    if (!sink.send(Msg{2})) return 2;
    if (!sink.send(Msg{3})) return 3;
    if (wakes != 1) return 4;                       // coalesced

    std::vector<Msg> out;
    in.drain(out);
    if (out.size() != 3 || out[0].v != 1 || out[2].v != 3) return 5;
    if (!in.empty()) return 6;

    sink.send(Msg{4});
    if (wakes != 2) return 7;                       // empty again → wakes again
    in.drain(out);

    // many producers, no lost messages
    {
        k::inbox<Msg> multi;
        auto s = multi.sink();
        std::vector<std::jthread> ts;
        for (int t = 0; t < 8; ++t)
            ts.emplace_back([s, t] { for (int i = 0; i < 100; ++i) s.send(Msg{t * 100 + i}); });
        ts.clear();
        std::vector<Msg> all;
        multi.drain(all);
        if (all.size() != 800) return 8;
    }

    // close: later sends fail, nothing crashes
    in.close();
    if (sink.send(Msg{5})) return 9;

    // a sink outliving its inbox: send returns false
    jaal::Sink<Msg> orphan;
    {
        k::inbox<Msg> tmp;
        orphan = tmp.sink();
        if (!orphan.open()) return 10;
    }
    if (orphan.open() || orphan.send(Msg{1})) return 11;
    return 0;
}

static int timer_tests() {
    sim_clock c;
    k::timer_heap<sim_clock, Msg> h;
    std::vector<Msg> fired;

    // one-shot: not before its deadline
    h.after(c.now(), 100ms, Msg{1});
    c.advance(99ms);
    h.collect_due(c.now(), fired);
    if (!fired.empty()) return 20;
    c.advance(2ms);
    h.collect_due(c.now(), fired);
    if (fired.size() != 1 || fired[0].v != 1 || !h.empty()) return 21;

    // repeating: fires once per period, and a long stall does NOT produce
    // a catch-up storm
    fired.clear();
    auto id = h.every(c.now(), 10ms, Msg{2});
    c.advance(10ms);
    h.collect_due(c.now(), fired);
    if (fired.size() != 1) return 22;
    fired.clear();
    c.advance(1s);                                  // stalled for 100 periods
    h.collect_due(c.now(), fired);
    if (fired.size() != 1) return 23;               // once, not 100 times

    // replace_payload keeps the phase: the timer does NOT restart
    fired.clear();
    c.advance(5ms);
    if (!h.replace_payload(id, Msg{99})) return 24;
    c.advance(5ms);
    h.collect_due(c.now(), fired);
    if (fired.size() != 1 || fired[0].v != 99) return 25;

    if (!h.cancel(id) || !h.empty()) return 26;
    if (h.cancel(id)) return 27;                    // already gone

    // ordering: earliest first, whatever order they were armed in
    fired.clear();
    h.after(c.now(), 30ms, Msg{3});
    h.after(c.now(), 10ms, Msg{1});
    h.after(c.now(), 20ms, Msg{2});
    if (h.next_deadline() != c.now() + 10ms) return 28;
    c.advance(30ms);
    h.collect_due(c.now(), fired);
    if (fired.size() != 3 || fired[0].v != 1 || fired[1].v != 2 || fired[2].v != 3) return 29;

    // "fire never" must not overflow into the past
    h.after(c.now(), sim_clock::duration::max(), Msg{0});
    if (h.next_deadline() != sim_clock::time_point::max()) return 30;
    return 0;
}

static int pool_tests() {
    // jobs run, and the pool reuses workers rather than one thread per job
    {
        k::pool p(4);
        std::atomic<int> ran{0};
        for (int i = 0; i < 64; ++i) p.post([&](std::stop_token) { ++ran; });
        while (ran < 64) std::this_thread::sleep_for(1ms);
        if (p.worker_count() > 4) return 40;
    }

    // a throwing task is reported, not fatal, and the pool keeps working
    {
        std::atomic<int> errors{0};
        k::pool p(2, [&](std::exception_ptr) { ++errors; });
        p.post([](std::stop_token) { throw std::runtime_error("boom"); });
        std::atomic<bool> after{false};
        p.post([&](std::stop_token) { after = true; });
        while (!after || errors == 0) std::this_thread::sleep_for(1ms);
        if (errors != 1) return 41;
    }

    // shutdown asks running work to stop and joins: no hang, no leak
    {
        k::pool p(2);
        std::atomic<bool> started{false}, saw_stop{false};
        p.post([&](std::stop_token st) {
            started = true;
            while (!st.stop_requested()) std::this_thread::sleep_for(1ms);
            saw_stop = true;
        });
        while (!started) std::this_thread::sleep_for(1ms);
        p.shutdown();                                // must return
        if (!saw_stop) return 42;
        p.shutdown();                                // idempotent
        p.post([](std::stop_token) {});              // ignored after shutdown
        if (p.queued() != 0) return 43;
    }

    // an isolated task runs on its own thread and is asked to stop at
    // shutdown, but is never joined (so a wedged one can't hang us)
    {
        std::atomic<bool> running{false}, released{false};
        {
            k::pool p(2);
            p.post_isolated([&](std::stop_token st) {
                running = true;
                while (!st.stop_requested()) std::this_thread::sleep_for(1ms);
                released = true;
            });
            while (!running) std::this_thread::sleep_for(1ms);
        }                                            // ~pool: request stop, don't join
        for (int i = 0; i < 2000 && !released; ++i) std::this_thread::sleep_for(1ms);
        if (!released) return 44;
    }
    return 0;
}

int main() {
    k::loop_bound<std::string> state("hi");
    k::loop_token tok{k::loop_key{}};
    if (state.get(tok) != "hi") return 50;
    state.with(tok, [](std::string& s) { s += "!"; });
    if (state.get(tok) != "hi!") return 51;

    if (int r = mailbox_tests()) return r;
    if (int r = timer_tests())   return r;
    if (int r = pool_tests())    return r;
    return 0;
}
