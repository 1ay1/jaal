// tests/platform/signals_test.cpp — POSIX signals as events.

#include <jaal/platform/signal.hpp>

#if defined(__unix__) || defined(__APPLE__)
#  include <jaal/platform/posix/poll_reactor.hpp>
#  include <jaal/platform/posix/signals.hpp>
#  include <csignal>
#  include <fcntl.h>
#  include <pthread.h>
#  include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
namespace pf = jaal::platform;
using pf::signal;
using pf::signal_set;

// ── signal_set is a plain value ─────────────────────────────────────────
static_assert(signal_set{}.empty());
static_assert(signal_set{signal::resize}.contains(signal::resize));
static_assert(!signal_set{signal::resize}.contains(signal::interrupt));
static_assert((signal_set{signal::resize} | signal_set{signal::hangup})
              == signal_set{signal::hangup, signal::resize});
static_assert(signal_set::from_bits(0xFF).bits() == 0x1F);    // unknown bits dropped
constexpr int count(signal_set s) { int n = 0; for (auto x : s) { (void)x; ++n; } return n; }
static_assert(count(signal_set{signal::interrupt, signal::child}) == 2);
static_assert(count(signal_set{}) == 0);

#if defined(__unix__) || defined(__APPLE__)

namespace {

bool wait_readable(int fd, std::chrono::milliseconds t) {
    auto r = pf::poll_reactor::create().value();
    auto reg = r.watch(fd, pf::interest::read, 1).value();
    auto res = r.wait(t).value();
    return res.count == 1;
}

int basic() {
    auto s = pf::posix_signals::install({signal::resize, signal::child}).value();
    if (s.watching() != signal_set{signal::resize, signal::child}) return 1;
    if (!s.take().empty()) return 2;

    ::raise(SIGWINCH);
    if (!wait_readable(s.handle(), 1s)) return 3;       // wakes the reactor
    auto got = s.take();
    if (got != signal_set{signal::resize}) return 4;
    if (!s.take().empty()) return 5;                    // taken once
    return 0;
}

int coalesces() {
    auto s = pf::posix_signals::install({signal::resize}).value();
    for (int i = 0; i < 50; ++i) ::raise(SIGWINCH);
    if (s.take() != signal_set{signal::resize}) return 11;    // 50 raises, one event
    if (wait_readable(s.handle(), 0ms)) return 12;            // and the pipe is drained
    return 0;
}

int two_sources_both_see_it() {
    // Two independent installers (say, two kernels) each get every signal
    // they asked for. One doesn't consume the other's.
    auto a = pf::posix_signals::install({signal::resize}).value();
    auto b = pf::posix_signals::install({signal::resize, signal::child}).value();
    ::raise(SIGWINCH);
    if (a.take() != signal_set{signal::resize}) return 21;
    if (b.take() != signal_set{signal::resize}) return 22;
    // a didn't ask for child: it doesn't get it
    ::raise(SIGCHLD);
    if (!a.take().empty()) return 23;
    if (b.take() != signal_set{signal::child}) return 24;
    return 0;
}

std::atomic<int> app_handler_calls{0};
extern "C" void app_handler(int) { ++app_handler_calls; }

int restores_previous_handler() {
    // The app had its own SIGWINCH handler before jaal. When the last jaal
    // source goes away, the app's handler must be back.
    struct sigaction sa {}, old {};
    sa.sa_handler = app_handler;
    sigemptyset(&sa.sa_mask);
    ::sigaction(SIGWINCH, &sa, &old);
    {
        auto s1 = pf::posix_signals::install({signal::resize}).value();
        {
            auto s2 = pf::posix_signals::install({signal::resize}).value();
        }                                                // s1 still installed
        ::raise(SIGWINCH);
        if (app_handler_calls != 0) return 31;           // jaal's handler is in place
        if (s1.take() != signal_set{signal::resize}) return 32;
    }                                                    // last one gone
    ::raise(SIGWINCH);
    if (app_handler_calls != 1) return 33;               // the app's handler is back
    ::sigaction(SIGWINCH, &old, nullptr);
    return 0;
}

int respects_inherited_ignore() {
    // nohup sets SIGHUP to SIG_IGN. Installing a handler would undo nohup.
    struct sigaction ign {}, old {};
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    ::sigaction(SIGHUP, &ign, &old);
    {
        auto s = pf::posix_signals::install({signal::hangup, signal::resize}).value();
        if (s.watching().contains(signal::hangup)) return 41;   // left ignored
        if (!s.watching().contains(signal::resize)) return 42;
        struct sigaction cur {};
        ::sigaction(SIGHUP, nullptr, &cur);
        if (cur.sa_handler != SIG_IGN) return 43;
        ::raise(SIGHUP);                                 // ignored: process lives on
    }
    ::sigaction(SIGHUP, &old, nullptr);
    return 0;
}

int teardown_under_fire() {
    // Tear sources down while another thread floods the process with
    // signals. The handler must never write into a closed fd, because the
    // kernel hands that fd NUMBER to the next pipe(). The window (load fd,
    // then write) is widened with a test hook so the race actually happens.
    //
    // Measured: with teardown's in-flight wait removed, this caught the bug
    // in 19 of 20 runs (and 8 of 8 in a separate batch); with the wait in
    // place, 0 of 20 failed. It's a probabilistic detector, not a proof. A
    // regression shows up within a run or two, which is what a CI test needs.
    pf::test::signals_window = 200000;
    std::atomic<bool> stop{false};
    std::jthread storm([&] {
        while (!stop) ::kill(::getpid(), SIGWINCH);
    });
    long stray = 0;
    for (int i = 0; i < 400; ++i) {
        {
            auto s = pf::posix_signals::install({signal::resize}).value();
            std::this_thread::yield();
        }                                                // teardown under fire
        int q[2];
        if (::pipe(q) != 0) return 51;                   // likely reuses the fd numbers
        ::fcntl(q[0], F_SETFL, O_NONBLOCK);
        std::this_thread::yield();
        char c;
        if (::read(q[0], &c, 1) == 1) ++stray;
        ::close(q[0]);
        ::close(q[1]);
    }
    stop = true;
    storm.join();
    pf::test::signals_window = 0;
    if (stray != 0) {
        std::fprintf(stderr, "signals_test: %ld stray writes into recycled fds\n", stray);
        return 52;
    }
    return 0;
}

}  // namespace

int main() {
    // The storm test sends SIGWINCH to the whole process; make sure the
    // default (ignore) is what's there when no source is installed.
    ::signal(SIGWINCH, SIG_DFL);
    int (*const checks[])() = {basic, coalesces, two_sources_both_see_it,
                               restores_previous_handler, respects_inherited_ignore,
                               teardown_under_fire};
    for (auto f : checks)
        if (int r = f()) {
            std::fprintf(stderr, "signals_test: check %d failed\n", r);
            return 1;
        }
    return 0;
}

#else

#include <jaal/platform/windows/console_signals.hpp>
#include <jaal/platform/windows/wait_reactor.hpp>
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

namespace {

bool wait_signalled(void* h, std::chrono::milliseconds t) {
    auto r = pf::wait_reactor::create().value();
    auto reg = r.watch(h, pf::interest::read, 1).value();
    return r.wait(t).value().count == 1;
}

int win_basic() {
    auto s = pf::console_signals::install(
        {signal::interrupt, signal::hangup, signal::resize}).value();
    // resize isn't a console control event: honestly left out
    if (s.watching() != signal_set{signal::interrupt, signal::hangup}) return 201;
    if (!s.take().empty()) return 202;

    // delivered on another thread, as Windows does
    std::jthread t([] { pf::test::console_ctrl(CTRL_C_EVENT); });
    t.join();
    if (!wait_signalled(s.handle(), 1s)) return 203;
    if (s.take() != signal_set{signal::interrupt}) return 204;
    if (wait_signalled(s.handle(), 0ms)) return 205;     // event reset by take()
    return 0;
}

int win_mapping_and_coalesce() {
    auto s = pf::console_signals::install(
        {signal::interrupt, signal::terminate, signal::hangup}).value();
    for (int i = 0; i < 20; ++i) pf::test::console_ctrl(CTRL_BREAK_EVENT);
    pf::test::console_ctrl(CTRL_SHUTDOWN_EVENT);
    pf::test::console_ctrl(CTRL_CLOSE_EVENT);
    if (s.take() != signal_set{signal::interrupt, signal::terminate, signal::hangup}) return 211;
    return 0;
}

int win_two_sources() {
    auto a = pf::console_signals::install({signal::interrupt}).value();
    auto b = pf::console_signals::install({signal::interrupt, signal::hangup}).value();
    pf::test::console_ctrl(CTRL_C_EVENT);
    if (a.take() != signal_set{signal::interrupt}) return 221;
    if (b.take() != signal_set{signal::interrupt}) return 222;
    pf::test::console_ctrl(CTRL_CLOSE_EVENT);
    if (!a.take().empty()) return 223;
    if (b.take() != signal_set{signal::hangup}) return 224;
    return 0;
}

}  // namespace

int main() {
    int (*const checks[])() = {win_basic, win_mapping_and_coalesce, win_two_sources};
    for (auto f : checks)
        if (int r = f()) {
            std::fprintf(stderr, "signals_test: check %d failed\n", r);
            return 1;
        }
    std::printf("signals_test[windows]: %d checks passed\n", 3);
    return 0;
}
#endif
