// tests/kernel/mailbox_race_test.cpp — the mailbox wakes UNDER its lock.
//
// Why it matters: the kernel's mailbox wakes a real reactor fd. If post()
// woke after unlocking, a poster preempted between unlock and wake could
// run its wake after the kernel had closed the mailbox and the reactor had
// closed that fd, and the kernel can hand the same fd NUMBER to something
// new. The wake would then write into the wrong file.
//
// This test holds a poster inside that window (the wake callback sleeps),
// closes the mailbox and the fd meanwhile, opens a pipe that reuses the fd
// number, and checks no byte landed in it.
//
// Measured: with wake-after-unlock, 20/20 rounds wrote into the reused fd.
// With wake-under-lock, 0/20. Deterministic, not probabilistic: the window
// is held open on purpose.

#include <jaal/kernel/mailbox.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

#if !defined(_WIN32)
#  include <fcntl.h>
#  include <unistd.h>
#endif

using namespace std::chrono_literals;

struct M { int v; };

int main() {
#if !defined(_WIN32)
    int stray = 0;
    for (int round = 0; round < 20; ++round) {
        int p[2];
        if (::pipe(p) != 0) return 2;
        std::atomic<int>  wfd{p[1]};
        std::atomic<bool> in_window{false};
        auto box = std::make_shared<jaal::kernel::mailbox<M>>([&] {
            in_window = true;
            std::this_thread::sleep_for(30ms);             // preempted here
            [[maybe_unused]] auto n = ::write(wfd.load(), "w", 1);
        });
        std::jthread poster([box] { box->post(M{1}); });
        while (!in_window) std::this_thread::yield();      // poster is mid-wake
        box->close();                                       // the kernel shuts down...
        ::close(p[0]);                                      // ...and the reactor's fd goes
        ::close(p[1]);
        int q[2];
        if (::pipe(q) != 0) return 3;                      // same fd numbers, reused
        ::fcntl(q[0], F_SETFL, O_NONBLOCK);
        poster.join();
        char c;
        if (::read(q[0], &c, 1) == 1) ++stray;
        ::close(q[0]);
        ::close(q[1]);
    }
    if (stray != 0) {
        std::fprintf(stderr, "mailbox_race_test: %d/20 wakes landed in a reused fd\n", stray);
        return 1;
    }
#endif
    return 0;
}
