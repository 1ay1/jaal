// src/platform/posix/file_lock.cpp — see include/jaal/platform/posix/file_lock.hpp

#include "jaal/platform/posix/file_lock.hpp"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

#include <utility>

namespace jaal::platform {

struct posix_file_lock::impl {
    int         fd   = -1;
    bool        held = false;
    std::string sidecar;

    ~impl() {
        // Order matters: drop the lock, THEN close. Closing first would also
        // release it, but through the close()-releases-everything rule rather
        // than deliberately, and that is the rule we do not want to lean on.
        if (fd >= 0) {
            if (held) {
                struct ::flock fl{};
                fl.l_type   = F_UNLCK;
                fl.l_whence = SEEK_SET;
                fl.l_start  = 0;
                fl.l_len    = 0;
                (void)::fcntl(fd, F_SETLK, &fl);
            }
            (void)::close(fd);
        }
    }
};

namespace {

// Open the sidecar. 0600: a lock file in a user's data directory should not
// be writable by anyone else, or another account can block this one forever.
result<int> open_sidecar(const std::string& sidecar) {
    const int fd = ::open(sidecar.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        return std::unexpected(
            error::from_errno(errno, "jaal: cannot open lock sidecar"));
    return fd;
}

// The whole file, which for a sidecar means "the lock". l_len 0 is POSIX for
// "to EOF, including anything appended later", so the region cannot be
// sidestepped by a process that extends the file first.
struct ::flock whole_file(lock_mode m) {
    struct ::flock fl{};
    fl.l_type   = (m == lock_mode::exclusive) ? F_WRLCK : F_RDLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start  = 0;
    fl.l_len    = 0;
    return fl;
}

}  // namespace

posix_file_lock::posix_file_lock() noexcept : p_(nullptr) {}
posix_file_lock::posix_file_lock(posix_file_lock&&) noexcept            = default;
posix_file_lock& posix_file_lock::operator=(posix_file_lock&&) noexcept = default;
posix_file_lock::~posix_file_lock()                                     = default;

result<posix_file_lock> posix_file_lock::acquire(const std::string& target,
                                                 lock_mode          m) {
    const std::string sidecar = lock_sidecar_for(target);
    auto              fd      = open_sidecar(sidecar);
    if (!fd) return std::unexpected(fd.error());

    auto p      = std::make_unique<impl>();
    p->fd       = *fd;
    p->sidecar  = sidecar;

    struct ::flock fl = whole_file(m);
    int            rc = 0;
    do {
        rc = ::fcntl(p->fd, F_SETLKW, &fl);
    } while (rc < 0 && errno == EINTR);  // a signal is not an answer
    if (rc < 0)
        return std::unexpected(
            error::from_errno(errno, "jaal: cannot lock sidecar"));

    p->held = true;
    posix_file_lock out;
    out.p_ = std::move(p);
    return out;
}

result<std::optional<posix_file_lock>> posix_file_lock::try_acquire(
    const std::string& target, lock_mode m) {
    const std::string sidecar = lock_sidecar_for(target);
    auto              fd      = open_sidecar(sidecar);
    if (!fd) return std::unexpected(fd.error());

    auto p     = std::make_unique<impl>();
    p->fd      = *fd;
    p->sidecar = sidecar;

    struct ::flock fl = whole_file(m);
    int            rc = 0;
    do {
        rc = ::fcntl(p->fd, F_SETLK, &fl);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0) {
        // EACCES/EAGAIN is "another process holds it" — an outcome. Anything
        // else is this filesystem telling us it cannot do locks, which the
        // caller must be able to tell apart.
        if (errno == EACCES || errno == EAGAIN)
            return std::optional<posix_file_lock>{};
        return std::unexpected(
            error::from_errno(errno, "jaal: cannot lock sidecar"));
    }

    p->held = true;
    posix_file_lock out;
    out.p_ = std::move(p);
    return std::optional<posix_file_lock>{std::move(out)};
}

bool posix_file_lock::held() const noexcept { return p_ && p_->held; }

void posix_file_lock::release() noexcept {
    if (!p_ || !p_->held) return;  // idempotent
    struct ::flock fl{};
    fl.l_type   = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start  = 0;
    fl.l_len    = 0;
    (void)::fcntl(p_->fd, F_SETLK, &fl);
    p_->held = false;
}

const std::string& posix_file_lock::path() const noexcept {
    static const std::string none;
    return p_ ? p_->sidecar : none;
}

}  // namespace jaal::platform
