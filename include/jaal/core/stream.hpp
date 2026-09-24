#pragma once
// jaal::fx::stream — long-running background work, as a subscription.
//
// A task runs once and ends. A lot of real work doesn't end on its own: a
// file watcher, a socket reader, a download with progress, a poll loop, a
// child process's output. The Elm answer is a SUBSCRIPTION: the program
// says "while I'm in this state, keep this running", and the runtime starts
// it, keeps it across model changes, and stops it the moment the program
// stops asking.
//
//   static Sub subscribe(const Model& m) {
//       if (!m.downloading) return Sub::none();
//       return Sub::stream("dl:" + m.url,                    // key
//           [](Sink<Msg> out, std::stop_token st, std::string url) {
//               for (auto chunk : fetch(url, st)) {          // honour st
//                   if (st.stop_requested()) return;
//                   out.send(Progress{chunk.size()});
//               }
//               out.send(Done{});
//           },
//           m.url);                                          // args, by value
//   }
//
// Rules (same safety model as tasks, docs/concurrency.md §4.6):
//   * the body is captureless; everything it needs is an argument, and every
//     argument is Sendable
//   * the KEY decides identity. Same key next subscribe(): the running
//     stream is kept, not restarted. Key gone: its stop_token fires. New
//     key: a fresh run starts.
//   * the body gets a Sink (weak) and a stop_token. When the program stops
//     subscribing, the token fires; a body that ignores it keeps running
//     but its sends are DISCARDED (the kernel drops messages from a stream
//     generation it has stopped), so a late message from a cancelled
//     stream can't land in a model that no longer expects it.
//   * a stream that returns on its own just ends. It isn't restarted while
//     its key stays subscribed: re-keying (or dropping and re-adding the
//     key) starts a new run.
//   * runs on a dedicated thread per stream (like an isolated task): streams
//     are long-lived and often block, and must not starve the task pool.

#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "effect.hpp"
#include "fx.hpp"
#include "sendable.hpp"
#include "sink.hpp"

namespace jaal {

namespace detail {

// A stream body, erased once: a function pointer plus Sendable args, held
// in a shared, immutable factory so the kernel can start it again (after a
// drop and re-subscribe) without the program re-creating it. Starting
// COPIES the args into the new run, so every run owns its inputs.
template <class Msg>
class stream_factory {
public:
    template <class... Args>
        requires (std::copy_constructible<Args> && ...)
    static stream_factory make(void (*fn)(Sink<Msg>, std::stop_token, Args...), Args... args) {
        return stream_factory(std::make_shared<impl<Args...>>(fn, std::move(args)...));
    }

    /// Start one run. Called on the stream's own thread.
    void run(Sink<Msg> out, std::stop_token st) const { f_->invoke(std::move(out), std::move(st)); }

    /// Re-target at another Msg type (Sub::map). The mapper is captureless,
    /// like a task's, because it runs on the stream's thread.
    template <class To>
    stream_factory<To> map(To (*g)(Msg)) const {
        return stream_factory<To>::from_mapped(*this, g);
    }

    /// Re-target with an ID carried by value: `g(id, msg)`. For a keyed list
    /// of children (core/children.hpp), where the mapper must know WHICH
    /// child a message came from but still can't capture.
    template <class To, class Id>
    stream_factory<To> map_with(Id id, To (*g)(const Id&, Msg)) const {
        return stream_factory<To>::from_mapped_with(*this, std::move(id), g);
    }

private:
    template <class> friend class stream_factory;

    struct base {
        virtual ~base() = default;
        virtual void invoke(Sink<Msg>, std::stop_token) const = 0;
    };
    template <class... Args>
    struct impl final : base {
        void (*fn)(Sink<Msg>, std::stop_token, Args...);
        std::tuple<Args...> args;
        impl(decltype(fn) f, Args... a) : fn(f), args(std::move(a)...) {}
        void invoke(Sink<Msg> out, std::stop_token st) const override {
            // copy the args for this run: the factory stays reusable
            std::apply([&](const Args&... a) { fn(std::move(out), std::move(st), Args(a)...); },
                       args);
        }
    };

    template <class From>
    static stream_factory from_mapped(stream_factory<From> inner, Msg (*g)(From)) {
        struct mapped final : base {
            stream_factory<From> inner;
            Msg (*g)(From);
            mapped(stream_factory<From> i, Msg (*h)(From)) : inner(std::move(i)), g(h) {}
            void invoke(Sink<Msg> out, std::stop_token st) const override {
                struct fwd final : mailbox_iface<From> {
                    Sink<Msg> out;
                    Msg (*g)(From);
                    fwd(Sink<Msg> o, Msg (*h)(From)) : out(std::move(o)), g(h) {}
                    bool post(From m) override { return out.send(g(std::move(m))); }
                };
                auto box = std::make_shared<fwd>(std::move(out), g);
                inner.run(sink_access::make<From>(std::weak_ptr<mailbox_iface<From>>(box)),
                          std::move(st));
            }
        };
        return stream_factory(std::make_shared<mapped>(std::move(inner), g));
    }

    // Same, with an owned id handed to every mapping call.
    template <class From, class Id>
    static stream_factory from_mapped_with(stream_factory<From> inner, Id id,
                                           Msg (*g)(const Id&, From)) {
        struct mapped final : base {
            stream_factory<From> inner;
            Id                   id;
            Msg (*g)(const Id&, From);
            mapped(stream_factory<From> i, Id k, Msg (*h)(const Id&, From))
                : inner(std::move(i)), id(std::move(k)), g(h) {}
            void invoke(Sink<Msg> out, std::stop_token st) const override {
                struct fwd final : mailbox_iface<From> {
                    Sink<Msg> out;
                    Id        id;
                    Msg (*g)(const Id&, From);
                    fwd(Sink<Msg> o, Id k, Msg (*h)(const Id&, From))
                        : out(std::move(o)), id(std::move(k)), g(h) {}
                    bool post(From m) override { return out.send(g(id, std::move(m))); }
                };
                auto box = std::make_shared<fwd>(std::move(out), id, g);
                inner.run(sink_access::make<From>(std::weak_ptr<mailbox_iface<From>>(box)),
                          std::move(st));
            }
        };
        return stream_factory(std::make_shared<mapped>(std::move(inner), std::move(id), g));
    }

    explicit stream_factory(std::shared_ptr<const base> f) noexcept : f_(std::move(f)) {}
    std::shared_ptr<const base> f_;       // immutable, shared across keeps
};

}  // namespace detail

namespace fx {

struct stream {
    static constexpr std::string_view name = "stream";

    template <class Msg> struct type {
        std::string                          key;
        ::jaal::detail::stream_factory<Msg>  body;
    };

    template <class F, class M>
    static auto fmap(F&& f, type<M> e) -> type<std::invoke_result_t<F, M>> {
        using To = std::invoke_result_t<F, M>;
        static_assert(detail_task::captureless_mapper<std::remove_cvref_t<F>, M>,
                      "jaal: Sub::map on a Sub holding a stream needs a captureless "
                      "mapper (it runs on the stream's thread)");
        To (*fp)(M) = f;
        return {std::move(e.key), e.body.map(fp)};
    }
    template <class Id, class F, class M>
    static auto fmap_with(const Id& id, F&& f, type<M> e)
        -> type<std::invoke_result_t<F, const Id&, M>> {
        using To = std::invoke_result_t<F, const Id&, M>;
        static_assert(std::is_convertible_v<std::remove_cvref_t<F>, To (*)(const Id&, M)>,
                      "jaal: mapping a Sub that holds a stream needs a captureless "
                      "mapper (it runs on the stream's thread)");
        static_assert(Sendable<Id>,
                      "jaal: the id a stream's mapper carries crosses to the stream "
                      "thread, so it must be Sendable");
        To (*fp)(const Id&, M) = f;
        return {std::move(e.key), e.body.template map_with<To, Id>(id, fp)};
    }

    // Source: identity is the key.
    using key_type = std::string;
    template <class M>
    static key_type key(const type<M>& e) { return e.key; }

    template <class Self, class Msg> struct ctors {
        template <class Body, class... Args>
            requires TaskBody<Body, Msg, Args...>
                  && (Sendable<Args> && ...)
                  && (std::copy_constructible<Args> && ...)
        [[nodiscard]] static Self stream(std::string key, Body body, Args... args) {
            void (*fn)(Sink<Msg>, std::stop_token, Args...) = body;
            return Self(type<Msg>{std::move(key),
                                  ::jaal::detail::stream_factory<Msg>::make(fn, std::move(args)...)});
        }
        // Wrong shape: say which rule it broke.
        template <class Body, class... Args>
            requires (!(TaskBody<Body, Msg, Args...> && (Sendable<Args> && ...)
                        && (std::copy_constructible<Args> && ...)))
        static Self stream(std::string, Body, Args...) {
            if constexpr (!(Sendable<Args> && ...))
                static_assert((Sendable<Args> && ...),
                    "jaal: a stream argument is not Sendable; pass owned values");
            else if constexpr (!(std::copy_constructible<Args> && ...))
                static_assert((std::copy_constructible<Args> && ...),
                    "jaal: a stream argument must be copyable: the stream can be "
                    "started again after a re-subscribe, and each run gets its own copy");
            else if constexpr (std::is_invocable_v<Body, Sink<Msg>, std::stop_token, Args...>)
                static_assert(TaskBody<Body, Msg, Args...>,
                    "jaal: a stream body must not capture anything; pass what it needs "
                    "as arguments after the body");
            else
                static_assert(TaskBody<Body, Msg, Args...>,
                    "jaal: a stream body must be callable as (Sink<Msg>, std::stop_token, Args...)");
            return Self{};
        }
    };
};

}  // namespace fx
}  // namespace jaal
