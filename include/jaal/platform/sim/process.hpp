#pragma once
// jaal::platform::sim_process — a child whose BEHAVIOUR is scripted, running
// on real handles.
//
// The temptation with a sim backend is to fake the OS: invent handles, invent
// readiness, and run the whole suite against a model of poll(). That buys
// determinism and loses the thing worth testing — a second handle universe
// agrees with itself by construction, and the real reactors never see it.
//
// So this fakes the other half. The handles are REAL pipes, which means every
// reactor backend watches a sim child exactly as it watches `/bin/sh`, and
// the Reactor contract is exercised rather than modelled. What is simulated
// is the PROGRAM: it does what the script says, when the test says, with no
// fork, no exec, no threads and no scheduler.
//
// That split is what makes the ordering rules testable at all:
//
//   "exit readiness observed late is not lost"   on a real OS this is a race
//                                                you can only lose sometimes;
//                                                here the test just doesn't
//                                                look until step 5.
//   "stop() works with a full pipe, no reader"   needs the pipe to be full,
//                                                which needs a writer that
//                                                keeps going until it can't —
//                                                a step, not a sleep.
//   "output before exit survives the exit"       needs bytes in flight at the
//                                                instant of exit, which is one
//                                                ordering out of many on a
//                                                real scheduler and exactly
//                                                one here.
//
// What this backend CANNOT model is anything about real process trees: a
// double-forked, setsid() grandchild is a fact about the kernel, not about a
// program's behaviour. Those checks belong to the POSIX fixture, and the
// suite says so rather than pretending sim covers them.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "../../core/error.hpp"
#include "../../core/sendable.hpp"
#include "../handle.hpp"
#include "../process.hpp"

namespace jaal::platform {

/// What a scripted child does, one step at a time.
///
/// A step is performed when the test asks for it (`step()`), never on a
/// clock and never on another thread. "When" is therefore a count the test
/// controls, which is the whole point: every interleaving this suite cares
/// about is reachable, and reachable the same way on every run.
struct sim_step {
    enum class kind : std::uint8_t {
        write_out,   ///< emit bytes on stdout
        write_err,   ///< emit bytes on stderr (or stdout, when merged)
        fill_pipe,   ///< write until the pipe refuses more, then stop
        exit_ok,     ///< exit(0)
        exit_code,   ///< exit(n)
        killed,      ///< die by signal n, as if from outside
    };
    kind         what = kind::exit_ok;
    std::string  bytes;        ///< for write_out / write_err
    std::int32_t value = 0;    ///< for exit_code / killed
};

/// THE SIM PROGRAM IS ITS ARGV.
///
/// A real child is identified by the program you name; so is this one. The
/// script is the argv, one step per argument:
///
///     {"sim", "out:hello", "fill", "exit:3"}
///
/// grammar:  out:<bytes> | err:<bytes> | fill | exit[:n] | kill:<sig>
///
/// The first draft kept the script in a thread_local that spawn() consumed,
/// and justified it in a comment -- the concept mandates a static
/// `P::spawn(spec)`, so where else would it go? jaal's own banlist answered
/// that: nowhere, because hidden per-thread state is the thing this runtime
/// exists to not have. Putting the script where a program's identity already
/// lives removes the question instead of excusing it, and the request stays
/// pure data that two threads can spawn concurrently without sharing
/// anything.
[[nodiscard]] std::vector<sim_step> parse_sim_script(
    const std::vector<std::string>& argv);

/// A scripted child. Satisfies Process, so the conformance suite runs
/// against it unmodified.
class sim_process {
  public:
    using handle = native_handle;

    sim_process(sim_process&&) noexcept;
    sim_process& operator=(sim_process&&) noexcept;
    sim_process(const sim_process&)            = delete;
    sim_process& operator=(const sim_process&) = delete;
    ~sim_process();

    [[nodiscard]] static result<sim_process> spawn(const process_spec&);

    [[nodiscard]] borrowed_handle                exit_handle()   const;
    [[nodiscard]] std::optional<borrowed_handle> stdout_handle() const;
    [[nodiscard]] std::optional<borrowed_handle> stderr_handle() const;
    [[nodiscard]] std::optional<borrowed_handle> stdin_handle()  const;

    [[nodiscard]] result<void>        stop(stop_mode, stop_scope);
    [[nodiscard]] result<exit_status> reap();

    /// Honest: a simulated child has no real descendants, so `tree` can only
    /// ever mean `leader` here. Saying false is what keeps the suite's
    /// tree checks from passing vacuously.
    [[nodiscard]] bool tree_is_exact() const noexcept;
    [[nodiscard]] std::uint64_t id()   const noexcept;

    // ── the simulation surface ──────────────────────────────────────────
    //
    // Not part of Process: the suite drives a sim child through these and a
    // real child through its fixture, which is exactly the seam that lets
    // one suite cover both.

    /// Perform the next scripted step. Returns false when the script is
    /// spent. Performing a step after the child has ended is a no-op, not
    /// an error — a program that exits stops doing things.
    bool step();

    /// Run every remaining step.
    void run_to_completion();

    /// Has the scripted child ended (exited or been stopped)?
    [[nodiscard]] bool ended() const noexcept;

  private:
    sim_process();
    struct impl;
    std::unique_ptr<impl> p_;
};

static_assert(Process<sim_process>,
              "the sim backend must satisfy the same concept as the real "
              "ones, or the one-suite-every-backend contract is a fiction");

}  // namespace jaal::platform

// Same ownership as the real backends.
template <> inline constexpr bool jaal::sendable_opt_in<jaal::platform::sim_process> = true;
