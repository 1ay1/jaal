#pragma once
// jaal::sim<P> — deterministic simulation.
//
// Runs a program with everything that's normally nondeterministic under
// the control of one seed:
//
//   * time is simulated (sim_clock), so an hour of timers runs in
//     microseconds
//   * task bodies don't go to the pool. Each is scheduled at now + a random
//     latency, and run on the loop thread when the sim reaches it. So task
//     results come back in a random but REPEATABLE order, interleaved with
//     timers and inputs
//   * tasks can be made to crash (throw) or get lost (never finish) at a
//     given rate, to test the program's fault paths
//   * ties (two things due at the same instant) are broken by the seed too
//
// Same seed, same run, bit for bit, on every platform: the RNG and its
// range reduction are jaal's own (std::uniform_int_distribution differs
// between standard libraries).
//
//   jaal::sim<Bank> s(seed);
//   s.at(0ms,  Deposit{100});
//   s.at(5ms,  Withdraw{70});
//   s.at(5ms,  Withdraw{70});
//   s.check("balance never negative", [](const Bank::Model& m) { return m.balance >= 0; });
//   auto r = s.run();
//   if (!r.ok()) puts(r.describe().c_str());      // seed, step, time, why
//
// explore() runs the same scenario over many seeds and returns the first
// that breaks an invariant, with the message log that reproduces it
// through jaal::replay.
//
// Limits, on purpose:
//   * a task body runs to completion on the loop thread. A body that blocks
//     waiting for its stop token (or for another thread) hangs the sim.
//     Tasks that do real I/O should be tested by scripting their result
//     messages with at(), not by running them.
//   * stream bodies are NOT run: they're long-lived and usually block. A
//     running stream's Sink is available as stream(key); a test feeds it.
//     When the program stops subscribing, that Sink goes dead, as in
//     production.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../core/rng.hpp"
#include "../kernel/executor.hpp"
#include "../kernel/kernel.hpp"
#include "../platform/clock.hpp"
#include "headless.hpp"

namespace jaal {

// ── the RNG ──────────────────────────────────────────────────────────────
/// The sim's randomness is jaal::rng (core/rng.hpp): one splitmix64 in the
/// project, so a sim seed and a Cmd::random seed mean the same thing. Kept
/// as a name because tests and reports talk about "the sim rng".
using sim_rng = rng;

// ── options and results ──────────────────────────────────────────────────
struct sim_options {
    using duration = platform::sim_clock::duration;

    /// Each task's result arrives this long after it's started, picked
    /// uniformly per task.
    duration task_latency_min = std::chrono::microseconds(0);
    duration task_latency_max = std::chrono::milliseconds(10);

    /// Fault injection, per task. crash: the body isn't run and the task
    /// fails as if it threw. lose: the body is never run at all and no
    /// result comes back (a hung request).
    double task_crash = 0.0;
    double task_lose  = 0.0;

    /// Stop the run after this many steps, or this much sim time. Hitting
    /// a limit isn't a failure: the report says which one stopped it.
    std::size_t max_steps = 100'000;
    duration    max_time  = std::chrono::hours(1);

    /// Passed to the kernel. fold_budget is forced to 1 so invariants are
    /// checked after every single message. An empty `faults` handler
    /// collects faults into the report instead of printing them.
    kernel::options kernel{};
};

/// Thrown into the kernel (as a task fault) by task_crash.
struct sim_crash : std::runtime_error {
    sim_crash() : std::runtime_error("jaal::sim: injected task crash") {}
};

/// Why a run ended. Exactly one reason.
enum class sim_end : std::uint8_t {
    idle,        // nothing left to do: no messages, timers or tasks
    quit,        // the program quit
    broke,       // an invariant failed
    step_limit,  // hit max_steps
    time_limit,  // hit max_time
};

template <Program P>
struct sim_report {
    using msg_type = typename P::Msg;

    std::uint64_t seed = 0;
    sim_end       end  = sim_end::idle;
    /// Set exactly when end == broke: the name of the invariant.
    std::optional<std::string> broken;
    std::optional<int>         exit;         // set exactly when end == quit
    std::size_t                steps = 0;
    platform::sim_clock::duration elapsed{};
    std::vector<msg_type>      messages;     // every folded message, in order
    std::vector<std::string>   faults;       // what each fault said

    [[nodiscard]] bool ok() const noexcept { return end != sim_end::broke; }

    [[nodiscard]] std::string describe() const {
        auto ms = std::chrono::duration<double, std::milli>(elapsed).count();
        char buf[256];
        if (broken) {
            std::snprintf(buf, sizeof buf,
                          "seed %llu broke \"%s\" at step %zu (t=%.3fms) after %zu message(s)",
                          static_cast<unsigned long long>(seed), broken->c_str(), steps, ms,
                          messages.size());
        } else {
            static constexpr const char* why[] = {"went idle", "quit", "broke",
                                                  "hit the step limit", "hit the time limit"};
            std::snprintf(buf, sizeof buf, "seed %llu %s at step %zu (t=%.3fms), %zu message(s)",
                          static_cast<unsigned long long>(seed),
                          why[static_cast<int>(end)], steps, ms, messages.size());
        }
        return buf;
    }
};

// ── the simulated world ──────────────────────────────────────────────────
namespace detail::simx {

using time_point = platform::sim_clock::time_point;

// Everything scheduled outside the kernel: task results and inputs. Ordered
// by (due, order); `order` is random, so equal-time items run in a seeded
// order rather than insertion order.
struct world {
    struct item {
        time_point            due;
        std::uint64_t         order;
        std::function<void()> run;
        bool operator>(const item& o) const noexcept {
            return due != o.due ? due > o.due : order > o.order;
        }
    };

    explicit world(std::uint64_t seed, const sim_options& o) : rng(seed), opt(o) {}

    void schedule(time_point due, std::function<void()> f) {
        q.push_back(item{due, rng.next(), std::move(f)});
        std::push_heap(q.begin(), q.end(), std::greater<>{});
    }

    [[nodiscard]] std::optional<time_point> next_due() const {
        if (q.empty()) return std::nullopt;
        return q.front().due;
    }

    void run_one() {
        std::pop_heap(q.begin(), q.end(), std::greater<>{});
        auto f = std::move(q.back().run);
        q.pop_back();
        f();                     // may schedule more; q is consistent here
    }

    sim_rng     rng;
    sim_options opt;
    time_point  now{};
    bool        stopped = false;
    std::vector<item> q;         // a min-heap on (due, order)
};

template <class Msg>
class executor final : public kernel::executor<Msg> {
    using base = kernel::executor<Msg>;
public:
    executor(world& w, typename base::error_fn e,
             std::map<std::string, Sink<Msg>, std::less<>>& streams)
        : w_(w), on_error_(std::move(e)), streams_(streams) {}

    void post(typename base::job j, fx::placement) override {
        if (w_.stopped) return;
        auto& o = w_.opt;
        // Draw in a fixed order so a seed means the same thing whatever the
        // options are.
        const bool crash = w_.rng.chance(o.task_crash);
        const bool lose  = w_.rng.chance(o.task_lose);
        const auto span  = static_cast<std::uint64_t>(
            std::max<std::int64_t>(0, (o.task_latency_max - o.task_latency_min).count()));
        const auto delay = o.task_latency_min + sim_options::duration(w_.rng.below(span + 1));
        if (lose) return;
        w_.schedule(w_.now + delay, [this, crash, j = std::move(j)]() mutable {
            if (w_.stopped) return;
            if (crash) { on_error_(std::make_exception_ptr(sim_crash{})); return; }
            try { j(stop_.get_token()); }
            catch (...) { on_error_(std::current_exception()); }
        });
    }

    void post_stream(std::string_view key, Sink<Msg> out, typename base::stream_job) override {
        if (w_.stopped) return;
        streams_.insert_or_assign(std::string(key), std::move(out));
    }

    std::size_t shutdown(std::chrono::milliseconds) override {
        w_.stopped = true;
        stop_.request_stop();
        return 0;
    }

private:
    world&                                          w_;
    typename base::error_fn                         on_error_;
    std::map<std::string, Sink<Msg>, std::less<>>&  streams_;
    std::stop_source                                stop_;
};

// The host: records effects like headless, and supplies the sim executor.
//
// `Event` is declared, never produced: the sim drives a program through
// MESSAGES, not input. It has to be nameable anyway, because the kernel
// type-checks a program's routers against the host's event_type — so a
// program whose subscribe() routes key events (any TUI) would otherwise
// fail to instantiate here with "routes events this host doesn't produce".
template <class Msg, class Event = kernel::no_events>
struct host : recorder {
    using event_type = Event;

    host(world& w) : w_(w) {}

    template <class M>
    std::unique_ptr<kernel::executor<M>> make_executor(typename kernel::executor<M>::error_fn e) {
        static_assert(std::same_as<M, Msg>);
        return std::make_unique<executor<M>>(w_, std::move(e), streams);
    }

    world& w_;
    std::map<std::string, Sink<Msg>, std::less<>> streams;
};

}  // namespace detail::simx

// ── sim ──────────────────────────────────────────────────────────────
//
// `Event` names the event type the program's routers expect. The sim never
// produces one — it drives the program with messages — but the kernel
// checks routers against the host's event_type, so a TUI program (whose
// subscribe() routes keys) has to say what those are:
//
//     jaal::sim<App, maya::terminal_events> s{seed};
//
// A program with no routers leaves it at the default.
template <Program P, class Event = kernel::no_events>
    requires std::copy_constructible<typename P::Msg>
class sim {
public:
    using model_type  = typename P::Model;
    using msg_type    = typename P::Msg;
    using duration    = platform::sim_clock::duration;
    using kernel_type = kernel::kernel<P, Event, platform::sim_clock>;
    using report_type = sim_report<P>;

    explicit sim(std::uint64_t seed, sim_options opt = {})
        : seed_(seed),
          world_(seed, opt),
          host_(world_),
          k_(kernel_type::start(host_, platform::sim_clock{}, kernel_opts(opt), {},
                                [this](const msg_type& m) { report_.messages.push_back(m); })) {
        report_.seed = seed;
    }

    sim(const sim&)            = delete;
    sim& operator=(const sim&) = delete;

    // ── the scenario ────────────────────────────────────────────────────
    /// Deliver `m` at sim time `t` (from the start of the run).
    void at(duration t, msg_type m) {
        world_.schedule(platform::sim_clock::time_point{} + t,
                        [this, m = std::move(m)]() mutable { k_.dispatch(std::move(m)); });
    }

    /// Deliver `m` at a random time in [from, to]. Drawn now, from the seed.
    void around(duration from, duration to, msg_type m) {
        const auto span = static_cast<std::uint64_t>(std::max<std::int64_t>(0, (to - from).count()));
        at(from + duration(world_.rng.below(span + 1)), std::move(m));
    }

    /// An invariant, checked on the initial model and after every message.
    template <class F>
        requires std::is_invocable_r_v<bool, F&, const model_type&>
    void check(std::string name, F f) {
        checks_.push_back({std::move(name), std::move(f)});
    }

    /// The sink of the running stream with this key; closed if there is no
    /// such stream (never started, or the program stopped subscribing).
    [[nodiscard]] Sink<msg_type> stream(std::string_view key) const {
        auto it = host_.streams.find(key);
        return it == host_.streams.end() ? Sink<msg_type>{} : it->second;
    }

    /// The seed's RNG, for scenarios that want their own randomness and
    /// still want to be reproducible.
    [[nodiscard]] sim_rng& rng() noexcept { return world_.rng; }

    // ── running ─────────────────────────────────────────────────────────
    /// Run to the end: idle, quit, a broken invariant, or a limit.
    report_type run() {
        const auto t0 = world_.now;
        if (!check_all()) return finish_report(sim_end::broke, t0);
        for (;;) {
            if (report_.steps >= world_.opt.max_steps) return finish_report(sim_end::step_limit, t0);

            k_.clock().set(world_.now);
            auto t = k_.step(host_);
            ++report_.steps;
            if (!check_all()) return finish_report(sim_end::broke, t0);
            if (t.exit) { report_.exit = t.exit; return finish_report(sim_end::quit, t0); }
            if (k_.has_pending()) continue;                // more to fold at this instant

            // What's next: a kernel timer or a world item (task result,
            // input). An exact tie goes to a coin flip from the seed.
            const auto kd = k_.next_deadline();
            const auto wd = world_.next_due();
            if (!kd && !wd) return finish_report(sim_end::idle, t0);
            const bool world_first =
                wd && (!kd || *wd < *kd || (*wd == *kd && (world_.rng.next() & 1)));
            const auto next = world_first ? *wd : *kd;
            if (next - t0 > world_.opt.max_time) return finish_report(sim_end::time_limit, t0);
            if (next > world_.now) world_.now = next;
            if (world_first) {
                k_.clock().set(world_.now);
                world_.run_one();
            }
        }
    }

    [[nodiscard]] const model_type& model() const noexcept { return k_.model(); }
    [[nodiscard]] recorder& record() noexcept { return host_; }
    [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }

private:
    struct named_check {
        std::string                             name;
        std::function<bool(const model_type&)>  f;
    };

    kernel::options kernel_opts(const sim_options& o) {
        kernel::options k = o.kernel;
        k.fold_budget = 1;
        // Cmd::random draws from the sim's seed too, unless the test fixed
        // one: ONE seed controls the whole run, which is the sim's promise.
        // Split rather than reused, so the program's draws don't consume the
        // scheduler's and vice versa.
        if (!k.random_seed) k.random_seed = jaal::rng{seed_}.split().state();
        if (!k.faults)
            k.faults = [this](const fault& f) {
                report_.faults.push_back(std::string(to_string(f.site)) + ": " + f.what);
            };
        return k;
    }

    bool check_all() {
        for (auto& c : checks_) {
            bool good = false;
            try { good = c.f(k_.model()); } catch (...) {}
            if (!good) { report_.broken = c.name; return false; }
        }
        return true;
    }

    report_type finish_report(sim_end why, platform::sim_clock::time_point t0) {
        report_.end     = why;
        report_.elapsed = world_.now - t0;
        return report_;
    }

    std::uint64_t              seed_;
    report_type                report_;      // before k_: init's messages are recorded into it
    detail::simx::world        world_;
    detail::simx::host<msg_type, Event> host_;
    std::vector<named_check>   checks_;
    kernel_type                k_;           // last: its executor refers to world_ and host_
};

// ── explore ──────────────────────────────────────────────────────────────
template <Program P>
struct explore_report {
    std::size_t runs = 0;                         // seeds tried
    std::optional<sim_report<P>> failure;         // the first broken run, if any
    [[nodiscard]] bool ok() const noexcept { return !failure; }
};

/// Run `scenario` (which sets up inputs and checks on a fresh sim) once per
/// seed in [first, first + count). Stops at the first broken invariant.
///
/// `Event` matches sim's: name the program's router event type, or leave it
/// for a program with no routers.
template <Program P, class Event = kernel::no_events, class Scenario>
    requires std::invocable<Scenario&, sim<P, Event>&>
explore_report<P> explore(std::uint64_t first, std::size_t count, sim_options opt,
                          Scenario scenario) {
    explore_report<P> out;
    for (std::size_t i = 0; i < count; ++i) {
        sim<P, Event> s(first + i, opt);
        scenario(s);
        auto r = s.run();
        ++out.runs;
        if (!r.ok()) { out.failure = std::move(r); break; }
    }
    return out;
}

}  // namespace jaal
