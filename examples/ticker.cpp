// examples/ticker.cpp — a complete jaal program on the real platform.
//
//   $ ./ticker
//   tick 1 ... tick 5, then quits with code 0
//   Ctrl+C at any point: prints "bye" and quits (the program subscribes)
//
// Shows: init with a Cmd, a timer subscription, a background task with its
// result coming back as a Msg, a signal as a Msg, and quit.

#include <jaal/core/core_fx.hpp>
#include <jaal/kernel/run.hpp>

#include <cstdio>
#include <string>
#include <variant>

using namespace std::chrono_literals;

struct Ticker {
    struct Model {
        int         ticks = 0;
        std::string host;
    };

    struct Tick {};
    struct GotHost { std::string name; };
    struct Interrupted {};
    using Msg = std::variant<Tick, GotHost, Interrupted>;

    using Cmd = jaal::CoreCmd<Msg>;
    using Sub = jaal::Sub<Msg, jaal::row_union<jaal::core_src,
                                               jaal::make_row<jaal::fx::on_signal>>>;

    static std::pair<Model, Cmd> init() {
        // Slow work goes to a task. It owns its inputs and reports back
        // through the Sink; it can't touch the model.
        return {{}, Cmd::task([](jaal::Sink<Msg> out, std::stop_token) {
            out.send(GotHost{"localhost"});
        })};
    }

    static std::pair<Model, Cmd> update(Model m, Msg msg) {
        return std::visit(jaal::overload{
            [&](Tick) -> std::pair<Model, Cmd> {
                ++m.ticks;
                std::printf("tick %d%s%s\n", m.ticks,
                            m.host.empty() ? "" : " on ", m.host.c_str());
                return {m, m.ticks == 5 ? Cmd::quit(0) : Cmd::none()};
            },
            [&](GotHost g) -> std::pair<Model, Cmd> {
                m.host = std::move(g.name);
                return {m, Cmd::none()};
            },
            [&](Interrupted) -> std::pair<Model, Cmd> {
                std::printf("bye\n");
                return {m, Cmd::quit(0)};
            },
        }, msg);
    }

    static Sub subscribe(const Model&) {
        return Sub::batch(
            Sub::every(500ms, Tick{}),
            Sub::on_signal({jaal::sig::interrupt},
                           [](jaal::sig) { return Msg{Interrupted{}}; }));
    }
};

int main() { return jaal::run<Ticker>(); }
