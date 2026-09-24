#pragma once
// jaal::run<P>() — run a program for real: kernel + native reactor + signals
// + whatever the host brings.
//
//   int main() { return jaal::run<Counter>(); }                // no host input
//   int main() { tui t; return jaal::run<Editor>(t); }         // a host
//
// The loop (docs/design.md §5.2), with nothing simulated:
//
//   1. start the kernel (init + init Cmd); its mailbox wakes the reactor
//   2. let the host register its handles (a tty fd, a listening socket, an
//      X11 connection) with the reactor
//   3. wait until the next deadline, a wake, a signal, or a host handle
//   4. signals and host events go through the program's subscriptions; step
//   5. let the host draw (if it draws); repeat until quit; finish()
//
// ── the host interface ───────────────────────────────────────────────────
// Everything is optional except event_type. A host provides what it needs:
//
//   struct my_host {
//       using event_type = std::variant<KeyEvent, MouseEvent>;   // what routers see
//
//       // 2. register handles; keep the registrations, they unwatch on drop
//       void attach(jaal::host_context<my_host>& cx);
//
//       // 3→4. one of the host's handles is ready: read it and emit events
//       void on_ready(jaal::host_context<my_host>& cx, const jaal::readiness& r);
//
//       // a signal arrived. Called BEFORE it's routed to the program, for
//       // signals that are the host's business: a terminal host answers
//       // sig::resize by asking the terminal its new size and emitting a
//       // resize event, since the program can't ask the terminal itself.
//       void on_signal(jaal::host_context<my_host>& cx, jaal::sig s);
//
//       // 5. after each step that changed the model: draw
//       template <class K> void present(K& kernel);
//
//       // optional: a frame present() couldn't finish (a renderer that
//       // backed off a congested terminal). While true, present() is called
//       // again even with no model change, and wait_hint() bounds how long
//       // the loop sleeps before that retry.
//       bool owes_frame() const;
//       std::optional<std::chrono::milliseconds> wait_hint() const;
//
//       // effects/sources beyond core: handle(E), start_source/stop_source
//
//       // teardown, before the kernel finishes (restore the terminal, ...)
//       void release();
//   };
//
// The host emits events through host_context::emit(ev). They're routed one
// at a time, re-subscribing between them (the ^T m o rule), exactly as if
// the kernel had read them itself.
//
// Signals are a built-in event source: a program subscribes with
// Sub::on_signal. A program that DOESN'T subscribe to interrupt/terminate/
// hangup stops with 128 + signo (130 for Ctrl+C), so it's never left
// un-killable because it forgot to ask. And once the loop has ended, the
// handlers come OFF before shutdown runs (D34): shutdown can take seconds,
// and a second ^C during it must kill the process, not queue a signal
// nobody is reading any more. If the host's event_type is a
// variant that includes signal_event, signals are routed through it;
// otherwise signals get their own lane.

#include <chrono>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "../core/program.hpp"
#include "../core/sub.hpp"
#include "../platform/clock.hpp"
#include "../platform/select.hpp"
#include "../platform/signal.hpp"
#include "kernel.hpp"
#include "teardown.hpp"
#include "timer_heap.hpp"

namespace jaal {

using platform::sig;
using platform::signal_set;
using platform::readiness;
using platform::interest;

// ── the signal router ────────────────────────────────────────────────────
/// The event signals arrive as: which signals arrived (one per event).
struct signal_event {
    signal_set signals;
};

namespace fx {

/// Sub::on_signal(set, f): call f for each signal in `set` that arrives.
/// A router over signal_event, so it's rebuilt with every subscribe() and
/// runs on the loop thread; f may capture.
struct on_signal {
    static constexpr std::string_view name = "on_signal";
    using event_type = signal_event;

    template <class Msg> struct type {
        signal_set                              wanted;
        std::function<std::optional<Msg>(sig)>  f;
    };
    template <class F, class M>
    static auto fmap(F&& f, type<M> e) -> type<std::invoke_result_t<F, M>> {
        using B = std::invoke_result_t<F, M>;
        return {e.wanted, [g = std::move(e.f), f = std::forward<F>(f)](sig s) -> std::optional<B> {
            if (auto m = g(s)) return f(std::move(*m));
            return std::nullopt;
        }};
    }
    template <class M>
    static std::optional<M> route(const type<M>& p, const signal_event& ev) {
        for (auto s : ev.signals)
            if (p.wanted.contains(s))
                if (auto m = p.f(s)) return m;
        return std::nullopt;
    }

    template <class Self, class Msg> struct ctors {
        template <class F>
            requires std::is_invocable_v<F&, sig>
        [[nodiscard]] static Self on_signal(signal_set wanted, F f) {
            using R = std::invoke_result_t<F&, sig>;
            if constexpr (std::is_convertible_v<R, std::optional<Msg>>
                          && !std::is_convertible_v<R, Msg>)
                return Self(type<Msg>{wanted, std::move(f)});
            else
                return Self(type<Msg>{wanted, [f = std::move(f)](sig s) -> std::optional<Msg> {
                    return Msg(f(s));
                }});
        }
    };
};

}  // namespace fx

/// The subscription row every run<P> program may use on top of core_src:
/// signals. `jaal::run_src` = core_src + on_signal.
using run_src = row_union<core_src, make_row<fx::on_signal>>;

// ── host interface ───────────────────────────────────────────────────────

/// A host with no screen and no input of its own: signals only.
struct headless_host {
    using event_type = signal_event;
};

namespace detail::run {

// The event type the KERNEL sees: the host's, with signal_event folded in.
//   host event E (not a variant)      → variant<E, signal_event>
//   host event variant<A...>          → variant<A..., signal_event> (unless present)
//   host event signal_event           → signal_event
template <class E> struct with_signals { using type = std::variant<E, signal_event>; };
template <> struct with_signals<signal_event> { using type = signal_event; };
template <class... A> struct with_signals<std::variant<A...>> {
    using type = std::conditional_t<(std::same_as<A, signal_event> || ...),
                                    std::variant<A...>,
                                    std::variant<A..., signal_event>>;
};

inline constexpr signal_set all_signals{sig::interrupt, sig::terminate, sig::hangup,
                                        sig::resize, sig::child};

constexpr int exit_code_for(sig s) noexcept {
    switch (s) {
        case sig::interrupt: return 128 + 2;    // SIGINT
        case sig::terminate: return 128 + 15;   // SIGTERM
        case sig::hangup:    return 128 + 1;    // SIGHUP
        default:             return 128;
    }
}

// Tokens the driver owns; host tokens start above these.
inline constexpr std::uint64_t kSignalToken = 1;
inline constexpr std::uint64_t kFirstHostToken = 1u << 16;

// A host event, as the kernel's event type (which also carries signals).
template <class KE, class HE>
KE to_kernel_event(const HE& ev) {
    if constexpr (std::same_as<HE, KE>)
        return ev;                                  // same type: pass through
    else if constexpr (requires { std::variant_size<HE>::value; })
        return std::visit([](const auto& e) { return KE{e}; }, ev);   // variant → wider variant
    else
        return KE{ev};                              // one type → the variant holding it
}

// The kernel's host is an adapter over the user's host: it forwards
// effects and sources (handle / start_source / stop_source) when the user's
// host has them, and nothing else. kernel.hpp's HostFor checks the program's
// row against exactly these, so a missing handler is still a compile error.
template <class H, class KE>
struct forward_host {
    H& h;
    using event_type = KE;
    template <class E> requires requires(H& x, E e) { x.handle(std::move(e)); }
    void handle(E e) { h.handle(std::move(e)); }
    template <class Pay, class Key, class Msg>
        requires requires(H& x, const Pay& p, const Key& k, Sink<Msg> s) { x.start_source(p, k, s); }
    void start_source(const Pay& p, const Key& k, Sink<Msg> s) { h.start_source(p, k, std::move(s)); }
    template <class D, class Key>
        requires requires(H& x, std::type_identity<D> d, const Key& k) { x.stop_source(d, k); }
    void stop_source(std::type_identity<D> d, const Key& k) { h.stop_source(d, k); }
};

}  // namespace detail::run

template <class H>
using kernel_event_t = typename detail::run::with_signals<typename H::event_type>::type;

namespace detail::run {
/// Does the host still owe a frame it tried to draw and couldn't finish?
/// A host that defers (a terminal renderer backing off a congested tty)
/// says so with owes_frame(); present() is then called again even though the
/// model hasn't changed, so the deferred frame gets its retry.
template <class H>
bool host_owes_frame(const H& h) {
    if constexpr (requires { { h.owes_frame() } -> std::convertible_to<bool>; })
        return h.owes_frame();
    else
        return false;
}
}  // namespace detail::run
using detail::run::host_owes_frame;

struct run_options {
    kernel::options kernel{};
    /// Stop with 128+signo on interrupt/terminate/hangup that the program
    /// didn't handle. Turn off for a program that must never stop that way.
    bool default_signal_exit = true;

    /// Told the seed Cmd::random ran from, once, at start-up. A real run
    /// picks its own seed (kernel::options::random_seed == 0), so a program
    /// that wants its bugs to be reproducible logs it here and passes it
    /// back as random_seed to replay the same draws. Unset = not reported.
    std::function<void(std::uint64_t)> on_seed;
};

/// Durability for run<P>(): a model to resume from, and a journal hook.
///
///   jaal::durable<App> d;
///   d.resume = jaal::replay<App>(load_journal());     // empty journal = fresh start
///   d.journal = [&](const App::Msg& m) { append(m); };
///   return jaal::run<App>(host, opt, std::move(d));
///
/// `journal` sees every message update() folds, in fold order, BEFORE it's
/// folded: write it durably there and replay<App>() of what you wrote
/// rebuilds the model exactly (update is pure). `resume` skips init();
/// `resume_cmd` is one-shot work a restarted program must redo.
template <Program P>
struct durable {
    std::optional<typename P::Model>                resume;
    cmd_of<P>                                       resume_cmd = cmd_of<P>::none();
    std::function<void(const typename P::Msg&)>     journal;
};

template <Program P, class H> int run(H& host, run_options opt, durable<P> d);

/// What a host gets from run(): the reactor to watch its handles with, and
/// a way to hand events to the program.
template <class H>
class host_context {
public:
    using reactor = platform::native_reactor;
    using event   = typename H::event_type;

    /// Watch a handle. Keep the registration: dropping it unwatches. The
    /// token passed to on_ready() is `token` (host tokens can't collide
    /// with the driver's).
    [[nodiscard]] result<typename reactor::registration>
    watch(typename reactor::handle h, interest what, std::uint64_t token) {
        return r_.watch(h, what, token + detail::run::kFirstHostToken);
    }

    /// Hand one event to the program: routed now, then re-subscribed
    /// before the next emit (the ^T m o rule).
    void emit(const event& ev) { emit_(ev); }

    /// End the program with an exit code (the terminal went away, ...).
    void stop(int code) { stop_(code); }

    [[nodiscard]] reactor& native_reactor() noexcept { return r_; }

private:
    template <Program P2, class H2> friend int run(H2&, run_options, durable<P2>);
    host_context(reactor& r, std::function<void(const event&)> e, std::function<void(int)> s)
        : r_(r), emit_(std::move(e)), stop_(std::move(s)) {}
    reactor&                           r_;
    std::function<void(const event&)>  emit_;
    std::function<void(int)>           stop_;
};

/// Run P on the native platform with host H. Returns the exit code.
template <Program P, class H>
int run(H& host, run_options opt, durable<P> d) {
    using KE = kernel_event_t<H>;
    using K  = kernel::kernel<P, KE, platform::steady_clock>;
    namespace pf = platform;

    auto reactor = pf::native_reactor::create();
    if (!reactor) {
        std::fprintf(stderr, "jaal: can't create reactor: %.*s\n",
                     static_cast<int>(reactor.error().what.size()), reactor.error().what.data());
        return 70;                                  // EX_SOFTWARE
    }
    auto waker = reactor->waker();

    auto sigs = pf::native_signals::install(detail::run::all_signals);
    std::optional<typename pf::native_reactor::registration> sig_reg;
    if (sigs) {
        auto r = reactor->watch(sigs->handle(), interest::read, detail::run::kSignalToken);
        if (r) sig_reg.emplace(std::move(*r));
    }
    // The signal source the teardown guard will own. It has to be watchable
    // for as long as the loop runs and released before shutdown, and those
    // are the guard's job, not this function's.
    using sig_source = decltype(sigs);

    detail::run::forward_host<H, KE> fwd{host};

    auto wake = [waker] { waker.wake(); };
    K k = d.resume
        ? K::start_from(fwd, std::move(*d.resume), std::move(d.resume_cmd),
                        platform::steady_clock{}, opt.kernel, wake, std::move(d.journal))
        : K::start(fwd, platform::steady_clock{}, opt.kernel, wake, std::move(d.journal));

    // Shutdown order, owned by a destructor (kernel/teardown.hpp): signals
    // off, then host.release(), then kernel.finish(). Declared here so it
    // runs on EVERY path out of this function, including an exception thrown
    // by a host callback (which used to skip release() entirely and shut the
    // kernel down with the signal handlers still installed).
    kernel::teardown<K, H, sig_source> guard{k, host, std::move(sigs)};

    // Before any of the program's messages are folded: a crash in the first
    // step should still have the seed in the log.
    if (opt.on_seed) opt.on_seed(k.seed_used());

    // Host events arriving from on_ready() route immediately, one at a time.
    host_context<H> cx(
        *reactor,
        [&](const typename H::event_type& ev) { k.route(detail::run::to_kernel_event<KE>(ev), fwd); },
        [&](int code) { k.stop(code); });

    if constexpr (requires { host.attach(cx); }) host.attach(cx);

    for (;;) {
        auto t = k.step(fwd);
        if constexpr (requires { host.present(k); })
            if (t.model_changed || t.folded == 0 || host_owes_frame(host)) host.present(k);
        if (t.quit()) break;

        auto timeout = kernel::timeout_from<platform::steady_clock>(
            k.clock().now(), k.next_deadline());
        // The host may owe work of its own that no handle will announce: a
        // renderer that coalesced a frame (the terminal was congested) and
        // needs to be asked again shortly, or bytes still queued for a slow
        // tty. It says how soon with wait_hint(); the loop waits no longer.
        // Found running maya on jaal in a real pty: maya's renderer defers a
        // frame and relies on its own loop to retry within a few ms, so under
        // jaal every keystroke drew the PREVIOUS model — the owed frame sat
        // unpainted until the next event came along.
        if constexpr (requires { { host.wait_hint() } -> std::convertible_to<std::optional<std::chrono::milliseconds>>; }) {
            if (const std::optional<std::chrono::milliseconds> h = host.wait_hint())
                timeout = timeout ? std::min(*timeout, *h) : *h;
        }
        auto res = reactor->wait(timeout);
        if (!res) {
            std::fprintf(stderr, "jaal: reactor wait failed: %.*s\n",
                         static_cast<int>(res.error().what.size()), res.error().what.data());
            k.stop(70);
            continue;
        }

        for (std::uint8_t i = 0; i < res->count; ++i) {
            const auto& r = res->ready[i];
            if (r.token == detail::run::kSignalToken) {
                if (!guard.signals()) continue;
                for (auto s : guard.signals()->take()) {
                    // The HOST sees it first. Some signals are the host's
                    // business before they're the program's: SIGWINCH means
                    // "the terminal changed size", and only the host can ask
                    // the terminal for its new size and hand the program a
                    // resize event it can use. Without this a terminal host
                    // had no way to learn about a resize at all — it only
                    // sees its own handles, and the signal pipe isn't one.
                    if constexpr (requires { host.on_signal(cx, s); }) host.on_signal(cx, s);
                    const auto produced = k.route(KE{signal_event{signal_set{s}}}, fwd);
                    if (produced == 0 && opt.default_signal_exit
                        && (s == sig::interrupt || s == sig::terminate || s == sig::hangup))
                        k.stop(detail::run::exit_code_for(s));
                }
            } else if (r.token >= detail::run::kFirstHostToken) {
                if constexpr (requires { host.on_ready(cx, r); }) {
                    readiness mine = r;
                    mine.token -= detail::run::kFirstHostToken;
                    host.on_ready(cx, mine);
                }
            }
        }
        // A wake just means "the mailbox has messages": step() drains it.
    }
    sig_reg.reset();              // unwatch the pipe before the source goes
    return guard.exit_code();     // signals off, release(), finish()
}

template <Program P, class H>
int run(H& host, run_options opt) { return run<P>(host, std::move(opt), durable<P>{}); }

template <Program P, class H>
int run(H& host) { return run<P>(host, run_options{}, durable<P>{}); }

/// Run P with no host of its own (signals, timers, tasks, streams).
template <Program P>
int run(run_options opt = {}) {
    headless_host h;
    return run<P>(h, opt);
}

}  // namespace jaal
