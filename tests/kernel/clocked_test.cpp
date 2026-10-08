// tests/kernel/clocked_test.cpp — a program that declares a Clock gets the
// step time as update's third argument.
//
// The properties, each one the reason the mechanism exists:
//
//   1. every message folded in ONE step sees the SAME `now`. A reducer that
//      read std::chrono itself saw a different instant per message, so two
//      deltas in one batch could disagree about whether a deadline passed.
//   2. on a sim clock, advancing by d moves update's `now` by exactly d — a
//      test controls time instead of sleeping, which it cannot do at all for
//      a reducer that reads the real clock.
//   3. a timed recording replays to the SAME model as the live run. This is
//      D23 for clocked programs: the record holds the run's inputs, and for
//      a clocked update those include the time.
//   4. given<> folds at its test clock, and advance() moves it.
//
// The program is tiny on purpose: a toast that expires 5s after it is shown,
// which is exactly the shape of most clock reads in a real reducer.

#include <jaal/jaal.hpp>

#include <chrono>
#include <cstdio>
#include <variant>
#include <vector>

using namespace std::chrono_literals;

namespace {

int failures = 0;
void ok(bool c, const char* what) {
    if (!c) { std::fprintf(stderr, "FAIL %s\n", what); ++failures; }
}

struct Toast {
    using Clock = jaal::platform::steady_clock;
    using time_point = Clock::time_point;

    struct Show {};
    struct Ping {};
    using Msg = std::variant<Show, Ping>;

    struct Model {
        time_point              shown_at{};
        time_point              expires_at{};
        bool                    visible = false;
        std::vector<time_point> seen;   // every `now` update was handed
    };
    using Cmd = jaal::Cmd<Msg>;

    static Cmd update(Model& m, Show, time_point now) {
        m.seen.push_back(now);
        m.shown_at   = now;
        m.expires_at = now + 5s;
        m.visible    = true;
        return {};
    }
    static Cmd update(Model& m, Ping, time_point now) {
        m.seen.push_back(now);
        if (m.visible && now >= m.expires_at) m.visible = false;
        return {};
    }
};

static_assert(jaal::detail::prog::clocked<Toast>);

}  // namespace

int main() {
    // 1 + 2: on the sim host, one step folds a batch at one instant, and
    // advance() moves that instant by exactly the amount asked.
    {
        jaal::headless<Toast> h;
        h.send(Toast::Msg{Toast::Show{}});
        h.send(Toast::Msg{Toast::Ping{}});
        h.send(Toast::Msg{Toast::Ping{}});
        h.run_until_idle();
        const auto& m = h.model();
        ok(m.seen.size() == 3, "1. three messages folded");
        ok(m.seen.size() == 3 && m.seen[0] == m.seen[1] && m.seen[1] == m.seen[2],
           "1. every message in one step saw the same now");
        ok(m.visible, "the toast is up right after it was shown");

        h.advance(4s);
        h.send(Toast::Msg{Toast::Ping{}});
        h.run_until_idle();
        ok(h.model().visible, "2. still up after 4s of a 5s toast");
        ok(h.model().seen.back() - h.model().seen.front() == 4s,
           "2. advance(4s) moved update's now by exactly 4s");

        h.advance(1s);
        h.send(Toast::Msg{Toast::Ping{}});
        h.run_until_idle();
        ok(!h.model().visible, "2. gone at exactly 5s, with no sleep");
    }

    // 3: a timed recording of a live run replays to the same model.
    {
        jaal::timed_recording<Toast> rec;
        jaal::headless<Toast> h;
        h.kernel().record_with_time(rec.hook());
        h.send(Toast::Msg{Toast::Show{}});
        h.run_until_idle();
        h.advance(3s);
        h.send(Toast::Msg{Toast::Ping{}});
        h.run_until_idle();
        h.advance(3s);
        h.send(Toast::Msg{Toast::Ping{}});
        h.run_until_idle();

        const auto replayed = jaal::replay<Toast>(rec.entries());
        ok(rec.entries().size() == 3, "3. all three folds were recorded with time");
        ok(replayed.visible == h.model().visible,
           "3. replay agrees with the live run on visibility");
        ok(replayed.seen == h.model().seen,
           "3. replay folded every message at the instant it was folded live");
    }

    // 4: given<> folds at its test clock.
    {
        const Toast::time_point t0{std::chrono::seconds{100}};
        jaal::given<Toast> g;
        g.at_time(t0).when(Toast::Msg{Toast::Show{}});
        ok(g.model().shown_at == t0, "4. given folds at at_time()");
        g.advance(5s).when(Toast::Msg{Toast::Ping{}});
        ok(!g.model().visible, "4. given::advance reaches the deadline exactly");
    }

    if (failures == 0) std::puts("clocked: all checks OK");
    return failures == 0 ? 0 : 1;
}
