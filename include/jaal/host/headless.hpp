#pragma once
// jaal::headless — a host with no screen: for tests, and for programs that
// only talk to the world through effects.
//
// It records every non-core effect a program returns, in order, instead of
// running it. A test asserts on that list. It runs on sim_clock by default,
// so timers fire when the test says `advance(...)`, never by sleeping.
//
//   headless<Counter> h;
//   h.send(Increment{});
//   h.advance(1s);                 // fires timers, folds, re-subscribes
//   CHECK(h.model().n == 2);
//   CHECK(h.effects<beep>().size() == 1);
//
// Handles ANY effect (it just records it), so any Program runs on it: the
// HostFor check always passes. That's deliberate for tests. A production
// headless host that should reject some effects would handle only those it
// supports.

#include <any>
#include <chrono>
#include <cstddef>
#include <functional>
#include <thread>
#include <typeindex>
#include <utility>
#include <vector>

#include "../kernel/kernel.hpp"
#include "../kernel/teardown.hpp"
#include "../platform/clock.hpp"

namespace jaal {

/// Tag: construct a test host from a recovered model instead of init().
struct resume_from_t { explicit resume_from_t() = default; };
inline constexpr resume_from_t resume_from{};

/// Records effects; the part the kernel talks to.
class recorder {
public:
    template <class E>
    void handle(E e) {
        log_.push_back({std::type_index(typeid(E)), std::any(std::move(e))});
    }

    // Sources other than `every`: record start/stop so tests can see them.
    template <class Payload, class Key, class Msg>
    void start_source(const Payload&, const Key& k, Sink<Msg> s) {
        started_.push_back({std::type_index(typeid(Key)), std::any(k)});
        sinks_.push_back(std::any(std::move(s)));
    }
    template <class D, class Key>
    void stop_source(std::type_identity<D>, const Key& k) {
        stopped_.push_back({std::type_index(typeid(Key)), std::any(k)});
    }

    /// Every recorded payload of type E, in order.
    template <class E>
    [[nodiscard]] std::vector<E> all() const {
        std::vector<E> out;
        for (auto& [t, v] : log_)
            if (t == std::type_index(typeid(E))) out.push_back(std::any_cast<E>(v));
        return out;
    }

    [[nodiscard]] std::size_t count() const noexcept { return log_.size(); }
    void clear() { log_.clear(); }

    template <class Key>
    [[nodiscard]] std::vector<Key> started() const { return pick<Key>(started_); }
    template <class Key>
    [[nodiscard]] std::vector<Key> stopped() const { return pick<Key>(stopped_); }

private:
    template <class Key>
    static std::vector<Key> pick(const std::vector<std::pair<std::type_index, std::any>>& v) {
        std::vector<Key> out;
        for (auto& [t, a] : v)
            if (t == std::type_index(typeid(Key))) out.push_back(std::any_cast<Key>(a));
        return out;
    }

    std::vector<std::pair<std::type_index, std::any>> log_;
    std::vector<std::pair<std::type_index, std::any>> started_, stopped_;
    std::vector<std::any> sinks_;
};

template <Program P, class Event = kernel::no_events,
          platform::Clock C = platform::sim_clock>
class headless {
public:
    using kernel_type = kernel::kernel<P, Event, C>;
    using msg_type    = typename P::Msg;

    /// A test host is deterministic: Cmd::random draws from this unless the
    /// test sets kernel::options::random_seed itself.
    static constexpr std::uint64_t default_seed = 0x1234'5678'9ABC'DEF0ULL;

    explicit headless(kernel::options opt = {},
                      std::function<void(const msg_type&)> record = {})
        : k_(kernel_type::start(rec_, C{}, with_seed(opt), {}, std::move(record))) {}

    /// Resume from a recovered model (kernel::start_from): init() doesn't
    /// run, subscribe(model) does. For testing crash recovery:
    ///   headless<App> h2(jaal::resume_from, jaal::replay<App>(journal));
    headless(resume_from_t, typename P::Model model,
             cmd_of<P> resume_cmd = cmd_of<P>::none(), kernel::options opt = {},
             std::function<void(const msg_type&)> record = {})
        : k_(kernel_type::start_from(rec_, std::move(model), std::move(resume_cmd), C{},
                                     with_seed(opt), {}, std::move(record))) {}

    headless(const headless&)            = delete;
    headless& operator=(const headless&) = delete;

    // ── driving ─────────────────────────────────────────────────────────
    /// Deliver a message, as if it came from the outside.
    void send(msg_type m) {
        k_.dispatch(std::move(m));
        k_.step(rec_);
    }

    /// Deliver a host event through the program's subscriptions.
    void event(const Event& ev) { k_.route(ev, rec_); }

    /// Move the clock forward, firing timers in order. Timers due at
    /// different points inside `d` fire at their own times, not all at the
    /// end, so a 10 ms timer advanced by 35 ms fires 3 times.
    void advance(typename C::duration d) requires std::same_as<C, platform::sim_clock> {
        const auto end = k_.clock().now() + d;
        for (;;) {
            auto next = k_.next_deadline();
            if (!next || *next > end) break;
            if (*next > k_.clock().now()) k_.clock().set(*next);
            k_.step(rec_);
            if (k_.quitting()) return;
        }
        k_.clock().set(end);
        k_.step(rec_);
    }

    /// Step until no work is left (background messages settle). Real-time
    /// waits for tasks on the pool; bounded so a broken test can't hang.
    bool run_until_idle(std::chrono::milliseconds limit = std::chrono::seconds(5)) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        for (;;) {
            k_.step(rec_);
            if (k_.quitting()) return true;
            if (!k_.has_pending() && idle_for_a_moment()) return true;
            if (std::chrono::steady_clock::now() > deadline) return false;
        }
    }

    // ── observing ───────────────────────────────────────────────────────
    [[nodiscard]] const typename P::Model& model() const noexcept { return k_.model(); }
    [[nodiscard]] bool quit() const noexcept { return k_.quitting(); }

    template <class D>
    [[nodiscard]] auto effects() const { return rec_.template all<payload_t<D, msg_type>>(); }

    [[nodiscard]] recorder& record() noexcept { return rec_; }
    [[nodiscard]] kernel_type& kernel() noexcept { return k_; }
    [[nodiscard]] Sink<msg_type> sink() const { return k_.sink(); }

    /// Shut down and report the exit code. Goes through the same ordered
    /// teardown as a real run (kernel/teardown.hpp): a test host has no
    /// signals and the recorder has no release(), so only the kernel step
    /// does anything — but it goes through the one path, so a test can't
    /// exercise an order production never uses.
    int finish() && {
        kernel::teardown<kernel_type, recorder, kernel::no_signals> guard{
            k_, rec_, kernel::no_signals{}};
        return guard.exit_code();
    }

private:
    static kernel::options with_seed(kernel::options o) noexcept {
        if (!o.random_seed) o.random_seed = default_seed;
        return o;
    }

    bool idle_for_a_moment() {
        // Give pool workers a chance to post; if nothing shows up, idle.
        for (int i = 0; i < 20; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            k_.step(rec_);
            if (k_.has_pending()) return false;
        }
        return !k_.has_pending();
    }

    recorder    rec_;          // must be constructed before k_ (start() uses it)
    kernel_type k_;
};

}  // namespace jaal
