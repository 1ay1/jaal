// Compile-fail cases for owned_handle / borrowed_handle. Each must NOT
// compile: the handle type earns its place by making double-close and
// silent int conversions impossible, and a guarantee nobody tests is a
// guarantee that quietly rots.

#include <jaal/platform/handle.hpp>

#include <utility>

using jaal::platform::borrowed_handle;
using jaal::platform::native_handle;
using jaal::platform::owned_handle;

void take_owned(owned_handle);
void take_borrow(borrowed_handle);

#if JAAL_CASE == 1
// copying an owner: two destructors, one descriptor, and the second close
// lands on whatever the OS handed out in the meantime.
void f(const owned_handle& h) {
    owned_handle copy = h;
    (void)copy;
}
#elif JAAL_CASE == 2
// copy-assigning an owner, same bug by another route.
void f(owned_handle& a, const owned_handle& b) { a = b; }
#elif JAAL_CASE == 3
// passing an owner by value without saying so: this is a move of ownership
// and has to be written down.
void f(owned_handle& h) { take_owned(h); }
#elif JAAL_CASE == 4
// a raw descriptor is not an owner. Adopting one is explicit, always, so
// that the places where ownership is claimed are greppable.
void f(int fd) { take_owned(fd); }
#elif JAAL_CASE == 5
// nor does an owner decay back into a number: no accidental ::close(h).
void f(owned_handle h) {
    int fd = h;
    (void)fd;
}
#elif JAAL_CASE == 6
// a borrow cannot close what it does not own. The operation is absent, not
// merely discouraged.
void f(borrowed_handle b) { b.reset(); }
#elif JAAL_CASE == 7
// nor can a borrow give away ownership it never had.
void f(borrowed_handle b) {
    auto raw = b.release();
    (void)raw;
}
#elif JAAL_CASE == 8
// a borrow is not an owner: handing one to an API that will close it must
// not compile.
void f(borrowed_handle b) { take_owned(b); }
#elif JAAL_CASE == 9
// and an owner is not silently a borrow either — .borrow() says it.
void f(owned_handle h) { take_borrow(h); }
#else
#error "no case selected"
#endif
