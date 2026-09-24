// examples/ticker.cpp — a complete jaal program.
//
//   $ ./ticker
//   runs a pretend download on a background stream while ticking, then
//   quits. Ctrl+C at any point prints "bye" and quits.
//
// Shows: program<> aliases, update returning just a model, a timer, a keyed
// stream (cancelled the moment the program stops subscribing), a signal as
// a message, and quit.

#include <jaal/jaal.hpp>

#include <cstdio>
#include <thread>
#include <variant>

using namespace std::chrono_literals;

namespace msg {
struct Tick {};
struct Progress { int percent; };
struct Done {};
struct Interrupted {};
}  // namespace msg

struct Model {
    int  ticks    = 0;
    int  percent  = 0;
    bool fetching = true;
};

using Msg = std::variant<msg::Tick, msg::Progress, msg::Done, msg::Interrupted>;

// Core effects and sources come for free; this program adds signals.
struct Ticker : jaal::program<Model, Msg, jaal::fx_list<>, jaal::src_list<jaal::fx::on_signal>> {
    static Model init() { return {}; }

    static step update(Model m, Msg in) {
        return std::visit(jaal::overload{
            [&](msg::Tick) -> step {
                ++m.ticks;
                std::printf("tick %d  (%d%%)\n", m.ticks, m.percent);
                return m;                                    // no effects
            },
            [&](msg::Progress p) -> step { m.percent = p.percent; return m; },
            [&](msg::Done) -> step {
                std::printf("done after %d ticks\n", m.ticks);
                m.fetching = false;
                return {m, Cmd::quit(0)};
            },
            [&](msg::Interrupted) -> step {
                std::printf("bye\n");
                return {m, Cmd::quit(0)};
            },
        }, in);
    }

    static Sub subscribe(const Model& m) {
        auto always = Sub::batch(
            Sub::every(250ms, msg::Tick{}),
            Sub::on_signal({jaal::sig::interrupt}, [](jaal::sig) { return Msg{msg::Interrupted{}}; }));
        if (!m.fetching) return always;
        // A stream lives exactly as long as this subscription does. When
        // `fetching` goes false, its stop_token fires and anything it still
        // sends is dropped.
        return Sub::batch(std::move(always),
            Sub::stream("fetch", [](jaal::Sink<Msg> out, std::stop_token st, int steps) {
                for (int i = 1; i <= steps && !st.stop_requested(); ++i) {
                    std::this_thread::sleep_for(100ms);      // pretend network
                    out.send(msg::Progress{i * 100 / steps});
                }
                out.send(msg::Done{});
            }, 10));
    }
};

int main() { return jaal::run<Ticker>(); }
