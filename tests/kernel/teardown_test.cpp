// tests/kernel/teardown_test.cpp — shutdown happens in order on every path
// out of run(), including the ones that skip the end of the function.
//
// D34 put the three teardown steps (signals off, host.release(),
// kernel.finish()) in the right order at the END of run(). That fixed the
// bug it was written for and left the class of bug open: a host callback
// that throws unwinds straight past those statements. Measured on that
// code — release() never ran, and the kernel then shut down for the full
// grace with the signal handlers still installed, which is exactly the
// unkillable-during-exit bug D34 was about, reached another way.
//
// So the order is a destructor now (kernel/teardown.hpp), and this test
// pins the behaviour that made it necessary:
//
//   * a throwing host callback still gets release(), and still shuts the
//     kernel down
//   * release() runs AFTER the signal handlers are restored, so a ^C during
//     a slow shutdown kills the process
//   * a release() that throws doesn't stop the kernel from finishing (a
//     wedged worker must not outlive the process's last chance to stop it)
//
// The compile-time half — that finish() can't be called without a
// teardown_key — is in tests/compile_fail/kernel.cpp cases 6 and 7.

#include <jaal/jaal.hpp>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

using namespace std::chrono_literals;

#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,   \
                         __LINE__, #c);                                   \
            return 1;                                                     \
        }                                                                 \
    } while (0)

namespace {

std::vector<std::string> log;

bool sigint_is_default() {
    struct sigaction sa {};
    ::sigaction(SIGINT, nullptr, &sa);
    return sa.sa_handler == SIG_DFL || sa.sa_handler == SIG_IGN;
}

struct Quick {
    struct Model { int ticks = 0; };
    struct Tick {};
    using Msg = std::variant<Tick>;
    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg>;

    static Cmd update(Model& m, Tick) { return ++m.ticks >= 3 ? Cmd::quit(7) : Cmd{}; }
    static Sub subscribe(const Model&) { return Sub::every(1ms, Tick{}); }
};

// ── a host whose draw throws ──────────────────────────────────────────────
struct throwing_present {
    using event_type = jaal::kernel::no_events;
    int frames = 0;

    template <class K>
    void present(K&) {
        if (++frames == 2) throw std::runtime_error("present blew up");
    }
    void release() {
        // The signals must already be back to their old disposition here:
        // that's the ordering D34 is about, and it has to hold even on this
        // path (which never reaches the end of run()).
        log.push_back(sigint_is_default() ? "release(signals-restored)"
                                          : "release(SIGNALS-STILL-OURS)");
    }
};

int a_throwing_host_still_tears_down_in_order() {
    log.clear();
    throwing_present host;
    bool threw = false;
    try {
        (void)jaal::run<Quick>(host);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);                                     // the throw still escapes
    CHECK(log.size() == 1);
    CHECK(log[0] == "release(signals-restored)");     // ran, and in order
    return 0;
}

// ── a host whose release() throws ─────────────────────────────────────────
struct throwing_release {
    using event_type = jaal::kernel::no_events;
    void release() {
        log.push_back("release(threw)");
        throw std::runtime_error("release blew up");
    }
};

int a_throwing_release_does_not_stop_the_kernel() {
    log.clear();
    throwing_release host;
    // finish() still runs, so run() returns the program's own exit code
    // rather than dying of release()'s exception.
    const int code = jaal::run<Quick>(host);
    CHECK(log.size() == 1 && log[0] == "release(threw)");
    CHECK(code == 7);                                 // Cmd::quit(7) from the program
    return 0;
}

// ── the ordinary path, for contrast ───────────────────────────────────────
struct plain {
    using event_type = jaal::kernel::no_events;
    void release() {
        log.push_back(sigint_is_default() ? "release(signals-restored)"
                                          : "release(SIGNALS-STILL-OURS)");
    }
};

int the_normal_path_is_the_same_order() {
    log.clear();
    plain host;
    const int code = jaal::run<Quick>(host);
    CHECK(code == 7);
    CHECK(log.size() == 1);
    CHECK(log[0] == "release(signals-restored)");
    return 0;
}

// ── a driver that never asks for the exit code ────────────────────────────
int the_destructor_shuts_down_even_if_nobody_asks() {
    // The guard used directly, the way a host that owns its own loop does.
    // Nothing calls exit_code(): the destructor must still do it all.
    log.clear();
    jaal::recorder rec;
    plain host;
    {
        auto k = jaal::kernel::kernel<Quick>::start(rec);
        jaal::kernel::teardown guard{k, host, jaal::kernel::no_signals{}};
        k.dispatch(Quick::Tick{});
        (void)k.step(rec);
    }                                                  // guard dies here
    CHECK(log.size() == 1);                            // release() ran anyway
    return 0;
}

}  // namespace

int main() {
    if (int r = a_throwing_host_still_tears_down_in_order()) return r;
    if (int r = a_throwing_release_does_not_stop_the_kernel()) return r;
    if (int r = the_normal_path_is_the_same_order()) return r;
    if (int r = the_destructor_shuts_down_even_if_nobody_asks()) return r;
    std::puts("teardown_test: ok");
    return 0;
}
