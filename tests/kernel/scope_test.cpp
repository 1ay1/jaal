// tests/kernel/scope_test.cpp — structured concurrency and guarded<T>.

#include <jaal/kernel/guarded.hpp>
#include <jaal/kernel/scope.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
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

static int scope_tests() {
    // 1. values come back through join(); [&] borrowing of locals is fine
    {
        const std::string text = "hello world";
        auto [a, b] = jaal::scope([&](jaal::nursery& n) {
            auto x = n.spawn([&] { return text.size(); });
            auto y = n.spawn([&] { return text.find('w'); });
            return std::pair{x.join(), y.join()};
        });
        if (a != 11 || b != 6) return 101;
    }

    // 2. a helper the block never joins is STILL joined before scope()
    //    returns: the block can't leak it by forgetting
    {
        std::atomic<bool> finished{false};
        jaal::scope([&](jaal::nursery& n) {
            (void)n.spawn([&] {
                std::this_thread::sleep_for(20ms);
                finished = true;
            });
            // no join here
        });
        if (!finished) return 102;
    }

    // 3. a throwing helper: siblings are asked to stop, everything is
    //    joined, and the error surfaces from scope()
    {
        std::atomic<bool> sibling_saw_stop{false};
        bool caught = false;
        try {
            jaal::scope([&](jaal::nursery& n) {
                (void)n.spawn([&](std::stop_token st) {
                    while (!st.stop_requested()) std::this_thread::sleep_for(1ms);
                    sibling_saw_stop = true;
                });
                (void)n.spawn([] { throw std::runtime_error("helper failed"); });
            });
        } catch (const std::runtime_error& e) {
            caught = std::string(e.what()) == "helper failed";
        }
        if (!caught) return 103;
        if (!sibling_saw_stop) return 104;          // it was stopped AND joined
    }

    // 4. the BLOCK throws while a helper is still running: the helper is
    //    stopped and joined before the exception leaves scope(). This is
    //    the case where a raw std::thread + [&] would touch a dead frame.
    {
        std::atomic<int> after_exit_writes{0};
        std::atomic<bool> scope_left{false};
        try {
            jaal::scope([&](jaal::nursery& n) {
                (void)n.spawn([&](std::stop_token st) {
                    while (!st.stop_requested()) std::this_thread::sleep_for(1ms);
                    if (scope_left) ++after_exit_writes;   // would be use-after-scope
                });
                throw std::logic_error("block failed");
            });
        } catch (const std::logic_error&) {
            scope_left = true;
        }
        std::this_thread::sleep_for(20ms);
        if (after_exit_writes != 0) return 105;
    }

    // 5. join() rethrows the helper's own error, and scope() doesn't throw
    //    it a second time
    {
        bool joined_err = false;
        jaal::scope([&](jaal::nursery& n) {
            auto h = n.spawn([]() -> int { throw std::runtime_error("x"); });
            try { (void)h.join(); } catch (const std::runtime_error&) { joined_err = true; }
        });
        if (!joined_err) return 106;
    }

    // 6. parent cancellation flows down
    {
        std::stop_source parent;
        std::atomic<bool> stopped{false};
        std::jthread canceller([&] {
            std::this_thread::sleep_for(20ms);
            parent.request_stop();
        });
        jaal::scope(parent.get_token(), [&](jaal::nursery& n) {
            (void)n.spawn([&](std::stop_token st) {
                while (!st.stop_requested()) std::this_thread::sleep_for(1ms);
                stopped = true;
            });
        });
        if (!stopped) return 107;
    }

    // 7. void helpers and many of them
    {
        std::atomic<int> sum{0};
        jaal::scope([&](jaal::nursery& n) {
            for (int i = 1; i <= 16; ++i) (void)n.spawn([&sum, i] { sum += i; });
        });
        if (sum != 136) return 108;
    }

    // 8. the mutual-join deadlock is REFUSED, not hung. Helper A tries to
    //    join helper B (which could be joining A): only the scope's thread
    //    may join, so A gets scope_misuse, which surfaces from scope().
    {
        bool refused = false;
        try {
            jaal::scope([&](jaal::nursery& n) {
                auto b = n.spawn([](std::stop_token st) {
                    while (!st.stop_requested()) std::this_thread::sleep_for(1ms);
                    return 2;
                });
                (void)n.spawn([&b] { return b.join(); });   // would wait on a sibling
            });
        } catch (const jaal::scope_misuse&) {
            refused = true;
        }
        if (!refused) return 109;
    }

    // 9. a helper can't spawn into the nursery (one writer for its list)
    {
        bool refused = false;
        try {
            jaal::scope([&](jaal::nursery& n) {
                (void)n.spawn([&n] { (void)n.spawn([] {}); });
            });
        } catch (const jaal::scope_misuse&) {
            refused = true;
        }
        if (!refused) return 110;
    }

    // 10. join() twice is refused (the result was moved out the first time)
    {
        bool refused = false;
        jaal::scope([&](jaal::nursery& n) {
            auto h = n.spawn([] { return 1; });
            (void)h.join();
            try { (void)h.join(); } catch (const std::logic_error&) { refused = true; }
        });
        if (!refused) return 111;
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

static int guarded_tests() {
    // many writers, one guarded map, no lost updates
    GMap g;
    {
        std::vector<std::jthread> ts;
        for (int t = 0; t < 8; ++t)
            ts.emplace_back([&g, t] {
                for (int i = 0; i < 1000; ++i)
                    g.with([](Map& m, std::string k) { ++m[k]; }, "k" + std::to_string(t % 3));
            });
    }
    const int total = g.read([](const Map& m) {
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
            ts.emplace_back([&] { for (int i = 0; i < 500; ++i) counter.with([](int& v) { ++v; }); });
        for (int t = 0; t < 4; ++t)
            ts.emplace_back([&] { for (int i = 0; i < 500; ++i) (void)counter.read([](const int& v) { return v; }); });
    }
    if (counter.read([](const int& v) { return v; }) != 2000) return 202;

    // arguments are moved in, results copied out
    jaal::guarded<std::string> s;
    s.with([](std::string& v, std::string a, int n) { v = a + std::to_string(n); }, std::string("x"), 7);
    if (s.read([](const std::string& v) { return v; }) != "x7") return 203;
    return 0;
}

int main() {
    int (*const checks[])() = {scope_tests, guarded_tests};
    for (auto f : checks)
        if (int r = f()) {
            std::fprintf(stderr, "scope_test: check %d failed\n", r);
            return 1;
        }
    return 0;
}
