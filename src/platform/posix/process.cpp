// src/platform/posix/process.cpp — fork, exec, and an exit you can watch.
//
// The child half of a fork is a hostile place: between fork() and exec()
// only async-signal-safe calls are legal, there is no allocation, and a
// failure has nowhere to report to. So the child does the minimum, in a
// fixed order, and reports failure through a close-on-exec pipe: if exec
// succeeds the pipe closes empty, and if it fails the child writes errno and
// _exit()s. The parent reads that pipe to tell "never ran" from "ran and
// failed", which is a distinction the caller can act on and a bare exit code
// cannot express.

#include "jaal/platform/posix/process.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__)
#  include <sys/syscall.h>
#endif

namespace jaal::platform {
namespace {

/// A pipe as two owned ends, so a half-built spawn cannot leak one.
struct pipe_pair {
    native_handle r = invalid_handle;
    native_handle w = invalid_handle;

    [[nodiscard]] bool open(bool cloexec) {
        int fds[2];
#if defined(__linux__)
        if (::pipe2(fds, cloexec ? O_CLOEXEC : 0) != 0) return false;
#else
        if (::pipe(fds) != 0) return false;
        if (cloexec) {
            ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
            ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
        }
#endif
        r = fds[0];
        w = fds[1];
        return true;
    }
    void close_read()  { if (is_valid(r)) { close_handle(r); r = invalid_handle; } }
    void close_write() { if (is_valid(w)) { close_handle(w); w = invalid_handle; } }
    void close_both()  { close_read(); close_write(); }
};

#if defined(__linux__)
[[nodiscard]] int open_pidfd(::pid_t pid) {
    return static_cast<int>(::syscall(SYS_pidfd_open, pid, 0u));
}

/// Signal through the pidfd. Names THIS child: no pid-reuse race, and no
/// dependence on what a pid means in whatever namespace it came from.
[[nodiscard]] bool signal_via_pidfd(int pidfd, int sig) {
    if (pidfd < 0) return false;
    return ::syscall(SYS_pidfd_send_signal, pidfd, sig, nullptr, 0u) == 0;
}
#endif

/// Write all of `n` bytes, retrying EINTR. Async-signal-safe: no allocation,
/// no stdio.
void write_all(int fd, const void* buf, std::size_t n) {
    const auto* p = static_cast<const char*>(buf);
    while (n > 0) {
        const auto w = ::write(fd, p, n);
        if (w > 0) { p += w; n -= static_cast<std::size_t>(w); continue; }
        if (w < 0 && errno == EINTR) continue;
        return;
    }
}

}  // namespace

struct posix_process::impl {
    ::pid_t pid = -1;

    pipe_pair out;       ///< child stdout (we hold the read end)
    pipe_pair err;       ///< child stderr
    pipe_pair in;        ///< child stdin (we hold the write end)
    pipe_pair death;     ///< non-Linux exit signal (we hold the read end)

    native_handle pidfd = invalid_handle;   ///< Linux exit signal

    bool        merged     = false;
    /// Only meaningful for an adopted child, where the caller asserts it.
    /// For one we forked we ASK instead -- see leads_own_group().
    bool        claimed_group = false;
    bool        adopted       = false;
    bool        reaped     = false;
    bool        exit_exact = false;
    exit_status status{};

    ~impl() {
        out.close_both();
        err.close_both();
        in.close_both();
        death.close_both();
        if (is_valid(pidfd)) close_handle(pidfd);
        // A child we never reaped would become a zombie held by this
        // process for its lifetime. Best effort: ask it to go, then reap
        // without blocking. We do NOT wait — a destructor that can hang is
        // worse than a zombie.
        if (pid > 0 && !reaped) {
            if (leads_own_group()) ::kill(-pid, SIGKILL);
#if defined(__linux__)
            if (!signal_via_pidfd(pidfd, SIGKILL)) ::kill(pid, SIGKILL);
#else
            ::kill(pid, SIGKILL);
#endif
            int st = 0;
            ::waitpid(pid, &st, WNOHANG);
        }
    }

    /// Asked, not remembered: setsid() fails when the caller already leads a
    /// group, so storing the request would claim a group we may not lead and
    /// kill(-pid) would signal a stranger.
    [[nodiscard]] bool leads_own_group() const {
        if (pid <= 0) return false;
        if (adopted) return claimed_group;   // we did not fork it; take the word
        return ::getpgid(pid) == pid;
    }

    [[nodiscard]] native_handle exit_fd() const {
        return is_valid(pidfd) ? pidfd : death.r;
    }
};

posix_process::posix_process() : p_(std::make_unique<impl>()) {}
posix_process::posix_process(posix_process&&) noexcept            = default;
posix_process& posix_process::operator=(posix_process&&) noexcept = default;
posix_process::~posix_process()                                   = default;

result<posix_process> posix_process::spawn(const process_spec& spec) {
    if (spec.argv.empty())
        return std::unexpected(error::make(std::errc::invalid_argument,
                                           "posix_process: empty argv"));

    posix_process self;
    auto& s = *self.p_;
    s.merged = spec.merge_stderr;

    const bool want_out = spec.stdout_to == stream_to::pipe;
    const bool want_err = spec.stderr_to == stream_to::pipe && !s.merged;
    const bool want_in  = spec.stdin_from == stream_to::pipe;

    // Our ends are close-on-exec so the child never inherits them; the
    // child's ends are dup2'd into place and therefore survive deliberately.
    if (want_out && !s.out.open(true))
        return std::unexpected(error::from_errno(errno, "posix_process: pipe"));
    if (want_err && !s.err.open(true))
        return std::unexpected(error::from_errno(errno, "posix_process: pipe"));
    if (want_in && !s.in.open(true))
        return std::unexpected(error::from_errno(errno, "posix_process: pipe"));

    // The exec-failure channel: closed-empty on success, carries errno on
    // failure. CLOEXEC is what makes "closed empty" mean "exec happened".
    pipe_pair report;
    if (!report.open(true))
        return std::unexpected(error::from_errno(errno, "posix_process: pipe"));

#if !defined(__linux__)
    // Death pipe: NOT cloexec, because the child must keep it across exec.
    if (!s.death.open(false)) {
        report.close_both();
        return std::unexpected(error::from_errno(errno, "posix_process: pipe"));
    }
#endif

    // Build argv/envp BEFORE fork: the child cannot allocate. Mutable
    // copies rather than casting away const on the caller's strings --
    // execvp takes char* for historical reasons, not because it writes.
    std::vector<std::string> argv_store(spec.argv.begin(), spec.argv.end());
    std::vector<char*> argv;
    argv.reserve(argv_store.size() + 1);
    for (auto& a : argv_store) argv.push_back(a.data());
    argv.push_back(nullptr);

    std::vector<std::string> env_store;
    std::vector<char*>       envp;
    if (!spec.env.empty() || spec.env_is_complete) {
        if (!spec.env_is_complete)
            for (char** e = environ; e && *e; ++e) env_store.emplace_back(*e);
        for (const auto& [k, v] : spec.env) env_store.push_back(k + "=" + v);
        envp.reserve(env_store.size() + 1);
        for (auto& e : env_store) envp.push_back(e.data());
        envp.push_back(nullptr);
    }

    const char* cwd = spec.cwd.empty() ? nullptr : spec.cwd.c_str();

    const ::pid_t pid = ::fork();
    if (pid < 0) {
        report.close_both();
        return std::unexpected(error::from_errno(errno, "posix_process: fork"));
    }

    if (pid == 0) {
        // ── child: async-signal-safe only from here ──────────────────────
        const auto die = [&](int e) {
            write_all(report.w, &e, sizeof e);
            ::_exit(127);
        };

        if (spec.new_session && ::setsid() < 0) { /* not fatal */ }
        if (cwd && ::chdir(cwd) != 0) die(errno);

        const int devnull = ::open("/dev/null", O_RDWR | O_CLOEXEC);

        if (want_in)                       { if (::dup2(s.in.r, 0) < 0) die(errno); }
        else if (spec.stdin_from == stream_to::null && devnull >= 0)
                                           { ::dup2(devnull, 0); }

        if (want_out)                      { if (::dup2(s.out.w, 1) < 0) die(errno); }
        else if (spec.stdout_to == stream_to::null && devnull >= 0)
                                           { ::dup2(devnull, 1); }

        if (s.merged && want_out)          { if (::dup2(1, 2) < 0) die(errno); }
        else if (want_err)                 { if (::dup2(s.err.w, 2) < 0) die(errno); }
        else if (spec.stderr_to == stream_to::null && devnull >= 0)
                                           { ::dup2(devnull, 2); }

        // Default the signal disposition: a child inheriting SIG_IGN on
        // SIGPIPE writes into a closed pipe forever instead of dying, which
        // is how a "finished" command stays alive.
        ::signal(SIGPIPE, SIG_DFL);

        if (envp.empty()) ::execvp(argv[0], argv.data());
        else              ::execvpe(argv[0], argv.data(), envp.data());
        die(errno);
    }

    // ── parent ──────────────────────────────────────────────────────────
    s.pid = pid;
    report.close_write();          // so our read sees EOF when exec succeeds
    s.out.close_write();
    s.err.close_write();
    s.in.close_read();
    s.death.close_write();         // the child holds the only other copy

#if defined(__linux__)
    s.pidfd      = open_pidfd(pid);
    s.exit_exact = is_valid(s.pidfd);
#else
    s.exit_exact = false;
#endif

    // Did exec happen? Empty ⇒ yes. errno ⇒ it never ran.
    int child_errno = 0;
    for (;;) {
        const auto n = ::read(report.r, &child_errno, sizeof child_errno);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) child_errno = 0;   // EOF: exec succeeded
        break;
    }
    report.close_read();

    if (child_errno != 0) {
        int st = 0;
        ::waitpid(pid, &st, 0);        // it already _exit()ed; no hang
        s.reaped = true;
        s.pid    = -1;
        return std::unexpected(error::from_errno(child_errno,
                                                 "posix_process: exec"));
    }

    // Non-blocking reads: a drain must never block past what is there.
    if (is_valid(s.out.r)) ::fcntl(s.out.r, F_SETFL, O_NONBLOCK);
    if (is_valid(s.err.r)) ::fcntl(s.err.r, F_SETFL, O_NONBLOCK);

    return self;
}

result<posix_process> posix_process::adopt(adopted_child c) {
    if (c.pid <= 0) {
        // Close what we were handed anyway: the caller passed ownership, and
        // leaking on the error path is how a refusal becomes a leak.
        for (int fd : {c.pidfd, c.stdout_fd, c.stderr_fd, c.stdin_fd})
            if (fd >= 0) close_handle(fd);
        return std::unexpected(error::make(std::errc::invalid_argument,
                                           "posix_process::adopt: no pid"));
    }

    posix_process self;
    auto& s = *self.p_;
    s.pid           = c.pid;
    s.merged        = c.merged;
    s.claimed_group = c.leads_own_group;
    s.adopted       = true;
    s.out.r  = c.stdout_fd;
    s.err.r  = c.merged ? invalid_handle : c.stderr_fd;
    s.in.w   = c.stdin_fd;

#if defined(__linux__)
    // Prefer the spawner's pidfd; open one otherwise. Either way the exit is
    // exact -- a pidfd names THIS child and cannot be confused by pid reuse,
    // which matters more here than for our own fork(): an adopted pid came
    // from code we did not write.
    s.pidfd      = (c.pidfd >= 0) ? c.pidfd : open_pidfd(c.pid);
    s.exit_exact = is_valid(s.pidfd);
#else
    if (c.pidfd >= 0) close_handle(c.pidfd);
    s.exit_exact = false;
#endif

    if (!is_valid(s.pidfd)) {
        // No watchable exit, and we cannot retrofit one: the death-pipe trick
        // needs a descriptor the child inherited, and this child was already
        // running before we were asked. Refuse rather than hand back a
        // process whose exit_handle() never fires -- a caller waiting on it
        // would hang forever, which is worse than a clear failure here.
        return std::unexpected(error::make(
            std::errc::not_supported,
            "posix_process::adopt: no pidfd, so the exit is not watchable"));
    }

    if (is_valid(s.out.r)) ::fcntl(s.out.r, F_SETFL, O_NONBLOCK);
    if (is_valid(s.err.r)) ::fcntl(s.err.r, F_SETFL, O_NONBLOCK);
    return self;
}

borrowed_handle posix_process::exit_handle() const {
    return borrowed_handle{p_->exit_fd()};
}

std::optional<borrowed_handle> posix_process::stdout_handle() const {
    if (!is_valid(p_->out.r)) return std::nullopt;
    return borrowed_handle{p_->out.r};
}

std::optional<borrowed_handle> posix_process::stderr_handle() const {
    if (p_->merged || !is_valid(p_->err.r)) return std::nullopt;
    return borrowed_handle{p_->err.r};
}

std::optional<borrowed_handle> posix_process::stdin_handle() const {
    if (!is_valid(p_->in.w)) return std::nullopt;
    return borrowed_handle{p_->in.w};
}

result<void> posix_process::stop(stop_mode mode, stop_scope scope) {
    if (p_->pid <= 0 || p_->reaped) return {};   // already gone: not an error
    const int sig = (mode == stop_mode::graceful) ? SIGTERM : SIGKILL;

    // Signalling does not require anyone to be draining: a full pipe blocks
    // the CHILD's write, not our kill.
    // The group, but only when we KNOW the pid leads one here. For an
    // adopted child from a PID namespace the number means something else on
    // this host, and kill(-n) would signal a stranger.
    if (scope == stop_scope::tree && p_->leads_own_group()) {
        if (::kill(-p_->pid, sig) == 0) return {};
        if (errno != ESRCH)
            return std::unexpected(error::from_errno(errno, "posix_process: killpg"));
    }
#if defined(__linux__)
    // Preferred for the leader: unambiguous, and correct across namespaces.
    if (signal_via_pidfd(p_->pidfd, sig)) return {};
#endif
    if (::kill(p_->pid, sig) != 0 && errno != ESRCH)
        return std::unexpected(error::from_errno(errno, "posix_process: kill"));
    return {};
}

result<exit_status> posix_process::reap() {
    if (p_->reaped) return p_->status;
    if (p_->pid <= 0)
        return std::unexpected(error::make(std::errc::no_such_process,
                                           "posix_process: nothing to reap"));
    int st = 0;
    for (;;) {
        const auto r = ::waitpid(p_->pid, &st, WNOHANG);
        if (r < 0 && errno == EINTR) continue;
        if (r == 0)
            return std::unexpected(error::make(std::errc::operation_would_block,
                                               "posix_process: still running"));
        if (r < 0)
            return std::unexpected(error::from_errno(errno, "posix_process: waitpid"));
        break;
    }
    if (WIFEXITED(st))
        p_->status = exit_status{exit_status::kind::exited, WEXITSTATUS(st)};
    else if (WIFSIGNALED(st))
        p_->status = exit_status{exit_status::kind::signalled, WTERMSIG(st)};
    p_->reaped = true;
    return p_->status;
}

bool posix_process::tree_is_exact() const noexcept { return false; }
bool posix_process::exit_is_exact() const noexcept { return p_->exit_exact; }
std::uint64_t posix_process::id() const noexcept {
    return static_cast<std::uint64_t>(p_->pid);
}

}  // namespace jaal::platform
