// tests/kernel/mailbox_bound_test.cpp — a mailbox with a capacity.

#include <jaal/kernel/mailbox.hpp>
#include <jaal/kernel/scope.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
namespace k = jaal::kernel;

struct M { int v; };

namespace {

k::mailbox_options opts(std::size_t cap, k::overflow o) { return {cap, o}; }

int drop_newest() {
    k::inbox<M> in({}, opts(4, k::overflow::drop_newest));
    auto s = in.sink();
    // posted from another thread, so it's not the loop-thread rule at play
    std::vector<bool> ok;
    std::jthread t([&] { for (int i = 0; i < 10; ++i) ok.push_back(s.send(M{i})); });
    t.join();
    int accepted = 0;
    for (bool b : ok) accepted += b;
    if (accepted != 4) return 101;
    std::vector<M> out;
    in.drain(out);
    if (out.size() != 4 || out[0].v != 0 || out[3].v != 3) return 102;   // the FIRST four
    if (in.stats().dropped != 6) return 103;
    return 0;
}

int drop_oldest() {
    k::inbox<M> in({}, opts(4, k::overflow::drop_oldest));
    auto s = in.sink();
    std::jthread t([&] { for (int i = 0; i < 10; ++i) if (!s.send(M{i})) std::abort(); });
    t.join();
    std::vector<M> out;
    in.drain(out);
    if (out.size() != 4 || out[0].v != 6 || out[3].v != 9) return 201;   // the LAST four
    if (in.stats().dropped != 6) return 202;
    if (in.stats().high_water != 4) return 203;
    return 0;
}

int block_backpressure() {
    // A fast producer, a slow consumer. Every message arrives, in order,
    // and the queue never holds more than the capacity.
    k::inbox<M> in({}, opts(4, k::overflow::block));
    auto s = in.sink();
    constexpr int N = 200;
    std::jthread producer([&] { for (int i = 0; i < N; ++i) if (!s.send(M{i})) std::abort(); });
    std::vector<M> all, batch;
    while (static_cast<int>(all.size()) < N) {
        std::this_thread::sleep_for(1ms);          // slow loop
        in.drain(batch);
        for (auto& m : batch) all.push_back(m);
    }
    producer.join();
    for (std::size_t i = 0; i < static_cast<std::size_t>(N); ++i)
        if (all[i].v != static_cast<int>(i)) return 301;               // in order, none lost
    if (in.stats().high_water > 4) return 302;                         // bounded
    if (in.stats().blocked == 0) return 303;                           // it really did push back
    return 0;
}

int close_releases_blocked_sender() {
    // A sender blocked on a full mailbox must not hang shutdown: close()
    // releases it with false.
    k::inbox<M> in({}, opts(1, k::overflow::block));
    auto s = in.sink();
    std::atomic<int> result{-1};
    std::jthread t([&] {
        s.send(M{0});                               // fills it
        result = s.send(M{1}) ? 1 : 0;              // blocks
    });
    std::this_thread::sleep_for(50ms);
    if (result != -1) return 401;                   // still blocked
    in.close();
    t.join();
    if (result != 0) return 402;                    // released with false
    return 0;
}

int loop_thread_never_blocks() {
    // Posting to your own full mailbox from the loop thread would wait for
    // a drain only the loop can do. It must refuse instead of hanging.
    std::jthread watchdog([](std::stop_token st) {
        for (int i = 0; i < 50 && !st.stop_requested(); ++i) std::this_thread::sleep_for(100ms);
        if (!st.stop_requested()) { std::fprintf(stderr, "mailbox_bound_test: deadlocked\n"); std::_Exit(2); }
    });
    k::inbox<M> in({}, opts(2, k::overflow::block));   // created on this thread = loop
    if (!in.post(M{0}) || !in.post(M{1})) return 501;
    if (in.post(M{2})) return 502;                  // full: refused, not blocked
    if (in.stats().loop_full != 1) return 503;
    return 0;
}

int unbounded_is_default() {
    k::inbox<M> in;
    auto s = in.sink();
    for (int i = 0; i < 10000; ++i) if (!s.send(M{i})) return 601;
    if (in.size() != 10000 || in.stats().dropped != 0) return 602;
    return 0;
}

// The cross-thread version of loop_thread_never_blocks. The loop opens a
// scope and joins a helper; the helper sends to the loop's FULL blocking
// mailbox. Only the loop can drain it, and the loop is waiting on the
// helper: a deadlock that used to hang forever. Now the helper's post is
// refused (counted in loop_full) and both finish.
int scope_helper_of_the_loop_never_blocks() {
    k::inbox<M> in({}, opts(1, k::overflow::block));   // made here: this thread is the loop
    auto s = in.sink();
    if (!s.send(M{0})) return 701;                     // now full
    bool sent = true;
    jaal::scope([&](jaal::nursery& n) {
        auto h = n.spawn([&s] { return s.send(M{1}); });   // would wait for a drain
        sent = h.join();                                     // the loop waits on it
    });
    if (sent) return 702;                              // refused, not queued
    if (in.stats().loop_full != 1) return 703;
    // A thread the loop is NOT waiting on still gets real backpressure.
    std::jthread other([&s] { (void)s.send(M{2}); });  // blocks until the drain
    std::this_thread::sleep_for(20ms);
    std::vector<M> out;
    in.drain(out);
    other.join();
    if (in.stats().blocked != 1) return 704;
    return 0;
}

}  // namespace

int main() {
    // Several bugs here show up as a HANG (a sender never released). A hang
    // must fail fast with a message, not wait for the harness timeout.
    std::jthread watchdog([](std::stop_token st) {
        for (int i = 0; i < 100 && !st.stop_requested(); ++i)
            std::this_thread::sleep_for(100ms);
        if (!st.stop_requested()) {
            std::fprintf(stderr, "mailbox_bound_test: hung for 10s (a blocked sender was never released)\n");
            std::_Exit(3);
        }
    });
    int (*const checks[])() = {drop_newest, drop_oldest, block_backpressure,
                               close_releases_blocked_sender, loop_thread_never_blocks,
                               unbounded_is_default, scope_helper_of_the_loop_never_blocks};
    for (auto f : checks)
        if (int r = f()) {
            std::fprintf(stderr, "mailbox_bound_test: check %d failed\n", r);
            return 1;
        }
    return 0;
}
