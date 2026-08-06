#pragma once

#include <string_view>

#include <welllog/core/export.hpp>

namespace welllog {

// Strict RFC 3629 validation: rejects overlong encodings, UTF-16
// surrogates, truncated sequences and code points above U+10FFFF.
[[nodiscard]] WELLLOG_CORE_API bool
is_valid_utf8(std::string_view text) noexcept;

} // namespace welllog
