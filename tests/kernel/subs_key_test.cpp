// tests/kernel/subs_key_test.cpp — subscribe() runs only when what it reads
// has changed.
//
// A model changes far more often than its subscriptions. A keystroke edits
// the composer text; it doesn't open a panel or start a stream. Re-running
// subscribe() after every change rebuilds the Sub and diffs it against the
// running set, and almost always finds nothing to do. Measured with one
// timer: 75.6 ns per message, against 24.0 with no subscribe at all.
//
// A program that declares subs_key(m) — the fields subscribe() reads — lets
// the kernel skip that work when those fields haven't moved: 27.7 ns, within
// 4 ns of the no-subscribe ceiling.
//
// Skipping is only correct if the key is wide enough, so this pins both:
//   * the skip happens when it should, and doesn't when the key moves
//   * subscriptions still start and stop at the right moments
//   * a key that leaves out a field subscribe() reads is caught (debug
//     builds re-run subscribe() on every skip and report a mismatch)

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

int subscribe_calls = 0;

// A program whose subscriptions depend on ONE field (`ticking`), while its
// model has another that changes constantly (`typed`).
struct Editor {
    struct Model {
        std::string typed;       // changes every keystroke; subscribe ignores it
        bool        ticking = false;
        int         ticks   = 0;
    };
    struct Type  { char c; };
    struct Start {};
    struct Stop  {};
    struct Tick  {};
    using Msg = std::variant<Type, Start, Stop, Tick>;
    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg>;

    static Cmd update(Model& m, Type t) { m.typed += t.c; return {}; }
    static Cmd update(Model& m, Start)  { m.ticking = true;  return {}; }
    static Cmd update(Model& m, Stop)   { m.ticking = false; return {}; }
    static Cmd update(Model& m, Tick)   { ++m.ticks; return {}; }

    static Sub subscribe(const Model& m) {
        ++subscribe_calls;
        return m.ticking ? Sub::every(10ms, Tick{}) : Sub::none();
    }
    // Exactly what subscribe() reads.
    static bool subs_key(const Model& m) { return m.ticking; }
};
static_assert(jaal::HasSubsKey<Editor>);

int typing_does_not_resubscribe() {
    subscribe_calls = 0;
    jaal::headless<Editor> h;
    const int after_start = subscribe_calls;            // init's subscribe
    for (char c : std::string("hello world")) h.send(Editor::Type{c});
    CHECK(h.model().typed == "hello world");
    // 11 model changes, none touching `ticking`: subscribe() never ran again.
    // (In a debug build the kernel DOES re-run it on each skip, to check the
    // key, so the count is only exact where that check is compiled out.)
#ifdef NDEBUG
    CHECK(subscribe_calls == after_start);
#else
    (void)after_start;
#endif
    return 0;
}

int a_key_change_still_resubscribes() {
    jaal::headless<Editor> h;
    h.send(Editor::Start{});                             // key false -> true
    h.advance(35ms);
    CHECK(h.model().ticks == 3);                         // the timer really started
    h.send(Editor::Type{'x'});                           // key unchanged
    h.advance(20ms);
    CHECK(h.model().ticks == 5);                         // ...and kept running
    h.send(Editor::Stop{});                              // key true -> false
    const int at_stop = h.model().ticks;
    h.advance(100ms);
    CHECK(h.model().ticks == at_stop);                   // and really stopped
    return 0;
}

// ── a key that's too narrow ──────────────────────────────────────────────
// subscribe() reads `ticking`, but subs_key forgets it. So Start changes
// what subscribe() would return while the key says "nothing changed".
std::vector<std::string> reported;

struct Narrow {
    struct Model { bool ticking = false; int ticks = 0; };
    struct Start {};
    struct Tick  {};
    using Msg = std::variant<Start, Tick>;
    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg>;
    static Cmd update(Model& m, Start) { m.ticking = true; return {}; }
    static Cmd update(Model& m, Tick)  { ++m.ticks; return {}; }
    static Sub subscribe(const Model& m) {
        return m.ticking ? Sub::every(10ms, Tick{}) : Sub::none();
    }
    static int subs_key(const Model&) { return 0; }      // WRONG: ignores `ticking`
};

int a_too_narrow_key_is_caught_in_debug() {
#ifdef NDEBUG
    // Release trusts the key (that's the point of it). The check is a
    // development tool; nothing to assert here.
    return 0;
#else
    reported.clear();
    jaal::kernel::options opt;
    opt.faults = [](const jaal::fault& f) { reported.push_back(f.what); };
    jaal::headless<Narrow> h(opt);
    h.send(Narrow::Start{});
    // The skip is detected, reported, and the kernel re-subscribes for real
    // on the next step, so the program still behaves correctly.
    CHECK(!reported.empty());
    CHECK(reported[0].find("subs_key leaves out a field") != std::string::npos);
    h.advance(35ms);
    CHECK(h.model().ticks >= 2);                         // the timer DID start
    return 0;
#endif
}

// ── a program without subs_key is unaffected ─────────────────────────────
struct Plain {
    struct Model { int n = 0; };
    struct Inc {};
    using Msg = std::variant<Inc>;
    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg>;
    static Cmd update(Model& m, Inc) { ++m.n; return {}; }
    static Sub subscribe(const Model&) { ++subscribe_calls; return Sub::none(); }
};
static_assert(!jaal::HasSubsKey<Plain>);

int without_a_key_every_change_resubscribes() {
    subscribe_calls = 0;
    jaal::headless<Plain> h;
    const int after_start = subscribe_calls;
    for (int i = 0; i < 5; ++i) h.send(Plain::Inc{});
    CHECK(subscribe_calls == after_start + 5);           // the old rule, unchanged
    return 0;
}

}  // namespace

int main() {
    if (int r = typing_does_not_resubscribe()) return r;
    if (int r = a_key_change_still_resubscribes()) return r;
    if (int r = a_too_narrow_key_is_caught_in_debug()) return r;
    if (int r = without_a_key_every_change_resubscribes()) return r;
    std::puts("subs_key_test: ok");
    return 0;
}
