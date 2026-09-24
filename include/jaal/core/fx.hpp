#pragma once
// jaal core effects: quit, after, task, now.
//
// These are the effects the kernel runs itself. Every host supports them.
//
// Tasks are where memory and concurrency bugs are born, so they get the
// strictest shape (docs/concurrency.md §4.6):
//
//   * the body is a CAPTURELESS function: it can't borrow `this`, the model,
//     a local, or the mailbox. Checked by conversion to a function pointer,
//     which the standard only allows for captureless lambdas.
//   * everything the body needs is passed as ARGUMENTS, moved in, and every
//     argument must be Sendable. The body owns its inputs outright.
//   * it gets a Sink (weak) and a stop_token, nothing else.
//
//   return Cmd::task([](Sink<Msg> out, std::stop_token st, std::string path) {
//       out.send(Loaded{read_file(path, st)});
//   }, path);
//
// The one hole: a captureless function can still touch globals/statics.
// jaal exposes none reachable from user code, and CI flags mutable statics.

#include <chrono>
#include <concepts>
#include <functional>
#include <memory>
#include <stop_token>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "../meta/fixed_string.hpp"
#include "effect.hpp"
#include "rng.hpp"
#include "sendable.hpp"
#include "sink.hpp"

namespace jaal {

// ── task bodies ──────────────────────────────────────────────────────────

/// F is a captureless callable taking (Sink<Msg>, stop_token, Args...).
template <class F, class Msg, class... Args>
concept TaskBody =
    std::is_convertible_v<F, void (*)(Sink<Msg>, std::stop_token, Args...)>;

namespace detail {

// A runnable task for Msg. jaal erases it ONCE, here, from a function
// pointer plus a tuple of owned Sendable args; nothing the user wrote can
// reach inside. It's move-only and single-shot.
template <class Msg>
class task_thunk {
public:
    template <class... Args>
    static task_thunk make(void (*fn)(Sink<Msg>, std::stop_token, Args...), Args... args) {
        return task_thunk(std::make_unique<impl<Args...>>(fn, std::move(args)...));
    }

    task_thunk(task_thunk&&) noexcept            = default;
    task_thunk& operator=(task_thunk&&) noexcept = default;

    /// Run once. Consumes the thunk.
    void run(Sink<Msg> out, std::stop_token st) && {
        auto b = std::move(body_);
        std::move(*b).invoke(std::move(out), std::move(st));
    }

    /// Re-target at another Msg type: wrap the Sink so the body's Msg is
    /// mapped on the way out. The mapper is a captureless function too.
    template <class To>
    auto map(To (*f)(Msg)) && -> task_thunk<To> {
        return task_thunk<To>::from_mapped(std::move(*this), f);
    }

    /// Re-target with an ID carried by value: `f(id, msg)`. This is how a
    /// LIST of children maps its messages (core/children.hpp) without a
    /// capture. The id is owned and Sendable, so the worker thread gets its
    /// own copy and nothing is borrowed from the loop.
    template <class To, class Id>
    auto map_with(Id id, To (*f)(const Id&, Msg)) && -> task_thunk<To> {
        return task_thunk<To>::from_mapped_with(std::move(*this), std::move(id), f);
    }

private:
    template <class> friend class task_thunk;

    struct base {
        virtual ~base() = default;
        virtual void invoke(Sink<Msg>, std::stop_token) && = 0;
    };
    template <class... Args>
    struct impl final : base {
        void (*fn)(Sink<Msg>, std::stop_token, Args...);
        std::tuple<Args...> args;
        impl(decltype(fn) f, Args... a) : fn(f), args(std::move(a)...) {}
        void invoke(Sink<Msg> out, std::stop_token st) && override {
            std::apply([&](Args&... a) { fn(std::move(out), std::move(st), std::move(a)...); },
                       args);
        }
    };

    explicit task_thunk(std::unique_ptr<base> b) noexcept : body_(std::move(b)) {}

    // Mapping: a task_thunk<Msg> built from a task_thunk<From> plus From→Msg.
    // The inner task sends From; a forwarding mailbox maps each to Msg and
    // posts it to the real sink.
    template <class From>
    static task_thunk from_mapped(task_thunk<From> inner, Msg (*f)(From)) {
        struct mapped final : base {
            task_thunk<From> inner;
            Msg (*f)(From);
            mapped(task_thunk<From> i, Msg (*g)(From)) : inner(std::move(i)), f(g) {}
            void invoke(Sink<Msg> out, std::stop_token st) && override {
                struct fwd final : mailbox_iface<From> {
                    Sink<Msg> out;
                    Msg (*f)(From);
                    fwd(Sink<Msg> o, Msg (*g)(From)) : out(std::move(o)), f(g) {}
                    bool post(From m) override { return out.send(f(std::move(m))); }
                };
                // The forwarder lives exactly as long as this invocation; the
                // inner sink is weak, so if the task stashes it past return,
                // sends just return false.
                auto box = std::make_shared<fwd>(std::move(out), f);
                auto s   = sink_access::make<From>(
                    std::weak_ptr<mailbox_iface<From>>(box));
                std::move(inner).run(std::move(s), std::move(st));
            }
        };
        return task_thunk(std::make_unique<mapped>(std::move(inner), f));
    }

    // Same, with an owned id passed to every mapping call. The id lives in
    // the thunk (not in a capture), so it crosses to the worker thread by
    // value like any task argument.
    template <class From, class Id>
    static task_thunk from_mapped_with(task_thunk<From> inner, Id id,
                                       Msg (*f)(const Id&, From)) {
        struct mapped final : base {
            task_thunk<From> inner;
            Id               id;
            Msg (*f)(const Id&, From);
            mapped(task_thunk<From> i, Id k, Msg (*g)(const Id&, From))
                : inner(std::move(i)), id(std::move(k)), f(g) {}
            void invoke(Sink<Msg> out, std::stop_token st) && override {
                struct fwd final : mailbox_iface<From> {
                    Sink<Msg> out;
                    Id        id;
                    Msg (*f)(const Id&, From);
                    fwd(Sink<Msg> o, Id k, Msg (*g)(const Id&, From))
                        : out(std::move(o)), id(std::move(k)), f(g) {}
                    bool post(From m) override { return out.send(f(id, std::move(m))); }
                };
                auto box = std::make_shared<fwd>(std::move(out), std::move(id), f);
                auto s   = sink_access::make<From>(
                    std::weak_ptr<mailbox_iface<From>>(box));
                std::move(inner).run(std::move(s), std::move(st));
            }
        };
        return task_thunk(std::make_unique<mapped>(std::move(inner), std::move(id), f));
    }

    std::unique_ptr<base> body_;
};

}  // namespace detail

// ── descriptors ──────────────────────────────────────────────────────────
namespace fx {

/// Stop the program after the current batch.
struct quit {
    static constexpr std::string_view name = "quit";
    template <class Msg> struct type {
        int code = 0;
    };
    template <class F, class M>
    static auto fmap(F&&, type<M> e) -> type<std::invoke_result_t<F, M>> { return {e.code}; }
    template <class Id, class F, class M>
    static auto fmap_with(const Id&, F&&, type<M> e)
        -> type<std::invoke_result_t<F, const Id&, M>> { return {e.code}; }

    template <class Self, class Msg> struct ctors {
        [[nodiscard]] static Self quit(int code = 0) { return Self(type<Msg>{code}); }
    };
};

/// Feed a message straight back into the loop.
///
/// The composition primitive: one update path reuses another without
/// calling it directly, so every state change still goes through the fold
/// (and so replay, tracing and `given` all see it).
///
///   return {m, Cmd::send(Msg{Refresh{}})};      // "and now also refresh"
///
/// Folded in the SAME step as the message that returned it, after the
/// current batch's effects are interpreted, in the order they were
/// returned. It is not a timer: `after(0ms, m)` goes through the timer heap
/// and doesn't arrive until the next step, which makes a UI feel a frame
/// late and makes tests sleep for no reason.
///
/// A program that sends itself a message on every fold never lets the loop
/// idle; the fold budget (options::fold_budget) keeps that from starving
/// the host, but it's still a bug.
struct send {
    static constexpr std::string_view name = "send";
    template <class Msg> struct type {
        Msg msg;
    };
    template <class F, class M>
    static auto fmap(F&& f, type<M> e) -> type<std::invoke_result_t<F, M>> {
        return {std::invoke(std::forward<F>(f), std::move(e.msg))};
    }
    template <class Id, class F, class M>
    static auto fmap_with(const Id& id, F&& f, type<M> e)
        -> type<std::invoke_result_t<F, const Id&, M>> {
        return {std::invoke(std::forward<F>(f), id, std::move(e.msg))};
    }

    template <class Self, class Msg> struct ctors {
        [[nodiscard]] static Self send(Msg m) { return Self(type<Msg>{std::move(m)}); }
    };
};

/// Deliver msg after delay (one-shot timer).
struct after {
    static constexpr std::string_view name = "after";
    template <class Msg> struct type {
        std::chrono::milliseconds delay;
        Msg msg;
    };
    template <class F, class M>
    static auto fmap(F&& f, type<M> e) -> type<std::invoke_result_t<F, M>> {
        return {e.delay, std::invoke(std::forward<F>(f), std::move(e.msg))};
    }
    template <class Id, class F, class M>
    static auto fmap_with(const Id& id, F&& f, type<M> e)
        -> type<std::invoke_result_t<F, const Id&, M>> {
        return {e.delay, std::invoke(std::forward<F>(f), id, std::move(e.msg))};
    }

    template <class Self, class Msg> struct ctors {
        [[nodiscard]] static Self after(std::chrono::milliseconds d, Msg m) {
            return Self(type<Msg>{d, std::move(m)});
        }
    };
};

namespace detail_task {
// Task payloads carry a thunk. map() needs a plain function pointer for
// the Msg mapping; the Cmd::map lambda is converted when captureless, and
// rejected (with a reason) otherwise. That keeps task mapping from
// smuggling a capture into a worker.
template <class F, class From>
concept captureless_mapper =
    std::is_convertible_v<F, std::invoke_result_t<F, From> (*)(From)>;
}  // namespace detail_task

/// Where a task runs. A closed enum, so "which thread" is one of exactly
/// two choices, spelled at the call site.
enum class placement : std::uint8_t {
    pool,       // the shared worker pool: short work, reused threads
    isolated,   // its own detached thread: work that may hang (a stuck
                // syscall leaks one thread instead of blocking the pool)
};

/// Run a captureless body with Sendable arguments off the loop. One effect,
/// two placements: Cmd::task(body, args...) runs on the pool;
/// Cmd::task(isolated, body, args...) runs on its own thread.
struct task {
    static constexpr std::string_view name = "task";
    template <class Msg> struct type {
        ::jaal::detail::task_thunk<Msg> thunk;
        placement                       where = placement::pool;
    };
    template <class F, class M>
    static auto fmap(F&& f, type<M> e) -> type<std::invoke_result_t<F, M>> {
        using To = std::invoke_result_t<F, M>;
        static_assert(detail_task::captureless_mapper<std::remove_cvref_t<F>, M>,
                      "jaal: Cmd::map on a Cmd holding a task needs a "
                      "captureless mapper (it runs on the worker thread)");
        To (*fp)(M) = f;
        return {std::move(e.thunk).map(fp), e.where};
    }
    template <class Id, class F, class M>
    static auto fmap_with(const Id& id, F&& f, type<M> e)
        -> type<std::invoke_result_t<F, const Id&, M>> {
        using To = std::invoke_result_t<F, const Id&, M>;
        static_assert(std::is_convertible_v<std::remove_cvref_t<F>, To (*)(const Id&, M)>,
                      "jaal: mapping a Cmd that holds a task needs a captureless "
                      "mapper (it runs on the worker thread)");
        static_assert(Sendable<Id>,
                      "jaal: the id a task's mapper carries crosses to the worker "
                      "thread, so it must be Sendable");
        To (*fp)(const Id&, M) = f;
        return {std::move(e.thunk).template map_with<To, Id>(id, fp), e.where};
    }
    template <class Self, class Msg> struct ctors {
        template <class Body, class... Args>
            requires TaskBody<Body, Msg, Args...> && (Sendable<Args> && ...)
        [[nodiscard]] static Self task(Body body, Args... args) {
            return make(placement::pool, body, std::move(args)...);
        }
        template <class Body, class... Args>
            requires TaskBody<Body, Msg, Args...> && (Sendable<Args> && ...)
        [[nodiscard]] static Self task(placement where, Body body, Args... args) {
            return make(where, body, std::move(args)...);
        }
        /// Kept as a spelling: Cmd::task_isolated(body, args...).
        template <class Body, class... Args>
            requires TaskBody<Body, Msg, Args...> && (Sendable<Args> && ...)
        [[nodiscard]] static Self task_isolated(Body body, Args... args) {
            return make(placement::isolated, body, std::move(args)...);
        }
        // Same call, wrong shape: say which rule it broke.
        template <class Body, class... Args>
            requires (!std::same_as<Body, placement>)
                  && (!(TaskBody<Body, Msg, Args...> && (Sendable<Args> && ...)))
        static Self task(Body, Args...) { explain<Body, Args...>(); return Self{}; }

    private:
        template <class Body, class... Args>
        static Self make(placement where, Body body, Args... args) {
            void (*fn)(Sink<Msg>, std::stop_token, Args...) = body;
            return Self(type<Msg>{::jaal::detail::task_thunk<Msg>::make(fn, std::move(args)...),
                                  where});
        }
        template <class Body, class... Args>
        static void explain() {
            if constexpr (!(Sendable<Args> && ...))
                static_assert((Sendable<Args> && ...),
                    "jaal: a task argument is not Sendable; pass owned values "
                    "(std::string, not std::string_view or a pointer)");
            else if constexpr (std::is_invocable_v<Body, Sink<Msg>, std::stop_token, Args...>)
                static_assert(TaskBody<Body, Msg, Args...>,
                    "jaal: a task body must not capture anything; pass what it "
                    "needs as arguments after the body");
            else
                static_assert(TaskBody<Body, Msg, Args...>,
                    "jaal: a task body must be callable as "
                    "(Sink<Msg>, std::stop_token, Args...)");
        }
    };
};

inline constexpr placement isolated = placement::isolated;

/// Read the KERNEL's clock and deliver the time as a message.
///
/// A task calling std::chrono would read the real clock, so the headless
/// host couldn't control it (advance(5s) wouldn't move it) and replay would
/// see different times. `now` reads the clock the kernel was started with:
/// real in production, simulated in tests.
///
/// The mapper runs on the loop thread, synchronously, so it may capture.
struct now {
    static constexpr std::string_view name = "now";
    using time_point = std::chrono::steady_clock::time_point;
    template <class Msg> struct type {
        std::function<Msg(time_point)> to_msg;
    };
    template <class F, class M>
    static auto fmap(F&& f, type<M> e) -> type<std::invoke_result_t<F, M>> {
        using To = std::invoke_result_t<F, M>;
        return {[g = std::move(e.to_msg), f = std::forward<F>(f)](time_point t) -> To {
            return f(g(t));
        }};
    }
    template <class Id, class F, class M>
    static auto fmap_with(const Id& id, F&& f, type<M> e)
        -> type<std::invoke_result_t<F, const Id&, M>> {
        using To = std::invoke_result_t<F, const Id&, M>;
        // Runs on the loop thread, so this one may capture the id.
        return {[g = std::move(e.to_msg), f = std::forward<F>(f), id](time_point t) -> To {
            return f(id, g(t));
        }};
    }
    template <class Self, class Msg> struct ctors {
        template <class F>
            requires std::is_invocable_r_v<Msg, F&, time_point>
        [[nodiscard]] static Self now(F f) { return Self(type<Msg>{std::move(f)}); }
    };
};

/// Draw random numbers and deliver them as a message.
///
/// Elm's Random.generate. Like `now`, this is a core effect because the
/// alternative is a global generator that replay and sim can't control: a
/// program that reads one directly isn't reproducible, and the bug you're
/// chasing never comes back. The KERNEL owns the stream, so the same seed
/// gives the same run (jaal::sim), and a recorded run replays exactly
/// (jaal::replay folds the messages, so the draws are already in them).
///
/// The mapper runs on the loop thread, synchronously, so it may capture.
///
///   return {m, Cmd::random([](jaal::rng& r) -> Msg {
///                  return Rolled{r.in(1, 6)};
///              })};
struct random {
    static constexpr std::string_view name = "random";
    template <class Msg> struct type {
        // Takes the rng by reference: one effect can draw as much as it
        // needs (a shuffle, a whole level) from one stream.
        std::function<Msg(rng&)> to_msg;
    };
    template <class F, class M>
    static auto fmap(F&& f, type<M> e) -> type<std::invoke_result_t<F, M>> {
        using To = std::invoke_result_t<F, M>;
        return {[g = std::move(e.to_msg), f = std::forward<F>(f)](rng& r) -> To {
            return f(g(r));
        }};
    }
    template <class Id, class F, class M>
    static auto fmap_with(const Id& id, F&& f, type<M> e)
        -> type<std::invoke_result_t<F, const Id&, M>> {
        using To = std::invoke_result_t<F, const Id&, M>;
        return {[g = std::move(e.to_msg), f = std::forward<F>(f), id](rng& r) -> To {
            return f(id, g(r));
        }};
    }
    template <class Self, class Msg> struct ctors {
        template <class F>
            requires std::is_invocable_r_v<Msg, F&, rng&>
        [[nodiscard]] static Self random(F f) { return Self(type<Msg>{std::move(f)}); }
    };
};

}  // namespace fx
}  // namespace jaal
