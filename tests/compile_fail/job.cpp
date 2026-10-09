// Jobs on pool and worker_group follow the task rule. Each case must NOT
// compile: a job that borrows a local or carries a pointer is exactly the
// use-after-free the rule exists to rule out.
#include <jaal/core/co_owned.hpp>
#include <jaal/kernel/pool.hpp>
#include <jaal/kernel/worker_group.hpp>

#include <stop_token>
#include <string>

struct Plain { std::string s; };   // writable: not Sync

#if JAAL_CASE == 1
// A capturing job. `local` dies when f returns; the job may still run.
void f(jaal::kernel::pool& p) {
    int local = 0;
    p.post([&local](std::stop_token) { local = 1; });
}
#elif JAAL_CASE == 2
// A raw pointer argument: may point at memory another thread frees.
void f(jaal::kernel::pool& p, int* raw) {
    p.post([](std::stop_token, int* r) { *r = 1; }, raw);
}
#elif JAAL_CASE == 3
// worker_group has the same gate.
void f(jaal::kernel::worker_group& g) {
    std::string s;
    g.post([&s](std::stop_token) { s += "x"; });
}
#elif JAAL_CASE == 4
// submit too.
void f(jaal::kernel::pool& p) {
    int local = 0;
    (void)p.submit([&local](std::stop_token) { return local; });
}
#elif JAAL_CASE == 5
// Sharing state that isn't Sync: co_owned refuses it.
void f() { (void)jaal::co_owned<Plain>::make(); }
#elif JAAL_CASE == 6
// A body that ignores the token's position: wrong shape.
void f(jaal::kernel::pool& p) { p.post([] {}); }
#else
#error "no case selected"
#endif
