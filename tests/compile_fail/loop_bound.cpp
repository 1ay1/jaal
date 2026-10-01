// loop_bound<T> is state only the loop thread may touch. The guarantee is
// that a worker cannot reach it, and it rests on three things:
//
//   1. get() needs a loop_token.
//   2. loop_key's constructor is PRIVATE, so a token cannot be minted by
//      naming the key. Only the kernel (through loop_detail::minter, its one
//      friend) and on_loop() — which CHECKS the calling thread first — can
//      produce one.
//   3. a token can be neither copied nor moved, so it cannot be smuggled
//      out of the callback that was handed one, and task bodies are
//      captureless anyway.
//
// Point 2 used to be weaker than it looked: loop_key's ctor was
// public-and-explicit. That stops `loop_token t{{}}` (CASE 2) because
// explicit defeats brace elision — but not `loop_token t{loop_key{}}`, which
// is two more characters and which CASE 3 below used to spell as a SETUP
// step, so nothing tested it. Any worker could forge proof. CASE 3 is now
// that forge, on its own.
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
// Forging a token by brace elision. loop_key's constructor is explicit, so
// `{}` does not convert.
int main() {
    kernel::loop_token t{{}};
    return g_state.get(t);
}

#elif JAAL_CASE == 3
// Forging a token by NAMING the key. loop_key's constructor is private, so
// a worker cannot conjure the proof even spelled out in full. This is the
// case that used to compile.
int main() {
    kernel::loop_token t{kernel::loop_key{}};
    return g_state.get(t);
}

#elif JAAL_CASE == 4
// Capturing a token into a lambda that a worker could run. The token is
// non-copyable and non-movable precisely so this cannot be written.
int main() {
    kernel::on_loop([](const kernel::loop_token& t) {
        auto smuggle = [t = std::move(t)]() { return 0; };
        return smuggle();
    });
    return 0;
}

#elif JAAL_CASE == 5
// Copying a token out of a loop callback into something longer-lived.
void handler(const kernel::loop_token& t) {
    static kernel::loop_token stash = t;    // copy ctor is deleted
    (void)stash;
}
int main() { return 0; }

#elif JAAL_CASE == 6
// Returning a reference to loop-bound state out of on_loop(), which would
// let a caller keep the handle after the proof is gone. Sendable's escape
// check rejects it the same way guarded<T> does.
int main() {
    int& leaked = kernel::on_loop([](const kernel::loop_token& t) -> int& {
        return g_state.get(t);
    });
    return leaked;
}

#elif JAAL_CASE == 7
// A loop_bound is pinned to one thread, so it must not be Sendable — i.e.
// it cannot ride along as a task argument into a worker.
int main() {
    using C = jaal::Cmd<Msg>;
    auto body = [](Sink<Msg>, std::stop_token, kernel::loop_bound<int>) {};
    return C::task(body, kernel::loop_bound<int>{3}), 0;
}
#endif
