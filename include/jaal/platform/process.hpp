#pragma once
// jaal::platform — the child-process capability.
//
// A host that runs other programs needs four things from the OS: start one,
// notice when it ends, read what it said, and stop it. Every program that
// needs them writes the same poll loop, and the loop is where the bugs are:
// deadline arithmetic that wraps, a pipe drained after the exit is reaped so
// the last bytes vanish, a grandchild that outlives the kill.
//
// None of that is process-specific. Waiting is the Reactor's job, deadlines
// are the clock's, and a descriptor is owned_handle. What is genuinely
// specific is small: how a child is started, how its end is OBSERVED as a
// readiness event rather than a signal handler, and how a whole tree is
// stopped. That is this header.
//
// WHAT IT DELIBERATELY DOES NOT DO
//
// No timeouts, no retries, no kill escalation policy, no output buffering.
// Those are the caller's, and they differ per caller: an idle deadline that
// resets on output is right for a build, an absolute ceiling is right for a
// runaway, and most programs want both. Policy over a reactor is testable
// under sim_clock in microseconds; policy baked in here would be testable
// only by sleeping. So the capability hands you an exit you can WATCH and a
// tree you can STOP, and stays out of the way.
//
// Lessons from the two hand-rolled runners this replaces, each a rule below:
//   * a child that keeps talking must still be stoppable: stopping may not
//     depend on anyone draining its output
//   * output buffered before exit survives the exit: EOF is the end of the
//     bytes, exit is not
//   * exit readiness is level-triggered and idempotent: a caller that learns
//     of it twice behaves the same
//   * the process group is NOT the tree: setsid() leaves it

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../core/error.hpp"
#include "concepts.hpp"
#include "handle.hpp"

namespace jaal::platform {

// ── how a child is started ──────────────────────────────────────────────

/// Where a standard stream goes.
///
/// `pipe` is the only one that yields a readable handle. `null` is /dev/null
/// (NUL on Windows) and `inherit` is the parent's, which a host should use
/// only for a child it means to let touch the terminal.
enum class stream_to : std::uint8_t { pipe, null, inherit };

struct process_spec {
    /// argv[0] is the program. No shell: a shell is a program you spawn like
    /// any other, and making it implicit is how a path with a space becomes
    /// two arguments and a quoted string becomes code.
    std::vector<std::string> argv;

    /// Replaces the environment when `env_is_complete`, otherwise layers on
    /// top of the parent's. Layering is the common case; a complete
    /// environment is for a host that must not leak its own.
    std::vector<std::pair<std::string, std::string>> env;
    bool env_is_complete = false;

    /// Empty inherits the parent's. Applied as a real directory change in
    /// the child, never as a `cd &&` prefix, so it cannot be re-parsed.
    std::string cwd;

    stream_to stdin_from = stream_to::null;
    stream_to stdout_to  = stream_to::pipe;
    stream_to stderr_to  = stream_to::pipe;

    /// Merge stderr into the stdout pipe. One handle to watch, and the
    /// interleaving the user would have seen on a terminal — which is what a
    /// log is for. Ignored unless both are `pipe`.
    bool merge_stderr = false;

    /// Put the child in its own session, so it cannot read the parent's
    /// terminal and a Ctrl-C to the parent's group does not reach it.
    /// Independent of `stop_tree`: a session is about the terminal, a tree
    /// is about who dies.
    bool new_session = true;
};

// ── how a child ends ────────────────────────────────────────────────────

/// Why a child is no longer running.
///
/// Two cases, not one integer. "128 + signal" is a shell convention, and
/// collapsing them loses the difference between a program that chose to exit
/// 137 and one the kernel killed — which is exactly the difference a
/// timeout caller is asking about.
struct exit_status {
    enum class kind : std::uint8_t { exited, signalled };
    kind         how  = kind::exited;
    std::int32_t code = 0;   ///< exit code, or the signal number

    [[nodiscard]] constexpr bool ok() const noexcept {
        return how == kind::exited && code == 0;
    }
};

/// How hard to stop a child.
///
/// `graceful` asks (SIGTERM / CTRL_BREAK) and a well-behaved program gets to
/// flush and exit. `forceful` does not ask (SIGKILL / TerminateProcess).
/// A caller that wants "ask, then insist" does both with a delay between,
/// because the delay is policy and policy lives in the caller.
enum class stop_mode : std::uint8_t { graceful, forceful };

/// Who to stop.
///
/// `leader` signals the child itself. `tree` stops it and everything it
/// started, including descendants that left the process group — a `setsid()`
/// escapes killpg, and so does anything a shell re-parented, which is how a
/// runaway outlives the tool that launched it.
///
/// `tree` is best-effort by backend and says so: Linux uses cgroup.kill when
/// the host delegated a cgroup (one atomic kernel operation over the whole
/// subtree, nothing to race) and falls back to the process group otherwise;
/// Windows uses a job object; macOS uses the process group. A backend that
/// can only reach the group reports `tree_is_exact() == false` so a host can
/// say so out loud instead of believing a guarantee it does not have.
enum class stop_scope : std::uint8_t { leader, tree };

// ── Process ─────────────────────────────────────────────────────────────
//
// A spawned child as a linear resource: move-only, reaped exactly once.
//
//   spawn(spec)        → process
//   p.exit_handle()    → watch() it on a Reactor; readable ⇒ it has ended
//   p.stdout_handle()  → watch() it; readable ⇒ bytes, hangup ⇒ EOF
//   p.stop(mode,scope) → signal it
//   p.reap()           → exit_status, once the exit handle said so
//
// THE ORDERING RULE. The exit handle and the output handle are independent,
// and a caller may learn of them in either order: a child can exit while its
// last bytes are still in the pipe, and a pipe can hang up while the child
// lingers. So:
//
//   * exit readiness is LEVEL-triggered: once ended, it stays ready. A
//     caller that checks late, or twice, sees the same thing. (Edge
//     triggering here is how "it exited before we started watching" turns
//     into a hang.)
//   * buffered output survives the exit. Reading after reap() still drains
//     what was written before it. EOF ends the bytes; exit does not.
//   * stop() never requires anyone to be reading. A child filling a pipe
//     nobody drains is the exact case a timeout exists for, and a stop that
//     deadlocks against a full pipe is not a stop.
//
// reap() after the exit handle is ready never blocks. Before it, reap()
// returns std::errc::operation_would_block rather than waiting — a blocking
// wait is the reactor's job, and offering it here would give every caller a
// second way to hang.
template <class P>
concept Process =
    requires {
        typename P::handle;   // same native type the Reactor watches
    }
    && std::movable<P>
    && !std::copyable<P>
    && requires(P& p, const P& cp, stop_mode m, stop_scope s) {
        { P::spawn(std::declval<const process_spec&>()) } -> std::same_as<result<P>>;

        // Watchable on a Reactor without a cast. nullopt when the spec said
        // the stream was not a pipe.
        { cp.exit_handle() }   -> std::same_as<borrowed_handle>;
        { cp.stdout_handle() } -> std::same_as<std::optional<borrowed_handle>>;
        { cp.stderr_handle() } -> std::same_as<std::optional<borrowed_handle>>;
        { cp.stdin_handle() }  -> std::same_as<std::optional<borrowed_handle>>;

        { p.stop(m, s) }       -> std::same_as<result<void>>;
        { p.reap() }           -> std::same_as<result<exit_status>>;

        // Does `stop_scope::tree` actually cover the tree here, or only the
        // process group? A host that reports its own guarantees needs to be
        // able to ask rather than assume.
        { cp.tree_is_exact() } noexcept -> std::same_as<bool>;

        // For diagnostics and for a host that must name the child in a log.
        // NOT for signalling: a pid is reusable and signalling one you only
        // remembered is how you kill a stranger.
        { cp.id() } noexcept -> std::same_as<std::uint64_t>;
    };

}  // namespace jaal::platform
