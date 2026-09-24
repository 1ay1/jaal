#pragma once
// jaal::given<P> — test update() directly, no kernel, no threads, no clock.
//
// update is pure, so a test is: a model, some messages, then look at the
// new model and at the effects it asked for. Effects are data, so they're
// inspected, not mocked.
//
//   auto t = jaal::given<Search>()            // from init(), or given<Search>(model)
//                .when(Type{1})
//                .when(Type{2});
//   t.expect("query is the latest", [](auto& m) { return m.query == 2; });
//   t.expect_effect<jaal::fx::task>(1);        // the last update asked for one task
//   t.settle();                                // run those tasks inline, fold what they send
//   t.expect("results shown", [](auto& m) { return m.shown == 2; });
//   CHECK(t.ok());                             // or: puts(t.report().c_str())
//
// What each step does:
//   * when(msg)    folds msg; the Cmd it returned becomes "the last Cmd"
//   * settle()     runs the last Cmd's tasks inline (in order, on this
//                  thread) and `now` with the given time, folds every
//                  message they produce, and repeats for the Cmds THOSE
//                  folds return, until no task is left (bounded). `after`
//                  and `quit` aren't run: they're recorded (after(), quit())
//   * expect(...)  records a failure with its name instead of stopping, so
//                  one report lists everything that broke
//
// When the model has a formatter or is a plain struct, failures show a
// field diff against the model before the last message.

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "../core/cmd.hpp"
#include "../core/core_fx.hpp"
#include "../core/diff.hpp"
#include "../core/program.hpp"
#include "../core/rng.hpp"
#include "../core/sink.hpp"
#include "../core/sub.hpp"

namespace jaal {

namespace detail::given {

// A mailbox that appends to a vector. Tasks run inline post into it.
template <class Msg>
struct collect final : mailbox_iface<Msg> {
    std::vector<Msg> got;
    bool post(Msg m) override { got.push_back(std::move(m)); return true; }
};

}  // namespace detail::given

template <Program P>
    requires std::copy_constructible<typename P::Model>
class given {
public:
    using model_type = typename P::Model;
    using msg_type   = typename P::Msg;
    using cmd_type   = cmd_of<P>;
    using time_point = fx::now::time_point;

    /// The seed `random` effects draw from unless with_seed() says otherwise.
    /// Fixed, so a test that rolls dice asserts on exact numbers.
    static constexpr std::uint64_t default_seed = 0x1234'5678'9ABC'DEF0ULL;

    /// Start from init() (its Cmd is the last Cmd, so settle() runs it).
    given() {
        auto [m, c] = prog::init<P>();
        model_.emplace(std::move(m));
        last_ = std::move(c);
    }

    /// Start from a given model, with no pending Cmd.
    explicit given(model_type m) { model_.emplace(std::move(m)); }

    // ── acting ──────────────────────────────────────────────────────────
    given& when(msg_type msg) {
        if (!model_) return *this;          // an earlier update threw: nothing to fold into
        before_ = *model_;
        try {
            last_ = prog::update<P>(*model_, std::move(msg));
            ++folds_;
            note(last_);
        } catch (const std::exception& e) {
            fail(std::string("update threw: ") + e.what());
            model_.reset();
        } catch (...) {
            fail("update threw a non-std exception");
            model_.reset();
        }
        return *this;
    }

    /// Several messages in order.
    template <class... Ms>
    given& when_all(Ms&&... ms) { (when(msg_type(std::forward<Ms>(ms))), ...); return *this; }

    /// The time a `now` effect gets during settle().
    given& at_time(time_point t) { now_ = t; return *this; }

    /// The seed `random` effects draw from during settle(). Call before the
    /// draws you care about; it restarts the stream.
    given& with_seed(std::uint64_t seed) { rng_ = rng{seed}; return *this; }

    /// Run tasks and `now` from the last Cmd inline, fold their messages,
    /// repeat for what those return. Stops after `max_rounds` rounds so a
    /// task that always starts another can't loop forever (reported).
    given& settle(std::size_t max_rounds = 1000) {
        for (std::size_t round = 0; round < max_rounds; ++round) {
            std::vector<msg_type> produced;
            run_inline(std::move(last_), produced);
            last_ = cmd_type{};
            if (produced.empty()) return *this;
            for (auto& m : produced) {
                cmd_type keep = std::move(last_);
                when(std::move(m));
                // Several messages each return a Cmd: collect them all so
                // the next round runs every one of them.
                last_ = cmd_type::batch(std::move(keep), std::move(last_));
            }
        }
        fail("settle: still producing messages after max_rounds; a task keeps starting tasks?");
        return *this;
    }

    // ── looking ─────────────────────────────────────────────────────────
    /// The current model. Precondition: no update threw (check ok()).
    [[nodiscard]] const model_type& model() const { return *model_; }
    [[nodiscard]] bool threw() const noexcept { return !model_; }

    /// The Cmd the last update returned (after settle(): what's left of it).
    [[nodiscard]] const cmd_type& cmd() const noexcept { return last_; }

    /// Effects of kind D in the last Cmd (batches flattened), in order.
    template <Effect D>
    [[nodiscard]] std::vector<const payload_t<D, msg_type>*> effects() const {
        std::vector<const payload_t<D, msg_type>*> out;
        collect<D>(last_, out);
        return out;
    }

    /// Every `after` and `quit` asked for since the start (settle doesn't
    /// run them), in order.
    struct timer { std::chrono::milliseconds delay; msg_type msg; };
    [[nodiscard]] const std::vector<timer>& after() const noexcept { return afters_; }
    [[nodiscard]] std::optional<int> quit() const noexcept { return quit_; }

    /// The subscriptions for the current model.
    [[nodiscard]] auto subs() const { return prog::subscribe<P>(*model_); }

    // ── expecting ───────────────────────────────────────────────────────
    template <class F>
        requires std::is_invocable_r_v<bool, F&, const model_type&>
    given& expect(std::string name, F f) {
        if (!model_) { fail(name + ": no model (update threw)"); return *this; }
        bool good = false;
        try { good = f(*model_); } catch (...) {}
        if (!good) {
            std::string why = name;
            if (before_) {
                auto d = diff(*before_, *model_);
                if (!d.empty()) why += "\n    last message changed:\n    " + indent(to_string(d));
            }
            fail(std::move(why));
        }
        return *this;
    }

    /// The last Cmd asked for exactly n effects of kind D.
    template <Effect D>
    given& expect_effect(std::size_t n) {
        const auto k = effects<D>().size();
        if (k != n)
            fail("expected " + std::to_string(n) + " '" + std::string(D::name)
                 + "' effect(s), got " + std::to_string(k));
        return *this;
    }

    /// The last Cmd asked for nothing.
    given& expect_no_effects() {
        if (!last_.is_none()) fail("expected no effects");
        return *this;
    }

    [[nodiscard]] bool ok() const noexcept { return failures_.empty(); }
    [[nodiscard]] const std::vector<std::string>& failures() const noexcept { return failures_; }
    [[nodiscard]] std::size_t folds() const noexcept { return folds_; }

    /// Every failure, one per line (with diffs), or "ok".
    [[nodiscard]] std::string report() const {
        if (failures_.empty()) return "ok";
        std::string s;
        for (auto& f : failures_) { s += "  - "; s += f; s += '\n'; }
        return s;
    }

private:
    void fail(std::string why) { failures_.push_back(std::move(why)); }

    static std::string indent(std::string s) {
        std::string out;
        for (char c : s) { out += c; if (c == '\n') out += "    "; }
        while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) out.pop_back();
        return out;
    }

    // Record after/quit as they're asked for (they're never run here).
    void note(const cmd_type& c) {
        std::visit([&]<class X>(const X& x) {
            using U = std::remove_cvref_t<X>;
            if constexpr (std::same_as<U, typename cmd_type::Batch>) {
                for (auto& inner : x.cmds) note(inner);
            } else if constexpr (in_row<fx::after, typename cmd_type::row_type>
                                 && std::same_as<U, payload_t<fx::after, msg_type>>) {
                if constexpr (std::copy_constructible<msg_type>) afters_.push_back({x.delay, x.msg});
            } else if constexpr (in_row<fx::quit, typename cmd_type::row_type>
                                 && std::same_as<U, payload_t<fx::quit, msg_type>>) {
                if (!quit_) quit_ = x.code;
            }
        }, c.inner);
    }

    template <class D, class Out>
    static void collect(const cmd_type& c, Out& out) {
        std::visit([&]<class X>(const X& x) {
            using U = std::remove_cvref_t<X>;
            if constexpr (std::same_as<U, typename cmd_type::Batch>) {
                for (auto& inner : x.cmds) collect<D>(inner, out);
            } else if constexpr (std::same_as<U, payload_t<D, msg_type>>) {
                out.push_back(&x);
            }
        }, c.inner);
    }

    void run_inline(cmd_type c, std::vector<msg_type>& produced) {
        std::visit([&]<class X>(X&& x) {
            using U = std::remove_cvref_t<X>;
            if constexpr (std::same_as<U, typename cmd_type::Batch>) {
                for (auto& inner : x.cmds) run_inline(std::move(inner), produced);
            } else if constexpr (in_row<fx::task, typename cmd_type::row_type>
                                 && std::same_as<U, payload_t<fx::task, msg_type>>) {
                auto box = std::make_shared<detail::given::collect<msg_type>>();
                auto sink = sink_access::make<msg_type>(
                    std::weak_ptr<detail::mailbox_iface<msg_type>>(box));
                try {
                    std::move(x.thunk).run(std::move(sink), std::stop_token{});
                } catch (const std::exception& e) {
                    fail(std::string("a task threw: ") + e.what());
                } catch (...) {
                    fail("a task threw a non-std exception");
                }
                for (auto& m : box->got) produced.push_back(std::move(m));
            } else if constexpr (in_row<fx::now, typename cmd_type::row_type>
                                 && std::same_as<U, payload_t<fx::now, msg_type>>) {
                produced.push_back(x.to_msg(now_));
            } else if constexpr (in_row<fx::random, typename cmd_type::row_type>
                                 && std::same_as<U, payload_t<fx::random, msg_type>>) {
                produced.push_back(x.to_msg(rng_));
            } else if constexpr (in_row<fx::send, typename cmd_type::row_type>
                                 && std::same_as<U, payload_t<fx::send, msg_type>>) {
                produced.push_back(std::move(x.msg));
            }
        }, std::move(c.inner));
    }

    std::optional<model_type> model_;
    std::optional<model_type> before_;
    cmd_type                  last_{};
    std::vector<timer>        afters_;
    std::optional<int>        quit_;
    time_point                now_{};
    rng                       rng_{default_seed};
    std::size_t               folds_ = 0;
    std::vector<std::string>  failures_;
};

}  // namespace jaal
