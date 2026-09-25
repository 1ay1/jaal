// handle.cpp — closing and duplicating a POSIX descriptor.
//
// The two calls the header promises, and the only place <unistd.h> is needed
// for them. EINTR is deliberately NOT retried on close: on Linux the
// descriptor is already gone when close() reports it, so a retry closes
// whatever number the OS handed out next.
#include <jaal/platform/handle.hpp>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

namespace jaal::platform {

void close_handle(native_handle h) noexcept {
    if (h >= 0) ::close(h);
}

result<native_handle> duplicate_handle(native_handle h) noexcept {
    if (h < 0) return std::unexpected(error::make(std::errc::bad_file_descriptor,
                                                  "duplicate_handle: not a handle"));
    int d;
    do {
        d = ::fcntl(h, F_DUPFD_CLOEXEC, 0);
    } while (d < 0 && errno == EINTR);
    if (d < 0) return std::unexpected(error::from_errno(errno, "fcntl(F_DUPFD_CLOEXEC)"));
    return d;
}

}  // namespace jaal::platform
