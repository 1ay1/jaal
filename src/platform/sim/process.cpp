// src/platform/sim/process.cpp — the scripted child.
//
// Real pipes, simulated program. See include/jaal/platform/sim/process.hpp
// for why that split and not the other one.
//
// The only OS calls here are pipe/write/read/close and the eventfd-or-pipe
// used to make "has ended" watchable. Everything a test observes about
// timing is a step it asked for.

#include "jaal/platform/sim/process.hpp"

#include <cerrno>
#include <utility>

#if defined(_WIN32)
#  error "sim_process is POSIX-only for now; the Windows backend lands with wait_reactor support"
#else
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace jaal::platform {
namespace {

thread_local std::vector<sim_step> g_next_script;

/// A pipe as two owned ends. Non-blocking on BOTH ends: the writer so
/// fill_pipe can stop at EAGAIN instead of deadlocking the test, the reader
/// so a drain never blocks past what is there.
struct pipe_pair {
    native_handle r = invalid_handle;
    native_handle w = invalid_handle;

    [[nodiscard]] bool open() {
        int fds[2];
        if (::pipe(fds) != 0) return false;
        ::fcntl(fds[0], F_SETFL, O_NONBLOCK);
        ::fcntl(fds[1], F_SETFL, O_NONBLOCK);
        r = fds[0];
        w = fds[1];
        return true;
    }
    void close_read()  { if (is_valid(r)) { close_handle(r); r = invalid_handle; } }
    void close_write() { if (is_valid(w)) { close_handle(w); w = invalid_handle; } }
    void close_both()  { close_read(); close_write(); }
};

}  // namespace

struct sim_process::impl {
    std::vector<sim_step> script;
    std::size_t           next = 0;

    pipe_pair out;      ///< child's stdout (read end is ours)
    pipe_pair err;      ///< child's stderr
    pipe_pair in;       ///< child's stdin (write end is ours)
    pipe_pair exitp;    ///< "it ended": we close the write end, reader sees hangup

    bool        merged  = false;
    bool        done    = false;
    bool        reaped  = false;
    exit_status status{};
    std::uint64_t ident = 0;

    ~impl() {
        out.close_both();
        err.close_both();
        in.close_both();
        exitp.close_both();
    }

    /// Ending is observable the way a real child's is: the handle becomes
    /// permanently ready. Closing the write end gives hangup, which is
    /// level-triggered by every reactor backend — a caller that looks late,
    /// or twice, sees the same thing. That is the ordering rule, enforced
    /// by construction rather than by remembering to re-arm something.
    void finish(exit_status s) {
        if (done) return;
        status = s;
        done   = true;
        // The child's ends of its own streams close when it ends, which is
        // what gives the reader EOF *after* it has drained what was written.
        out.close_write();
        err.close_write();
        in.close_read();
        exitp.close_write();
    }

    void emit(std::string_view bytes, bool to_err) {
        if (done) return;
        pipe_pair& p = (to_err && !merged) ? err : out;
        if (!is_valid(p.w)) return;
        std::size_t off = 0;
        while (off < bytes.size()) {
            const auto n = ::write(p.w, bytes.data() + off, bytes.size() - off);
            if (n > 0) { off += static_cast<std::size_t>(n); continue; }
            if (n < 0 && errno == EINTR) continue;
            break;   // EAGAIN: the pipe is full and nobody is draining
        }
    }

    /// Write until the pipe refuses. Deterministic in OUTCOME ("it is now
    /// full") even though the byte count is the OS's business, which is the
    /// property the suite actually needs.
    void fill() {
        if (done || !is_valid(out.w)) return;
        static constexpr char block[4096] = {};
        for (;;) {
            const auto n = ::write(out.w, block, sizeof block);
            if (n > 0) continue;
            if (n < 0 && errno == EINTR) continue;
            break;
        }
    }
};

sim_process::sim_process() : p_(std::make_unique<impl>()) {}
sim_process::sim_process(sim_process&&) noexcept            = default;
sim_process& sim_process::operator=(sim_process&&) noexcept = default;
sim_process::~sim_process()                                 = default;

void sim_script(std::vector<sim_step> steps) { g_next_script = std::move(steps); }

result<sim_process> sim_process::spawn(const process_spec& spec) {
    static std::uint64_t next_id = 1;

    sim_process sp;
    auto& s = *sp.p_;
    s.script = std::exchange(g_next_script, {});
    s.merged = spec.merge_stderr;
    s.ident  = next_id++;

    if (!s.exitp.open()) return std::unexpected(error::from_errno(errno, "sim_process: pipe"));
    if (spec.stdout_to == stream_to::pipe && !s.out.open())
        return std::unexpected(error::from_errno(errno, "sim_process: pipe"));
    if (spec.stderr_to == stream_to::pipe && !s.merged && !s.err.open())
        return std::unexpected(error::from_errno(errno, "sim_process: pipe"));
    if (spec.stdin_from == stream_to::pipe && !s.in.open())
        return std::unexpected(error::from_errno(errno, "sim_process: pipe"));

    return sp;
}

borrowed_handle sim_process::exit_handle() const {
    return borrowed_handle{p_->exitp.r};
}

std::optional<borrowed_handle> sim_process::stdout_handle() const {
    if (!is_valid(p_->out.r)) return std::nullopt;
    return borrowed_handle{p_->out.r};
}

std::optional<borrowed_handle> sim_process::stderr_handle() const {
    // When merged, stderr IS stdout — reporting a second handle would invite
    // a caller to watch one stream twice and call the duplicate a race.
    if (p_->merged || !is_valid(p_->err.r)) return std::nullopt;
    return borrowed_handle{p_->err.r};
}

std::optional<borrowed_handle> sim_process::stdin_handle() const {
    if (!is_valid(p_->in.w)) return std::nullopt;
    return borrowed_handle{p_->in.w};
}

result<void> sim_process::stop(stop_mode mode, stop_scope) {
    // Stopping does NOT require anyone to be draining. A real backend has to
    // work to keep that true (a signal does not care about a full pipe); here
    // it is true because ending is a state change, not a write. The suite
    // checks it against both.
    if (p_->done) return {};
    p_->finish(mode == stop_mode::graceful
                   ? exit_status{exit_status::kind::signalled, 15}
                   : exit_status{exit_status::kind::signalled, 9});
    return {};
}

result<exit_status> sim_process::reap() {
    if (!p_->done) return std::unexpected(error::make(std::errc::operation_would_block,
                                       "sim_process: still running"));
    p_->reaped = true;
    return p_->status;
}

bool sim_process::tree_is_exact() const noexcept { return false; }
std::uint64_t sim_process::id() const noexcept { return p_->ident; }

bool sim_process::step() {
    auto& s = *p_;
    if (s.next >= s.script.size()) return false;
    const auto st = s.script[s.next++];
    switch (st.what) {
        case sim_step::kind::write_out: s.emit(st.bytes, false); break;
        case sim_step::kind::write_err: s.emit(st.bytes, true);  break;
        case sim_step::kind::fill_pipe: s.fill();                break;
        case sim_step::kind::exit_ok:
            s.finish(exit_status{exit_status::kind::exited, 0});
            break;
        case sim_step::kind::exit_code:
            s.finish(exit_status{exit_status::kind::exited, st.value});
            break;
        case sim_step::kind::killed:
            s.finish(exit_status{exit_status::kind::signalled, st.value});
            break;
    }
    return true;
}

void sim_process::run_to_completion() { while (step()) {} }

bool sim_process::ended() const noexcept { return p_->done; }

}  // namespace jaal::platform
