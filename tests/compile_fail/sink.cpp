// A Sink is a WEAK handle to the loop's mailbox, and the safety story
// depends on two things nobody can write around:
//
//   1. Only the kernel mints one (the constructor is private; sink_access is
//      the sole friend). A forged Sink would be a raw pointer to loop state
//      handed to a worker — exactly what the type exists to prevent.
//   2. It holds a WEAK reference, so a worker outliving the loop finds the
//      mailbox gone and send() returns false, rather than writing into
//      freed memory.
//
// The default constructor is public on purpose: it makes a CLOSED sink, and
// a closed sink is harmless — send() just answers false.
#include <jaal/jaal.hpp>

#include <memory>

using namespace jaal;

struct Msg { int v = 0; };

#if JAAL_CASE == 1
// Forging a Sink around a mailbox you got hold of some other way.
int main() {
    std::weak_ptr<detail::mailbox_iface<Msg>> box;
    Sink<Msg> forged{box};                  // ctor is private
    return forged.open() ? 1 : 0;
}

#elif JAAL_CASE == 2
// Reaching into the box a Sink holds, to keep the loop's mailbox alive past
// the loop — a weak handle that can be upgraded and stored is a strong one.
int main() {
    Sink<Msg> s;
    auto keep = s.box_;                     // private member
    (void)keep;
    return 0;
}

#elif JAAL_CASE == 3
// Reaching the mailbox through a Sink you legitimately hold. `box_` is the
// weak handle itself: upgrading and storing it would turn a weak route into
// the loop into a strong one that outlives it.
int main() {
    Sink<Msg> s;
    auto strong = s.box_.lock();            // private member
    return strong ? 1 : 0;
}
#endif

// NOT a case, and worth saying why: `sink_access::make` IS public, by
// design. It is the kernel's minting key, and several of jaal's own headers
// (fx, stream, mailbox, given) need it across namespace boundaries where a
// friend declaration would not reach. What keeps app code out of it is the
// ban-list (tests/lint/banlist.cmake), which allows `sink_access` only in
// the handful of files that implement the safe types.
//
// That check runs over jaal's OWN sources. A consumer forging a sink is
// outside its reach — the ban-list is a jaal-internal discipline, not a
// property of the API. Consumers that care should run the same check over
// their tree.
