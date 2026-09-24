#include <jaal/jaal.hpp>
#include <atomic>
#include <cstdio>
#include <thread>
#include <sys/resource.h>
#include <mach/mach.h>
static std::atomic<long> folds{0};
struct Spin {
    struct Model { long n = 0; }; struct Go {};
    using Msg = std::variant<Go>; using Cmd = jaal::Cmd<Msg>;
    static Cmd init(Model&) { return Cmd::send(Go{}); }
    static Cmd update(Model& m, Go) { ++m.n; ++folds; return Cmd::send(Go{}); }
};
// per-THREAD cpu of the main (loop) thread, via mach
static mach_port_t loop_thread;
static double thread_cpu(mach_port_t t){ thread_basic_info_data_t i; mach_msg_type_number_t c=THREAD_BASIC_INFO_COUNT;
  thread_info(t, THREAD_BASIC_INFO, (thread_info_t)&i, &c);
  return i.user_time.seconds+i.user_time.microseconds/1e6+i.system_time.seconds+i.system_time.microseconds/1e6; }
int main() {
    loop_thread = mach_thread_self();
    std::thread([]{
        double a=thread_cpu(loop_thread); std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        std::fprintf(stderr,"LOOP THREAD: %.2fs CPU in 1.5s wall\n", thread_cpu(loop_thread)-a);
        std::_Exit(0); }).detach();
    return jaal::run<Spin>();
}
