// tests/kernel/stream_stop_test.cpp — a stopped stream's messages never
// reach update(), however the threads interleave.
//
// The bug this pins (found under tsan, ~1 run in 40): liveness was checked
// on the SENDER's side ("is my stream still subscribed?"). A message that
// passed that check and was queued before the loop stopped the stream was
// still in the mailbox afterwards, and the loop folded it into a model that
// had moved on. A per-send check can't close that window; it has to be
// decided on the loop, when the message is taken out.
//
// Two windows, both closed now, both reproduced here deterministically
// (no sanitizer, no luck):
//   A. the message is still in the MAILBOX when the stream stops: dropped
//      at drain (the mailbox retires the stream's origin)
//   B. the message was drained into the SAME fold batch as the message
//      that stops its stream: dropped at fold (the kernel re-subscribes
//      before folding a subscription's message, then checks its origin)
//
// Window A, step by step:
//   1. the stream queues a message and signals
//   2. the loop stops the stream via route() (folds + reconciles, and does
//      NOT drain the mailbox)
//   3. only then does step() drain
// Before the fix step 3 folded the stale message. Now it's dropped and
// counted in mailbox_stats::retired.

#include <jaal/jaal.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <variant>

using namespace std::chrono_literals;

#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,   \
                         __LINE__, #c);                                   \
            return 1;                                                     \
        }                                                                 \
    } while (0)

namespace {

std::atomic<int> queued{0};

struct Feed {
    struct Model { int epoch = 0; bool on = true; int stale = 0; int got = 0; };
    struct Item { int epoch; };
    struct Rekey {};
    struct Off {};
    using Msg = std::variant<Item, Rekey, Off>;
    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg>;

    static Cmd update(Model& m, Item i) {
        if (i.epoch != m.epoch || !m.on) ++m.stale; else ++m.got;
        return {};
    }
    static Cmd update(Model& m, Rekey) { ++m.epoch; return {}; }
    static Cmd update(Model& m, Off)   { m.on = false; return {}; }

    static Sub subscribe(const Model& m) {
        if (!m.on) return Sub::none();
        return Sub::stream("feed/" + std::to_string(m.epoch),
            [](jaal::Sink<Msg> out, std::stop_token, int epoch) {
                // Ignores its stop token on purpose: dropping its messages
                // is the kernel's job, not the body's good manners.
                if (epoch == 0) {
                    for (int i = 0; i < 3; ++i) out.send(Item{epoch});
                    queued.store(3);
                }
                std::this_thread::sleep_for(100ms);
            }, m.epoch);
    }
};

void wait_queued() {
    while (queued.load() < 3) std::this_thread::sleep_for(1ms);
}

// The stream is REPLACED (new key): its queued messages are dropped.
int replaced_stream_messages_are_dropped() {
    queued = 0;
    jaal::headless<Feed> h;
    wait_queued();                                   // 3 epoch-0 Items in the mailbox
    auto& k = h.kernel();
    k.dispatch(Feed::Rekey{});
    (void)k.route(jaal::kernel::no_events{}, h.record());   // fold + reconcile, no drain
    CHECK(h.model().epoch == 1);
    k.step(h.record());                              // drains the 3 stale Items
    CHECK(h.model().stale == 0);
    CHECK(k.mailbox_load().retired == 3);
    return 0;
}

// The stream is DROPPED (unsubscribed): same.
int unsubscribed_stream_messages_are_dropped() {
    queued = 0;
    jaal::headless<Feed> h;
    wait_queued();
    auto& k = h.kernel();
    k.dispatch(Feed::Off{});
    (void)k.route(jaal::kernel::no_events{}, h.record());
    k.step(h.record());
    CHECK(h.model().stale == 0);
    CHECK(k.mailbox_load().retired == 3);
    return 0;
}

// Control: without a stop, the same messages ARE delivered. So the test
// above is dropping stale messages, not all messages.
int live_stream_messages_are_delivered() {
    queued = 0;
    jaal::headless<Feed> h;
    wait_queued();
    h.kernel().step(h.record());
    CHECK(h.model().got == 3);
    CHECK(h.kernel().mailbox_load().retired == 0);
    return 0;
}

// The one that bit in practice (features_test, 1 run in 4 on clang): the
// stop and the stale message arrive in the SAME fold batch, stop first.
// headless::send(Rekey) queues Rekey and then step() drains the mailbox
// behind it: pending = [Rekey, Item(0), Item(0), Item(0)]. Folding Rekey
// makes the stream unwanted, but nothing re-subscribed before the Items
// were folded. Now a subscription's message re-subscribes first if the
// model changed, and is dropped if its subscription is gone.
int stop_and_stale_message_in_one_batch() {
    queued = 0;
    jaal::headless<Feed> h;
    wait_queued();                                   // Items queued, not drained
    h.send(Feed::Rekey{});                           // dispatch + step: one batch
    CHECK(h.model().epoch == 1);
    CHECK(h.model().stale == 0);
    CHECK(h.kernel().mailbox_load().retired == 3);
    return 0;
}

}  // namespace

int main() {
    if (int r = replaced_stream_messages_are_dropped()) return r;
    if (int r = unsubscribed_stream_messages_are_dropped()) return r;
    if (int r = live_stream_messages_are_delivered()) return r;
    if (int r = stop_and_stale_message_in_one_batch()) return r;
    std::puts("stream_stop_test: ok");
    return 0;
}
