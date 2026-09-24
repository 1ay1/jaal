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
        return [this](const Msg& m) { log_.with([&](auto& v) { v.push_back(m); }); };
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
[[nodiscard]] typename P::Model replay_from(typename P::Model model,
                                            const std::vector<typename P::Msg>& msgs);

/// Replay recorded messages through P::update from P's initial model, with
/// no effects run. Returns the final model.
template <Program P>
[[nodiscard]] typename P::Model replay(const std::vector<typename P::Msg>& msgs) {
    auto [model, init_cmd] = run_init<P>();
    (void)init_cmd;                          // effects are outputs: not re-run
    return replay_from<P>(std::move(model), msgs);
}

/// Replay `msgs` on top of a model you already have: a snapshot plus the
/// journal written since it. With a snapshot every N messages, recovery
/// folds at most N instead of the whole history.
template <Program P>
[[nodiscard]] typename P::Model replay_from(typename P::Model model,
                                            const std::vector<typename P::Msg>& msgs) {
    for (const auto& m : msgs) {
        auto [next, cmd] = detail::prog::split(P::update(std::move(model), m));
        model = std::move(next);
        (void)cmd;
    }
    return model;
}

/// Replay and hand every intermediate model to `on_model` (for bisecting
/// where a run went wrong: the first model that fails a check).
template <Program P, class F>
    requires std::invocable<F&, std::size_t, const typename P::Model&>
void replay_each(const std::vector<typename P::Msg>& msgs, F&& on_model) {
    auto [model, init_cmd] = run_init<P>();
    (void)init_cmd;
    on_model(std::size_t{0}, std::as_const(model));
    for (std::size_t i = 0; i < msgs.size(); ++i) {
        auto [next, cmd] = detail::prog::split(P::update(std::move(model), msgs[i]));
        model = std::move(next);
        (void)cmd;
        on_model(i + 1, std::as_const(model));
    }
}

}  // namespace jaal
