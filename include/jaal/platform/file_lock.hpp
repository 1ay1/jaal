#pragma once
// jaal::platform — the cross-process critical-section capability.
//
// jaal::guarded<T> makes in-process races impossible to write. It cannot help
// with the other kind: two COPIES OF THE PROGRAM doing read-modify-write on
// the same file. A std::mutex is per-process, so both read the same state,
// both write their own stale copy, and whichever lands second silently drops
// the other's work. The file never corrupts (an atomic write renames), which
// is what makes it nasty — the damage is a missing entry, not a parse error,
// and it survives until something rebuilds from scratch.
//
// Nothing about that is specific to any one file, and every program that
// needs it grows the same half-right answer. This is the primitive.
//
// WHAT A HOLDER OF THIS LOCK ACTUALLY HAS
//
// Four properties, each of which has bitten somebody:
//
//   * THE LOCK LIVES ON A SIDECAR, NEVER ON THE FILE ITSELF. An atomic write
//     renames a new inode over the target, so a lock taken on the target
//     guards an inode that is no longer the file. Locking `x.json` and then
//     rewriting it atomically protects nothing at all. `acquire("x.json")`
//     therefore locks `x.json.lock`, and that file exists only to be locked.
//
//   * IT IS ADVISORY. Processes that take the lock are serialized against
//     each other and against nobody else. A text editor, a backup tool or an
//     older build of the same program writing that file is not stopped. This
//     buys cooperation between instances of ONE program, which is the actual
//     problem, and it is not a general mutual-exclusion guarantee.
//
//   * IT IS CROSS-PROCESS AND **NOT** CROSS-THREAD. This is the trap. POSIX
//     record locks are owned by the PROCESS: two threads of one process both
//     "acquire" successfully and neither waits, and closing ANY descriptor
//     onto that file releases the lock for all of them. So a host that
//     replaces its std::mutex with this has not fixed the race, it has
//     widened it. Pair the two: jaal::guarded<T> for the threads, this for
//     the processes. The conformance suite pins the behaviour so a host can
//     rely on it rather than discovering it.
//
//   * ACQUISITION CAN FAIL, AND THAT IS NOT FATAL. A read-only HOME, a
//     filesystem with no lock support, an exhausted descriptor table. The
//     capability REPORTS it and stops there, because what to do next is
//     policy and differs per caller: a host writing a cache may proceed
//     unlocked and accept a lost update; one writing credentials must not.
//     An error here is information, not a verdict.
//
// WHAT IT DELIBERATELY DOES NOT DO
//
// No deadlock ordering, no lock hierarchy, no re-entrance, no fairness. Hold
// ONE of these at a time and the first three cannot arise; holding two is a
// lock-order inversion between processes, which no amount of API can detect
// from inside one of them. Fairness belongs to the kernel.
//
// Nor does it carry the data. A guarded<T> can own its value because the
// value is in memory; a file's contents live in the filesystem and the whole
// point is that another process can change them. So this hands you a scope,
// and reading/writing inside it stays the caller's.

#include <concepts>
#include <optional>
#include <string>
#include <type_traits>

#include "../core/error.hpp"

namespace jaal::platform {

/// Exclusive excludes everyone; shared excludes only writers.
///
/// Shared is here because the read side of a read-modify-write file is a
/// real use (many readers of a cache, one writer) and a capability that only
/// offered exclusive would push hosts into serializing their readers too.
enum class lock_mode : std::uint8_t {
    exclusive,  ///< one holder, no readers
    shared,     ///< many holders, no exclusive holder
};

// ── the capability ──────────────────────────────────────────────────────
//
// A lock is a LINEAR RESOURCE, like owned_handle: move-only, released
// exactly once, and released by the destructor so an early return or a
// throw inside the critical section cannot leave it held. There is no
// copy and no way to ask for the underlying descriptor, because a second
// owner of either is a double release.
template <class L>
concept FileLock =
    std::move_constructible<L> && std::is_nothrow_move_assignable_v<L>
    && !std::copy_constructible<L>
    && requires(L& l, const L& cl, const std::string& path, lock_mode m) {
        /// Wait until the lock is ours. Blocks; retries on EINTR with the
        /// time it had left, because a signal is not an answer.
        { L::acquire(path, m) } -> std::same_as<result<L>>;

        /// Take it only if it is free. `nullopt` means SOMEONE ELSE HOLDS
        /// IT, which is an outcome and not an error — a host polling a
        /// lock should not have to parse an errno to tell "busy" from
        /// "this filesystem cannot lock at all". The latter is the error.
        { L::try_acquire(path, m) } -> std::same_as<result<std::optional<L>>>;

        /// Do we hold it right now? False after a release() or a move-out,
        /// so a host can assert its own critical sections.
        { cl.held() } noexcept -> std::same_as<bool>;

        /// Give it up early. Idempotent: releasing twice is a no-op, not an
        /// error, so a host may release inside the scope and still let the
        /// destructor run.
        { l.release() } noexcept;

        /// The sidecar this lock is actually held on. For diagnostics — a
        /// host that logs "waiting for the settings lock" should be able to
        /// name the file a user can inspect.
        { cl.path() } noexcept -> std::same_as<const std::string&>;
    };

/// The sidecar path `acquire(target)` locks, exposed so a host can name it
/// in a log or clean it up. One function, so every backend and every caller
/// agrees on it; a backend inventing its own suffix would let two programs
/// lock the same file through different sidecars and never contend.
[[nodiscard]] inline std::string lock_sidecar_for(const std::string& target) {
    return target + ".lock";
}

}  // namespace jaal::platform
