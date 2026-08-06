#pragma once

// Shared, non-Qt helpers for the table-export writers (CSV / XML / XLSX, #155).
// File-local to this component: svg.cpp/pdf.cpp have their own copies; a later
// cleanup ticket can consolidate all of them onto one core formatter.

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

namespace welllog {
namespace export_table {

// Appends the shortest round-trip representation of `value` to `output`
// (std::to_chars, general format, max_digits10 — equivalent to the Qt
// clipboard's 'g',17). Zero is special-cased to a single '0' for stable,
// minimal output. Throws std::bad_alloc only if to_chars fails (it cannot for
// a 64-char buffer and a finite double).
inline void append_number(std::string &output, double value) {
  if (value == 0.0) {
    output.push_back('0');
    return;
  }
  std::array<char, 64> buffer{};
  const auto result =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                    std::chars_format::general,
                    std::numeric_limits<double>::max_digits10);
  if (result.ec != std::errc{}) {
    throw std::bad_alloc{};
  }
  output.append(buffer.data(), result.ptr);
}

// Appends a base-10 integer (for row counts, revisions, indices).
template <typename Integer>
  requires std::is_integral_v<Integer>
void append_integer(std::string &output, Integer value) {
  std::array<char, 32> buffer{};
  const auto result =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (result.ec != std::errc{}) {
    throw std::bad_alloc{};
  }
  output.append(buffer.data(), result.ptr);
}

} // namespace export_table
} // namespace welllog
