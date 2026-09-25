// handle.cpp — closing and duplicating a Win32 HANDLE.
//
// The Windows half of jaal/platform/handle.hpp. DuplicateHandle needs the
// process handle on both sides; the copy is made non-inheritable, which
// matches F_DUPFD_CLOEXEC on POSIX.
#include <jaal/platform/handle.hpp>

#include <windows.h>

namespace jaal::platform {

void close_handle(native_handle h) noexcept {
    if (is_valid(h)) ::CloseHandle(static_cast<HANDLE>(h));
}

result<native_handle> duplicate_handle(native_handle h) noexcept {
    if (!is_valid(h))
        return std::unexpected(error::make(std::errc::bad_file_descriptor,
                                           "duplicate_handle: not a handle"));
    HANDLE out = nullptr;
    const HANDLE self = ::GetCurrentProcess();
    if (!::DuplicateHandle(self, static_cast<HANDLE>(h), self, &out, 0, FALSE,
                           DUPLICATE_SAME_ACCESS))
        return std::unexpected(error{std::errc::io_error,
                                     static_cast<std::int32_t>(::GetLastError()),
                                     "DuplicateHandle()"});
    return static_cast<native_handle>(out);
}

}  // namespace jaal::platform
