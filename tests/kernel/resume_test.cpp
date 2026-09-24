// tests/kernel/resume_test.cpp — restart a program from a recovered model.
//
// The durability loop jaal supports: journal every folded message, crash,
// rebuild the model with replay (or snapshot + replay_from of the tail),
// resume with start_from. What must hold:
//   * the resumed model is exactly the model at the crash
//   * init() and its Cmd do NOT run again (they already happened)
//   * subscribe(model) DOES run: timers the model asks for are live again
//   * resume_cmd runs, once, for work a restart must redo
//   * the resumed run keeps journaling, so a second crash recovers too
//   * snapshot + tail gives the same model as the whole journal

#include <jaal/jaal.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <variant>
#include <vector>

using namespace std::chrono_literals;

#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,   \
                         __LINE__, #c);                                   \
            return 1;                                                     \
        }                                                                 \
    } while (0)

namespace {

struct Hello { int n; };
using hello = jaal::pure_fx<Hello, "hello">;

// A tiny ledger: deposits, and a heartbeat timer while the account is open.
struct Ledger {
    static inline int init_calls = 0;
    struct Model {
        long balance = 0;
        int  beats   = 0;
        bool open    = true;
    };
    struct Deposit { long amount; };
    struct Close {};
    struct Beat {};
    using Msg = std::variant<Deposit, Close, Beat>;
    using Cmd = jaal::Cmd<Msg, hello>;
    using Sub = jaal::Sub<Msg>;

    static Cmd init(Model&) {
        ++init_calls;
        return Cmd(Hello{1});
    }
    static Cmd update(Model& m, Deposit d) { m.balance += d.amount; return {}; }
    static Cmd update(Model& m, Close)     { m.open = false;        return {}; }
    static Cmd update(Model& m, Beat)      { ++m.beats;             return {}; }
    static Sub subscribe(const Model& m) {
        return m.open ? Sub::every(10ms, Beat{}) : Sub::none();
    }
};

bool same(const Ledger::Model& a, const Ledger::Model& b) {
    return a.balance == b.balance && a.beats == b.beats && a.open == b.open;
}

int resumes_where_it_crashed() {
    Ledger::init_calls = 0;
    jaal::recording<Ledger::Msg> journal;
    Ledger::Model at_crash;
    {
        jaal::headless<Ledger> h({}, journal.hook());
        h.send(Ledger::Deposit{100});
        h.advance(25ms);                               // two beats
        h.send(Ledger::Deposit{-30});
        at_crash = h.model();
        CHECK(h.effects<hello>().size() == 1);         // init's effect ran once
    }                                                  // "crash"
    CHECK(Ledger::init_calls == 1);
    CHECK(at_crash.balance == 70 && at_crash.beats == 2);

    auto recovered = jaal::replay<Ledger>(journal.messages());
    CHECK(same(recovered, at_crash));

    const int calls_before = Ledger::init_calls;
    jaal::recording<Ledger::Msg> journal2;
    jaal::headless<Ledger> h2(jaal::resume_from, recovered, Ledger::Cmd::none(), {},
                              journal2.hook());
    CHECK(Ledger::init_calls == calls_before);     // resuming never calls init()
    CHECK(h2.effects<hello>().empty());            // init's effect NOT re-run
    CHECK(same(h2.model(), at_crash));

    h2.advance(10ms);                              // subscription is live again
    CHECK(h2.model().beats == 3);
    h2.send(Ledger::Deposit{5});
    CHECK(h2.model().balance == 75);

    // Second crash: first journal + second journal rebuilds it.
    auto all = journal.messages();
    for (auto& m : journal2.messages()) all.push_back(m);
    CHECK(same(jaal::replay<Ledger>(all), h2.model()));
    return 0;
}

int a_closed_account_resumes_without_its_timer() {
    Ledger::Model m;
    m.open = false;
    jaal::headless<Ledger> h(jaal::resume_from, m);
    h.advance(100ms);
    CHECK(h.model().beats == 0);
    return 0;
}

int resume_cmd_runs_once() {
    jaal::headless<Ledger> h(jaal::resume_from, Ledger::Model{},
                             Ledger::Cmd(Hello{2}));
    auto hs = h.effects<hello>();
    CHECK(hs.size() == 1 && hs[0].n == 2);
    h.send(Ledger::Deposit{1});
    CHECK(h.effects<hello>().size() == 1);
    return 0;
}

int snapshot_plus_tail_equals_the_whole_journal() {
    std::vector<Ledger::Msg> log;
    for (long i = 1; i <= 50; ++i) log.push_back(Ledger::Deposit{i});
    log.push_back(Ledger::Beat{});

    const std::vector<Ledger::Msg> head(log.begin(), log.begin() + 20);
    const std::vector<Ledger::Msg> tail(log.begin() + 20, log.end());
    auto snapshot = jaal::replay<Ledger>(head);
    CHECK(same(jaal::replay_from<Ledger>(snapshot, tail), jaal::replay<Ledger>(log)));
    return 0;
}

}  // namespace

int main() {
    if (int r = resumes_where_it_crashed()) return r;
    if (int r = a_closed_account_resumes_without_its_timer()) return r;
    if (int r = resume_cmd_runs_once()) return r;
    if (int r = snapshot_plus_tail_equals_the_whole_journal()) return r;
    std::puts("resume_test: ok");
    return 0;
}
