#pragma once
// jaal::kernel tracing — see what a running program does.
//
// One hook, set in kernel::options::trace, called on the loop thread for
// each thing the kernel does. It costs one branch per event when unset.
//
//   opt.trace = [](const jaal::trace_event& e) {
//       if (e.kind == jaal::trace_kind::fold)
//           log("update {} took {}us", e.msg_index, e.duration.count());
//   };
//
// What it reports:
//   fold        update() ran for one message: the Msg's variant index (when
//               Msg is a variant) and how long update() took
//   effect      an effect was interpreted: its descriptor name
//   subscribe   subscribe() ran: how many sources started / stopped / kept
//   fault       a fault was reported (also goes to the fault handler)
//   step        one step() finished: messages folded, time spent
//
// Deliberately NOT a Msg dump: Msg types are the program's, and printing
// them is the program's call. The hook gets indices and names, which is
// enough to see load and timing without forcing every Msg to be printable.
// A program that wants full records can wrap update() itself.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

namespace jaal {

enum class trace_kind : std::uint8_t { fold, effect, subscribe, fault, step };

struct trace_event {
    trace_kind                kind;
    std::string_view          name{};        // effect name, or fault site
    std::size_t               msg_index = 0; // fold: Msg's variant index (0 if not a variant)
    std::size_t               started = 0;   // subscribe
    std::size_t               stopped = 0;   // subscribe
    std::size_t               kept    = 0;   // subscribe
    std::size_t               folded  = 0;   // step
    std::chrono::nanoseconds  duration{};    // fold, step
};

using trace_hook = std::function<void(const trace_event&)>;

}  // namespace jaal
