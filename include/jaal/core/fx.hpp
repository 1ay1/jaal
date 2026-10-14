#pragma once
// jaal core effects: quit, after, task, now.
//
// These are the effects the kernel runs itself. Every host supports them.
//
// Tasks are where memory and concurrency bugs are born, so they get the
// strictest shape (docs/concurrency.md §4.6):
//
//   * the body is CAPTURELESS: it can't borrow `this`, the model, a local,
//     or the mailbox. A lambda with any capture (even an unused [&]) is not
//     default-constructible; one without is, and so it holds nothing. A
//     plain function is the other captureless shape.
//   * everything the body needs is passed as ARGUMENTS, moved in, and every
//     argument must be Sendable. The body owns its inputs outright.
//   * it gets a Sink (weak), and a stop_token if it asks for one.
//
// The same rule covers task, stream, perform and attempt. A body may take
// its parameters as `const T&` or `T`, and may be a generic lambda.
//
//   return Cmd::task([](Sink<Msg> out, std::stop_token st, std::string path) {
//       out.send(Loaded{read_file(path, st)});
//   }, path);
//
// The one hole: a captureless function can still touch globals/statics.
// jaal exposes none reachable from user code, and CI flags mutable statics.

#include <chrono>
#include <concepts>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "../meta/fixed_string.hpp"
#include "capability.hpp"
#include "effect.hpp"
#include "rng.hpp"
#include "sendable.hpp"
#include "sink.hpp"

namespace jaal {

// ── task bodies ──────────────────────────────────────────────────────────

/// F is a captureless callable taking (Sink<Msg>, stop_token, Args...).
///
/// This concept, ANDed with `Sendable` over the arguments, is the real
/// question "is this a well-formed task" — and the only way to ask it.
///
///     template <class Body, class... Args>
///     concept ok = TaskBody<Body, Msg, Args...> && (Sendable<Args> && ...);
///
/// Do NOT probe with `requires { Cmd::task(body, args...); }`. That is
/// always true, by design: the bad-input overload (`explain`, below) stays
/// VIABLE so it can fire a static_assert naming the rule you broke, rather
/// than dumping "no matching function" with every candidate. A
/// requires-expression only reports substitution failure, and a
/// static_assert in an instantiated body is a hard error instead — so the
/// probe answers "fine" and the build then fails anyway at the call.
/// tests/core/task_gate_test.cpp pins both halves of this.
///
/// A body may also take `const jaal::unlocked&` FIRST:
///     (const unlocked&, Sink<Msg>, std::stop_token, Args...)
/// A task runs on a worker thread that holds no lock, so jaal hands it one.
/// That is how background work earns the right to take locks
/// (core/capability.hpp); a body that doesn't ask can't take any.
namespace detail_task {

// Captureless: an empty lambda (Body{} IS the function) or a plain function
// pointer, which is carried along as data since its value isn't its type.
template <class B>
concept empty_body = std::is_empty_v<B> && std::default_initializable<B>;
template <class B>
concept fn_ptr = std::is_pointer_v<B> && std::is_function_v<std::remove_pointer_t<B>>;
template <class B>
concept captureless = empty_body<B> || fn_ptr<B>;

// The four call shapes a body may have, most specific first.
enum class shape : std::uint8_t { none, cap_stop, cap, stop, plain };

template <class B, class Msg, class... Args>
consteval shape shape_of() {
    using S = Sink<Msg>;
    if constexpr (std::is_invocable_v<B&, const unlocked&, S, std::stop_token, Args...>)
        return shape::cap_stop;
    else if constexpr (std::is_invocable_v<B&, const unlocked&, S, Args...>)
        return shape::cap;
    else if constexpr (std::is_invocable_v<B&, S, std::stop_token, Args...>)
        return shape::stop;
    else if constexpr (std::is_invocable_v<B&, S, Args...>)
        return shape::plain;
    else
        return shape::none;
}

template <class B, class Msg, class... Args>
concept callable_body = shape_of<B, Msg, Args...>() != shape::none;

// The one function every body is erased to. `F` is the body itself when it's
// a function pointer, and an empty tag otherwise.
template <class Body, class Msg, class F, class... Args>
void invoke_body(Sink<Msg> out, std::stop_token st, F f, Args... args) {
    auto&& body = [&]() -> decltype(auto) {
        if constexpr (fn_ptr<Body>) return f;
        else                        return Body{};
    }();
    constexpr shape s = shape_of<Body, Msg, Args...>();
    if constexpr (s == shape::cap_stop || s == shape::cap) {
        const unlocked u = cap_access::fresh_thread();
        if constexpr (s == shape::cap_stop)
            body(u, std::move(out), std::move(st), std::move(args)...);
        else
            body(u, std::move(out), std::move(args)...);
    } else if constexpr (s == shape::stop) {
        body(std::move(out), std::move(st), std::move(args)...);
    } else {
        body(std::move(out), std::move(args)...);
    }
}

struct no_fn {};
template <class Body>
using carried_t = std::conditional_t<fn_ptr<Body>, Body, no_fn>;

/// The erased entry point for a well-formed body, and what it carries.
template <class Body, class Msg, class... Args>
auto entry() -> void (*)(Sink<Msg>, std::stop_token, carried_t<Body>, Args...) {
    return &invoke_body<Body, Msg, carried_t<Body>, Args...>;
}
template <class Body>
carried_t<Body> carried(Body b) {
    if constexpr (fn_ptr<Body>) return b;
    else                        { (void)b; return no_fn{}; }
}
}  // namespace detail_task

/// Is Body a well-formed task body for Msg and Args? Captureless, and
/// callable with one of the shapes above. Pair it with Sendable<Args>...:
///
///     concept ok = TaskBody<Body, Msg, Args...> && (Sendable<Args> && ...);
template <class F, class Msg, class... Args>
concept TaskBody = detail_task::captureless<F> && detail_task::callable_body<F, Msg, Args...>;

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

}  // namespace fx

/// A task's name. A keyed task REPLACES the running task with the same key:
/// the old one is asked to stop, and anything it sends from then on, even
/// messages already queued, never reaches update(). Cmd::cancel(key) does
/// the same without starting a new one.
///
///   return Cmd::task("search", body, m.query);             // latest wins
///   return Cmd::task(jaal::key("scan", jaal::fx::isolated), body, dir);
///
/// Converts from a string, so a literal works wherever a key is taken.
struct task_key {
    std::string   name;
    fx::placement where = fx::placement::pool;

    task_key() = default;
    task_key(const char* n) : name(n) {}                       // NOLINT: implicit on purpose
    task_key(std::string n) : name(std::move(n)) {}            // NOLINT
    task_key(std::string_view n) : name(n) {}                  // NOLINT
    task_key(std::string n, fx::placement w) : name(std::move(n)), where(w) {}
};

/// A key with a placement: jaal::key("scan", jaal::fx::isolated).
inline task_key key(std::string name, fx::placement where = fx::placement::pool) {
    return task_key(std::move(name), where);
}

namespace detail_perform {

// The same captureless rule as every other body (detail_task).
using detail_task::captureless;
using detail_task::fn_ptr;

template <class B, class... Args>
concept takes_stop = std::is_invocable_v<B&, std::stop_token, Args...>;

template <class B, class... Args>
concept takes_args = std::is_invocable_v<B&, Args...>;

template <class B, class... Args>
struct result {
    using type = std::invoke_result_t<B&, Args...>;
};
template <class B, class... Args>
    requires takes_stop<B, Args...>
struct result<B, Args...> {
    using type = std::invoke_result_t<B&, std::stop_token, Args...>;
};
template <class B, class... Args>
using result_t = typename result<B, Args...>::type;

/// Can Case carry what the body returned? `Case{r}`, or `Case{}` for void.
template <class Case, class R, class Msg>
concept wraps = (std::is_void_v<R> && requires { Msg(Case{}); })
             || (!std::is_void_v<R> && requires(R r) { Msg(Case{std::move(r)}); });

/// Can Fail be built from a caught exception: its message, the
/// exception_ptr itself, or nothing at all.
template <class Fail, class Msg>
concept failure_case = requires(std::string s) { Msg(Fail{std::move(s)}); }
                    || requires(std::exception_ptr e) { Msg(Fail{e}); }
                    || requires { Msg(Fail{}); };

template <class Fail, class Msg>
Msg make_failure(std::exception_ptr e) {
    if constexpr (requires(std::string s) { Msg(Fail{std::move(s)}); }) {
        std::string what = "unknown exception";
        try { std::rethrow_exception(e); }
        catch (const std::exception& x) { what = x.what(); }
        catch (...) {}
        return Msg(Fail{std::move(what)});
    } else if constexpr (requires { Msg(Fail{e}); }) {
        return Msg(Fail{std::move(e)});
    } else {
        return Msg(Fail{});
    }
}

// The task body perform/attempt build: call the user's body, wrap what it
// returns in Ok and send it. With a Fail case, a throw is sent as Fail
// instead of becoming a task fault. `F` is the body itself when it's a
// function pointer, and an empty tag otherwise.
template <class Msg, class Ok, class Fail, class Body, class F, class... Args>
void shim(Sink<Msg> out, std::stop_token st, F f, Args... args) {
    auto call = [&]() -> decltype(auto) {
        auto&& body = [&]() -> decltype(auto) {
            if constexpr (fn_ptr<Body>) return f;
            else                        return Body{};
        }();
        if constexpr (takes_stop<Body, Args...>) return body(std::move(st), std::move(args)...);
        else                                     return body(std::move(args)...);
    };
    auto deliver = [&] {
        if constexpr (std::is_void_v<result_t<Body, Args...>>) {
            call();
            out.send(Msg(Ok{}));
        } else {
            out.send(Msg(Ok{call()}));
        }
    };
    if constexpr (std::is_void_v<Fail>) {
        deliver();
    } else {
        try {
            deliver();
        } catch (...) {
            out.send(make_failure<Fail, Msg>(std::current_exception()));
        }
    }
}

}  // namespace detail_perform

namespace fx {

/// Run a captureless body with Sendable arguments off the loop. One effect,
/// two placements: Cmd::task(body, args...) runs on the pool;
/// Cmd::task(isolated, body, args...) runs on its own thread.
struct task {
    static constexpr std::string_view name = "task";
    template <class Msg> struct type {
        ::jaal::detail::task_thunk<Msg> thunk;
        placement                       where = placement::pool;
        std::string                     key;      // empty: not keyed
    };
    template <class F, class M>
    static auto fmap(F&& f, type<M> e) -> type<std::invoke_result_t<F, M>> {
        using To = std::invoke_result_t<F, M>;
        static_assert(detail_task::captureless_mapper<std::remove_cvref_t<F>, M>,
                      "jaal: Cmd::map on a Cmd holding a task needs a "
                      "captureless mapper (it runs on the worker thread)");
        To (*fp)(M) = f;
        return {std::move(e.thunk).map(fp), e.where, std::move(e.key)};
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
        return {std::move(e.thunk).template map_with<To, Id>(id, fp), e.where, std::move(e.key)};
    }
    template <class Self, class Msg> struct ctors {
        template <class Body, class... Args>
            requires TaskBody<Body, Msg, Args...> && (Sendable<Args> && ...)
        [[nodiscard]] static Self task(Body body, Args... args) {
            return make(placement::pool, {}, body, std::move(args)...);
        }
        template <class Body, class... Args>
            requires TaskBody<Body, Msg, Args...> && (Sendable<Args> && ...)
        [[nodiscard]] static Self task(placement where, Body body, Args... args) {
            return make(where, {}, body, std::move(args)...);
        }
        /// A keyed task: replaces the running task with the same key.
        template <class Body, class... Args>
            requires TaskBody<Body, Msg, Args...> && (Sendable<Args> && ...)
        [[nodiscard]] static Self task(task_key k, Body body, Args... args) {
            return make(k.where, std::move(k.name), body, std::move(args)...);
        }
        /// Kept as a spelling: Cmd::task_isolated(body, args...).
        template <class Body, class... Args>
            requires TaskBody<Body, Msg, Args...> && (Sendable<Args> && ...)
        [[nodiscard]] static Self task_isolated(Body body, Args... args) {
            return make(placement::isolated, {}, body, std::move(args)...);
        }
        // Same call, wrong shape: say which rule it broke.
        template <class Body, class... Args>
            requires (!std::same_as<Body, placement>)
                  && (!std::convertible_to<Body, task_key>)
                  && (!(TaskBody<Body, Msg, Args...> && (Sendable<Args> && ...)))
        static Self task(Body, Args...) { explain<Body, Args...>(); return Self{}; }
        template <class Body, class... Args>
            requires (!(TaskBody<Body, Msg, Args...> && (Sendable<Args> && ...)))
        static Self task(task_key, Body, Args...) { explain<Body, Args...>(); return Self{}; }

        // ...and the same for the isolated spelling. Without this overload
        // a bad isolated task fails as a raw "no matching function" dump
        // naming every candidate, instead of the one line that says which
        // rule was broken — the diagnostic IS the guarantee here, since a
        // programmer who can't see the rule will work around it.
        template <class Body, class... Args>
            requires (!(TaskBody<Body, Msg, Args...> && (Sendable<Args> && ...)))
        static Self task_isolated(Body, Args...) {
            explain<Body, Args...>();
            return Self{};
        }

    private:
        template <class Body, class... Args>
        static Self make(placement where, std::string key, Body body, Args... args) {
            namespace dt = ::jaal::detail_task;
            return Self(type<Msg>{
                ::jaal::detail::task_thunk<Msg>::make(dt::entry<Body, Msg, Args...>(),
                                                      dt::carried(body), std::move(args)...),
                where, std::move(key)});
        }

    public:
        /// Run `body(args...)` off the loop and deliver what it returns as
        /// `Ok{result}` (`Ok{}` for void). Elm's Task.perform: no Sink, no
        /// send, no message plumbing in the body.
        ///
        ///   return Cmd::perform<Loaded>([](const std::string& url) {
        ///       return fetch(url);
        ///   }, m.url);
        ///
        /// The body may take a std::stop_token first. Same rules as task: it
        /// captures nothing, and its arguments are Sendable. A throw is a task
        /// fault; use attempt() when failure is a message.
        template <class Ok, class Body, class... Args>
            requires (!std::convertible_to<Body, task_key>)
        [[nodiscard]] static Self perform(Body body, Args... args) {
            return make_perform<Ok, void>(task_key{}, body, std::move(args)...);
        }
        /// perform, keyed: the latest call with this key wins.
        template <class Ok, class Body, class... Args>
        [[nodiscard]] static Self perform(task_key k, Body body, Args... args) {
            return make_perform<Ok, void>(std::move(k), body, std::move(args)...);
        }

        /// perform, where a throw becomes a message: `Fail{e.what()}`,
        /// `Fail{exception_ptr}` or `Fail{}`, whichever Fail can hold.
        /// Elm's Task.attempt.
        ///
        ///   return Cmd::attempt<Loaded, LoadFailed>(load, path);
        template <class Ok, class Fail, class Body, class... Args>
            requires (!std::convertible_to<Body, task_key>)
        [[nodiscard]] static Self attempt(Body body, Args... args) {
            return make_perform<Ok, Fail>(task_key{}, body, std::move(args)...);
        }
        /// attempt, keyed: the latest call with this key wins.
        template <class Ok, class Fail, class Body, class... Args>
        [[nodiscard]] static Self attempt(task_key k, Body body, Args... args) {
            return make_perform<Ok, Fail>(std::move(k), body, std::move(args)...);
        }

    private:
        template <class Ok, class Fail, class Body, class... Args>
        static Self make_perform(task_key k, Body body, Args... args) {
            namespace dp = ::jaal::detail_perform;
            constexpr bool callable = dp::takes_stop<Body, Args...> || dp::takes_args<Body, Args...>;
            if constexpr (!(Sendable<Args> && ...)) {
                static_assert((Sendable<Args> && ...),
                    "jaal: a perform argument is not Sendable; pass owned values "
                    "(std::string, not std::string_view, a pointer or a literal)");
            } else if constexpr (!callable) {
                static_assert(callable,
                    "jaal: a perform body must be callable as (Args...) or "
                    "(std::stop_token, Args...) with the arguments given after it");
            } else if constexpr (!dp::captureless<Body>) {
                static_assert(dp::captureless<Body>,
                    "jaal: a perform body must not capture anything; pass what it "
                    "needs as arguments after the body");
            } else if constexpr (!dp::wraps<Ok, dp::result_t<Body, Args...>, Msg>) {
                static_assert(dp::wraps<Ok, dp::result_t<Body, Args...>, Msg>,
                    "jaal: perform<Ok>: can't build the message Ok{result} from what "
                    "the body returns; give Ok one field of the body's result type "
                    "(or no fields, for a void body), and make Ok a case of Msg");
            } else if constexpr (!std::is_void_v<Fail> && !dp::failure_case<Fail, Msg>) {
                static_assert(dp::failure_case<Fail, Msg>,
                    "jaal: attempt<Ok, Fail>: Fail must be a case of Msg built from a "
                    "std::string (the exception's what()), a std::exception_ptr, or "
                    "nothing");
            } else {
                // A function pointer rides along as the first argument; a
                // lambda needs nothing (it's rebuilt as Body{}).
                if constexpr (dp::fn_ptr<Body>) {
                    void (*fn)(Sink<Msg>, std::stop_token, Body, Args...) =
                        &dp::shim<Msg, Ok, Fail, Body, Body, Args...>;
                    return Self(type<Msg>{
                        ::jaal::detail::task_thunk<Msg>::make(fn, body, std::move(args)...),
                        k.where, std::move(k.name)});
                } else {
                    struct none {};
                    void (*fn)(Sink<Msg>, std::stop_token, none, Args...) =
                        &dp::shim<Msg, Ok, Fail, Body, none, Args...>;
                    return Self(type<Msg>{
                        ::jaal::detail::task_thunk<Msg>::make(fn, none{}, std::move(args)...),
                        k.where, std::move(k.name)});
                }
            }
            return Self{};
        }
        template <class Body, class... Args>
        static void explain() {
            namespace dt = ::jaal::detail_task;
            if constexpr (!(Sendable<Args> && ...))
                static_assert((Sendable<Args> && ...),
                    "jaal: a task argument is not Sendable; pass owned values "
                    "(std::string, not std::string_view or a pointer). A capability is "
                    "never Sendable: the body asks for its own `const jaal::unlocked&`.");
            else if constexpr (!dt::callable_body<Body, Msg, Args...>)
                static_assert(dt::callable_body<Body, Msg, Args...>,
                    "jaal: a task body must be callable as "
                    "([const jaal::unlocked&,] Sink<Msg>, [std::stop_token,] Args...) "
                    "with the arguments given after it");
            else
                static_assert(dt::captureless<Body>,
                    "jaal: a task body must not capture anything; pass what it "
                    "needs as arguments after the body");
        }
    };
};

inline constexpr placement isolated = placement::isolated;

/// Stop the keyed task with this key, if one is running. Its stop_token
/// fires, and nothing it sends from then on reaches update(), even messages
/// already queued. Cancelling a key with nothing running does nothing.
///
///   return Cmd::cancel("search");
struct cancel {
    static constexpr std::string_view name = "cancel";
    template <class Msg> struct type {
        std::string key;
    };
    template <class F, class M>
    static auto fmap(F&&, type<M> e) -> type<std::invoke_result_t<F, M>> {
        return {std::move(e.key)};
    }
    template <class Id, class F, class M>
    static auto fmap_with(const Id&, F&&, type<M> e)
        -> type<std::invoke_result_t<F, const Id&, M>> {
        return {std::move(e.key)};
    }
    template <class Self, class Msg> struct ctors {
        [[nodiscard]] static Self cancel(task_key k) { return Self(type<Msg>{std::move(k.name)}); }
    };
};

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
