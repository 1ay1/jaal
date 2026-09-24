#pragma once
// jaal::kernel faults — what happens when program code throws.
//
// update() changes the model in place. If it throws halfway, the model is
// half-changed: whatever update did before the throw stays done. So "just
// catch it" isn't enough; the kernel has to decide, per program, whether
// it can put back the last good model.
//
//   fault_policy::stop      (default) report the fault and quit with exit
//                           code 70. The model is left as update left it
//                           (possibly half-changed): it's only read for
//                           the final report, never folded into again.
//                           The safe default: a program that threw is in a
//                           state its author didn't plan for.
//   fault_policy::skip      report the fault, drop the message that caused
//                           it, keep going with the last good model. For
//                           long-running programs (servers, agents) that
//                           would rather lose one message than stop.
//
// Keeping the last good model needs a copy taken BEFORE update runs, and
// that costs every message. It's only paid when it buys something:
//   * copyable model + policy skip:  copy before each update; on a throw,
//     restore it. Strong guarantee: the model is exactly as before.
//   * move-only model, or policy stop: no copy. A move-only model can't be
//     restored, so `skip` with one becomes `stop`, reported once at start
//     (see kernel::options) rather than silently weakened.
//
// Faults from subscribe() and from effects run on the loop are handled the
// same way. Faults from TASKS (worker threads) are reported separately (see
// task_fault) because the model isn't involved.

#include <cstdint>
#include <exception>
#include <functional>
#include <string>
#include <string_view>

namespace jaal {

enum class fault_policy : std::uint8_t { stop, skip };

/// Where a fault happened.
enum class fault_site : std::uint8_t {
    update,        // P::update threw
    subscribe,     // P::subscribe threw
    effect,        // a host handle()/start_source() threw on the loop
    task,          // a task (pool or isolated) threw on a worker thread
};

[[nodiscard]] constexpr std::string_view to_string(fault_site s) noexcept {
    switch (s) {
        case fault_site::update:    return "update";
        case fault_site::subscribe: return "subscribe";
        case fault_site::effect:    return "effect";
        case fault_site::task:      return "task";
    }
    return "?";
}

/// A fault, as the kernel reports it.
struct fault {
    fault_site         site;
    std::exception_ptr error;       // rethrow it to inspect
    std::string        what;        // e.what() if it was a std::exception
    bool               model_kept;  // the model is exactly as before the fault
    bool               stopping;    // the kernel is quitting because of it

    [[nodiscard]] static std::string describe(const std::exception_ptr& e) {
        if (!e) return {};
        try { std::rethrow_exception(e); }
        catch (const std::exception& x) { return x.what(); }
        catch (...) { return "non-std exception"; }
    }
};

/// Called on the LOOP thread for every fault. Default: print one line to
/// stderr. Must not throw (a throwing handler is itself caught and ignored).
using fault_handler = std::function<void(const fault&)>;

/// Exit code for a program stopped by a fault (EX_SOFTWARE).
inline constexpr int fault_exit_code = 70;

}  // namespace jaal
