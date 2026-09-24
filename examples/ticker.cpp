// examples/ticker.cpp — a complete jaal program.
//
//   $ ./ticker
//   runs a pretend download on a background stream while ticking, then
//   quits. Ctrl+C at any point prints "bye" and quits.
//
// Shows the whole shape of a program: one update per message, a timer, a
// keyed stream (cancelled the moment the program stops subscribing), a
// signal as a message, an effect of the program's own (`say`), and quit.

#include <jaal/jaal.hpp>

#include <cstdio>
#include <string>
#include <thread>
#include <variant>

using namespace std::chrono_literals;

// update is pure: it never prints. It returns a `say` effect instead, and
// the host prints it. That keeps the program replayable and testable
// (headless records every line it would have printed).
struct Say { std::string text; };
using say = jaal::pure_fx<Say, "say">;

struct Ticker {
    struct Model {
        int  ticks    = 0;
        int  percent  = 0;
        bool fetching = true;
    };

    struct Tick {};
    struct Progress { int percent; };
    struct Done {};
    struct Interrupted {};
    using Msg = std::variant<Tick, Progress, Done, Interrupted>;

    using Cmd = jaal::Cmd<Msg, say>;                   // core effects + say
    using Sub = jaal::Sub<Msg, jaal::fx::on_signal>;   // core sources + signals

    static Cmd update(Model& m, Tick) {
        ++m.ticks;
        return Say{"tick " + std::to_string(m.ticks) + "  (" + std::to_string(m.percent) + "%)"};
    }
    static Cmd update(Model& m, Progress p) {
        m.percent = p.percent;
        return {};
    }
    static Cmd update(Model& m, Done) {
        m.fetching = false;
        return Cmd::batch(Say{"done after " + std::to_string(m.ticks) + " ticks"}, Cmd::quit(0));
    }
    static Cmd update(Model&, Interrupted) { return Cmd::batch(Say{"bye"}, Cmd::quit(0)); }

    static Sub subscribe(const Model& m) {
        auto always = Sub::batch(
            Sub::every(250ms, Tick{}),
            Sub::on_signal({jaal::sig::interrupt}, [](jaal::sig) { return Msg{Interrupted{}}; }));
        if (!m.fetching) return always;
        // A stream lives exactly as long as this subscription does. When
        // `fetching` goes false, its stop_token fires and anything it still
        // sends is dropped.
        return Sub::batch(std::move(always),
            Sub::stream("fetch", [](jaal::Sink<Msg> out, std::stop_token st, int steps) {
                for (int i = 1; i <= steps && !st.stop_requested(); ++i) {
                    std::this_thread::sleep_for(100ms);      // pretend network
                    out.send(Progress{i * 100 / steps});
                }
                out.send(Done{});
            }, 10));
    }
};

// The host: runs `say` by printing. That's all a host has to be for a
// program with one effect of its own.
struct console {
    using event_type = jaal::kernel::no_events;
    void handle(Say s) { std::printf("%s\n", s.text.c_str()); }
};

int main() {
    console c;
    return jaal::run<Ticker>(c);
}
