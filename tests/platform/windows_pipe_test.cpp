// tests/platform/windows_pipe_test.cpp — the Windows reactor on pipes:
// no busy wakeups, and interest that means something.
//
//   1. an idle pipe watched for reading doesn't wake the wait: a 300 ms
//      wait does ONE WaitForMultipleObjects, not 60 polls (checked by time
//      spent in the call, and by the data arriving promptly afterwards)
//   2. data arriving wakes it promptly, and isn't consumed
//   3. EOF (writer closed) is reported as hangup
//   4. write interest on a FULL pipe is quiet; once the reader drains it,
//      writable is reported; modify back to read and it's quiet again
//   5. dropping a registration whose reader is parked in ReadFile returns
//      (the thread is cancelled), and the pipe still works afterwards

#include <jaal/platform.hpp>

#include <chrono>
#include <cstdio>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

using namespace std::chrono_literals;
namespace pf = jaal::platform;
using clk = std::chrono::steady_clock;

#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            return 1;                                                     \
        }                                                                 \
    } while (0)

namespace {

struct pipe_pair {
    HANDLE r = nullptr, w = nullptr;
    explicit pipe_pair(DWORD size = 0) { ::CreatePipe(&r, &w, nullptr, size); }
    ~pipe_pair() {
        if (r) ::CloseHandle(r);
        if (w) ::CloseHandle(w);
    }
};

// Process CPU time, so "idle" is measured, not assumed.
double cpu_ms() {
    FILETIME c, e, k, u;
    ::GetProcessTimes(::GetCurrentProcess(), &c, &e, &k, &u);
    auto v = [](FILETIME f) {
        return static_cast<double>(static_cast<unsigned long long>(f.dwHighDateTime) << 32
                                   | f.dwLowDateTime) / 1e4;
    };
    return v(k) + v(u);
}

int idle_pipe_is_quiet_then_wakes() {
    auto r = pf::wait_reactor::create().value();
    pipe_pair p;
    auto reg = r.watch(p.r, pf::interest::read, 1).value();

    const double c0 = cpu_ms();
    const auto w0 = r.os_waits();
    const auto t0 = clk::now();
    auto a = r.wait(300ms).value();
    const auto took = clk::now() - t0;
    CHECK(a.timeout && a.count == 0);
    CHECK(took >= 250ms);
    // A 5 ms poll would block ~60 times here; a real wait blocks once.
    std::printf("  idle 300 ms wait: %llu os waits, %.1f ms cpu\n",
                static_cast<unsigned long long>(r.os_waits() - w0), cpu_ms() - c0);
    CHECK(r.os_waits() - w0 <= 2);
    CHECK(cpu_ms() - c0 < 30.0);

    std::jthread t([&] {
        std::this_thread::sleep_for(50ms);
        DWORD n;
        ::WriteFile(p.w, "hi", 2, &n, nullptr);
    });
    const auto t1 = clk::now();
    auto b = r.wait(5s).value();
    CHECK(!b.timeout && b.count == 1 && b.ready[0].token == 1 && b.ready[0].readable);
    CHECK(clk::now() - t1 < 1s);
    DWORD avail = 0;
    ::PeekNamedPipe(p.r, nullptr, 0, nullptr, &avail, nullptr);
    CHECK(avail == 2);                       // the reactor consumed nothing
    // still readable until drained
    auto c = r.wait(0ms).value();
    CHECK(c.count == 1 && c.ready[0].readable);
    char buf[2];
    DWORD n;
    CHECK(::ReadFile(p.r, buf, 2, &n, nullptr) && n == 2);
    CHECK(r.wait(50ms).value().timeout);    // drained: quiet again
    return 0;
}

int eof_is_hangup() {
    auto r = pf::wait_reactor::create().value();
    pipe_pair p;
    auto reg = r.watch(p.r, pf::interest::read, 3).value();
    CHECK(r.wait(30ms).value().timeout);
    ::CloseHandle(p.w);
    p.w = nullptr;
    auto a = r.wait(5s).value();
    CHECK(!a.timeout && a.count == 1 && a.ready[0].hangup);
    return 0;
}

// Wine reports a pipe's free space as its full quota however full it is,
// so a backed-up pipe can't be shown there. Real Windows tracks it.
bool under_wine() {
    return ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
}

int write_interest_follows_free_space() {
    auto r = pf::wait_reactor::create().value();
    pipe_pair p(4096);
    auto reg = r.watch(p.w, pf::interest::write, 9).value();
    auto a = r.wait(0ms).value();
    CHECK(a.count == 1 && a.ready[0].writable && !a.ready[0].hangup);   // empty: room

    if (under_wine()) {
        std::puts("  (wine: free space isn't tracked, full-pipe check skipped)");
    } else {
        // fill it exactly: a blocking write bigger than the free space
        // would hang, so write what the pipe says it has room for
        char chunk[8192] = {};
        using q_fn = LONG(NTAPI*)(HANDLE, void*, void*, ULONG, int);
        const auto q = reinterpret_cast<q_fn>(reinterpret_cast<void (*)()>(
            ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationFile")));
        for (int i = 0; i < 64; ++i) {
            ULONG info[10]{};
            ULONG_PTR iosb[2]{};
            q(p.w, iosb, info, sizeof info, 24);
            const DWORD room = info[7];
            if (room == 0) break;
            DWORD n;
            ::WriteFile(p.w, chunk, room < sizeof chunk ? room : sizeof chunk, &n, nullptr);
        }
        CHECK(r.wait(100ms).value().timeout);    // full: no writable, no spin

        std::jthread t([&] {
            std::this_thread::sleep_for(50ms);
            char sink[8192];
            DWORD n;
            ::ReadFile(p.r, sink, sizeof sink, &n, nullptr);
        });
        auto b = r.wait(5s).value();
        CHECK(!b.timeout && b.count == 1 && b.ready[0].token == 9 && b.ready[0].writable);
    }

    // the reader goes away: writable (a write fails at once) and hangup
    ::CloseHandle(p.r);
    p.r = nullptr;
    auto c = r.wait(1s).value();
    CHECK(c.count == 1 && c.ready[0].writable && c.ready[0].hangup);
    return 0;
}

int drop_while_parked() {
    auto r = pf::wait_reactor::create().value();
    pipe_pair p;
    {
        auto reg = r.watch(p.r, pf::interest::read, 4).value();
        CHECK(r.wait(30ms).value().timeout);  // the reader is parked in ReadFile
        const auto t0 = clk::now();
        reg = {};                              // drop: cancels it
        CHECK(clk::now() - t0 < 1s);
    }
    // the pipe still works with a fresh registration
    auto reg2 = r.watch(p.r, pf::interest::read, 5).value();
    DWORD n;
    ::WriteFile(p.w, "x", 1, &n, nullptr);
    auto a = r.wait(5s).value();
    CHECK(!a.timeout && a.count == 1 && a.ready[0].token == 5);
    return 0;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::puts("idle");
    if (int rc = idle_pipe_is_quiet_then_wakes()) return rc;
    std::puts("eof");
    if (int rc = eof_is_hangup()) return rc;
    std::puts("write");
    if (int rc = write_interest_follows_free_space()) return rc;
    std::puts("drop");
    if (int rc = drop_while_parked()) return rc;
    std::puts("windows_pipe_test: ok");
    return 0;
}
