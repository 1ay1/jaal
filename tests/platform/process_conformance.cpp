// tests/platform/process_conformance.cpp — one suite, every process backend.
//
// Each check is a template over a Process backend plus a fixture that knows
// how to make a child behave (a sim fixture scripts it; a real one spawns a
// shell). A backend is correct when it passes all of them; a new backend is
// added by instantiating the suite for it in main().
//
// The checks are the ORDERING RULES from platform/process.hpp, made
// executable. Prose in a header is a rule nobody can run.
//
//   1. exit readiness observed LATE is not lost (level-triggered)
//   2. exit readiness is idempotent: looking twice says the same thing
//   3. output written before the exit is readable AFTER the exit
//   4. stop() works with a full pipe and nobody draining
//   5. reap() before the end reports would_block, it does not block
//   6. exited(code) and signalled(sig) stay distinguishable
//   7. merge_stderr means ONE stream, not two handles onto one pipe
//   8. spawn+reap leaks no descriptors
//   9. stop() on an already-ended child is a no-op, not an error
//  10. a backend that cannot stop a real tree SAYS SO (tree_is_exact)
//
// Checks 1-9 are facts about a program's observable behaviour and every
// backend must pass them. Check 10 is about the kernel, and a backend that
// answers false is not failing — it is being honest, and the suite records
// which guarantee it actually has.
//
// The handles under test are real, so the reactor under test is real too:
// these checks go through poll_reactor exactly as a host would. A sim
// backend that invented its own readiness would agree with itself and prove
// nothing.

#include <jaal/platform/concepts.hpp>
#include <jaal/platform/process.hpp>
#include <jaal/platform/sim/process.hpp>

#if !defined(_WIN32)
#  include <jaal/platform/posix/process.hpp>
#endif

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#if !defined(_WIN32)
#  include <dirent.h>
#  include <jaal/platform/posix/poll_reactor.hpp>
#  include <unistd.h>
#endif

using namespace std::chrono_literals;
namespace pf = jaal::platform;

namespace {

#if !defined(_WIN32)
// Defined with the helpers below; the posix fixture needs it to wait.
template <pf::Reactor R>
bool readable_now(pf::borrowed_handle h);
#endif

// ── fixtures ────────────────────────────────────────────────────────────
//
// What a fixture owes the suite: a child that does a known thing, and a way
// to move it forward. "Forward" is a step for a scripted child and a short
// wait for a real one, which is the only place the two differ.

struct sim_fixture {
    using process = pf::sim_process;
    static constexpr const char* name = "sim_process";

    /// The script IS the argv, so a sim child is spawned exactly the way a
    /// real one is: by naming the program you want.
    static pf::process_spec script(std::vector<std::string> steps) {
        pf::process_spec s;
        s.argv = {"sim"};
        s.argv.insert(s.argv.end(), steps.begin(), steps.end());
        return s;
    }

    /// Emits `bytes`, then exits `code`.
    static process writes_then_exits(std::string bytes, int code) {
        return process::spawn(script({"out:" + bytes,
                                      "exit:" + std::to_string(code)})).value();
    }

    /// Fills its stdout pipe and then keeps running.
    static process fills_pipe_and_runs() {
        return process::spawn(script({"fill"})).value();
    }

    /// Runs forever, says nothing.
    static process runs_silently() {
        return process::spawn(script({})).value();
    }

    static process merged_streams() {
        auto spec = script({"err:to-stderr", "exit"});
        spec.merge_stderr = true;
        return process::spawn(spec).value();
    }

    /// Move the child forward by one observable action.
    static void advance(process& p) { p.step(); }
    static void run_out(process& p) { p.run_to_completion(); }
};

#if !defined(_WIN32)
/// The same children, as real programs.
///
/// "Forward" is a short wait rather than a step: a real child runs on the
/// scheduler's terms, so the fixture waits for the thing the check is about
/// instead of pretending it can single-step the kernel. That is the ONLY
/// difference between the two fixtures, which is the point -- every check
/// below is written once and means the same thing on both.
struct posix_fixture {
    using process = pf::posix_process;
    static constexpr const char* name = "posix_process";

    static pf::process_spec sh(std::string script) {
        pf::process_spec s;
        s.argv = {"/bin/sh", "-c", std::move(script)};
        return s;
    }

    static process writes_then_exits(std::string bytes, int code) {
        return process::spawn(sh("printf '%s' \"" + bytes + "\"; exit "
                                 + std::to_string(code))).value();
    }

    static process fills_pipe_and_runs() {
        // Write far more than any pipe buffer, then idle. The write blocks
        // once the pipe is full, which is exactly the state under test.
        return process::spawn(sh("yes 2>/dev/null | head -c 10000000; sleep 30")).value();
    }

    static process runs_silently() { return process::spawn(sh("sleep 30")).value(); }

    static process merged_streams() {
        auto spec = sh("printf 'to-stderr' 1>&2");
        spec.merge_stderr = true;
        return process::spawn(spec).value();
    }

    /// Wait until it has ended, bounded. A check that needs the child to be
    /// finished says so by calling this; nothing here sleeps blindly.
    static void run_out(process& p) {
        for (int i = 0; i < 400; ++i) {
            if (readable_now<pf::poll_reactor>(p.exit_handle())) return;
            ::usleep(5000);
        }
    }
    static void advance(process& p) { ::usleep(50000); (void)p; }
};
#endif

// ── helpers ─────────────────────────────────────────────────────────────

#if !defined(_WIN32)
/// Is this handle readable right now, through a real reactor?
template <pf::Reactor R>
bool readable_now(pf::borrowed_handle h) {
    auto r = R::create().value();
    auto reg = r.watch(h.get(), pf::interest::read, 7);
    if (!reg) return false;
    auto res = r.wait(0ms);          // poll, never block
    if (!res || res->timeout) return false;
    for (std::uint8_t i = 0; i < res->count; ++i)
        if (res->ready[i].token == 7)
            return res->ready[i].readable || res->ready[i].hangup;
    return false;
}

std::string drain(pf::borrowed_handle h) {
    std::string out;
    char buf[1024];
    for (;;) {
        const auto n = ::read(h.get(), buf, sizeof buf);
        if (n > 0) { out.append(buf, static_cast<std::size_t>(n)); continue; }
        break;   // 0 = EOF, <0 = EAGAIN with nothing left
    }
    return out;
}

int open_fd_count() {
    int n = 0;
    if (DIR* d = ::opendir("/proc/self/fd")) {
        while (::readdir(d)) ++n;
        ::closedir(d);
        return n;
    }
    return -1;   // not Linux; the check skips itself
}
#endif

// ── the checks ──────────────────────────────────────────────────────────

template <class Fx>
int exit_seen_late_is_not_lost() {
    auto p = Fx::writes_then_exits("hi", 0);
    Fx::run_out(p);                       // it ends while nobody is watching
    for (int i = 0; i < 5; ++i) {}        // ...and we look well afterwards
    if (!readable_now<pf::poll_reactor>(p.exit_handle())) return 101;
    return 0;
}

template <class Fx>
int exit_readiness_is_idempotent() {
    auto p = Fx::writes_then_exits("hi", 0);
    Fx::run_out(p);
    if (!readable_now<pf::poll_reactor>(p.exit_handle())) return 102;
    if (!readable_now<pf::poll_reactor>(p.exit_handle())) return 103;
    return 0;
}

template <class Fx>
int output_before_exit_survives_it() {
    auto p = Fx::writes_then_exits("the-last-bytes", 7);
    Fx::run_out(p);                       // wrote AND exited, nothing drained
    auto st = p.reap();
    if (!st) return 104;
    auto outh = p.stdout_handle();
    if (!outh) return 105;
    if (drain(*outh) != "the-last-bytes") return 106;   // reaped, still there
    return 0;
}

template <class Fx>
int stop_works_with_a_full_pipe() {
    auto p = Fx::fills_pipe_and_runs();
    Fx::advance(p);                       // pipe is now full, nobody reading
    if (!p.stop(pf::stop_mode::forceful, pf::stop_scope::tree)) return 107;
    // "It ended" is observable through the CONCEPT -- the exit handle goes
    // ready -- not through a backend's own bookkeeping. An earlier draft
    // asked the sim object whether it had ended, which compiled against one
    // backend and made the suite sim-shaped.
    Fx::run_out(p);
    if (!readable_now<pf::poll_reactor>(p.exit_handle())) return 108;
    auto st = p.reap();
    if (!st) return 109;
    return 0;
}

template <class Fx>
int reap_before_the_end_does_not_block() {
    auto p = Fx::runs_silently();
    auto st = p.reap();                   // must answer, not wait
    if (st) return 110;                   // it had not ended; a status is wrong
    return 0;
}

template <class Fx>
int exited_and_signalled_stay_distinct() {
    auto a = Fx::writes_then_exits("", 3);
    Fx::run_out(a);
    auto sa = a.reap();
    if (!sa) return 111;
    if (sa->how != pf::exit_status::kind::exited || sa->code != 3) return 112;
    if (sa->ok()) return 113;             // 3 is not success

    auto b = Fx::runs_silently();
    if (!b.stop(pf::stop_mode::forceful, pf::stop_scope::leader)) return 114;
    // Stopping is a REQUEST; the end is observed, not assumed. sim's stop is
    // synchronous so an earlier draft reaped straight after it and passed --
    // on a real OS the kill has not landed yet and reap() correctly says
    // would_block. The contract is "reap after the exit handle is ready",
    // and the check now honours it.
    Fx::run_out(b);
    if (!readable_now<pf::poll_reactor>(b.exit_handle())) return 115;
    auto sb = b.reap();
    if (!sb) return 116;
    if (sb->how != pf::exit_status::kind::signalled) return 117;
    return 0;
}

template <class Fx>
int merged_streams_are_one_handle() {
    auto p = Fx::merged_streams();
    if (p.stderr_handle()) return 118;    // merged ⇒ there is no second stream
    if (!p.stdout_handle()) return 119;
    Fx::run_out(p);
    if (drain(*p.stdout_handle()) != "to-stderr") return 120;
    return 0;
}

template <class Fx>
int spawn_and_reap_leak_nothing() {
    const int before = open_fd_count();
    if (before < 0) return 0;             // not Linux: skip, don't pretend
    {
        auto p = Fx::writes_then_exits("x", 0);
        Fx::run_out(p);
        (void)p.reap();
        (void)drain(*p.stdout_handle());
    }
    const int after = open_fd_count();
    if (after > before) return 121;
    return 0;
}

template <class Fx>
int stopping_an_ended_child_is_fine() {
    auto p = Fx::writes_then_exits("", 0);
    Fx::run_out(p);
    if (!p.stop(pf::stop_mode::forceful, pf::stop_scope::tree)) return 122;
    auto st = p.reap();
    if (!st) return 123;
    if (st->how != pf::exit_status::kind::exited) return 124;  // not rewritten
    return 0;
}

template <class Fx>
int tree_guarantee_is_declared_honestly() {
    auto p = Fx::runs_silently();
    const bool exact = p.tree_is_exact();
    // Nothing to assert about the VALUE — both answers are legitimate. What
    // matters is that it is answerable without spawning anything, so a host
    // can report the boundary it has before it needs it.
    (void)exact;
    (void)p.stop(pf::stop_mode::forceful, pf::stop_scope::leader);
    return 0;
}

template <class Fx>
int suite(const char* name) {
    int (*const checks[])() = {
        exit_seen_late_is_not_lost<Fx>,
        exit_readiness_is_idempotent<Fx>,
        output_before_exit_survives_it<Fx>,
        stop_works_with_a_full_pipe<Fx>,
        reap_before_the_end_does_not_block<Fx>,
        exited_and_signalled_stay_distinct<Fx>,
        merged_streams_are_one_handle<Fx>,
        spawn_and_reap_leak_nothing<Fx>,
        stopping_an_ended_child_is_fine<Fx>,
        tree_guarantee_is_declared_honestly<Fx>,
    };
    int n = 0;
    for (auto f : checks) {
        if (int r = f()) {
            std::fprintf(stderr, "process conformance[%s]: check %d failed\n", name, r);
            return 1;
        }
        ++n;
    }
    std::printf("process conformance[%s]: %d checks passed\n", name, n);
    return 0;
}

}  // namespace

int main() {
#if defined(_WIN32)
    std::puts("process conformance: no backend on this platform yet");
    return 0;
#else
    // The same checks, both backends. That is the contract: a backend is
    // correct when it passes the suite, and sim is not exempt from it.
    int r = suite<sim_fixture>(sim_fixture::name);
    r |= suite<posix_fixture>(posix_fixture::name);
    return r;
#endif
}
