// tests/kernel/signal_shutdown_test.cpp — ^C still works while a program
// is shutting down.
//
// The bug: run() installed signal handlers for the whole of its lifetime,
// INCLUDING shutdown. Shutdown is not instant — a task that ignores its
// stop token holds it for the full options::shutdown_grace (2 s by
// default) — and during that window every SIGINT was caught into a pipe
// that nothing was draining any more. So a program that was slow to exit
// could not be interrupted: measured at 20 SIGINTs over a 10 s shutdown,
// all swallowed, with the process unkillable by ^C the whole time.
//
// The fix: drop the signal source before shutdown, which restores each
// signal's previous disposition. From then on a second ^C kills the process
// the ordinary way, which is what a user pressing it twice means.
//
// This is a fork test, not a unit test: the thing under test is what the
// PROCESS does with a signal, so it needs a real process and a real signal.

#include <jaal/jaal.hpp>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <variant>

#include <sys/wait.h>
#include <unistd.h>

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

// A program whose shutdown takes the whole grace: its task never notices
// the stop token, so finish() has to wait it out.
struct Stubborn {
    struct Model { bool quitting = false; };
    struct Bye {};
    using Msg = std::variant<Bye>;
    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg, jaal::fx::on_signal>;

    static Cmd init(Model&) {
        return Cmd::task([](jaal::Sink<Msg>, std::stop_token) {
            std::this_thread::sleep_for(30s);          // ignores its stop token
        });
    }
    static Cmd update(Model& m, Bye) {
        m.quitting = true;
        return Cmd::quit(0);
    }
    static Sub subscribe(const Model&) {
        return Sub::on_signal({jaal::sig::interrupt}, [](jaal::sig) { return Msg{Bye{}}; });
    }
};

// release() is called after the loop has ended and before finish(), which
// is exactly the window under test. Saying so here makes the test
// deterministic: the parent doesn't have to guess when shutdown began, and
// doesn't race the much smaller window between update() and the handlers
// coming off.
struct announce {
    using event_type = jaal::kernel::no_events;
    void release() {
        std::fputs("shutting-down\n", stdout);
        std::fflush(stdout);
    }
};

/// The child: a program with a long shutdown. Never returns normally in
/// this test (the parent kills it or it exits on its own).
[[noreturn]] void be_the_program(int fd) {
    ::dup2(fd, STDOUT_FILENO);
    jaal::run_options opt;
    opt.kernel.shutdown_grace = 10s;    // a wide window to fire signals into
    announce host;
    std::_Exit(jaal::run<Stubborn>(host, opt));
}

int a_second_interrupt_kills_a_program_that_is_shutting_down() {
    int pipefd[2];
    CHECK(::pipe(pipefd) == 0);

    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        ::close(pipefd[0]);
        be_the_program(pipefd[1]);
    }
    ::close(pipefd[1]);

    // 1. first SIGINT: the program handles it, ends its loop, and enters
    //    shutdown. Wait until it says it's there, so this is not a race.
    std::this_thread::sleep_for(150ms);
    CHECK(::kill(pid, SIGINT) == 0);

    std::string said;
    char buf[64];
    while (said.find("shutting-down") == std::string::npos) {
        const auto n = ::read(pipefd[0], buf, sizeof buf);
        if (n > 0) said.append(buf, static_cast<std::size_t>(n));
        else if (n == 0 || errno != EINTR) break;
    }
    ::close(pipefd[0]);
    CHECK(said.find("shutting-down") != std::string::npos);

    // It's inside a 10 s shutdown now (a task that won't stop), so it can't
    // have exited on its own.
    int status = 0;
    CHECK(::waitpid(pid, &status, WNOHANG) == 0);

    // 2. second SIGINT: must kill it. Before the fix the handler was still
    //    installed here, so this went into a pipe nobody drained and the
    //    process sat out the whole grace, unkillable by ^C.
    CHECK(::kill(pid, SIGINT) == 0);

    const auto deadline = std::chrono::steady_clock::now() + 4s;
    pid_t done = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        done = ::waitpid(pid, &status, WNOHANG);
        if (done == pid) break;
        std::this_thread::sleep_for(10ms);
    }
    if (done != pid) {                          // still running: the bug
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
        std::fputs("signal_shutdown_test: the second SIGINT was ignored\n", stderr);
        return 1;
    }

    // Killed by the signal, not a tidy exit: the handler is gone, so SIGINT
    // has its default action again.
    CHECK(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGINT);
    return 0;
}

}  // namespace

int main() {
    if (int r = a_second_interrupt_kills_a_program_that_is_shutting_down()) return r;
    std::puts("signal_shutdown_test: ok");
    return 0;
}
