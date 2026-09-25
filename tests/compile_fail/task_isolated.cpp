// task_isolated shares task's safety contract. These prove it, because the
// two are separate factories and a future refactor could easily relax one.
//
// agentty runs every device login and every thread load on this path, so
// "isolated" must not quietly mean "less checked".
#include <jaal/jaal.hpp>

#include <string>

using namespace jaal;

struct Msg { int v = 0; };
using TCmd = jaal::Cmd<Msg>;          // not `Cmd`: that name is jaal's own

struct NotSendable {
    int* raw = nullptr;          // a bare pointer: may dangle on a worker
};

#if JAAL_CASE == 1
// A capturing body. The whole point of a captureless body is that nothing
// it touches can outlive the call that made it.
int main() {
    int local = 0;
    auto c = TCmd::task_isolated(
        [&local](Sink<Msg>, std::stop_token) { local = 1; });
    (void)c;
    return 0;
}

#elif JAAL_CASE == 2
// A non-Sendable argument. Sendable is what proves the value can cross to
// another thread; an isolated task crosses just as hard as a pooled one.
int main() {
    auto c = TCmd::task_isolated(
        [](Sink<Msg>, std::stop_token, NotSendable) {},
        NotSendable{});
    (void)c;
    return 0;
}

#elif JAAL_CASE == 3
// A body whose signature the runtime can't call. Getting this wrong must be
// an error, not a silently-ignored task — the same class of bug as a jaal
// hook whose signature reads as "absent".
int main() {
    auto c = TCmd::task_isolated([](int, int) {});
    (void)c;
    return 0;
}
#endif
