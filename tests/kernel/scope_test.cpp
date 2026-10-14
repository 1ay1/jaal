// tests/kernel/scope_test.cpp — structured concurrency and guarded<T>.

#include <jaal/core/co_owned.hpp>
#include <jaal/kernel/guarded.hpp>
#include <jaal/kernel/scope.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

// ── static guarantees ───────────────────────────────────────────────────
static_assert(!std::is_copy_constructible_v<jaal::nursery>);
static_assert(!std::is_move_constructible_v<jaal::nursery>);
static_assert(!std::is_copy_constructible_v<jaal::handle<int>>);
static_assert(!std::is_move_constructible_v<jaal::handle<int>>);
// no public way to make a nursery outside scope()
static_assert(!std::is_constructible_v<jaal::nursery, std::stop_token>);
// none of these can be handed to a helper, so a helper can't spawn or join
static_assert(!jaal::Sendable<jaal::nursery>);
static_assert(!jaal::Sendable<jaal::nursery*>);
static_assert(!jaal::Sendable<jaal::handle<int>>);
static_assert(!jaal::Sendable<jaal::helper_group>);
static_assert(!jaal::Sendable<const jaal::unlocked*>);

// What helpers share with the owner, and the only way they can: Sync state
// behind a co_owned (reference counted, so nobody frees it under a helper).
struct Flags {
    std::atomic<bool> finished{false};
    std::atomic<bool> saw_stop{false};
    std::atomic<int>  sum{0};
    const std::stop_source done;
};
using Shared = jaal::co_owned<Flags>;

static int scope_tests(const jaal::unlocked& cap) {
    // 1. values go in by value and come back through join()
    {
        const std::string text = "hello world";
        auto [a, b] = jaal::scope(cap, [&](jaal::nursery& n) {
            auto x = n.spawn([](std::string t) { return t.size(); }, text);
            auto y = n.spawn([](std::string t) { return t.find('w'); }, text);
            return std::pair{x.join(), y.join()};
        });
        if (a != 11 || b != 6) return 101;
    }

    // 2. a helper the block never joins is STILL joined before scope()
    //    returns (its handle's destructor joins it)
    {
        auto f = Shared::make();
        jaal::scope(cap, [&](jaal::nursery& n) {
            auto h = n.spawn([](Shared s) {
                std::this_thread::sleep_for(20ms);
                s->finished = true;
            }, f);
            // no join here
        });
        if (!f->finished) return 102;
    }

    // 3. a throwing helper: siblings are asked to stop, everything is
    //    joined, and the error surfaces from scope()
    {
        auto f = Shared::make();
        bool caught = false;
        try {
            jaal::scope(cap, [&](jaal::nursery& n) {
                auto sib = n.spawn([](std::stop_token st, Shared s) {
                    while (!st.stop_requested()) std::this_thread::sleep_for(1ms);
                    s->saw_stop = true;
                }, f);
                auto bad = n.spawn([] { throw std::runtime_error("helper failed"); });
                (void)bad.join();
            });
        } catch (const std::runtime_error& e) {
            caught = std::string(e.what()) == "helper failed";
        }
        if (!caught) return 103;
        if (!f->saw_stop) return 104;               // it was stopped AND joined
    }

    // 4. the BLOCK throws while a helper still runs: the helper is stopped
    //    and joined before scope() rethrows. It owns what it touches (its
    //    co_owned keeps `done` alive), so nothing it uses can die under it.
    {
        auto f = Shared::make();
        bool caught = false;
        try {
            jaal::scope(cap, [&](jaal::nursery& n) {
                auto h = n.spawn([](std::stop_token st, Shared s) {
                    while (!st.stop_requested() && !s->done.stop_requested())
                        std::this_thread::sleep_for(1ms);
                    s->saw_stop = true;
                }, f);
                throw std::logic_error("block failed");
            });
        } catch (const std::logic_error&) {
            caught = true;
        }
        if (!caught || !f->saw_stop) return 105;
    }

    // 5. join() rethrows the helper's own error, and scope() doesn't throw
    //    it a second time
    {
        bool joined_err = false;
        jaal::scope(cap, [&](jaal::nursery& n) {
            auto h = n.spawn([]() -> int { throw std::runtime_error("x"); });
            try { (void)h.join(); } catch (const std::runtime_error&) { joined_err = true; }
        });
        if (!joined_err) return 106;
    }

    // 6. parent cancellation flows down
    {
        std::stop_source parent;
        auto f = Shared::make();
        std::jthread canceller([&] {
            std::this_thread::sleep_for(20ms);
            parent.request_stop();
        });
        jaal::scope(cap, parent.get_token(), [&](jaal::nursery& n) {
            auto h = n.spawn([](std::stop_token st, Shared s) {
                while (!st.stop_requested()) std::this_thread::sleep_for(1ms);
                s->saw_stop = true;
            }, f);
        });
        if (!f->saw_stop) return 107;
    }

    // 7. many helpers side by side, each with its index and its own args
    {
        auto f = Shared::make();
        jaal::scope(cap, [&](jaal::nursery& n) {
            n.spawn_n(16, [](std::size_t i, Shared s) {
                s->sum += static_cast<int>(i) + 1;
            }, f).join();
        });
        if (f->sum != 136) return 108;
    }

    // 8. a helper may take its own `unlocked` and a stop token
    {
        auto r = jaal::scope(cap, [](jaal::nursery& n) {
            return n.spawn([](const jaal::unlocked&, std::stop_token, int v) { return v * 2; },
                           21).join();
        });
        if (r != 42) return 109;
    }

    // 9. a function pointer works as a body
    {
        auto r = jaal::scope(cap, [](jaal::nursery& n) {
            int (*twice)(int) = [](int v) { return v * 2; };
            return n.spawn(twice, 5).join();
        });
        if (r != 10) return 110;
    }

    // 10. join() twice is refused (the result was moved out the first time)
    {
        bool refused = false;
        jaal::scope(cap, [&](jaal::nursery& n) {
            auto h = n.spawn([] { return 1; });
            (void)h.join();
            try { (void)h.join(); } catch (const std::logic_error&) { refused = true; }
        });
        if (!refused) return 111;
    }

    // 11. a helper that throws in spawn_n stops its siblings, and the group
    //     rethrows it
    {
        auto f = Shared::make();
        bool caught = false;
        try {
            jaal::scope(cap, [&](jaal::nursery& n) {
                n.spawn_n(4, [](std::stop_token st, std::size_t i, Shared s) {
                    if (i == 0) throw std::runtime_error("share 0 failed");
                    while (!st.stop_requested()) std::this_thread::sleep_for(1ms);
                    s->sum += 1;
                }, f).join();
            });
        } catch (const std::runtime_error& e) {
            caught = std::string(e.what()) == "share 0 failed";
        }
        if (!caught || f->sum != 3) return 112;
    }

    // 12. parallel_for: every index exactly once, the body borrowed from
    //     the caller (the one borrow scope allows, joined inside the call)
    {
        std::vector<std::atomic<int>> hits(1000);
        const auto bump = [&hits](std::size_t i) { hits[i].fetch_add(1); };
        jaal::parallel_for(cap, hits.size(), 8, bump);
        for (auto& h : hits)
            if (h.load() != 1) return 113;
    }

    // 13. parallel_for rethrows a share's error after every share stopped
    {
        std::atomic<int> after{0};
        bool caught = false;
        try {
            jaal::parallel_for(cap, 10'000, 4, [&after](std::size_t i) {
                if (i == 7) throw std::runtime_error("index 7");
                after.fetch_add(1);
            });
        } catch (const std::runtime_error& e) {
            caught = std::string(e.what()) == "index 7";
        }
        if (!caught || after.load() >= 10'000) return 114;
    }

    // 14. width 1 and n 0 run inline / not at all
    {
        int sum = 0;
        jaal::parallel_for(cap, 4, 1, [&sum](std::size_t i) { sum += static_cast<int>(i); });
        jaal::parallel_for(cap, 0, 8, [](std::size_t) { std::abort(); });
        if (sum != 6) return 115;
    }
    return 0;
}

// ── guarded<T> ────────────────────────────────────────────────────────────────────────
// The rules are compile-time, so they're checked on the building blocks;
// tests/compile_fail/scope.cpp checks that real calls fail with the
// messages.
using Map  = std::map<std::string, int>;
using GMap = jaal::guarded<Map>;

// Nothing points into the data from outside the lock.
using ret_ref  = decltype([](Map& m) -> int& { return m["a"]; });
using ret_ptr  = decltype([](Map& m) { return &m; });
using ret_view = decltype([](Map&) { return std::string_view("x"); });
using ret_val  = decltype([](Map& m) { return m.size(); });
static_assert(!jaal::detail::guard::escapable<std::invoke_result_t<ret_ref, Map&>>);
static_assert(!jaal::detail::guard::escapable<std::invoke_result_t<ret_ptr, Map&>>);
static_assert(!jaal::detail::guard::escapable<std::invoke_result_t<ret_view, Map&>>);
static_assert(jaal::detail::guard::escapable<std::invoke_result_t<ret_val, Map&>>);

// A second lock can't be reached from inside the first: the function can't
// capture one, and no argument can carry one.
using by_val_capture = decltype([x = 1](Map&) { (void)x; });
using no_capture     = decltype([](Map&, int) {});
static_assert(!jaal::detail::guard::captureless<by_val_capture>);
static_assert(jaal::detail::guard::captureless<no_capture>);
static_assert(!jaal::Sendable<GMap>);                        // can't be an argument
static_assert(!jaal::Sendable<GMap*>);                       // nor a pointer to one
static_assert(!jaal::Sendable<std::reference_wrapper<GMap>>); // nor a reference
static_assert(!std::is_copy_constructible_v<GMap>);

static int guarded_tests(const jaal::unlocked& cap) {
    // many writers, one guarded map, no lost updates
    GMap g;
    {
        std::vector<std::jthread> ts;
        for (int t = 0; t < 8; ++t)
            ts.emplace_back([&g, t] {
                jaal::thread_root([&](const jaal::unlocked& tc) {
                    for (int i = 0; i < 1000; ++i)
                        g.with(tc, [](Map& m, std::string k) { ++m[k]; }, "k" + std::to_string(t % 3));
                });
            });
    }
    const int total = g.read(cap, [](const Map& m) {
        int s = 0;
        for (auto& [_, v] : m) s += v;
        return s;
    });
    if (total != 8000) return 201;

    // readers run alongside each other (shared lock), writers exclude them
    jaal::guarded<int> counter(0);
    {
        std::vector<std::jthread> ts;
        for (int t = 0; t < 4; ++t)
            ts.emplace_back([&] {
                jaal::thread_root([&](const jaal::unlocked& tc) {
                    for (int i = 0; i < 500; ++i) counter.with(tc, [](int& v) { ++v; });
                });
            });
        for (int t = 0; t < 4; ++t)
            ts.emplace_back([&] {
                jaal::thread_root([&](const jaal::unlocked& tc) {
                    for (int i = 0; i < 500; ++i) (void)counter.read(tc, [](const int& v) { return v; });
                });
            });
    }
    if (counter.read(cap, [](const int& v) { return v; }) != 2000) return 202;

    // arguments are moved in, results copied out
    jaal::guarded<std::string> s;
    s.with(cap, [](std::string& v, std::string a, int n) { v = a + std::to_string(n); }, std::string("x"), 7);
    if (s.read(cap, [](const std::string& v) { return v; }) != "x7") return 203;

    // try_*: run when the lock is free, nullopt (no wait) while it is held
    jaal::guarded<int> busy(5);
    if (busy.try_read(cap, [](const int& v) { return v; }) != std::optional<int>{5}) return 204;
    if (busy.try_with(cap, [](int& v) { return ++v; }) != std::optional<int>{6}) return 205;
    {
        static std::atomic<bool> holding{false}, release{false};
        std::jthread holder([&] {
            jaal::thread_root([&](const jaal::unlocked& tc) {
                busy.with(tc, [](int&) {
                    holding.store(true);
                    while (!release.load()) std::this_thread::yield();
                });
            });
        });
        while (!holding.load()) std::this_thread::yield();
        const bool r_none = !busy.try_read(cap, [](const int& v) { return v; });
        const bool w_none = !busy.try_with(cap, [](int& v) { return v; });
        release.store(true);
        if (!r_none || !w_none) return 206;
    }

    // wait_with: a worker sleeps until there is work, drains it, stops on a flag
    {
        struct Q { std::vector<int> items; bool stop = false; };
        static jaal::guarded<Q> q;
        static std::atomic<int> sum{0};
        std::jthread worker([] {
            jaal::thread_root([](const jaal::unlocked& tc) {
                for (;;) {
                    auto batch = q.wait_with(tc,
                        [](const Q& w) { return !w.items.empty() || w.stop; },
                        [](Q& w) { return std::exchange(w.items, {}); });
                    if (batch.empty()) return;   // stop with nothing left
                    for (int v : batch) sum += v;
                }
            });
        });
        for (int i = 1; i <= 100; ++i)
            q.with(cap, [](Q& w, int v) { w.items.push_back(v); }, i);
        q.with(cap, [](Q& w) { w.stop = true; });
        worker.join();
        if (sum.load() != 5050) return 207;
    }

    // wait_with_for: gives up after the budget with pred false, and wakes
    // early (pred true) the moment a writer makes it hold.
    {
        using namespace std::chrono_literals;
        static jaal::guarded<int> v(0);
        const auto t0 = std::chrono::steady_clock::now();
        auto [got, held] = v.wait_with_for(cap, 50ms,
            [](const int& x) { return x > 0; }, [](int& x) { return x; });
        const auto waited = std::chrono::steady_clock::now() - t0;
        if (held || got != 0 || waited < 40ms) return 208;

        std::jthread writer([] {
            std::this_thread::sleep_for(20ms);
            jaal::thread_root([](const jaal::unlocked& tc) {
                v.with(tc, [](int& x) { x = 7; });
            });
        });
        const auto t1 = std::chrono::steady_clock::now();
        auto [got2, held2] = v.wait_with_for(cap, 5s,
            [](const int& x) { return x > 0; }, [](int& x) { return x; });
        if (!held2 || got2 != 7 || std::chrono::steady_clock::now() - t1 > 2s) return 209;
    }
    return 0;
}

int main() {
    return jaal::thread_root([&](const jaal::unlocked& cap) -> int {
        int (*const checks[])(const jaal::unlocked&) = {scope_tests, guarded_tests};
        for (auto f : checks)
            if (int r = f(cap)) {
                std::fprintf(stderr, "scope_test: check %d failed\n", r);
                return 1;
            }
        return 0;
    });
}
