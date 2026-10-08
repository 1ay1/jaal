#pragma once
// jaal::replay — record a program's inputs, replay them, get the same run.
//
// An Elm program is a fold over messages: model_n = update(...update(init,
// m1)..., mn). So the messages, in order, ARE the run. Record them and the
// run can be replayed exactly, on the headless host, with no terminal, no
// network, no threads: a production bug report becomes a unit test.
//
//   // in production (or a failing test):
//   jaal::recording<Msg> rec;
//   auto k = K::start(host, clock, opt, wake, rec.hook());   // or headless<App>(opt, rec.hook())
//
//   // later, anywhere:
//   auto model = jaal::replay<App>(rec.messages());
//   assert(model == expected);
//
// What's recorded: every message update() folds, in fold order, whatever
// its source (host event, task result, timer, stream). Replaying folds
// them straight through update() and interprets NOTHING: effects are the
// run's OUTPUTS, and re-running them (network calls, file writes) is
// exactly what a replay must not do. The recorded messages already contain
// what those effects produced.
//
// This works because update() is pure. A program whose update reads a
// clock or a global breaks replay; that's the purity rule (docs/design.md 3.1)
// showing up as a test failure, which is the point.

#include <concepts>
#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

#include "../core/program.hpp"
#include "guarded.hpp"

namespace jaal {

/// Collects folded messages. Thread-safe to read while the loop records.
template <class Msg>
    requires std::copy_constructible<Msg>
class recording {
public:
    /// A hook for kernel::options::record.
    [[nodiscard]] std::function<void(const Msg&)> hook() {
        return [this](const Msg& m) { log_.with([](auto& v, Msg c) { v.push_back(std::move(c)); }, m); };
    }

    [[nodiscard]] std::vector<Msg> messages() const {
        return log_.read([](const auto& v) { return v; });
    }

    [[nodiscard]] std::size_t size() const {
        return log_.read([](const auto& v) { return v.size(); });
    }

private:
    guarded<std::vector<Msg>> log_;
};

template <Program P>
    requires(!detail::prog::clocked<P>)
[[nodiscard]] typename P::Model replay_from(typename P::Model model,
                                            const std::vector<typename P::Msg>& msgs);

/// Replay recorded messages through P::update from P's initial model, with
/// no effects run. Returns the final model.
///
/// Unclocked programs only. A clocked program's update depends on `now`, so
/// its messages alone don't determine the run; it replays from a
/// timed_recording (below), and this overload refuses it rather than fold
/// every message at the epoch.
template <Program P>
    requires(!detail::prog::clocked<P>)
[[nodiscard]] typename P::Model replay(const std::vector<typename P::Msg>& msgs) {
    auto [model, init_cmd] = prog::init<P>();
    (void)init_cmd;                          // effects are outputs: not re-run
    return replay_from<P>(std::move(model), msgs);
}

/// Replay `msgs` on top of a model you already have: a snapshot plus the
/// journal written since it. With a snapshot every N messages, recovery
/// folds at most N instead of the whole history.
template <Program P>
    requires(!detail::prog::clocked<P>)
[[nodiscard]] typename P::Model replay_from(typename P::Model model,
                                            const std::vector<typename P::Msg>& msgs) {
    for (const auto& m : msgs) {
        (void)prog::update<P>(model, m);
    }
    return model;
}

/// Replay and hand every intermediate model to `on_model` (for bisecting
/// where a run went wrong: the first model that fails a check).
template <Program P, class F>
    requires(!detail::prog::clocked<P>)
         && std::invocable<F&, std::size_t, const typename P::Model&>
void replay_each(const std::vector<typename P::Msg>& msgs, F&& on_model) {
    auto [model, init_cmd] = prog::init<P>();
    (void)init_cmd;
    on_model(std::size_t{0}, std::as_const(model));
    for (std::size_t i = 0; i < msgs.size(); ++i) {
        (void)prog::update<P>(model, msgs[i]);
        on_model(i + 1, std::as_const(model));
    }
}

/// Replay with the per-message time a clocked program's update depends on.
///
/// A clocked program's update takes `now`, so its messages alone are no
/// longer the whole run: the same message folded at a different instant can
/// produce a different model (a toast that has or hasn't expired, a stall
/// timer that has or hasn't fired). So a clocked run records each message
/// WITH the time it was folded at, and replay folds it at exactly that time.
/// D23 holds unchanged — the record is still the run's inputs, it simply
/// includes the one input update reads besides the message.
///
///   jaal::timed_recording<App> rec;
///   kernel.record_with_time(rec.hook());
///   auto model = jaal::replay<App>(rec.entries());
template <Program P>
    requires detail::prog::clocked<P>
class timed_recording {
public:
    using time_point = detail::prog::now_t<P>;
    struct entry {
        time_point          at;
        typename P::Msg     msg;
    };

    [[nodiscard]] std::function<void(const typename P::Msg&, time_point)> hook() {
        return [this](const typename P::Msg& m, time_point at) {
            log_.with([](auto& v, entry e) { v.push_back(std::move(e)); },
                      entry{at, m});
        };
    }

    [[nodiscard]] std::vector<entry> entries() const {
        return log_.read([](const auto& v) { return v; });
    }

private:
    guarded<std::vector<entry>> log_;
};

/// Fold a timed record through update, each message at its recorded time.
template <Program P>
    requires detail::prog::clocked<P>
[[nodiscard]] typename P::Model replay(
    const std::vector<typename timed_recording<P>::entry>& entries) {
    auto [model, init_cmd] = prog::init<P>();
    (void)init_cmd;
    for (const auto& e : entries)
        (void)prog::update<P>(model, e.msg, e.at);
    return model;
}

}  // namespace jaal
