#pragma once
// jaal::kernel::executor<Msg> — where task and stream bodies run.
//
// The kernel never runs background work itself; it hands each job to an
// executor. There are two:
//
//   * pool_executor (the default): the real worker pool, real threads
//   * sim_executor (host/sim.hpp): runs jobs on the loop thread, one at a
//     time, at sim times picked by a seeded RNG, so a run is reproducible
//     from its seed
//
// The kernel makes its executor through options::executor. Rules for an
// executor: every exception a job throws goes to the error callback (none
// escape), and after shutdown() no job that hasn't started is run.

#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <stop_token>
#include <string_view>

#include "../core/fx.hpp"
#include "../core/sink.hpp"
#include "pool.hpp"

namespace jaal::kernel {

template <class Msg>
class executor {
public:
    using job        = std::function<void(std::stop_token)>;
    using stream_job = std::function<void(Sink<Msg>, std::stop_token)>;
    using error_fn   = std::function<void(std::exception_ptr)>;

    virtual ~executor() = default;

    /// A task body. `where` is the program's placement choice.
    virtual void post(job j, fx::placement where) = 0;

    /// A stream body. `key` is its subscription key and `out` the sink it
    /// posts through (it goes dead when the stream is dropped). Given
    /// separately so a simulation can script a stream instead of running it.
    virtual void post_stream(std::string_view key, Sink<Msg> out, stream_job j) = 0;

    /// Stop every job and refuse new ones. Returns how many workers were
    /// abandoned because they ignored their stop token past `grace`.
    virtual std::size_t shutdown(std::chrono::milliseconds grace) = 0;
};

/// Makes the kernel's executor. Given the kernel's error callback.
/// Empty (or returning null) = pool_executor.
template <class Msg>
using executor_factory =
    std::function<std::unique_ptr<executor<Msg>>(typename executor<Msg>::error_fn)>;

/// The real one: jobs run on kernel::pool.
template <class Msg>
class pool_executor final : public executor<Msg> {
    using base = executor<Msg>;
public:
    pool_executor(unsigned max_workers, typename base::error_fn on_error)
        : pool_(max_workers, std::move(on_error)) {}

    void post(typename base::job j, fx::placement where) override {
        if (where == fx::placement::isolated) pool_.post_isolated(std::move(j));
        else                                  pool_.post(std::move(j));
    }

    // Streams are long-lived and often block: always a thread of their own.
    void post_stream(std::string_view, Sink<Msg> out, typename base::stream_job j) override {
        pool_.post_isolated([out = std::move(out), j = std::move(j)](std::stop_token st) mutable {
            j(std::move(out), std::move(st));
        });
    }

    std::size_t shutdown(std::chrono::milliseconds grace) override { return pool_.shutdown(grace); }

private:
    pool pool_;
};

}  // namespace jaal::kernel
