// examples/server.cpp — a TCP line-echo server as a jaal host.
//
//   $ ./server 8099
//   $ printf 'hello\nquit\n' | nc 127.0.0.1 8099
//   welcome
//   you said: hello
//   bye
//
// The companion to examples/host.cpp (a terminal host); this is the other
// shape a host takes: many handles, coming and going, with writes that
// don't always finish. It shows the three things a real socket host needs:
//
//   1. one registration per connection, kept in a map, dropped to unwatch
//   2. LINE FRAMING in the host: bytes are the host's problem, and the
//      program only ever sees whole lines. Each line is emitted on its own,
//      so the program is re-subscribed between them (docs/decisions.md D13:
//      the "^T m o" rule applies to a socket exactly as to a keyboard)
//   3. BACKPRESSURE: a write that returns EAGAIN parks the rest in a
//      per-connection buffer and calls registration::modify() to start
//      watching for writability; when the buffer drains it modifies back to
//      read-only. Without modify() this needs an unwatch + re-watch, which
//      costs two syscalls and drops any readiness in between.
//
// The program below is pure: it never touches a socket. It returns `reply`
// effects, and the host decides how (and when) the bytes go out. So the
// same program runs on headless<> in a test with no network at all.

#include <jaal/jaal.hpp>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <variant>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace std::chrono_literals;

// ── what the host reports, as subscription kinds ──────────────────────────
struct Accepted { int fd; };
struct Line     { int fd; std::string text; };
struct Closed   { int fd; };

using on_accept = jaal::router<Accepted, "on_accept">;
using on_line   = jaal::router<Line, "on_line">;
using on_close  = jaal::router<Closed, "on_close">;

// ── what the program can ask the host to do ───────────────────────────────
struct Reply { int fd; std::string text; };
using reply = jaal::pure_fx<Reply, "reply">;

// ── the program ───────────────────────────────────────────────────────────
struct Echo {
    struct Conn { long lines = 0; };
    struct Model {
        std::map<int, Conn> conns;
        long               total = 0;
    };

    struct Joined { int fd; };
    struct Got    { int fd; std::string text; };
    struct Left   { int fd; };
    struct Report {};
    struct Stop   {};
    using Msg = std::variant<Joined, Got, Left, Report, Stop>;

    using Cmd = jaal::Cmd<Msg, reply>;
    using Sub = jaal::Sub<Msg, on_accept, on_line, on_close, jaal::fx::on_signal>;

    static Cmd update(Model& m, Joined j) {
        m.conns[j.fd] = {};
        return Reply{j.fd, "welcome\n"};
    }

    static Cmd update(Model& m, Got g) {
        auto it = m.conns.find(g.fd);
        if (it == m.conns.end()) return {};      // closed while this was in flight
        ++it->second.lines;
        ++m.total;
        if (g.text == "quit") return Cmd::batch(Reply{g.fd, "bye\n"}, Cmd::send(Left{g.fd}));
        if (g.text == "flood") {                 // 1 MB: makes the socket block
            std::string big(1024 * 1024, 'x');
            big += '\n';
            return Reply{g.fd, std::move(big)};
        }
        return Reply{g.fd, "you said: " + g.text + "\n"};
    }

    static Cmd update(Model& m, Left l) { m.conns.erase(l.fd); return {}; }

    static Cmd update(const Model& m, Report) {
        std::printf("[stats] %zu connection(s), %ld line(s)\n", m.conns.size(), m.total);
        return {};
    }

    static Cmd update(Model&, Stop) { return Cmd::quit(0); }

    static Sub subscribe(const Model&) {
        return Sub::batch(
            Sub::on(on_accept{}, [](const Accepted& a) { return Msg{Joined{a.fd}}; }),
            Sub::on(on_line{},   [](const Line& l) { return Msg{Got{l.fd, l.text}}; }),
            Sub::on(on_close{},  [](const Closed& c) { return Msg{Left{c.fd}}; }),
            Sub::every(30s, Report{}),
            Sub::on_signal({jaal::sig::interrupt, jaal::sig::terminate},
                           [](jaal::sig) { return Msg{Stop{}}; }));
    }
};

// ── the host ──────────────────────────────────────────────────────────────
class server {
public:
    using event_type = std::variant<Accepted, Line, Closed>;
    using reactor    = jaal::platform::native_reactor;

    explicit server(int port) : port_(port) {}

    // 2. register handles. The listening socket is the only one at first.
    void attach(jaal::host_context<server>& cx) {
        listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_ < 0) { std::perror("socket"); cx.stop(70); return; }
        int on = 1;
        ::setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = htons(static_cast<std::uint16_t>(port_));
        if (::bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0
            || ::listen(listen_, 64) != 0) {
            std::perror("bind/listen");
            cx.stop(70);
            return;
        }
        set_nonblocking(listen_);
        auto reg = cx.watch(listen_, jaal::interest::read, kListenToken);
        if (!reg) { cx.stop(70); return; }
        listen_reg_.emplace(std::move(*reg));
        std::printf("listening on 127.0.0.1:%d\n", port_);
    }

    // 3→4. a handle is ready: accept, read lines, or flush a blocked write.
    void on_ready(jaal::host_context<server>& cx, const jaal::readiness& r) {
        if (r.token == kListenToken) { accept_one(cx); return; }

        const int fd = static_cast<int>(r.token);
        auto it = conns_.find(fd);
        if (it == conns_.end()) return;

        if (r.writable) flush(it->second);           // room again: drain the buffer
        if (r.readable) { if (!read_lines(cx, *it)) return; }   // closed: `it` is gone
        if (r.hangup || r.error) { drop(fd); cx.emit(Closed{fd}); }
    }

    // The program's effect. Writing may not finish, and that's the point.
    void handle(Reply rep) {
        auto it = conns_.find(rep.fd);
        if (it == conns_.end()) return;              // closed before we got here
        it->second.out += rep.text;
        flush(it->second);
    }

    void release() {
        conns_.clear();                              // each registration unwatches
        listen_reg_.reset();
        if (listen_ >= 0) ::close(listen_);
    }

private:
    // Host tokens are the connection's fd; the listener gets one that can't
    // collide with one.
    static constexpr std::uint64_t kListenToken = 1ULL << 40;

    struct conn {
        int                     fd = -1;
        std::string             in;                  // bytes not yet a whole line
        std::string             out;                 // bytes the socket wouldn't take
        bool                    watching_write = false;
        reactor::registration   reg;

        ~conn() { if (fd >= 0) ::close(fd); }
        conn() = default;
        conn(conn&&) = default;
        conn& operator=(conn&&) = default;
    };

    static void set_nonblocking(int fd) {
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    }

    void accept_one(jaal::host_context<server>& cx) {
        const int fd = ::accept(listen_, nullptr, nullptr);
        if (fd < 0) return;                          // spurious wakeup: fine
        set_nonblocking(fd);
        auto reg = cx.watch(fd, jaal::interest::read, static_cast<std::uint64_t>(fd));
        if (!reg) { ::close(fd); return; }
        auto& c = conns_[fd];
        c.fd    = fd;
        c.reg   = std::move(*reg);
        cx.emit(Accepted{fd});
    }

    /// Read what's there and emit whole lines. False when the connection
    /// closed (and was erased).
    bool read_lines(jaal::host_context<server>& cx, std::pair<const int, conn>& e) {
        const int fd = e.first;
        char buf[16 * 1024];
        for (;;) {
            const auto n = ::read(fd, buf, sizeof buf);
            if (n > 0) {
                e.second.in.append(buf, static_cast<std::size_t>(n));
                if (static_cast<std::size_t>(n) < sizeof buf) break;
                continue;                            // maybe more
            }
            if (n == 0) { drop(fd); cx.emit(Closed{fd}); return false; }   // peer closed
            if (errno == EINTR) continue;
            break;                                   // EAGAIN: nothing more for now
        }
        // One line at a time: the kernel re-subscribes between them, so a
        // line that changes what the program listens for takes effect
        // before the next line is routed.
        auto& in = conns_[fd].in;
        for (auto nl = in.find('\n'); nl != std::string::npos; nl = in.find('\n')) {
            std::string line = in.substr(0, nl);
            in.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            cx.emit(Line{fd, std::move(line)});
            if (!conns_.contains(fd)) return false;   // the program closed us
        }
        return true;
    }

    /// Write as much of `c.out` as the socket takes, and keep the write
    /// interest in step with whether anything is left.
    void flush(conn& c) {
        while (!c.out.empty()) {
            const auto n = ::write(c.fd, c.out.data(), c.out.size());
            if (n > 0) { c.out.erase(0, static_cast<std::size_t>(n)); continue; }
            if (n < 0 && errno == EINTR) continue;
            break;                                   // EAGAIN (or a dead socket)
        }
        const bool want_write = !c.out.empty();
        if (want_write == c.watching_write) return;   // nothing to change
        const auto what = want_write ? jaal::interest::read_write : jaal::interest::read;
        if (c.reg.modify(what)) c.watching_write = want_write;
    }

    void drop(int fd) { conns_.erase(fd); }           // unwatches and closes

    int                                  port_;
    int                                  listen_ = -1;
    std::optional<reactor::registration> listen_reg_;
    std::map<int, conn>                  conns_;
};

int main(int argc, char** argv) {
    server host(argc > 1 ? std::atoi(argv[1]) : 8099);
    return jaal::run<Echo>(host);
}
