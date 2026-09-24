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
    return 0;
}

// ── guarded<T> ──────────────────────────────────────────────────────────
// with(bad) is DECLARED so it can explain itself, which means a
// requires-expression would see it as callable. So test the rule itself;
// tests/compile_fail/scope.cpp checks a real call fails with the message.
template <class G, class F>
concept can_with = jaal::detail::guard::escapable<
    std::invoke_result_t<F, std::map<std::string, int>&>>;
using GMap = jaal::guarded<std::map<std::string, int>>;
using ret_ref  = decltype([](std::map<std::string, int>& m) -> int& { return m["a"]; });
using ret_ptr  = decltype([](std::map<std::string, int>& m) { return &m; });
using ret_view = decltype([](std::map<std::string, int>&) { return std::string_view("x"); });
using ret_val  = decltype([](std::map<std::string, int>& m) { return m.size(); });
static_assert(!can_with<GMap, ret_ref>);    // a reference into the data can't escape
static_assert(!can_with<GMap, ret_ptr>);    // nor a pointer
static_assert(!can_with<GMap, ret_view>);   // nor a view
static_assert(can_with<GMap, ret_val>);     // a copy can
static_assert(!std::is_copy_constructible_v<GMap>);

static int guarded_tests() {
    // many writers, one guarded map, no lost updates
    GMap g;
    {
        std::vector<std::jthread> ts;
        for (int t = 0; t < 8; ++t)
            ts.emplace_back([&g, t] {
                for (int i = 0; i < 1000; ++i)
                    g.with([&](auto& m) { ++m["k" + std::to_string(t % 3)]; });
            });
    }
    const int total = g.read([](const auto& m) {
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

#ifndef NDEBUG
    // nesting two guarded locks on one thread is rejected in debug builds
    // (lock-order deadlocks start with nested locks)
    jaal::guarded<int> a(1), b(2);
    bool rejected = false;
    try {
        a.with([&](int&) { b.with([](int&) {}); });
    } catch (const jaal::nested_guard_error&) {
        rejected = true;
    }
    if (!rejected) return 203;
    // and the outer lock was released on the way out: it's usable again
    if (a.read([](const int& v) { return v; }) != 1) return 204;
#endif
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
