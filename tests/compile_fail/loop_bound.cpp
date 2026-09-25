// loop_bound<T> is state only the loop thread may touch. The guarantee is
// that a task body cannot reach it, and it rests on two things:
//
//   1. get() needs a loop_token, which only the kernel mints (loop_key's
//      constructor is explicit and the kernel is its only caller).
//   2. a token can be neither copied nor moved, so it cannot be smuggled
//      into a lambda — and task bodies are captureless anyway, so there is
//      no way to name one from inside.
//
// Each case below tries one of the routes in and must not compile.
#include <jaal/jaal.hpp>
#include <jaal/kernel/loop.hpp>

using namespace jaal;

struct Msg { int v = 0; };

kernel::loop_bound<int> g_state{7};

#if JAAL_CASE == 1
// Reaching the value without proof of being on the loop.
int main() {
    return g_state.get();          // no token: get() is not callable this way
}

#elif JAAL_CASE == 2
// Forging a token. loop_key's constructor is explicit, so `{}` does not
// convert — a worker cannot conjure the proof.
int main() {
    kernel::loop_token t{{}};
    return g_state.get(t);
}

#elif JAAL_CASE == 3
// Capturing a token into a lambda that a worker could run. The token is
// non-copyable and non-movable precisely so this cannot be written.
int main() {
    kernel::loop_token t{kernel::loop_key{}};
    auto smuggle = [t = std::move(t)]() { return 0; };
    return smuggle();
}

#elif JAAL_CASE == 4
// Copying a token out of a loop callback into something longer-lived.
void on_loop(const kernel::loop_token& t) {
    static kernel::loop_token stash = t;    // copy ctor is deleted
    (void)stash;
}
int main() { return 0; }
#endif
