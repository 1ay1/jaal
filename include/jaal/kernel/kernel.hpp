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
// Effects: the kernel runs core_fx itself (quit, after, task,
// isolated_task). Every other effect in the program's row goes to
// host.handle(effect). A host that can't handle one is a compile error
// naming the effect (HostFor, below).

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "../core/core_fx.hpp"
#include "../core/program.hpp"
#include "../core/sub.hpp"
#include "../platform/clock.hpp"
#include "loop.hpp"
#include "mailbox.hpp"
#include "pool.hpp"
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

// ── kernel ───────────────────────────────────────────────────────────────
namespace kernel {

/// What one step() did, for the host to act on.
struct turn {
    bool model_changed = false;   // a drawing host should consider a frame
    bool quit          = false;   // stop driving; call finish()
    int  exit_code     = 0;
    std::size_t folded = 0;       // messages folded this turn
};

struct options {
    /// Max messages folded per step, so a message storm can't starve the
    /// host's input and drawing. The rest carry over to the next step.
    std::size_t fold_budget = 4096;
    unsigned    max_workers = 0;  // 0 = max(4, hardware_concurrency)
};

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
    using sub_type   = Sub<msg_type, sub_row>;
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
    template <class H>
    [[nodiscard]] static kernel start(H& host, C clock = {}, options opt = {},
                                      std::function<void()> wake = {}) {
        require_host_for<H, P>();
        auto [m, c] = run_init<P>();
        return kernel(host, std::move(m), std::move(c), std::move(clock), opt,
                      std::move(wake));
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
        if (quit_) return 0;
        const auto before = pending_.size();
        for (auto& r : routers_) r(ev, pending_);
        const auto produced = pending_.size() - before;
        fold_pending(host);
        reconcile(host);
        return produced;
    }

    /// Queue a message from the loop thread itself (a host callback).
    void dispatch(msg_type m) { pending_.push_back(std::move(m)); }

    /// End the program from outside update() (a signal with no handler, a
    /// host that lost its terminal). The next step() reports quit. A quit
    /// the program already asked for keeps its own exit code.
    void stop(int code) noexcept {
        if (quit_) return;
        quit_      = true;
        exit_code_ = code;
        pending_.clear();
    }

    // ── one turn ────────────────────────────────────────────────────────
    template <class H>
    turn step(H& host) {
        turn t;
        if (quit_) { t.quit = true; t.exit_code = exit_code_; return t; }

        // 1. background messages, every turn, whether or not we were woken
        inbox_.drain(scratch_);
        for (auto& m : scratch_) pending_.push_back(std::move(m));

        // 2. timers due now
        fired_.clear();
        timers_.collect_due(clock_.now(), fired_);
        for (auto& m : fired_) pending_.push_back(std::move(m));

        // 3. fold, within the budget
        const auto before = folds_;
        const bool changed = fold_pending(host);
        t.folded = folds_ - before;

        // 4. re-subscribe if the model changed
        reconcile(host);

        t.model_changed = changed;
        t.quit          = quit_;
        t.exit_code     = exit_code_;
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
    [[nodiscard]] bool quitting() const noexcept { return quit_; }
    [[nodiscard]] C& clock() noexcept { return clock_; }

    /// Ordered shutdown, once. Consumes the kernel.
    int finish() && {
        shutdown();
        return exit_code_;
    }

private:
    template <class H>
    kernel(H& host, model_type m, cmd_type init_cmd, C clock, options opt,
           std::function<void()> wake)
        : clock_(std::move(clock)),
          opt_(opt),
          model_(std::move(m)),
          inbox_(std::move(wake)),
          pool_(std::make_unique<pool>(opt.max_workers)) {
        interpret(std::move(init_cmd), host);
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
        while (!pending_.empty() && !quit_ && n < opt_.fold_budget) {
            // Take the batch; anything the effects add lands in a fresh
            // pending_ and is folded next time round the loop.
            auto batch = std::move(pending_);
            pending_.clear();
            std::size_t i = 0;
            for (; i < batch.size(); ++i) {
                if (n >= opt_.fold_budget) break;
                auto [m, c] = P::update(std::move(model_), std::move(batch[i]));
                model_ = std::move(m);
                changed = true;
                subs_dirty_ = true;
                ++n;
                ++folds_;
                interpret(std::move(c), host);
                if (quit_) {
                    // Rule 1: a quit stops the batch. Nothing after it runs
                    // its effects.
                    ++i;
                    break;
                }
            }
            if (quit_) { pending_.clear(); break; }
            // Budget hit mid-batch: put the unfolded tail back, in order,
            // AHEAD of anything the effects queued.
            if (i < batch.size()) {
                std::vector<msg_type> rest;
                rest.reserve(batch.size() - i + pending_.size());
                for (; i < batch.size(); ++i) rest.push_back(std::move(batch[i]));
                for (auto& m : pending_) rest.push_back(std::move(m));
                pending_ = std::move(rest);
            }
        }
        return changed;
    }

    // ── effects ─────────────────────────────────────────────────────────
    template <class H>
    void interpret(cmd_type c, H& host) {
        std::visit([&]<class X>(X&& x) {
            using U = std::remove_cvref_t<X>;
            if constexpr (std::same_as<U, typename cmd_type::None>) {
            } else if constexpr (std::same_as<U, typename cmd_type::Batch>) {
                for (auto& inner : x.cmds) {
                    if (quit_) return;
                    interpret(std::move(inner), host);
                }
            } else if constexpr (std::same_as<U, payload_t<fx::quit, msg_type>>) {
                quit_      = true;
                exit_code_ = x.code;
            } else if constexpr (std::same_as<U, payload_t<fx::after, msg_type>>) {
                timers_.after(clock_.now(), x.delay, std::move(x.msg));
            } else if constexpr (std::same_as<U, payload_t<fx::task, msg_type>>) {
                pool_->post([t = std::make_shared<detail::task_thunk<msg_type>>(std::move(x.thunk)),
                             s = inbox_.sink()](std::stop_token st) mutable {
                    std::move(*t).run(s, std::move(st));
                });
            } else if constexpr (std::same_as<U, payload_t<fx::isolated_task, msg_type>>) {
                pool_->post_isolated([t = std::make_shared<detail::task_thunk<msg_type>>(std::move(x.thunk)),
                                      s = inbox_.sink()](std::stop_token st) mutable {
                    std::move(*t).run(s, std::move(st));
                });
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
        routers_.clear();

        auto next = run_subscribe<P>(model_);
        sub_type widened = std::move(next);
        auto plan = sources_.reconcile(widened, [&]<class R>(const R& r) {
            add_router(r);
        });
        for (auto& s : plan.stop)  stop_source(s.k, host);
        for (auto& s : plan.keep)  keep_source(s.k, s.p);
        for (auto& s : plan.start) start_source(s.k, s.p, host);
        duplicates_ += plan.duplicates.size();
    }

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
                const auto id = timers_.every(clock_.now(), payload.interval, payload.msg);
                timer_of_[k] = id;
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
                    timers_.replace_payload(it->second,
                                            std::get<payload_t<D, msg_type>>(p).msg);
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
                    timers_.cancel(it->second);
                    timer_of_.erase(it);
                }
            } else {
                call_stop_source(host, std::type_identity<D>{}, tk.key);
            }
        }, k);
    }

    template <class TK> struct key_desc { using type = void; };
    template <class D> struct key_desc<tagged_key<D>> { using type = D; };

    // ── shutdown ────────────────────────────────────────────────────────
    // Fixed order (DESIGN.md 4.7):
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
        if (pool_) pool_->shutdown();
        inbox_.close();
    }

    using router_fn = std::function<void(const event_type&, std::vector<msg_type>&)>;

    C                   clock_;
    options             opt_;
    model_type          model_;
    inbox<msg_type>     inbox_;
    std::unique_ptr<pool> pool_;
    timer_heap<C, msg_type> timers_;
    running_sources<msg_type, sub_row> sources_;
    std::unordered_map<src_key, timer_id, detail::rec::key_hash> timer_of_;
    std::vector<router_fn> routers_;
    std::vector<msg_type> pending_;
    std::vector<msg_type> scratch_;
    std::vector<msg_type> fired_;
    std::uint64_t       folds_      = 0;
    std::size_t         duplicates_ = 0;
    int                 exit_code_  = 0;
    bool                quit_       = false;
    bool                subs_dirty_ = true;
    bool                down_       = false;
};

}  // namespace kernel
}  // namespace jaal
