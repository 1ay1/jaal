// handle_test.cpp — owned_handle closes exactly once, and borrows can't.
//
// The type exists so that four libraries stop writing four incompatible fd
// wrappers. What it has to earn is the "exactly once" claim, so these tests
// go at it from both ends: the handle really is closed when it goes out of
// scope (checked with fcntl, which fails on a closed descriptor), and it is
// NOT closed when it was moved, released, or only borrowed.
#include <jaal/platform/handle.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <utility>
#include <vector>

namespace {

int failures = 0;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

using jaal::platform::borrowed_handle;
using jaal::platform::native_handle;
using jaal::platform::owned_handle;

/// A descriptor we can ask about later. A pipe read end: cheap, and closing
/// it is observable.
int fresh_fd() {
    int fds[2];
    if (::pipe(fds) != 0) return -1;
    ::close(fds[1]);
    return fds[0];
}

/// Is this number still a live descriptor in this process?
bool live(int fd) { return ::fcntl(fd, F_GETFD) != -1; }

// ── the guarantee ───────────────────────────────────────────────────────

void closes_once_at_scope_end() {
    std::printf("an owned handle closes at the end of its scope\n");
    const int fd = fresh_fd();
    CHECK(fd >= 0);
    CHECK(live(fd));
    {
        owned_handle h{fd};
        CHECK(h.valid());
        CHECK(live(fd));
    }
    CHECK(!live(fd));
    std::printf("  and not before\n");
}

void a_move_does_not_close() {
    std::printf("moving hands the descriptor over, it is not closed twice\n");
    const int fd = fresh_fd();
    CHECK(fd >= 0);
    {
        owned_handle a{fd};
        owned_handle b{std::move(a)};
        // The moved-from owner holds nothing: its destructor must be a no-op,
        // or this descriptor gets closed twice and the second close lands on
        // whatever the OS handed out in between.
        CHECK(!a.valid());              // NOLINT(bugprone-use-after-move)
        CHECK(b.valid() && b.get() == fd);
        CHECK(live(fd));
    }
    CHECK(!live(fd));

    // Move-assignment closes what the target was holding first, or that one
    // leaks.
    const int first = fresh_fd(), second = fresh_fd();
    {
        owned_handle h{first};
        h = owned_handle{second};
        CHECK(!live(first));
        CHECK(live(second));
    }
    CHECK(!live(second));
    std::printf("  and assigning over one closes the old descriptor\n");
}

void release_hands_it_off() {
    std::printf("release() gives the descriptor away\n");
    const int fd = fresh_fd();
    int taken = -1;
    {
        owned_handle h{fd};
        taken = h.release();
        CHECK(!h.valid());
    }
    CHECK(taken == fd);
    CHECK(live(fd));   // the scope ended and it is STILL open: ours now
    ::close(taken);
    CHECK(!live(fd));
    std::printf("  the scope ends and the descriptor survives\n");
}

void reset_closes_early() {
    std::printf("reset() closes now, not at the end of the scope\n");
    const int fd = fresh_fd();
    owned_handle h{fd};
    h.reset();
    CHECK(!live(fd));
    CHECK(!h.valid());
    h.reset();   // idempotent: a second reset must not close anything
    std::printf("  and a second reset does nothing\n");
}

void a_borrow_never_closes() {
    std::printf("a borrow is not an owner\n");
    const int fd = fresh_fd();
    {
        owned_handle h{fd};
        borrowed_handle b = h.borrow();
        CHECK(b.valid());
        CHECK(b.get() == fd);
        // Copying a borrow is fine: none of the copies can close it. That is
        // the difference the two types are drawing.
        borrowed_handle c = b;
        CHECK(c == b);
        {
            borrowed_handle inner = b;
            (void)inner;
        }
        CHECK(live(fd));   // three borrows went out of scope, none closed it
    }
    CHECK(!live(fd));
    std::printf("  borrows come and go, the owner decides\n");
}

void the_empty_handle_is_safe() {
    std::printf("a default handle owns nothing\n");
    owned_handle h;
    CHECK(!h.valid());
    CHECK(!h);
    CHECK(!h.borrow().valid());
    h.reset();                  // must not call close(-1)
    CHECK(h.release() == jaal::platform::invalid_handle);
    borrowed_handle b;
    CHECK(!b.valid());
    std::printf("  and destroying it closes nothing\n");
}

void duplicate_makes_a_second_owner() {
    std::printf("duplicate() when a descriptor genuinely needs two owners\n");
    const int fd = fresh_fd();
    owned_handle h{fd};
    auto copy = h.duplicate();
    CHECK(copy.has_value());
    if (copy) {
        CHECK(copy->valid());
        CHECK(copy->get() != fd);      // a new number, the same open file
        // Close-on-exec, like F_DUPFD_CLOEXEC promises: a descriptor that
        // leaks into a child process is a compositor handing a client's
        // buffer to something else entirely.
        CHECK((::fcntl(copy->get(), F_GETFD) & FD_CLOEXEC) != 0);
        const int dup_fd = copy->get();
        copy->reset();
        CHECK(!live(dup_fd));
        CHECK(live(fd));               // closing the copy left the original
    }
    // Duplicating nothing is not an error, it is nothing.
    owned_handle empty;
    auto none = empty.duplicate();
    CHECK(none.has_value());
    CHECK(none && !none->valid());
    std::printf("  a separate descriptor, close-on-exec, closed separately\n");
}

void duplicating_a_bad_handle_is_an_error() {
    std::printf("a bad descriptor comes back as an error, not a crash\n");
    owned_handle h{99999};   // never opened
    auto d = h.duplicate();
    CHECK(!d.has_value());
    if (!d) CHECK(d.error().code == std::errc::bad_file_descriptor);
    (void)h.release();       // don't try to close it
    std::printf("  errc::bad_file_descriptor, as a value\n");
}

// ── the type itself ─────────────────────────────────────────────────────
//
// These are the properties the compile-fail suite backs up: what CAN be done
// is checked here, what CANNOT is in tests/compile_fail.

static_assert(!std::is_copy_constructible_v<owned_handle>);
static_assert(!std::is_copy_assignable_v<owned_handle>);
static_assert(std::is_move_constructible_v<owned_handle>);
static_assert(std::is_move_assignable_v<owned_handle>);
static_assert(std::is_nothrow_move_constructible_v<owned_handle>);

// A borrow is a value: copying it is the whole point.
static_assert(std::is_copy_constructible_v<borrowed_handle>);
static_assert(std::is_trivially_copyable_v<borrowed_handle>);

// Same size as the number it wraps, so passing one costs nothing and a
// struct full of them is a struct full of ints.
static_assert(sizeof(owned_handle) == sizeof(native_handle));
static_assert(sizeof(borrowed_handle) == sizeof(native_handle));

// Neither converts to or from the raw type on its own: adopting ownership is
// always written out.
static_assert(!std::is_convertible_v<native_handle, owned_handle>);
static_assert(!std::is_convertible_v<native_handle, borrowed_handle>);
static_assert(!std::is_convertible_v<owned_handle, native_handle>);

// Usable in constant expressions, so a handle can sit in a constexpr table.
static_assert(!borrowed_handle{}.valid());
static_assert(borrowed_handle{native_handle{3}}.valid());

}  // namespace

int main() {
    std::printf("jaal owned_handle\n\n");
    closes_once_at_scope_end();
    a_move_does_not_close();
    release_hands_it_off();
    reset_closes_early();
    a_borrow_never_closes();
    the_empty_handle_is_safe();
    duplicate_makes_a_second_owner();
    duplicating_a_bad_handle_is_an_error();
    std::printf("\n%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
