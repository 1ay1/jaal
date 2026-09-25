// tests/platform/same_tty_test.cpp — two registrations on one tty.
//
// A terminal program's input (stdin) and output (stdout) are usually the
// SAME tty. The host keeps a READ registration on stdin for its lifetime and
// adds a WRITE registration on stdout only while output is backed up, then
// drops it. Dropping the write watch must leave the read watch working:
// a lost read watch is a program that ignores every key, `q` included.
//
// Found live: maya's doom_fire on jaal over ssh stopped responding to input
// after its first backed-up frame; lsof showed its kqueue down to one
// registration (the waker).

#include <jaal/jaal.hpp>

#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

using namespace std::chrono_literals;

int main() {
    int master = -1, slave = -1;
    if (::openpty(&master, &slave, nullptr, nullptr, nullptr) != 0) return 2;
    struct termios t{};
    ::tcgetattr(slave, &t); ::cfmakeraw(&t); ::tcsetattr(slave, TCSANOW, &t);
    ::fcntl(slave, F_SETFL, O_NONBLOCK);
    const int in = slave, out = ::dup(slave);

    auto r = jaal::platform::native_reactor::create();
    if (!r) return 3;
    auto rd = r->watch(in, jaal::interest::read, 1);
    if (!rd) return 4;

    for (int round = 0; round < 3; ++round) {
        {
            auto wr = r->watch(out, jaal::interest::write, 2);
            if (!wr) return 5;
            (void)r->wait(0ms);                    // it fires (a tty is writable)
        }                                          // write watch dropped here
        (void)::write(master, "q", 1);
        auto w = r->wait(500ms);
        if (!w) return 6;
        bool read_fired = false;
        for (std::uint8_t k = 0; k < w->count; ++k)
            if (w->ready[k].token == 1 && w->ready[k].readable) read_fired = true;
        if (!read_fired) {
            std::fprintf(stderr, "round %d: the READ watch on the tty is gone after "
                                 "dropping a WRITE watch on the same tty\n", round);
            return 10 + round;
        }
        char c[8]; (void)::read(in, c, sizeof c);
    }
    std::puts("same_tty_test: ok");
    return 0;
}
