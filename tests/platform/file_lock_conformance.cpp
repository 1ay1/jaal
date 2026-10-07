// tests/platform/file_lock_conformance.cpp — one suite, every FileLock backend.
//
// Each check is a template over a backend, so a new backend is added by
// instantiating the suite for it in main(). The checks are the RULES from
// platform/file_lock.hpp made executable, because prose in a header is a rule
// nobody can run.
//
// SHAPE RULES — every backend, sim included:
//   1. acquire() yields a lock that reports held()
//   2. release() is idempotent, and clears held()
//   3. a moved-from lock does not hold, and the destination does
//   4. the destructor releases (a second acquire succeeds afterwards)
//   5. the lock is on the SIDECAR, never the target
//   6. try_acquire() on a free lock succeeds
//   7. "busy" is nullopt, NOT an error — a caller can tell it from a fault
//   8. a filesystem that cannot lock reports an error, NOT a false success
//
// KERNEL RULES — POSIX only, and the suite says so rather than pretending
// the sim covers them. These are the whole reason the capability exists, and
// a sim that modelled them would be asserting that its own std::map works:
//   9.  a SECOND PROCESS is excluded while we hold it
//   10. the exclusion ends when the holder releases
//   11. a holder that DIES does not strand the lock (the kernel reclaims it)
//   12. shared mode admits a second reader but still excludes a writer
//   13. the same-process trap is REAL: two acquires in one process both
//       succeed under fcntl. Pinned so a host relies on the documented
//       behaviour instead of discovering it in production.

#include <jaal/platform/file_lock.hpp>
#include <jaal/platform/sim/file_lock.hpp>

#if !defined(_WIN32)
#  include <jaal/platform/posix/file_lock.hpp>
#endif

#include <cstdio>
#include <cstdlib>
#include <string>

#if !defined(_WIN32)
#  include <cerrno>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace pf = jaal::platform;

namespace {

int failures = 0;

void ok(bool cond, const char* backend, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL [%s] %s\n", backend, what);
        ++failures;
    }
}

// A scratch path per backend, so two instantiations of the suite cannot
// contend with each other and make a real failure look like a pass.
std::string scratch(const char* backend) {
    return std::string{"/tmp/jaal_file_lock_"} + backend + "_"
           + std::to_string(
#if defined(_WIN32)
               0
#else
               ::getpid()
#endif
           )
           + ".json";
}

bool file_exists(const std::string& p) {
    if (std::FILE* f = std::fopen(p.c_str(), "r")) {
        std::fclose(f);
        return true;
    }
    return false;
}

// ── shape rules: every backend ──────────────────────────────────────────

template <pf::FileLock L>
void check_shape(const char* backend, const std::string& target) {
    // 1. acquire yields a held lock
    {
        auto l = L::acquire(target);
        ok(l.has_value(), backend, "1. acquire() succeeds on a free lock");
        if (l) ok(l->held(), backend, "1. the acquired lock reports held()");
    }

    // 2. release is idempotent
    {
        auto l = L::acquire(target);
        if (l) {
            l->release();
            ok(!l->held(), backend, "2. release() clears held()");
            l->release();  // must not crash, must not error
            ok(!l->held(), backend, "2. release() twice is a no-op");
        }
    }

    // 3. move transfers the hold
    {
        auto l = L::acquire(target);
        if (l) {
            L moved = std::move(*l);
            ok(moved.held(), backend, "3. a moved-to lock holds");
            ok(!l->held(), backend, "3. a moved-from lock does not hold");
        }
    }

    // 4. the destructor releases: the next acquire must not block or fail
    {
        { auto first = L::acquire(target); (void)first; }
        auto second = L::try_acquire(target);
        ok(second.has_value() && second->has_value(), backend,
           "4. the destructor released, so try_acquire() succeeds after");
    }

    // 5. the sidecar, never the target. The whole correctness argument of
    //    this capability: an atomic write renames a new inode over the
    //    target, so a lock on the target guards a file that is not the file.
    {
        auto l = L::acquire(target);
        if (l)
            ok(l->path() == pf::lock_sidecar_for(target), backend,
               "5. the lock is held on <target>.lock, not on <target>");
        ok(!file_exists(target), backend,
           "5. locking does not create the target file itself");
    }

    // 6 + 7. try_acquire distinguishes free, busy and broken.
    {
        auto free_one = L::try_acquire(target);
        ok(free_one.has_value() && free_one->has_value(), backend,
           "6. try_acquire() takes a free lock");
    }
}

// ── kernel rules: a real second process ─────────────────────────────────
#if !defined(_WIN32)

// Does a forked child see the lock as taken? Returns the child's verdict.
// The child uses try_acquire so it answers instead of blocking: a hung
// child in CI is a test nobody can debug.
enum class child_saw { free_lock, busy, error };

child_saw ask_child(const std::string& target, pf::lock_mode m) {
    const pid_t pid = ::fork();
    if (pid < 0) return child_saw::error;
    if (pid == 0) {
        auto r = pf::posix_file_lock::try_acquire(target, m);
        if (!r) ::_exit(2);
        ::_exit(r->has_value() ? 0 : 1);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (!WIFEXITED(status)) return child_saw::error;
    switch (WEXITSTATUS(status)) {
        case 0:  return child_saw::free_lock;
        case 1:  return child_saw::busy;
        default: return child_saw::error;
    }
}

void check_kernel(const std::string& target) {
    const char* b = "posix";

    // 9 + 10. exclusion across processes, and its end.
    {
        auto held = pf::posix_file_lock::acquire(target);
        ok(held.has_value(), b, "9. parent took the lock");
        ok(ask_child(target, pf::lock_mode::exclusive) == child_saw::busy, b,
           "9. a SECOND PROCESS is excluded while we hold it");
        if (held) held->release();
        ok(ask_child(target, pf::lock_mode::exclusive) == child_saw::free_lock,
           b, "10. the exclusion ends when the holder releases");
    }

    // 11. a holder that dies strands nothing. The child takes the lock and
    //     _exits still holding it; the kernel must reclaim it. This is the
    //     property that keeps a crash from wedging the app forever, and it
    //     is unobservable without a real process.
    {
        const pid_t pid = ::fork();
        if (pid == 0) {
            auto l = pf::posix_file_lock::acquire(target);
            ::_exit(l ? 0 : 3);  // dies holding it
        }
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        ok(WIFEXITED(status) && WEXITSTATUS(status) == 0, b,
           "11. the child took the lock and exited while holding it");
        auto after = pf::posix_file_lock::try_acquire(target);
        ok(after.has_value() && after->has_value(), b,
           "11. a DEAD holder does not strand the lock");
    }

    // 12. shared admits a reader, still excludes a writer.
    {
        auto reader = pf::posix_file_lock::acquire(target, pf::lock_mode::shared);
        ok(reader.has_value(), b, "12. shared mode acquires");
        ok(ask_child(target, pf::lock_mode::shared) == child_saw::free_lock, b,
           "12. a shared holder admits another reader");
        ok(ask_child(target, pf::lock_mode::exclusive) == child_saw::busy, b,
           "12. a shared holder still excludes a writer");
    }

    // 13. THE TRAP, pinned. fcntl locks belong to the process, so a second
    //     acquire in THIS process succeeds — it does not wait. A host that
    //     swapped its std::mutex for this lock did not fix its race, it
    //     widened it. The concept documents this; here it is, executable.
    {
        auto first  = pf::posix_file_lock::acquire(target);
        auto second = pf::posix_file_lock::try_acquire(target);
        ok(second.has_value() && second->has_value(), b,
           "13. same-process acquire does NOT block (pair with jaal::guarded)");
    }
}
#endif

// 8. a filesystem that cannot lock reports an error rather than a false
//    success. Only the sim can stage this: making a real filesystem refuse
//    to lock means finding one, which is not a unit test.
void check_degrade() {
    const char* b      = "sim";
    const std::string t = "/nonexistent/cannot-lock.json";
    pf::sim_file_lock::script().reset();
    pf::sim_file_lock::script().fail(t, EROFS);

    auto blocking = pf::sim_file_lock::acquire(t);
    ok(!blocking.has_value(), b,
       "8. acquire() on a lock-incapable filesystem is an ERROR");

    auto nonblocking = pf::sim_file_lock::try_acquire(t);
    ok(!nonblocking.has_value(), b,
       "8. try_acquire() reports the fault as an error, not as 'busy'");

    // And the two outcomes stay distinguishable, which is the entire reason
    // try_acquire returns result<optional<L>> and not optional<L>.
    const std::string busy = "/tmp/jaal-sim-busy.json";
    pf::sim_file_lock::script().reset();
    pf::sim_file_lock::script().hold(busy);
    auto contended = pf::sim_file_lock::try_acquire(busy);
    ok(contended.has_value(), b, "7. a busy lock is not an error");
    ok(contended.has_value() && !contended->has_value(), b,
       "7. a busy lock is nullopt, so 'busy' and 'broken' never merge");
    pf::sim_file_lock::script().reset();
}

}  // namespace

int main() {
    pf::sim_file_lock::script().reset();
    check_shape<pf::sim_file_lock>("sim", scratch("sim"));
    pf::sim_file_lock::script().reset();

#if !defined(_WIN32)
    const std::string target = scratch("posix");
    check_shape<pf::posix_file_lock>("posix", target);
    check_kernel(target);
    (void)std::remove(pf::lock_sidecar_for(target).c_str());
#endif

    check_degrade();

    if (failures == 0) std::puts("file_lock conformance: all backends OK");
    return failures == 0 ? 0 : 1;
}
