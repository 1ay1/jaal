// src/platform/sim/file_lock.cpp — see include/jaal/platform/sim/file_lock.hpp

#include "jaal/platform/sim/file_lock.hpp"

#include <cerrno>
#include <map>
#include <string>
#include <utility>

namespace jaal::platform {

namespace {

struct fact {
    bool held_by_other = false;
    int  fail_errno    = 0;
    int  acquisitions  = 0;
};

// Process-global, deliberately unsynchronized: see the header. A test that
// wants concurrency wants jaal::guarded, not a mutex bolted on here.
std::map<std::string, fact>& facts() {
    static std::map<std::string, fact> f;
    return f;
}

}  // namespace

void sim_file_lock::world::hold(const std::string& target) {
    facts()[target].held_by_other = true;
}
void sim_file_lock::world::drop(const std::string& target) {
    facts()[target].held_by_other = false;
}
void sim_file_lock::world::fail(const std::string& target, int err) {
    facts()[target].fail_errno = err;
}
void sim_file_lock::world::reset() { facts().clear(); }

int sim_file_lock::world::acquisitions(const std::string& target) const {
    const auto it = facts().find(target);
    return it == facts().end() ? 0 : it->second.acquisitions;
}

sim_file_lock::world& sim_file_lock::script() noexcept {
    static world w;
    return w;
}

struct sim_file_lock::impl {
    std::string target;
    std::string sidecar;
    bool        held = false;

    ~impl() {
        if (held) facts()[target].held_by_other = false;
    }
};

sim_file_lock::sim_file_lock() noexcept : p_(nullptr) {}
sim_file_lock::sim_file_lock(sim_file_lock&&) noexcept            = default;
sim_file_lock& sim_file_lock::operator=(sim_file_lock&&) noexcept = default;
sim_file_lock::~sim_file_lock()                                   = default;

result<sim_file_lock> sim_file_lock::acquire(const std::string& target,
                                             lock_mode          m) {
    fact& f = facts()[target];
    if (f.fail_errno != 0)
        return std::unexpected(
            error::from_errno(f.fail_errno, "sim: scripted lock failure"));
    // A blocking acquire against a scripted holder can never be satisfied —
    // nothing in the sim will ever drop it. Hanging would be the honest
    // simulation and a useless one, so say deadlock and let the test fail
    // with a reason instead of a timeout.
    if (f.held_by_other && m == lock_mode::exclusive)
        return std::unexpected(error::make(
            std::errc::resource_deadlock_would_occur,
            "sim: acquire() would block forever on a scripted holder; the "
            "test wants try_acquire(), or world::drop() first"));

    ++f.acquisitions;
    auto p     = std::make_unique<impl>();
    p->target  = target;
    p->sidecar = lock_sidecar_for(target);
    p->held    = true;
    if (m == lock_mode::exclusive) f.held_by_other = true;

    sim_file_lock out;
    out.p_ = std::move(p);
    return out;
}

result<std::optional<sim_file_lock>> sim_file_lock::try_acquire(
    const std::string& target, lock_mode m) {
    fact& f = facts()[target];
    if (f.fail_errno != 0)
        return std::unexpected(
            error::from_errno(f.fail_errno, "sim: scripted lock failure"));
    if (f.held_by_other && m == lock_mode::exclusive)
        return std::optional<sim_file_lock>{};

    ++f.acquisitions;
    auto p     = std::make_unique<impl>();
    p->target  = target;
    p->sidecar = lock_sidecar_for(target);
    p->held    = true;
    if (m == lock_mode::exclusive) f.held_by_other = true;

    sim_file_lock out;
    out.p_ = std::move(p);
    return std::optional<sim_file_lock>{std::move(out)};
}

bool sim_file_lock::held() const noexcept { return p_ && p_->held; }

void sim_file_lock::release() noexcept {
    if (!p_ || !p_->held) return;  // idempotent
    facts()[p_->target].held_by_other = false;
    p_->held                          = false;
}

const std::string& sim_file_lock::path() const noexcept {
    static const std::string none;
    return p_ ? p_->sidecar : none;
}

}  // namespace jaal::platform
