// examples/host.cpp — a complete host, the one docs/hosts.md walks through.
//
// It reads stdin, turns bytes into typed Key events, lets the program
// subscribe to them through a router it defines, adds one host effect
// (ring_bell), and draws a line whenever the model changes.
//
// This is the shape maya or a GUI binding takes: the program below knows
// nothing about terminals, and this file knows nothing about the program.
//
// Run it and type; 'q' quits, 'b' rings the bell, anything else counts.

#include <jaal/jaal.hpp>

#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

#if defined(_WIN32)
int main() {
    std::puts("examples/host: POSIX only (it reads stdin as a raw fd)");
    return 0;
}
#else

#  include <unistd.h>

// ── what the host produces ───────────────────────────────────────────────
struct Key {
    char ch;
};

// ── the router: how a program subscribes to those events ─────────────────
// A descriptor with an `event_type` is a router: the kernel offers it every
// matching event and the program's callback turns it into a Msg, or not.
struct on_key {
    static constexpr std::string_view name = "on_key";
    using event_type = Key;

    template <class Msg> struct type {
        std::function<std::optional<Msg>(Key)> f;
    };

    template <class F, class M>
    static auto fmap(F&& f, type<M> e) -> type<std::invoke_result_t<F, M>> {
        using B = std::invoke_result_t<F, M>;
        return {[g = std::move(e.f), f = std::forward<F>(f)](Key k) -> std::optional<B> {
            if (auto m = g(k)) return f(std::move(*m));
            return std::nullopt;
        }};
    }

    // On the loop thread, rebuilt every subscribe(): it may capture.
    template <class M>
    static std::optional<M> route(const type<M>& p, const Key& k) {
        return p.f(k);
    }

    template <class Self, class Msg> struct ctors {
        template <class F>
            requires std::is_invocable_v<F&, Key>
        [[nodiscard]] static Self on_key(F f) {
            return Self(type<Msg>{std::move(f)});
        }
    };
};

// ── a host effect: something only this host can do ───────────────────────
struct Bell {};
using ring_bell = jaal::pure_fx<Bell, "ring_bell">;

// ── the program: no terminal anywhere in it ──────────────────────────────
struct Counter {
    struct Model {
        int  typed = 0;
        bool bell  = false;
    };
    struct Typed {};
    struct Ring {};
    struct Quit {};
    using Msg = std::variant<Typed, Ring, Quit>;

    // The row says what this program uses: core effects plus ring_bell, and
    // core sources plus on_key. A host that can't do these won't compile.
    using Cmd = jaal::Cmd<Msg, jaal::row_union<jaal::core_fx, jaal::make_row<ring_bell>>>;
    using Sub = jaal::Sub<Msg, jaal::row_union<jaal::core_src,
                                               jaal::make_row<on_key, jaal::fx::on_signal>>>;

    static Model init() { return {}; }

    static std::pair<Model, Cmd> update(Model m, Msg msg) {
        if (std::holds_alternative<Quit>(msg)) return {m, Cmd::quit(0)};
        if (std::holds_alternative<Ring>(msg)) {
            m.bell = true;
            return {m, Cmd::batch(Cmd(Bell{}), Cmd::send(Msg{Typed{}}))};
        }
        ++m.typed;
        m.bell = false;
        return {m, Cmd::none()};
    }

    static Sub subscribe(const Model&) {
        return Sub::batch(
            Sub::on_key([](Key k) -> std::optional<Msg> {
                if (k.ch == 'q') return Msg{Quit{}};
                if (k.ch == 'b') return Msg{Ring{}};
                if (k.ch == '\n') return std::nullopt;     // ignored
                return Msg{Typed{}};
            }),
            Sub::on_signal({jaal::sig::interrupt}, [](jaal::sig) { return Msg{Quit{}}; }));
    }
};

// ── the host ─────────────────────────────────────────────────────────────
class tty_host {
public:
    using event_type = Key;

    static constexpr std::uint64_t kStdin = 1;

    // 2. register the handles we can wait on.
    void attach(jaal::host_context<tty_host>& cx) {
        if (auto r = cx.watch(STDIN_FILENO, jaal::interest::read, kStdin))
            reg_ = std::move(*r);
        else
            cx.stop(70);                      // no input: nothing to drive us
    }

    // 3→4. a handle is ready: read it and emit typed events.
    void on_ready(jaal::host_context<tty_host>& cx, const jaal::readiness& r) {
        if (r.token != kStdin) return;
        char buf[256];
        const auto n = ::read(STDIN_FILENO, buf, sizeof buf);
        if (n <= 0) {                          // EOF or error: we're done
            cx.stop(0);
            return;
        }
        for (std::ptrdiff_t i = 0; i < n; ++i) cx.emit(Key{buf[i]});
    }

    // 5. draw, after any step that changed the model.
    template <class K>
    void present(K& kernel) {
        const auto& m = kernel.model();
        if (m.typed == shown_) return;         // nothing new to say
        shown_ = m.typed;
        std::printf("typed %d\n", m.typed);
        std::fflush(stdout);
    }

    // Our own effect. The kernel calls handle(payload) for every effect in
    // the program's row that isn't core; without this, Counter wouldn't
    // compile against this host.
    void handle(Bell) {
        std::fputs("\a", stdout);
        std::fflush(stdout);
    }

    void release() { std::puts("bye"); }

private:
    std::optional<jaal::platform::native_reactor::registration> reg_;
    int shown_ = -1;
};

int main() {
    std::puts("type: q quits, b rings the bell");
    tty_host h;
    return jaal::run<Counter>(h);
}

#endif
