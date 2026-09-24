// tests/platform/modify_test.cpp — registration::modify() against a real
// socket, which is what it exists for.
//
// The conformance suite (tests/platform/conformance.cpp) checks modify's
// CONTRACT on every backend with a pipe: the token survives, the same
// interest is idempotent, an empty registration errors, a dead reactor
// errors. This test checks the thing that motivated it, end to end: a
// socket whose send buffer is full.
//
//   1. write until the socket says EAGAIN (the buffer is full)
//   2. modify() to read_write: the reactor now reports writability
//   3. the peer reads, so there's room; write the rest
//   4. modify() back to read: writability is no longer reported, and a
//      reactor that kept reporting it would spin the loop at 100% CPU
//
// Step 4 is the one a hand-rolled "just re-watch" gets wrong, and the
// symptom is a busy loop rather than a wrong answer, so it needs a test
// that counts wakeups.

#include <jaal/platform.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace std::chrono_literals;
namespace pf = jaal::platform;

#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,   \
                         __LINE__, #c);                                   \
            return 1;                                                     \
        }                                                                 \
    } while (0)

namespace {

struct pair {
    int a = -1, b = -1;
    pair() {
        int fds[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0) {
            a = fds[0];
            b = fds[1];
            ::fcntl(a, F_SETFL, ::fcntl(a, F_GETFL) | O_NONBLOCK);
            ::fcntl(b, F_SETFL, ::fcntl(b, F_GETFL) | O_NONBLOCK);
        }
    }
    ~pair() {
        if (a >= 0) ::close(a);
        if (b >= 0) ::close(b);
    }
};

/// Fill the socket's send buffer. Returns how many bytes went in.
std::size_t fill(int fd) {
    const std::string chunk(64 * 1024, 'x');
    std::size_t total = 0;
    for (;;) {
        const auto n = ::write(fd, chunk.data(), chunk.size());
        if (n <= 0) break;                       // EAGAIN: full
        total += static_cast<std::size_t>(n);
        if (total > 64u * 1024 * 1024) break;    // a socket that never fills
    }
    return total;
}

int blocked_write_becomes_writable_then_quiet() {
    auto r = pf::native_reactor::create().value();
    pair p;
    CHECK(p.a >= 0);

    auto reg = r.watch(p.a, pf::interest::read, 42).value();

    const std::size_t queued = fill(p.a);
    CHECK(queued > 0);

    // 2. we have bytes left to send: start watching for writability.
    CHECK(reg.modify(pf::interest::read_write).has_value());

    // 3. the peer drains, so there's room and the reactor says so.
    std::vector<char> sink(queued);
    std::size_t drained = 0;
    while (drained < queued) {
        const auto n = ::read(p.b, sink.data(), sink.size());
        if (n <= 0) break;
        drained += static_cast<std::size_t>(n);
    }
    CHECK(drained > 0);

    auto w = r.wait(2s).value();
    CHECK(!w.timeout);
    CHECK(w.count == 1);
    CHECK(w.ready[0].token == 42);               // same token as watch()
    CHECK(w.ready[0].writable);

    // 4. nothing left to send: stop watching for writability. The socket is
    //    still writable, so a reactor that ignored this would report it
    //    every time round the loop.
    CHECK(reg.modify(pf::interest::read).has_value());
    auto quiet = r.wait(150ms).value();
    CHECK(quiet.timeout);                        // no readiness at all
    CHECK(quiet.count == 0);

    // and reads still work: modify changed the write interest only
    CHECK(::write(p.b, "hi\n", 3) == 3);
    auto rd = r.wait(2s).value();
    CHECK(!rd.timeout);
    CHECK(rd.count == 1);
    CHECK(rd.ready[0].token == 42);
    CHECK(rd.ready[0].readable);
    CHECK(!rd.ready[0].writable);                // not asked for, not reported
    return 0;
}

int modify_does_not_lose_pending_readability() {
    // Changing the write interest must not drop readability that's already
    // there. (Dropping the registration and re-watching can.)
    auto r = pf::native_reactor::create().value();
    pair p;
    CHECK(p.a >= 0);
    auto reg = r.watch(p.a, pf::interest::read, 7).value();
    CHECK(::write(p.b, "x", 1) == 1);            // readable NOW
    CHECK(reg.modify(pf::interest::read_write).has_value());
    auto w = r.wait(2s).value();
    CHECK(!w.timeout);
    CHECK(w.count == 1);
    CHECK(w.ready[0].token == 7);
    CHECK(w.ready[0].readable);                  // still there after modify
    return 0;
}

}  // namespace

int main() {
    if (int rc = blocked_write_becomes_writable_then_quiet()) return rc;
    if (int rc = modify_does_not_lose_pending_readability()) return rc;
    std::puts("modify_test: ok");
    return 0;
}
