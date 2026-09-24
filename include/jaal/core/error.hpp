#pragma once
// jaal::error / jaal::result — errors as values across the platform
// boundary. No exceptions cross it.

#include <cstdint>
#include <expected>
#include <string_view>
#include <system_error>

namespace jaal {

struct error {
    std::errc        code   = std::errc{};
    std::int32_t     native = 0;          // errno / GetLastError
    std::string_view what   = {};         // static text: no allocation

    [[nodiscard]] static error from_errno(int e, std::string_view w) noexcept {
        return {static_cast<std::errc>(e), e, w};
    }
    [[nodiscard]] static error make(std::errc c, std::string_view w) noexcept {
        return {c, 0, w};
    }
};

template <class T>
using result = std::expected<T, error>;

}  // namespace jaal
