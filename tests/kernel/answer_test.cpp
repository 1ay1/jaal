// tests/kernel/answer_test.cpp — a host effect that ANSWERS with a Msg.
//
// Some effects must run on the loop thread and produce a result: a terminal
// handing the tty to an interactive child (sudo, $EDITOR) and reporting how
// it exited. jaal keeps that a value: the host's handle() returns the Msg
// (or an optional one), and the kernel folds it in the SAME step, in the
// order the effects were returned — like send/now/random. No callback
// crosses into the host, so nothing about the effect stops being data.

#include <jaal/jaal.hpp>

#include <cstdio>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace {

struct RunChild { std::string cmd; };
using run_child = jaal::pure_fx<RunChild, "run_child">;
struct Beep {};
using beep = jaal::pure_fx<Beep, "beep">;

struct App {
    struct Model { std::vector<std::string> log; };
    struct Go {}; struct Exited { std::string cmd; int code; }; struct Marker { int n; };
    using Msg = std::variant<Go, Exited, Marker>;
    using Cmd = jaal::Cmd<Msg, run_child, beep>;

    static Cmd update(Model& m, Go) {
        m.log.push_back("go");
        // Order is the contract: send(1), the child's answer, send(2).
        return Cmd::batch(Cmd::send(Marker{1}), Cmd(RunChild{"vim"}), Cmd(Beep{}),
                          Cmd::send(Marker{2}));
    }
    static Cmd update(Model& m, Exited e) {
        m.log.push_back("exited " + e.cmd + " " + std::to_string(e.code));
        return {};
    }
    static Cmd update(Model& m, Marker k) { m.log.push_back("marker " + std::to_string(k.n)); return {}; }
};

// A host whose run_child answers, and whose beep doesn't. Both shapes
// coexist on one host.
struct host {
    using Msg = App::Msg;
    int beeps = 0;
    int children = 0;
    std::optional<Msg> handle(RunChild r) {
        ++children;
        if (r.cmd.empty()) return std::nullopt;   // an answer may be "nothing"
        return App::Exited{r.cmd, 7};
    }
    void handle(Beep) { ++beeps; }
};

static_assert(jaal::HostFor<host, App>);

int answers_in_order() {
    host h;
    auto k = jaal::kernel::kernel<App, jaal::kernel::no_events, jaal::platform::sim_clock>::start(h);
    k.dispatch(App::Go{});
    k.step(h);
    const std::vector<std::string> want{"go", "marker 1", "exited vim 7", "marker 2"};
    if (k.model().log != want) {
        for (auto& s : k.model().log) std::fprintf(stderr, "  %s\n", s.c_str());
        return 101;
    }
    if (h.children != 1 || h.beeps != 1) return 102;
    return 0;
}

}  // namespace

int main() {
    if (int r = answers_in_order()) { std::fprintf(stderr, "answers_in_order: %d\n", r); return r; }
    std::puts("answer_test: ok");
    return 0;
}
