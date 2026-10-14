// examples/host.cpp — a complete host, the one docs/hosts.md walks through.
//
// It reads stdin, turns bytes into typed Key events (the program handles
// them with on(model, Key)), adds one host effect (ring_bell), and draws a
// line whenever the model changes.
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

    // Core effects are always in; list only what this program adds. A host
    // that can't do these won't compile.
    using Cmd = jaal::Cmd<Msg, ring_bell>;

    static Cmd update(Model& m, Typed) {
        ++m.typed;
        m.bell = false;
        return {};
    }
    static Cmd update(Model& m, Ring) {
        m.bell = true;
        return Cmd::batch(Bell{}, Cmd::send(Typed{}));
    }
    static Cmd update(Model&, Quit) { return Cmd::quit(0); }

    // Input: one on() per kind of event, reading the model as it is now.
    static std::optional<Msg> on(const Model&, const Key& k) {
        if (k.ch == 'q')  return Quit{};
        if (k.ch == 'b')  return Ring{};
        if (k.ch == '\n') return std::nullopt;             // ignored
        return Typed{};
    }
    static std::optional<Msg> on(const Model&, jaal::sig s) {
        if (s == jaal::sig::interrupt) return Quit{};
        return std::nullopt;
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
