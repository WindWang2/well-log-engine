#pragma once

// Checked arithmetic for untrusted length/stride/offset calculations
// (ADR 0042, #171). Returns nullopt on overflow — callers map to
// ErrorCode::arithmetic_overflow or resource_exhausted as appropriate.

#include <cstdint>
#include <limits>
#include <optional>

namespace welllog {

[[nodiscard]] inline std::optional<std::uint64_t>
checked_add_u64(std::uint64_t a, std::uint64_t b) noexcept {
  if (a > std::numeric_limits<std::uint64_t>::max() - b) {
    return std::nullopt;
  }
  return a + b;
}

[[nodiscard]] inline std::optional<std::uint64_t>
checked_mul_u64(std::uint64_t a, std::uint64_t b) noexcept {
  if (a == 0 || b == 0) {
    return 0;
  }
  if (a > std::numeric_limits<std::uint64_t>::max() / b) {
    return std::nullopt;
  }
  return a * b;
}

// Extent of a strided buffer: (length - 1) * stride + element_size when
// length > 0; 0 when length == 0.
[[nodiscard]] inline std::optional<std::uint64_t>
checked_strided_extent_u64(std::uint64_t length, std::uint64_t stride_bytes,
                           std::uint64_t element_size) noexcept {
  if (length == 0) {
    return 0;
  }
  if (stride_bytes < element_size) {
    return std::nullopt;
  }
  const auto span = checked_mul_u64(length - 1, stride_bytes);
  if (!span.has_value()) {
    return std::nullopt;
  }
  return checked_add_u64(*span, element_size);
}

} // namespace welllog
