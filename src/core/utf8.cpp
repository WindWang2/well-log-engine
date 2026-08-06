#include <welllog/core/utf8.hpp>

#include <cstdint>

namespace welllog {

bool is_valid_utf8(std::string_view text) noexcept {
  const auto *bytes =
      reinterpret_cast<const std::uint8_t *>(text.data()); // NOLINT
  const auto size = text.size();
  std::size_t index = 0;
  while (index < size) {
    const auto first = bytes[index];
    if (first <= 0x7F) {
      ++index;
      continue;
    }
    std::size_t continuation_count = 0;
    std::uint32_t code_point = 0;
    std::uint32_t minimum = 0;
    if (first >= 0xC2 && first <= 0xDF) {
      continuation_count = 1;
      code_point = first & 0x1FU;
      minimum = 0x80;
    } else if (first >= 0xE0 && first <= 0xEF) {
      continuation_count = 2;
      code_point = first & 0x0FU;
      minimum = 0x800;
    } else if (first >= 0xF0 && first <= 0xF4) {
      continuation_count = 3;
      code_point = first & 0x07U;
      minimum = 0x10000;
    } else {
      return false;
    }
    if (index + continuation_count >= size) {
      return false;
    }
    for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
      const auto continuation = bytes[index + offset];
      if ((continuation & 0xC0U) != 0x80U) {
        return false;
      }
      code_point = (code_point << 6U) | (continuation & 0x3FU);
    }
    if (code_point < minimum || code_point > 0x10FFFF ||
        (code_point >= 0xD800 && code_point <= 0xDFFF)) {
      return false;
    }
    index += continuation_count + 1;
  }
  return true;
}

} // namespace welllog
