#pragma once
// jaal::run<P>() — run a program for real: kernel + native reactor + signals.
//
//   int main() { return jaal::run<Counter>(); }
//
// This is the loop DESIGN.md §5.2 describes, with nothing simulated:
//
//   1. start the kernel (init + init Cmd); its mailbox wakes the reactor
//   2. wait on the reactor until the next timer, a wake, a signal, or input
//   3. hand signals and host events to the kernel; step it
//   4. repeat until the program quits; finish() in a fixed order
//
// Signals are ordinary input. A program that wants them subscribes:
//
//   static Sub subscribe(const Model&) {
//       return Sub::on_signal({sig::interrupt, sig::terminate},
//                             [](jaal::signal s) { return Msg{Shutdown{}}; });
//   }
//
// and gets a Msg on the loop thread. A program that DOESN'T subscribe to
// interrupt/terminate gets the conventional behaviour: the loop stops with
// exit code 128 + signal number (130 for Ctrl+C, 143 for SIGTERM), the way
// a shell reports it. So a jaal program is never left un-killable by Ctrl+C
// just because it forgot to ask.
//
// Hosts: run<P>() uses a host with no screen and no input of its own
// (headless_host), which suits servers and tools. A terminal or GUI host
// (maya) plugs in through run<P>(host), supplying its own events and
// effects.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include "../core/program.hpp"
#include "../core/sub.hpp"
#include "../platform/clock.hpp"
#include "../platform/select.hpp"
#include "../platform/signal.hpp"
#include "kernel.hpp"
#include "timer_heap.hpp"

namespace jaal {

using platform::sig;
using platform::signal_set;

// ── the signal router ────────────────────────────────────────────────────
/// The event the driver routes to programs: which signals arrived.
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
        signal_set                                 wanted;
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
    // A signal_event can carry several signals; route() returns the first
    // match. The driver delivers one signal per event, so in practice each
    // event is exactly one signal.
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
            if constexpr (std::is_convertible_v<R, std::optional<Msg>>)
                return Self(type<Msg>{wanted, std::move(f)});
            else
                return Self(type<Msg>{wanted, [f = std::move(f)](sig s) -> std::optional<Msg> {
                    return f(s);
                }});
        }
    };
};

}  // namespace fx

// ── hosts ────────────────────────────────────────────────────────────────
/// A host with no screen and no input of its own: signals only. Runs only
/// programs whose effects are all core ones (HostFor checks that).
struct headless_host {
    using event_type = signal_event;
};

namespace detail::run {

// The driver installs one signal source for every portable signal, once.
// The program's subscriptions decide what each signal means; the source
// just makes sure it arrives as an event.
inline constexpr signal_set all_signals{sig::interrupt, sig::terminate,
                                        sig::hangup, sig::resize,
                                        sig::child};

// Conventional exit code for dying of a signal: 128 + the POSIX number.
constexpr int exit_code_for(sig s) noexcept {
    switch (s) {
        case sig::interrupt: return 128 + 2;    // SIGINT
        case sig::terminate: return 128 + 15;   // SIGTERM
        case sig::hangup:    return 128 + 1;    // SIGHUP
        default:                return 128;
    }
}

}  // namespace detail::run

struct run_options {
    kernel::options kernel{};
    /// Stop with 128+signo on interrupt/terminate/hangup that the program
    /// didn't handle. Turn off for a program that must never stop that way.
    bool default_signal_exit = true;
};

/// Run P on the native platform with host H. Returns the exit code.
template <Program P, class H>
    requires std::same_as<typename H::event_type, signal_event>
int run(H& host, run_options opt = {}) {
    using K = kernel::kernel<P, signal_event, platform::steady_clock>;
    namespace pf = platform;

    auto reactor = pf::native_reactor::create();
    if (!reactor) {
        std::fprintf(stderr, "jaal: can't create reactor: %.*s\n",
                     static_cast<int>(reactor.error().what.size()), reactor.error().what.data());
        return 70;                                  // EX_SOFTWARE
    }
    auto waker = reactor->waker();

    // Declared BEFORE the kernel so they're destroyed AFTER it: the kernel's
    // mailbox wakes the reactor, and its shutdown must finish first.
    auto sigs = pf::native_signals::install(detail::run::all_signals);
    std::optional<typename pf::native_reactor::registration> sig_reg;
    constexpr std::uint64_t kSignalToken = 1;
    if (sigs) {
        auto r = reactor->watch(sigs->handle(), pf::interest::read, kSignalToken);
        if (r) sig_reg.emplace(std::move(*r));
    }

    K k = K::start(host, platform::steady_clock{}, opt.kernel, [waker] { waker.wake(); });

    for (;;) {
        auto t = k.step(host);
        if (t.quit) break;

        // Sleep until the next deadline (rounded up: see timer_heap.hpp).
        const auto timeout = kernel::timeout_from<platform::steady_clock>(
            k.clock().now(), k.next_deadline());
        auto res = reactor->wait(timeout);
        if (!res) {
            std::fprintf(stderr, "jaal: reactor wait failed: %.*s\n",
                         static_cast<int>(res.error().what.size()), res.error().what.data());
            k.stop(70);
            continue;
        }

        for (std::uint8_t i = 0; i < res->count; ++i) {
            if (res->ready[i].token != kSignalToken || !sigs) continue;
            for (auto s : sigs->take()) {
                const auto produced = k.route(signal_event{signal_set{s}}, host);
                // Nobody subscribed to an interrupt/terminate/hangup: act
                // like a normal process and stop, instead of ignoring it.
                if (produced == 0 && opt.default_signal_exit
                    && (s == sig::interrupt || s == sig::terminate || s == sig::hangup))
                    k.stop(detail::run::exit_code_for(s));
            }
        }
        // A wake just means "the mailbox has messages": step() drains it.
    }
    return std::move(k).finish();
}

/// Run P with the headless host.
template <Program P>
int run(run_options opt = {}) {
    headless_host h;
    return run<P>(h, opt);
}

}  // namespace jaal
