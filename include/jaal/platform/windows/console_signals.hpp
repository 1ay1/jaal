#pragma once
// jaal::platform::console_signals — Windows console control events as
// jaal signals.
//
// Windows has no POSIX signals. Ctrl+C, closing the console window, logoff
// and shutdown arrive through SetConsoleCtrlHandler, which Windows calls on
// a NEW THREAD it creates. So, like the POSIX source, the handler only sets
// a bit and signals a manual-reset event that the reactor watches; the loop
// calls take() on its own thread.
//
//   CTRL_C_EVENT, CTRL_BREAK_EVENT        → interrupt
//   CTRL_LOGOFF_EVENT, CTRL_SHUTDOWN_EVENT → terminate
//   CTRL_CLOSE_EVENT                       → hangup
//
// `resize` and `child` aren't console control events. Console resize comes
// through console input records, which is a terminal host's job (maya), not
// this layer's; child exit is a waitable process handle, watched directly.
// install() leaves them out of watching(), so a program can see exactly
// what it got.
//
// One Windows-specific catch, stated plainly: for CTRL_CLOSE_EVENT Windows
// terminates the process a few seconds after the handler returns, no matter
// what. The handler returns TRUE (handled) and the loop has that time to
// clean up; it can't extend it.

#include <cstdint>
#include <utility>

#include "../../core/error.hpp"
#include "../signal.hpp"

namespace jaal::platform {

class console_signals {
public:
    using native_handle = void*;          // a manual-reset event HANDLE

    [[nodiscard]] static result<console_signals> install(signal_set wanted);

    console_signals(console_signals&& o) noexcept;
    console_signals& operator=(console_signals&& o) noexcept;
    console_signals(const console_signals&)            = delete;
    console_signals& operator=(const console_signals&) = delete;
    ~console_signals();

    [[nodiscard]] native_handle handle() const noexcept;
    [[nodiscard]] signal_set    take() noexcept;
    [[nodiscard]] signal_set    watching() const noexcept { return got_; }

private:
    console_signals(int slot, signal_set got) noexcept : slot_(slot), got_(got) {}
    void release() noexcept;
    int        slot_ = -1;
    signal_set got_{};
};

static_assert(SignalSource<console_signals>);

/// Test-only: run the console control handler with `type` (a CTRL_*_EVENT
/// value), as Windows would on its own thread.
namespace test { void console_ctrl(unsigned long type); }

}  // namespace jaal::platform
