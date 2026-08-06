#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace welllog::detail {

struct DecodedCodePoint {
  char32_t code_point{};
  std::uint32_t byte_offset{};
  std::uint32_t byte_length{};
};

// Decodes valid UTF-8. Callers must validate the input with
// welllog::is_valid_utf8 first; invalid sequences decode as U+FFFD with a
// single-byte length so iteration always terminates.
[[nodiscard]] inline std::vector<DecodedCodePoint>
decode_utf8(std::string_view text) {
  std::vector<DecodedCodePoint> result;
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(text.data());
  const auto size = text.size();
  std::size_t index = 0;
  while (index < size) {
    const auto first = bytes[index];
    if (first <= 0x7F) {
      result.push_back(DecodedCodePoint{
          .code_point = first,
          .byte_offset = static_cast<std::uint32_t>(index),
          .byte_length = 1,
      });
      ++index;
      continue;
    }
    std::uint32_t continuation_count = 0;
    char32_t code_point = 0;
    if (first >= 0xC2 && first <= 0xDF) {
      continuation_count = 1;
      code_point = first & 0x1FU;
    } else if (first >= 0xE0 && first <= 0xEF) {
      continuation_count = 2;
      code_point = first & 0x0FU;
    } else if (first >= 0xF0 && first <= 0xF4) {
      continuation_count = 3;
      code_point = first & 0x07U;
    } else {
      result.push_back(DecodedCodePoint{
          .code_point = 0xFFFD,
          .byte_offset = static_cast<std::uint32_t>(index),
          .byte_length = 1,
      });
      ++index;
      continue;
    }
    if (index + continuation_count >= size) {
      result.push_back(DecodedCodePoint{
          .code_point = 0xFFFD,
          .byte_offset = static_cast<std::uint32_t>(index),
          .byte_length = 1,
      });
      ++index;
      continue;
    }
    bool valid = true;
    for (std::uint32_t offset = 1; offset <= continuation_count; ++offset) {
      const auto continuation = bytes[index + offset];
      if ((continuation & 0xC0U) != 0x80U) {
        valid = false;
        break;
      }
      code_point = (code_point << 6U) | (continuation & 0x3FU);
    }
    if (!valid) {
      result.push_back(DecodedCodePoint{
          .code_point = 0xFFFD,
          .byte_offset = static_cast<std::uint32_t>(index),
          .byte_length = 1,
      });
      ++index;
      continue;
    }
    result.push_back(DecodedCodePoint{
        .code_point = code_point,
        .byte_offset = static_cast<std::uint32_t>(index),
        .byte_length = continuation_count + 1,
    });
    index += continuation_count + 1;
  }
  return result;
}

} // namespace welllog::detail
