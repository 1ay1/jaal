// tests/kernel/pacing_test.cpp — run_options::min_present_interval (D40).
//
// A host that records every present(), driving a program whose model changes
// every millisecond. The claims:
//
//   1. unpaced (the default), every change is a frame
//   2. paced at 20ms, a 200ms run is about 10 frames, not ~200
//   3. the LAST change still reaches the screen: the final frame draws the
//      final model, even though it came inside the pacing window
//   4. the first frame is not held back
#include <jaal/jaal.hpp>

#include <chrono>
#include <cstdio>
#include <thread>
#include <variant>
#include <vector>

using namespace std::chrono_literals;

namespace {

struct Counter {
    struct Model {
        int ticks = 0;
    };
    struct Tick {};
    struct Stop {};
    using Msg = std::variant<Tick, Stop>;
    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg>;

    // Stop after 200ms; tick every millisecond until then.
    static Cmd init(Model&) { return Cmd::after(200ms, Stop{}); }
    static Cmd update(Model& m, Tick) {
        ++m.ticks;
        return {};
    }
    static Cmd update(Model&, Stop) { return Cmd::quit(0); }
    static Sub subscribe(const Model&) { return Sub::every(1ms, Tick{}); }
};

/// Records what was drawn, and when.
struct Recorder {
    using event_type = jaal::signal_event;
    std::vector<int> drawn;  // the model each present() saw
    std::vector<std::chrono::steady_clock::time_point> at;

    template <class K>
    void present(K& k) {
        drawn.push_back(k.model().ticks);
        at.push_back(std::chrono::steady_clock::now());
    }
};

int unpaced_draws_every_change() {
    Recorder host;
    if (jaal::run<Counter>(host) != 0) return 101;
    // Every tick that changed the model was drawn (timer ticks can coalesce
    // into one step, so "about one per tick", not exactly).
    if (host.drawn.size() < 50) return 102;
    return 0;
}

int paced_draws_few_and_the_last() {
    Recorder host;
    jaal::run_options opt;
    opt.min_present_interval = 20ms;
    if (jaal::run<Counter>(host, opt) != 0) return 201;

    // ~200ms / 20ms = ~10 frames, plus the first and the last. Generous
    // bounds: a loaded CI machine stretches the gaps, never shrinks them.
    if (host.drawn.size() < 3 || host.drawn.size() > 20) {
        std::fprintf(stderr, "  paced: %zu frames\n", host.drawn.size());
        return 202;
    }
    // No two frames closer than the interval, except the very last one
    // (the frame after quit is never held back).
    for (std::size_t i = 1; i + 1 < host.at.size(); ++i)
        if (host.at[i] - host.at[i - 1] < 19ms) return 203;

    if (host.drawn.front() != 0) return 204;   // the first frame, at once
    if (host.drawn.back() < 100) return 205;   // the final count, not a stale one
    return 0;
}

/// Changes in a burst, then goes quiet: nothing else wakes the loop. If the
/// frame owed at the end of the burst were not a deadline of its own, it would
/// never be drawn, and the screen would show a model several changes old
/// until something unrelated happened.
struct Burst {
    struct Model {
        int n = 0;
    };
    struct Bump {};
    struct Stop {};
    using Msg = std::variant<Bump, Stop>;
    using Cmd = jaal::Cmd<Msg>;

    // 30 changes, one per millisecond (so each is its own step, and all
    // but the first land inside the pacing window), then nothing at all
    // until Stop at 150ms.
    static Cmd init(Model&) {
        return Cmd::batch(Cmd::after(150ms, Stop{}), Cmd::after(1ms, Bump{}));
    }
    static Cmd update(Model& m, Bump) {
        ++m.n;
        if (m.n < 30) return Cmd::after(1ms, Bump{});
        return {};
    }
    static Cmd update(Model&, Stop) { return Cmd::quit(0); }
};

/// Records the model at each present(), and the time.
struct BurstRecorder {
    using event_type = jaal::signal_event;
    std::vector<int> drawn;
    std::vector<std::chrono::steady_clock::time_point> at;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    template <class K>
    void present(K& k) {
        drawn.push_back(k.model().n);
        at.push_back(std::chrono::steady_clock::now());
    }
};

int owed_frame_is_drawn_without_another_event() {
    BurstRecorder host;
    jaal::run_options opt;
    opt.min_present_interval = 40ms;
    if (jaal::run<Burst>(host, opt) != 0) return 301;

    // The final count was on screen well BEFORE the quit at 150ms: the owed
    // frame woke the loop on its own, about one interval after the burst.
    bool shown_early = false;
    for (std::size_t i = 0; i < host.drawn.size(); ++i)
        if (host.drawn[i] == 30 && host.at[i] - host.start < 120ms) shown_early = true;
    if (!shown_early) return 302;
    return 0;
}

}  // namespace

int main() {
    std::jthread watchdog([](std::stop_token st) {
        for (int i = 0; i < 100 && !st.stop_requested(); ++i) std::this_thread::sleep_for(100ms);
        if (!st.stop_requested()) {
            std::fprintf(stderr, "pacing_test: hung\n");
            std::_Exit(2);
        }
    });
    int (*const checks[])() = {unpaced_draws_every_change, paced_draws_few_and_the_last,
                               owed_frame_is_drawn_without_another_event};
    for (auto f : checks)
        if (int r = f()) {
            std::fprintf(stderr, "pacing_test: check %d failed\n", r);
            return 1;
        }
    return 0;
}
