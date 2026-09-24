#pragma once
// jaal::meta diagnostics — readable compile errors.
//
// A concept failure deep in a template prints pages. jaal checks its
// contracts at the OUTERMOST call and fails through JAAL_REQUIRE, whose
// message is a sentence. With C++26 user-generated static_assert messages
// (P2741), the message can include computed text like an effect's name;
// on C++23 it falls back to the fixed text.
//
//   JAAL_REQUIRE(Effect<D>, "jaal: not an effect descriptor");
//   JAAL_REQUIRE_MSG(ok, jaal::meta::cat("jaal: host cannot run '", D::name, "'"));

#include <array>
#include <cstddef>
#include <string_view>

namespace jaal::meta {

#if defined(__cpp_static_assert) && __cpp_static_assert >= 202306L
inline constexpr bool has_computed_messages = true;
#else
inline constexpr bool has_computed_messages = false;
#endif

// Compile-time concatenation into a fixed buffer, for computed messages.
// Returns an object with .data() and .size(), which is what P2741 asks for.
template <std::size_t Cap = 256>
struct message {
    std::array<char, Cap> buf{};
    std::size_t len = 0;

    constexpr message& operator+=(std::string_view s) {
        for (char c : s)
            if (len < Cap) buf[len++] = c;
        return *this;
    }
    [[nodiscard]] constexpr const char* data() const { return buf.data(); }
    [[nodiscard]] constexpr std::size_t size() const { return len; }
};

template <std::size_t Cap = 256, class... Parts>
[[nodiscard]] constexpr auto cat(const Parts&... parts) -> message<Cap> {
    message<Cap> m;
    ((m += std::string_view(parts)), ...);
    return m;
}

}  // namespace jaal::meta

#define JAAL_REQUIRE(cond, text) static_assert((cond), text)

#if defined(__cpp_static_assert) && __cpp_static_assert >= 202306L
#  define JAAL_REQUIRE_MSG(cond, computed, fallback) static_assert((cond), computed)
#else
#  define JAAL_REQUIRE_MSG(cond, computed, fallback) static_assert((cond), fallback)
#endif
