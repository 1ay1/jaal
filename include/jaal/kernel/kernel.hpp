#pragma once
// jaal::kernel::kernel<P, Clock> — the Elm loop as a value.
//
// The kernel NEVER owns the thread. It's driven: by jaal::run, by maya, by
// a test, or by someone's Qt loop. It doesn't know what a frame, a
// terminal or a socket is. Hosts do.
//
//   auto k = kernel<P, Clock>::start(host, clock);     // runs init + init Cmd
//   k.route(host_event, host);                          // input → Msgs → fold
//   auto t = k.step(host);                              // bg msgs, timers, reconcile
//   k.next_deadline();                                  // when to wake next
//   std::move(k).finish();                              // ordered shutdown
//
// What it carries over from maya's run<P>, each one a test in
// tests/kernel/kernel_test.cpp:
//   1. quit stops the rest of the batch: later Msgs' effects never run
//   2. events are routed ONE AT A TIME, re-subscribing between them (the
//      "^T m o" bug: m and o routed by the pre-^T subscription)
//   3. subscribe() only runs when the model changed
//   4. a kept timer keeps its phase; two same-interval timers both fire
//   5. after() can't overflow
//   6. a Sink doesn't keep the kernel alive; sends after finish() fail
//   7. background Msgs are drained every step, wake or no wake
//
// Effects: the kernel runs core_fx itself (quit, after, task, now). Every
// other effect in the program's row goes to host.handle(effect). A host
// that can't handle one is a compile error naming the effect (HostFor).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <stop_token>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "../core/core_fx.hpp"
#include "../core/program.hpp"
#include "../core/stream.hpp"
#include "../core/sub.hpp"
#include "../platform/clock.hpp"
#include "fault.hpp"
#include "guarded.hpp"
#include "trace.hpp"
#include "loop.hpp"
#include "mailbox.hpp"
#include "executor.hpp"
#include "reconcile.hpp"
#include "timer_heap.hpp"

namespace jaal {

// ── hosts ────────────────────────────────────────────────────────────────

/// H can run effect D (a non-core effect) for message type Msg.
template <class H, class D, class Msg>
concept handles = requires(H& h, payload_t<D, Msg> e) {
    h.handle(std::move(e));
};

namespace detail::host {

template <class H, class Msg, class L> struct handles_all;
template <class H, class Msg, class... Ds>
struct handles_all<H, Msg, meta::list<Ds...>>
    : std::bool_constant<(handles<H, Ds, Msg> && ...)> {};

// First effect in L that H can't handle, for the error message.
template <class H, class Msg, class L> struct first_unhandled { using type = void; };
template <class H, class Msg, class D, class... Rest>
struct first_unhandled<H, Msg, meta::list<D, Rest...>> {
    using type = std::conditional_t<handles<H, D, Msg>,
                                    typename first_unhandled<H, Msg, meta::list<Rest...>>::type,
                                    D>;
};

// Sources other than `every` need the host to start/stop them.
template <class H, class D, class Msg>
concept runs_source = requires(H& h, const payload_t<D, Msg>& p,
                               const typename D::key_type& k, Sink<Msg> s) {
    h.start_source(p, k, s);
    h.stop_source(std::type_identity<D>{}, k);
};

template <class H, class Msg, class L> struct runs_all_sources;
template <class H, class Msg, class... Ds>
struct runs_all_sources<H, Msg, meta::list<Ds...>>
    : std::bool_constant<((!SourceDescriptor<Ds> || std::same_as<Ds, fx::every>
                           || std::same_as<Ds, fx::stream>
                           || runs_source<H, Ds, Msg>) && ...)> {};

}  // namespace detail::host

/// Can host H run program P? Every non-core effect P can return must be
/// handled by H, and every non-core source must be startable by H.
template <class H, class P>
concept HostFor =
    Program<P>
    && detail::host::handles_all<
           H, typename P::Msg,
           typename row_minus<fx_of<P>, core_fx>::effects>::value
    && detail::host::runs_all_sources<
           H, typename P::Msg, typename src_of<P>::effects>::value;

/// A readable error at the call site when HostFor fails.
template <class H, class P>
consteval void require_host_for() {
    if constexpr (Program<P> && !HostFor<H, P>) {
        using missing = typename detail::host::first_unhandled<
            H, typename P::Msg, typename row_minus<fx_of<P>, core_fx>::effects>::type;
        if constexpr (!std::is_void_v<missing>) {
#if defined(__cpp_static_assert) && __cpp_static_assert >= 202306L
            static_assert(HostFor<H, P>,
                meta::cat<256>("jaal: host '", meta::type_name<H>(),
                               "' cannot run effect '", effect_name<missing>(),
                               "' that program '", meta::type_name<P>(),
                               "' can return; add handle() for it"));
#else
            static_assert(HostFor<H, P>, "jaal: host cannot run an effect this program returns");
#endif
        } else {
            static_assert(HostFor<H, P>,
                "jaal: host cannot start a subscription source this program asks for; "
                "add start_source()/stop_source() for it");
        }
    }
}

namespace detail::host_ev {  // in jaal::, not jaal::kernel::
// Can a router over RE see events from a host whose event type is HE?
//   HE == RE                       yes: every event is one
//   HE == variant<..., RE, ...>    yes: when the variant holds an RE
template <class HE, class RE> inline constexpr bool in_variant_v = false;
template <class... Ts, class RE>
inline constexpr bool in_variant_v<std::variant<Ts...>, RE> = (std::same_as<Ts, RE> || ...);

template <class HE, class RE>
inline constexpr bool routable_v = std::same_as<HE, RE> || in_variant_v<HE, RE>;

/// The event as an RE, or null when this event is some other kind.
template <class RE, class HE>
const RE* as(const HE& ev) noexcept {
    if constexpr (std::same_as<HE, RE>) return &ev;
    else                                return std::get_if<RE>(&ev);
}
}  // namespace detail::host_ev

namespace detail::rnd {
/// A seed for a REAL run, when the caller didn't fix one. Mixes the OS
/// entropy source with the clock, so two kernels started in the same
/// millisecond still differ. Never used when options::random_seed is set:
/// tests and sim stay deterministic.
inline std::uint64_t pick_seed() {
    std::random_device d;
    std::uint64_t s = (static_cast<std::uint64_t>(d()) << 32) ^ d();
    s ^= static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return s ? s : 0x9E3779B97F4A7C15ULL;   // never 0: that means "unset"
}
}  // namespace detail::rnd

// ── kernel ───────────────────────────────────────────────────────────────
namespace kernel {

/// What one step() did, for the host to act on.
///
/// `exit` holds the exit code exactly when the program is quitting. An exit
/// code without a quit (or a quit without a code) can't be represented.
struct turn {
    bool               model_changed = false;   // a drawing host should consider a frame
    std::size_t        folded        = 0;       // messages folded this turn
    std::optional<int> exit;                    // set: stop driving, call finish()

    [[nodiscard]] bool quit() const noexcept { return exit.has_value(); }
};

struct options {
    /// Max messages folded per step, so a message storm can't starve the
    /// host's input and drawing. The rest carry over to the next step.
    std::size_t fold_budget = 4096;
    unsigned    max_workers = 0;  // 0 = max(4, hardware_concurrency)

    /// What to do when program code throws (see kernel/fault.hpp).
    fault_policy on_fault = fault_policy::stop;
    /// Told about every fault, on the loop thread. Empty = one line to
    /// stderr.
    fault_handler faults;

    /// How long finish() waits for pool workers to notice their stop token.
    /// A worker still running after this is abandoned (detached) and
    /// reported, instead of hanging shutdown forever.
    std::chrono::milliseconds shutdown_grace{2000};

    /// Mailbox bound and what to do when it's full (kernel/mailbox.hpp).
    /// Default: unbounded.
    mailbox_options mailbox{};

    /// Called on the loop thread for each fold, effect, subscribe, fault
    /// and step (kernel/trace.hpp). One branch per event when unset.
    trace_hook trace;

    /// The seed for Cmd::random. 0 (the default) means "pick one": the
    /// kernel draws from the OS so a real run differs each time, and
    /// REPORTS it through seed_used() so a run can be reproduced by passing
    /// it back. A test host (headless, sim) fixes it instead, so the same
    /// test always draws the same numbers.
    std::uint64_t random_seed = 0;
};

// Storage for the last subs_key, sized by the program. A trait rather than
// std::conditional_t, because conditional_t names BOTH branches, and
// decltype(P::subs_key(...)) doesn't exist for a program without one.
template <class P>
struct subs_key_slot { using type = std::monostate; };
template <class P>
    requires HasSubsKey<P>
struct subs_key_slot<P> {
    using type = std::optional<
        std::remove_cvref_t<decltype(P::subs_key(std::declval<const typename P::Model&>()))>>;
};

/// Proof that a shutdown step is being taken by kernel::teardown, in order.
/// Only teardown can make one, so kernel::finish() can't be called by hand:
/// the kernel must go down LAST (signals off, host.release(), then it), and
/// calling finish() directly is how that order gets got wrong. See
/// kernel/teardown.hpp and docs/decisions.md D35.
class teardown_key {
    teardown_key() = default;
    template <class K, class H, class S> friend class teardown;
};

template <class K, class H, class S> class teardown;   // kernel/teardown.hpp

/// Marker for a host with no input events (a headless server, a test).
struct no_events {};


template <Program P, class Event = no_events, platform::Clock C = platform::steady_clock>
class kernel {
public:
    using program    = P;
    using event_type = Event;
    using model_type = typename P::Model;
    using msg_type   = typename P::Msg;
    using cmd_type   = cmd_of<P>;
    using sub_row    = src_of<P>;
    using sub_type   = sub_of<P>;
    using time_point = typename C::time_point;

    // Not copyable or movable. Tasks and sources hold Sinks into this
    // kernel's mailbox, and the host drives it by reference. A kernel is
    // made once with start() and lives where it was made (start() returns
    // it by guaranteed copy elision, so `auto k = kernel::start(...)` works).
    kernel(const kernel&)            = delete;
    kernel& operator=(const kernel&) = delete;
    kernel(kernel&&)                 = delete;
    kernel& operator=(kernel&&)      = delete;

    ~kernel() { shutdown(); }

    /// The only constructor: runs init() and its Cmd. There is no
    /// "constructed but not started" state to misuse.
    ///
    /// `record`, if set, is called with every message update() folds, in
    /// fold order, whatever its source (kernel/replay.hpp). It's given here
    /// rather than set later because init's effects can produce messages
    /// right away.
    template <class H>
    [[nodiscard]] static kernel start(H& host, C clock = {}, options opt = {},
                                      std::function<void()> wake = {},
                                      std::function<void(const msg_type&)> record = {}) {
        require_host_for<H, P>();
        auto [m, c] = prog::init<P>();
        return kernel(host, std::move(m), std::move(c), std::move(clock), opt,
                      std::move(wake), std::move(record));
    }

    /// Start from a model you already have, instead of init(): the restart
    /// half of durability. Rebuild the model with replay<P>(journal) (or
    /// load a snapshot), then resume here.
    ///
    /// init() and its Cmd do NOT run: those effects already happened in the
    /// run that produced the model, and effects are outputs, never re-run
    /// (the same rule as replay). What must be live again comes back on its
    /// own: subscribe(model) runs before the first fold, so timers, streams
    /// and routers the model asks for restart. For one-shot work a resumed
    /// program must redo (reconnect, re-announce), pass `resume_cmd`.
    template <class H>
    [[nodiscard]] static kernel start_from(H& host, model_type model,
                                           cmd_type resume_cmd = cmd_type::none(),
                                           C clock = {}, options opt = {},
                                           std::function<void()> wake = {},
                                           std::function<void(const msg_type&)> record = {}) {
        require_host_for<H, P>();
        return kernel(host, std::move(model), std::move(resume_cmd), std::move(clock), opt,
                      std::move(wake), std::move(record));
    }

    // ── input ───────────────────────────────────────────────────────────
    /// Route ONE host event through the current subscriptions, then fold
    /// the result and re-subscribe before the next event is routed. That
    /// ordering is the fix for maya's "^T m o" bug.
    ///
    /// Returns how many messages the event produced, so a driver can tell
    /// "nobody subscribed to this" from "handled" (an unhandled Ctrl+C
    /// should still stop the program).
    template <class H>
    std::size_t route(const event_type& ev, H& host) {
        if (exit_) return 0;
        // A host event is its own instant: it may come long after the last
        // step (a keystroke after the program sat idle for a minute), and a
        // timer it arms must count from NOW, not from the last step's time.
        // step() resets this too; route() is the other way into the fold.
        now_cached_ = false;
        const auto before = pending_.size();
        for (auto& r : routers_) r(ev, routed_);
        for (auto& m : routed_) pending_.push_back({0, std::move(m)});
        routed_.clear();
        const auto produced = pending_.size() - before;
        // The fold happens HERE, not in the next step() — so step() will find
        // nothing pending and report model_changed = false, and a drawing
        // host would never redraw for this event. Found by driving maya's
        // terminal host in a real pty: every keystroke updated the model and
        // none of them reached the screen until an unrelated timer happened
        // to force a frame. So the change is carried to the next turn.
        if (fold_pending(host)) changed_since_turn_ = true;
        reconcile(host);
        return produced;
    }

    /// Queue a message from the loop thread itself (a host callback).
    void dispatch(msg_type m) { pending_.push_back({0, std::move(m)}); }

    /// End the program from outside update() (a signal with no handler, a
    /// host that lost its terminal). The next step() reports quit. A quit
    /// the program already asked for keeps its own exit code.
    void stop(int code) noexcept {
        if (exit_) return;
        exit_ = code;
        pending_.clear();
    }

    // ── one turn ────────────────────────────────────────────────────────
    template <class H>
    turn step(H& host) {
        turn t;
        if (exit_) { t.exit = exit_; return t; }

        // The idle fast path. Most steps have nothing to do: the loop woke
        // for a host event it already routed, or a timer that isn't due, or
        // nothing at all. Every phase below checks for its own work, but
        // reaching each check costs something. So ask ONE question first —
        // is there anything anywhere? — and leave if not.
        //
        // Each clause is a single load, and together they're exactly the
        // conditions under which a phase below would do work:
        //   pending_          messages already queued (dispatch, route)
        //   mailbox           messages from other threads (lock-free hint)
        //   task faults       errors from worker threads (lock-free flag)
        //   subs_dirty_       a model change that hasn't re-subscribed yet
        //   timers            something armed whose deadline could be now
        // Timers are the one that needs the clock, so they're checked last
        // and only when armed, which keeps a program with no timers from
        // ever reading it here.
        //
        // Tracing turns it off: a trace promises a `step` event for every
        // step() (kernel/trace.hpp), and "nothing happened" is information
        // a trace reader wants, not noise. The trace hook is already a cost
        // an instrumented run chose to pay.
        if (!opt_.trace && pending_.empty() && !inbox_.maybe_nonempty()
            && !task_faults_->pending.load(std::memory_order_acquire)
            && !subs_dirty_
            && (timers_.empty() || clock_.now() < *timers_.next_deadline())) {
            // Nothing to DO — but a route() since the last turn may have
            // changed the model, and the host must still hear about it or
            // it never redraws. Reporting it is free; skipping it isn't.
            t.model_changed = std::exchange(changed_since_turn_, false);
            return t;
        }
        now_cached_ = false;                  // a new step: time has moved
        const auto t0 = opt_.trace ? std::chrono::steady_clock::now()
                                   : std::chrono::steady_clock::time_point{};

        // 1. background messages, every turn, whether or not we were woken
        inbox_.drain_tagged(scratch_);
        for (auto& e : scratch_) pending_.push_back({e.from, std::move(e.msg)});
        drain_task_faults();
        if (exit_) { t.exit = exit_; return t; }

        // 2. timers due now
        //
        // Only read the clock when a timer could actually be due. The clock
        // is the most expensive thing on this path: steady_clock::now() is a
        // ~16 ns syscall-backed read on macOS, more than the entire fold of
        // a message, and it was being paid on EVERY step — idle ones
        // included — even by a program with no timers at all. With nothing
        // armed there's nothing to be due, so there's nothing to ask.
        // (The benchmarks run on sim_clock, whose now() costs 0.3 ns, which
        // is exactly why this hid: the real-clock idle step was 20 ns.)
        if (!timers_.empty()) {
            fired_.clear();
            timers_.collect_due(step_now(), fired_);
            for (auto& q : fired_) pending_.push_back(std::move(q));
        }

        // 3. fold, within the budget
        const auto before = folds_;
        const bool changed = fold_pending(host);
        t.folded = folds_ - before;

        // 4. re-subscribe if the model changed
        reconcile(host);

        t.model_changed = changed || std::exchange(changed_since_turn_, false);
        t.exit          = exit_;
        if (opt_.trace) {
            trace_event e{trace_kind::step};
            e.folded   = t.folded;
            e.duration = std::chrono::steady_clock::now() - t0;
            emit_trace(e);
        }
        return t;
    }

    // ── scheduling ──────────────────────────────────────────────────────
    /// When the host should wake us next. Zero wait if work is already
    /// queued; nullopt if nothing is scheduled at all.
    [[nodiscard]] std::optional<time_point> next_deadline() const {
        if (!pending_.empty()) return clock_.now();
        return timers_.next_deadline();
    }

    [[nodiscard]] bool has_pending() const noexcept { return !pending_.empty(); }

    /// A Sink into this kernel, for host code that produces messages
    /// off-thread. Weak: it can't keep the kernel alive.
    [[nodiscard]] Sink<msg_type> sink() const { return inbox_.sink(); }

    [[nodiscard]] const model_type& model() const noexcept { return model_; }
    [[nodiscard]] bool quitting() const noexcept { return exit_.has_value(); }
    [[nodiscard]] C& clock() noexcept { return clock_; }

    /// The seed Cmd::random is running from. Pass it back as
    /// options::random_seed to reproduce this run's draws. Log it on a
    /// crash and the run comes back.
    [[nodiscard]] std::uint64_t seed_used() const noexcept { return seed_used_; }

    /// The kernel's random stream, for a host that needs draws from the
    /// same reproducible sequence. Loop thread only.
    [[nodiscard]] rng& random() noexcept { return rng_; }

    /// Faults reported so far (update, subscribe, effect or task).
    [[nodiscard]] std::uint64_t fault_count() const noexcept { return faults_; }

    /// Mailbox load: drops, blocked sends, high-water mark.
    [[nodiscard]] mailbox_stats mailbox_load() const { return inbox_.stats(); }

    /// Ordered shutdown, once. Consumes the kernel.
    ///
    /// Takes a teardown_key, which only jaal::kernel::teardown can make. The
    /// kernel is the LAST thing to go down (signals off, host.release(),
    /// then this), and calling finish() by hand is how that order gets got
    /// wrong: a program that shuts down with its signal handlers still
    /// installed is unkillable by ^C for the whole grace (D34). So the order
    /// lives in teardown's destructor and this is unreachable without it:
    ///
    ///   kernel::teardown guard{k, host, std::move(sigs)};   // ... loop ...
    ///   return guard.exit_code();
    int finish(teardown_key) && {
        shutdown();
        return exit_.value_or(0);
    }

private:
    // Task faults arrive from worker threads. They're queued here and
    // reported from step(), on the loop thread, like every other fault.
    //
    // Hot path: step() runs on every wakeup and task faults are rare, so
    // checking must not take the lock. `pending` is set (release) AFTER
    // the error is queued and read (acquire) before locking; step() only
    // locks when it's set. Measured: taking the lock unconditionally was
    // ~75% of an idle step. The errors themselves stay under guarded<T>.
    // Every message waiting to be folded, with where it came from. The
    // origin rides along to the moment of folding, and fold_pending checks
    // it THERE, on the loop thread: a stream retired after its message was
    // drained (a batch [Rekey, Item] retires Item's stream at Rekey) still
    // never reaches update().
    struct queued {
        origin_id from = 0;
        msg_type  msg;
    };

    struct task_fault_box {
        guarded<std::vector<std::exception_ptr>> errors;
        std::atomic<bool>                        pending{false};
    };

    template <class H>
    kernel(H& host, model_type m, cmd_type init_cmd, C clock, options opt,
           std::function<void()> wake, std::function<void(const msg_type&)> record)
        : record_(std::move(record)),
          clock_(std::move(clock)),
          opt_(opt),
          rng_(opt.random_seed ? opt.random_seed : detail::rnd::pick_seed()),
          seed_used_(rng_.state()),
          model_(std::move(m)),
          inbox_(wake, opt.mailbox),
          task_faults_(std::make_shared<task_fault_box>()),
          pool_(make_executor(host, opt, task_faults_, wake)) {
        // `skip` promises the model survives a fault, which needs a copy
        // taken before update(). A move-only model can't be copied, so a
        // fault would lose it: that's `stop`, not `skip`. Say so instead of
        // quietly weakening the promise.
        if constexpr (!std::copy_constructible<model_type>) {
            if (opt_.on_fault == fault_policy::skip)
                report(fault{fault_site::update, nullptr,
                             "fault_policy::skip needs a copyable Model; using stop",
                             false, false});
            opt_.on_fault = fault_policy::stop;
        }
        try {
            interpret(std::move(init_cmd), host);
        } catch (...) {
            fault_raised(fault_site::effect, std::current_exception(), true);
        }
        fold_pending(host);
        reconcile(host);
    }

    // ── the fold ────────────────────────────────────────────────────────
    // Returns whether the model changed. Messages queued by effects during
    // the fold are folded too (they're appended to pending_), up to budget.
    template <class H>
    bool fold_pending(H& host) {
        bool changed = false;
        std::size_t n = 0;
        while (!pending_.empty() && !exit_ && n < opt_.fold_budget) {
            // Take the batch; anything the effects add lands in a fresh
            // pending_ and is folded next time round the loop.
            //
            // SWAP, don't move. `batch = std::move(pending_)` steals the
            // buffer, so the next dispatch() had to heap-allocate a new one:
            // one malloc + free per step. That's most of the cost of a step
            // that folds a single message — the shape every interactive app
            // has, one keystroke or one frame at a time. Swapping with a
            // scratch vector keeps both buffers alive, so a steady state
            // allocates nothing. Measured: 39 ns -> 24 ns per dispatch+step,
            // and 1.00 -> 0.00 allocations.
            batch_.clear();
            batch_.swap(pending_);
            auto& batch = batch_;
            std::size_t i = 0;
            for (; i < batch.size(); ++i) {
                if (n >= opt_.fold_budget) break;
                // A message from a subscription meets the model it's about
                // to change. If an earlier message in this batch changed the
                // model, re-subscribe FIRST, so "is this subscription still
                // wanted?" is answered for THIS model. Then drop it if its
                // subscription is gone. All on the loop thread: no
                // interleaving of stream threads can get a stopped
                // subscription's message into update(). (Plain messages
                // keep batching: they have no subscription to go stale.)
                if (batch[i].from != 0) {
                    if (subs_dirty_) reconcile(host);
                    if (!live_origins_.contains(batch[i].from)) {
                        inbox_.count_retired();
                        continue;
                    }
                }
                ++n;
                if (!fold_one(std::move(batch[i].msg), host)) {
                    // The message faulted. Under `stop` the kernel is now
                    // quitting; under `skip` it's dropped and we go on.
                    if (exit_) { ++i; break; }
                    continue;
                }
                changed = true;
                if (exit_) {
                    // Rule 1: a quit stops the batch. Nothing after it runs
                    // its effects.
                    ++i;
                    break;
                }
            }
            if (exit_) { pending_.clear(); break; }
            // Budget hit mid-batch: put the unfolded tail back, in order,
            // AHEAD of anything the effects queued.
            if (i < batch.size()) {
                std::vector<queued> rest;
                rest.reserve(batch.size() - i + pending_.size());
                for (; i < batch.size(); ++i) rest.push_back(std::move(batch[i]));
                for (auto& m : pending_) rest.push_back(std::move(m));
                pending_ = std::move(rest);
            }
        }
        return changed;
    }

    // One message through update(), with fault containment. Returns false
    // when it faulted (and was reported).
    //
    // update() changes the model in place. If it throws halfway, the model
    // is half-changed, so keeping the program's state means copying it
    // FIRST. That copy is only made when it's needed: policy `skip` with a
    // copyable model (a move-only model can't be copied, so the constructor
    // turns `skip` into `stop` for it and says so).
    template <class H>
    bool fold_one(msg_type msg, H& host) {
        std::optional<model_type> saved;
        if constexpr (std::copy_constructible<model_type>)
            if (opt_.on_fault == fault_policy::skip) saved.emplace(model_);

        cmd_type c;
        const auto t0 = opt_.trace ? std::chrono::steady_clock::now()
                                   : std::chrono::steady_clock::time_point{};
        const std::size_t index = msg.index();
        // Record BEFORE update consumes the message (kernel/replay.hpp).
        if (record_) {
            try { record_(msg); } catch (...) {}
        }
        try {
            c = prog::update<P>(model_, std::move(msg));
        } catch (...) {
            const bool kept = saved.has_value();
            if (kept) model_ = std::move(*saved);
            fault_raised(fault_site::update, std::current_exception(), kept);
            return false;
        }
        if (opt_.trace) {
            trace_event e{trace_kind::fold};
            e.msg_index = index;
            e.duration  = std::chrono::steady_clock::now() - t0;
            emit_trace(e);
        }
        subs_dirty_ = true;
        ++folds_;
        try {
            interpret(std::move(c), host);
        } catch (...) {
            // update() succeeded; the model is its new value. An effect the
            // host ran on the loop threw.
            fault_raised(fault_site::effect, std::current_exception(), true);
        }
        return true;
    }

    // The trace hook runs program-supplied code: contain it like the fault
    // handler (a throwing hook must not take the loop down).
    void emit_trace(const trace_event& e) noexcept {
        try { opt_.trace(e); } catch (...) {}
    }

    // Report a fault and apply the policy.
    void fault_raised(fault_site site, std::exception_ptr e, bool model_kept) {
        const bool stop = opt_.on_fault == fault_policy::stop || !model_kept;
        fault f{site, e, fault::describe(e), model_kept, stop};
        ++faults_;
        report(f);
        if (stop) this->stop(fault_exit_code);
    }

    void report(const fault& f) noexcept {
        if (opt_.trace) {
            trace_event e{trace_kind::fault};
            e.name = to_string(f.site);
            emit_trace(e);
        }
        try {
            if (opt_.faults) {
                opt_.faults(f);
            } else {
                std::fprintf(stderr, "jaal: fault in %.*s: %s%s\n",
                             static_cast<int>(to_string(f.site).size()),
                             to_string(f.site).data(), f.what.c_str(),
                             f.stopping ? " (stopping)" : " (message dropped)");
            }
        } catch (...) {
            // A throwing fault handler must not take the loop down.
        }
    }

    void drain_task_faults() {
        if (!task_faults_->pending.load(std::memory_order_acquire)) return;   // hot path
        task_faults_->pending.store(false, std::memory_order_relaxed);
        // Cleared BEFORE draining: an error queued between the clear and the
        // swap sets it again and is drained now or next step, never lost.
        auto errs = task_faults_->errors.with([](auto& v) { return std::exchange(v, {}); });
        // A task fault doesn't touch the model, so the model is kept. The
        // policy still applies: `stop` quits, `skip` carries on.
        for (auto& e : errs) fault_raised(fault_site::task, e, true);
    }

    // ── time ────────────────────────────────────────────────────────────
    // "Now", as seen by everything in one step. Read lazily, at most once.
    //
    // Two reasons, one about speed and one about meaning:
    //   * steady_clock::now() costs ~16 ns on macOS — more than folding a
    //     message. A step that arms 100 timers read it 100 times.
    //   * every timer armed in one step should agree on when "now" was. With
    //     a fresh read each, `after(10ms)` from two updates in the same batch
    //     got two different deadlines for what the program sees as one
    //     instant, and their order depended on how long update() took.
    // Lazy, so a step that arms nothing and has no timers never reads it.
    // Reset at the top of step(); a clock that moves mid-step (sim_clock
    // advanced by a test between steps) is picked up at the next step.
    [[nodiscard]] typename C::time_point step_now() {
        if (!now_cached_) {
            now_        = clock_.now();
            now_cached_ = true;
        }
        return now_;
    }

    // ── effects ─────────────────────────────────────────────────────────
    template <class H>
    void interpret(cmd_type c, H& host) {
        // Most updates change the model and ask for nothing, so the common
        // Cmd is None: check the index before entering a visit over every
        // effect in the row.
        if (c.is_none()) return;
        std::visit([&]<class X>(X&& x) {
            using U = std::remove_cvref_t<X>;
            if constexpr (!std::same_as<U, typename cmd_type::None>
                          && !std::same_as<U, typename cmd_type::Batch>) {
                if (opt_.trace) {
                    trace_event e{trace_kind::effect};
                    e.name = effect_name_of<U>();
                    emit_trace(e);
                }
            }
            if constexpr (std::same_as<U, typename cmd_type::None>) {
            } else if constexpr (std::same_as<U, typename cmd_type::Batch>) {
                for (auto& inner : x.cmds) {
                    if (exit_) return;
                    interpret(std::move(inner), host);
                }
            } else if constexpr (std::same_as<U, payload_t<fx::quit, msg_type>>) {
                if (!exit_) exit_ = x.code;
            } else if constexpr (std::same_as<U, payload_t<fx::send, msg_type>>) {
                // Straight onto the pending queue: folded in this step, in
                // the order the effects were returned. No timer involved.
                pending_.push_back({0, std::move(x.msg)});
            } else if constexpr (std::same_as<U, payload_t<fx::after, msg_type>>) {
                timers_.after(step_now(), x.delay, queued{0, std::move(x.msg)});
            } else if constexpr (std::same_as<U, payload_t<fx::task, msg_type>>) {
                auto job = [t = std::make_shared<detail::task_thunk<msg_type>>(std::move(x.thunk)),
                            s = inbox_.sink()](std::stop_token st) mutable {
                    std::move(*t).run(s, std::move(st));
                };
                pool_->post(std::move(job), x.where);
            } else if constexpr (std::same_as<U, payload_t<fx::now, msg_type>>) {
                // The KERNEL's clock, so tests control it. The time is
                // expressed as a steady_clock time_point: the sim clock's
                // count since its epoch, which is what tests compare.
                const auto d = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    clock_.now().time_since_epoch());
                pending_.push_back({0, x.to_msg(std::chrono::steady_clock::time_point(d))});
            } else if constexpr (std::same_as<U, payload_t<fx::random, msg_type>>) {
                // The KERNEL's stream, so a seed reproduces the whole run.
                // Synchronous, on the loop thread: the draw happens now, in
                // the order the effects were returned.
                pending_.push_back({0, x.to_msg(rng_)});
            } else {
                // A non-core effect: only reachable when the program's row
                // has one, and HostFor has already checked the host handles
                // it. Spelled through a dependent helper so a host with no
                // handle() at all (headless_host) still compiles when the
                // program never returns such an effect.
                call_handle(host, std::forward<X>(x));
            }
        }, std::move(c.inner));
    }

    // The descriptor name for an effect payload type U in this row.
    template <class U>
    static constexpr std::string_view effect_name_of() {
        std::string_view n = "?";
        [&]<class... Ds>(meta::list<Ds...>) {
            ((std::same_as<U, payload_t<Ds, msg_type>> ? (n = Ds::name, 0) : 0), ...);
        }(typename cmd_type::row_type::effects{});
        return n;
    }

    template <class H, class X>
    static void call_handle(H& host, X&& x) {
        if constexpr (requires { host.handle(std::forward<X>(x)); })
            host.handle(std::forward<X>(x));
        else
            static_assert(sizeof(X) == 0, "jaal: host has no handle() for this effect");
    }

    // ── subscriptions ───────────────────────────────────────────────────
    template <class H>
    void reconcile(H& host) {
        if (!subs_dirty_) return;             // rule 3: only after a change
        subs_dirty_ = false;

        // Rule 3, finer: after a change that can't affect subscriptions.
        //
        // A model changes far more often than its subscriptions do, and
        // re-running subscribe() means rebuilding the Sub AND diffing it.
        // Measured with one timer: 71.7 ns per message, against 24.1 with no
        // subscribe at all — the whole difference is work that finds nothing
        // to do. A program that declares subs_key (the fields subscribe()
        // reads) lets the kernel skip it when those fields haven't moved.
        if constexpr (HasSubsKey<P>) {
          if (!subs_key_distrusted_) {
            auto key = P::subs_key(model_);
            if (subs_key_ && *subs_key_ == key) {
#ifndef NDEBUG
                // Debug: prove the key was wide enough. Run subscribe()
                // anyway and check it would have changed nothing. A key
                // that leaves out a field subscribe() reads fails HERE, the
                // first time that field changes — in development, loudly —
                // rather than as a timer that silently never stops. When
                // it does fail, fall through and subscribe for real, NOW:
                // the program keeps behaving correctly while the developer
                // fixes the key, instead of carrying a stale set until some
                // unrelated change happens to re-trigger it.
                if (subs_key_covers_subscribe()) return;
                // Proven too narrow. Stop using it for the rest of the run:
                // every later change runs subscribe(), so the program stays
                // correct (just without the saving) until the key is fixed.
                subs_key_distrusted_ = true;
                subs_key_.reset();
#else
                return;
#endif
            } else {
                if (subs_key_) *subs_key_ = std::move(key);
                else           subs_key_.emplace(std::move(key));
            }
          }
        }

        // subscribe() is program code: it can throw. If it does, keep the
        // subscriptions that are running (the last good set) rather than
        // tearing them down on a bad model.
        std::optional<sub_type> widened;
        try {
            widened.emplace(prog::subscribe<P>(model_));
        } catch (...) {
            // The key describes the model we FAILED to subscribe for, so it
            // can't stand for the running set: forget it, and the next
            // change retries subscribe() instead of skipping it.
            if constexpr (HasSubsKey<P>) subs_key_.reset();
            fault_raised(fault_site::subscribe, std::current_exception(), true);
            return;
        }

        routers_.clear();
        const auto& plan = sources_.reconcile(*widened, [&]<class R>(const R& r) {
            add_router(r);
        });
        try {
            for (auto& s : plan.stop)  stop_source(s.k, host);
            for (auto& s : plan.keep)  keep_source(s.k, s.p);
            for (auto& s : plan.start) start_source(s.k, s.p, host);
        } catch (...) {
            fault_raised(fault_site::effect, std::current_exception(), true);
        }
        duplicates_ += plan.duplicates.size();
        if (opt_.trace) {
            trace_event e{trace_kind::subscribe};
            e.started = plan.start.size();
            e.stopped = plan.stop.size();
            e.kept    = plan.keep.size();
            emit_trace(e);
        }
    }

#ifndef NDEBUG
    // Debug only: subs_key said "unchanged", so subscribe() was about to be
    // skipped. Run it anyway: true if the running sources would be the same
    // (the key was wide enough). If not, report it as a subscribe fault
    // naming the rule, and return false so the caller subscribes for real.
    [[nodiscard]] bool subs_key_covers_subscribe() {
        sub_type fresh;
        try {
            fresh = prog::subscribe<P>(model_);
        } catch (...) {
            return true;                      // subscribe()'s own faults surface on the real path
        }
        if (sources_.same_sources(fresh)) return true;
        report(fault{fault_site::subscribe, nullptr,
                     "subs_key() reported no change, but subscribe() now returns "
                     "different sources: subs_key leaves out a field subscribe() "
                     "reads. Add it to subs_key (see HasSubsKey in core/program.hpp).",
                     false, false});
        return false;
    }
#endif

    template <class R>
    void add_router(const R& r) {
        // Find the router descriptor whose payload is R, and bind it.
        add_router_impl(r, typename sub_row::effects{});
    }
    template <class R, class... Ds>
    void add_router_impl(const R& r, meta::list<Ds...>) {
        ([&] {
            if constexpr (RouterDescriptor<Ds> && std::same_as<R, payload_t<Ds, msg_type>>) {
                // A router sees events of ONE type (key, mouse, signal...).
                // It fits this host when that type is the host's event type,
                // or one alternative of it when the host produces a variant
                // (a terminal: keys, mouse, resize; a GUI: clicks, keys,
                // window events). Anything else is a program/host mismatch,
                // rejected at compile time rather than silently never firing.
                using RE = typename Ds::event_type;
                static_assert(::jaal::detail::host_ev::routable_v<event_type, RE>,
                              "jaal: a subscription routes events this host doesn't "
                              "produce (its event_type is neither the host's event "
                              "type nor one of the host's variant alternatives)");
                routers_.push_back([r](const event_type& ev, std::vector<msg_type>& out) {
                    if (const RE* e = ::jaal::detail::host_ev::as<RE>(ev))
                        if (auto m = Ds::route(r, *e)) out.push_back(std::move(*m));
                });
            }
        }(), ...);
    }

    using src_key     = typename running_sources<msg_type, sub_row>::key;
    using src_payload = typename running_sources<msg_type, sub_row>::payload;

    template <class H>
    void start_source(const src_key& k, const src_payload& p, H& host) {
        std::visit([&]<class TK>(const TK& tk) {
            using D = typename key_desc<TK>::type;
            if constexpr (std::is_void_v<D>) {
                // no_source_key: can't be constructed, never reached
            } else if constexpr (std::same_as<D, fx::every>) {
                const auto& payload = std::get<payload_t<D, msg_type>>(p);
                const origin_id origin = inbox_.open_origin();
                live_origins_.insert(origin);
                const auto id = timers_.every(step_now(), payload.interval,
                                              queued{origin, payload.msg});
                timer_of_[k] = {id, origin};
            } else if constexpr (std::same_as<D, fx::stream>) {
                start_stream(k, std::get<payload_t<D, msg_type>>(p));
            } else {
                const auto& payload = std::get<payload_t<D, msg_type>>(p);
                call_start_source(host, payload, tk.key, inbox_.sink());
            }
        }, k);
    }

    template <class H, class Payload, class Key>
    static void call_start_source(H& host, const Payload& p, const Key& k, Sink<msg_type> s) {
        if constexpr (requires { host.start_source(p, k, s); })
            host.start_source(p, k, std::move(s));
        else
            static_assert(sizeof(Payload) == 0, "jaal: host has no start_source() for this source");
    }

    template <class H, class D, class Key>
    static void call_stop_source(H& host, std::type_identity<D> d, const Key& k) {
        if constexpr (requires { host.stop_source(d, k); })
            host.stop_source(d, k);
        else
            static_assert(sizeof(Key) == 0, "jaal: host has no stop_source() for this source");
    }

    void keep_source(const src_key& k, const src_payload& p) {
        std::visit([&]<class TK>(const TK&) {
            using D = typename key_desc<TK>::type;
            if constexpr (std::same_as<D, fx::every>) {
                if (auto it = timer_of_.find(k); it != timer_of_.end())
                    timers_.replace_payload(it->second.id,
                                            queued{it->second.origin,
                                                   std::get<payload_t<D, msg_type>>(p).msg});
            }
        }, k);
    }

    template <class H>
    void stop_source(const src_key& k, H& host) {
        std::visit([&]<class TK>(const TK& tk) {
            using D = typename key_desc<TK>::type;
            if constexpr (std::is_void_v<D>) {
            } else if constexpr (std::same_as<D, fx::every>) {
                if (auto it = timer_of_.find(k); it != timer_of_.end()) {
                    timers_.cancel(it->second.id);
                    live_origins_.erase(it->second.origin);
                    inbox_.retire(it->second.origin);
                    timer_of_.erase(it);
                }
            } else if constexpr (std::same_as<D, fx::stream>) {
                stop_stream(k);
            } else {
                call_stop_source(host, std::type_identity<D>{}, tk.key);
            }
        }, k);
    }

    // ── streams ──────────────────────────────────────────────────────────
    // Each running stream gets its own thread (streams are long and often
    // block; they must not starve the task pool) and its own mailbox ORIGIN.
    // Stopping the stream retires the origin, and the mailbox drops every
    // message from it at drain, including messages queued before the stop
    // (kernel/mailbox.hpp, "Origins"). Liveness is decided on the loop, so
    // a stopped stream's messages can't reach update() however the threads
    // interleave, even if the body ignores its stop token.
    struct stream_run {
        origin_id        origin;
        std::stop_source stop;
        // Keeps the origin-tagging sink alive while the run is subscribed.
        // The Sink the body holds is weak: after this goes, its sends
        // return false.
        std::shared_ptr<detail::mailbox_iface<msg_type>> tagger;
    };

    void start_stream(const src_key& k, const payload_t<fx::stream, msg_type>& p) {
        const origin_id origin = inbox_.open_origin();
        live_origins_.insert(origin);
        auto [tagger, sink] = inbox_.sink_from(origin);
        stream_run run{origin, std::stop_source{}, std::move(tagger)};
        auto own = run.stop.get_token();
        streams_[k] = std::move(run);

        // The body's stop_token must fire on EITHER the stream being dropped
        // (own) or the kernel shutting down (the pool's token). A fresh
        // source joined to both by stop_callbacks gives one token that does.
        pool_->post_stream(p.key, std::move(sink), [factory = p.body, own](
                                 Sink<msg_type> out, std::stop_token pool_stop) mutable {
            std::stop_source merged;
            std::stop_callback a(own,       [&] { merged.request_stop(); });
            std::stop_callback b(pool_stop, [&] { merged.request_stop(); });
            factory.run(std::move(out), merged.get_token());
        });
    }

    void stop_stream(const src_key& k) {
        auto it = streams_.find(k);
        if (it == streams_.end()) return;
        live_origins_.erase(it->second.origin);   // queued or batched: dropped at fold
        inbox_.retire(it->second.origin);         // still in the mailbox: dropped at drain
        it->second.stop.request_stop();
        streams_.erase(it);
    }

    template <class TK> struct key_desc { using type = void; };
    template <class D> struct key_desc<tagged_key<D>> { using type = D; };

    // ── shutdown ────────────────────────────────────────────────────────
    // Fixed order (docs/design.md 4.7):
    //   1. stop timers and routers       (nothing new becomes due)
    //   2. stop workers: request stop on every task, join pool workers
    //      (isolated threads are asked to stop, not joined)
    //   3. close the mailbox: every Sink now returns false
    void shutdown() {
        if (down_) return;
        down_ = true;
        timers_.clear();
        routers_.clear();
        timer_of_.clear();
        // Streams: retire and stop each, so nothing they send is folded.
        for (auto& [k, run] : streams_) {
            inbox_.retire(run.origin);
            run.stop.request_stop();
        }
        streams_.clear();
        live_origins_.clear();
        // Close the mailbox FIRST. A worker blocked in send() on a full
        // mailbox is released with `false`, and every later send fails at
        // once, so workers finish quickly instead of running out the grace.
        inbox_.close();
        if (pool_) {
            // Bounded: a task ignoring its stop token can't hang finish().
            abandoned_ = pool_->shutdown(opt_.shutdown_grace);
            if (abandoned_ != 0) {
                std::string what = std::to_string(abandoned_)
                    + " worker(s) still running after the shutdown grace; abandoned";
                report(fault{fault_site::task, nullptr, std::move(what), true, true});
            }
        }
    }

    using router_fn = std::function<void(const event_type&, std::vector<msg_type>&)>;

    std::function<void(const msg_type&)> record_;   // first: set before the ctor body folds
    C                   clock_;
    // The clock, read at most ONCE per step (see step_now).
    typename C::time_point now_{};
    bool                   now_cached_ = false;
    options             opt_;
    rng                 rng_;
    std::uint64_t       seed_used_ = 0;
    model_type          model_;
    inbox<msg_type>     inbox_;
    std::shared_ptr<task_fault_box> task_faults_;
    std::unique_ptr<executor<msg_type>> pool_;

    // Where background work runs (kernel/executor.hpp). A host that has
    // make_executor<Msg>(on_error) picks it (the simulation runs jobs on
    // the loop thread in a seeded order); anything else gets the real pool.
    //
    // The error callback runs on whatever thread the job ran on: it just
    // queues the error (under a lock) and wakes the loop, which reports it
    // on its own thread.
    template <class H>
    static std::unique_ptr<executor<msg_type>> make_executor(
            H& host, const options& opt, std::shared_ptr<task_fault_box> box,
            std::function<void()> wake) {
        typename executor<msg_type>::error_fn on_error =
            [box = std::move(box), wake](std::exception_ptr e) {
                box->errors.with([](auto& v, std::exception_ptr x) { v.push_back(std::move(x)); }, std::move(e));
                box->pending.store(true, std::memory_order_release);
                if (wake) wake();
            };
        if constexpr (requires { host.template make_executor<msg_type>(on_error); }) {
            if (auto e = host.template make_executor<msg_type>(on_error)) return e;
        }
        return std::make_unique<pool_executor<msg_type>>(opt.max_workers, std::move(on_error));
    }
    timer_heap<C, queued> timers_;
    running_sources<msg_type, sub_row> sources_;
    struct running_timer {
        timer_id  id;
        origin_id origin;
    };
    std::unordered_map<src_key, running_timer, detail::rec::key_hash> timer_of_;
    std::unordered_map<src_key, stream_run, detail::rec::key_hash> streams_;
    std::vector<router_fn> routers_;
    std::vector<msg_type>  routed_;              // scratch for route()
    std::vector<queued> pending_;
    std::vector<queued> batch_;                  // scratch for fold_pending, reused
    std::vector<typename inbox<msg_type>::entry> scratch_;
    // Origins of the subscriptions running right now (streams and every
    // timers). Loop-only, so the check in fold_pending has no window.
    std::unordered_set<origin_id> live_origins_;
    std::vector<queued> fired_;
    std::uint64_t       folds_      = 0;
    std::uint64_t       faults_     = 0;
    std::size_t         abandoned_  = 0;
    std::size_t         duplicates_ = 0;
    std::optional<int>  exit_;           // set = quitting, with this code
    bool                subs_dirty_ = true;
    // A route() between two steps changed the model; the next turn reports
    // it (see route()).
    bool                changed_since_turn_ = false;
    // The last subs_key, when the program declares one. Empty = "no known
    // key": the next change always runs subscribe().
    [[no_unique_address]] typename subs_key_slot<P>::type subs_key_{};
    // Debug: a subs_key once shown to be too narrow is never trusted again.
    bool                subs_key_distrusted_ = false;
    bool                down_       = false;
};

}  // namespace kernel
}  // namespace jaal
