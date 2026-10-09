// tests/kernel/worker_group_test.cpp — jaal::kernel::worker_group.
//
//   1. stop() is a barrier: it returns only after a running job has finished,
//      however long that takes (no grace, no abandon)
//   2. stop() requests stop on the job's token, so a cooperative job ends
//      promptly
//   3. post() after stop() is dropped, not run
//   4. the destructor stops too

#include <jaal/kernel/delay.hpp>
#include <jaal/kernel/worker_group.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <stop_token>
#include <thread>

using namespace std::chrono_literals;

namespace {
int failures = 0;
void ok(bool c, const char* w) {
    if (!c) { std::fprintf(stderr, "FAIL %s\n", w); ++failures; }
}
}  // namespace

int main() {
    // 1. barrier: a job that ignores its token still finishes before stop()
    //    returns.
    {
        std::atomic<bool> started{false}, finished{false};
        jaal::kernel::worker_group g;
        g.post([&](std::stop_token) {
            started = true;
            std::this_thread::sleep_for(300ms);   // deliberately not cooperative
            finished = true;
        });
        while (!started) std::this_thread::yield();
        g.stop();
        ok(finished.load(), "1: stop() waited for the running job");
    }

    // 2. stop reaches the job's token.
    {
        std::atomic<bool> saw_stop{false};
        jaal::kernel::worker_group g;
        g.post([&](std::stop_token st) {
            if (jaal::kernel::delay_for(st, 30s)) saw_stop = true;
        });
        const auto t0 = std::chrono::steady_clock::now();
        g.stop();
        ok(saw_stop.load(), "2: the job's token was stopped");
        ok(std::chrono::steady_clock::now() - t0 < 5s, "2: and it ended promptly");
    }

    // 3. admission is closed after stop.
    {
        std::atomic<int> ran{0};
        jaal::kernel::worker_group g;
        g.stop();
        g.post([&](std::stop_token) { ++ran; });
        g.stop();
        ok(ran.load() == 0, "3: a post after stop() is dropped");
    }

    // 4. destructor is a barrier too.
    {
        std::atomic<bool> finished{false};
        {
            jaal::kernel::worker_group g;
            g.post([&](std::stop_token) {
                std::this_thread::sleep_for(100ms);
                finished = true;
            });
        }
        ok(finished.load(), "4: destruction waited for the job");
    }

    if (failures) return 1;
    std::printf("worker_group: ok\n");
    return 0;
}
