#pragma once

// Shared test-side minimal PNG reader (E5 content-level verification). The
// production writer emits 8-bit RGBA (color type 6) or gray (color type 0)
// with per-row None/Sub/Up filters; this decoder inflates IDAT via zlib and
// reconstructs the raw samples.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include <zlib.h>

namespace welllog::test {

struct DecodedPng {
  std::uint32_t width{};
  std::uint32_t height{};
  unsigned char bit_depth{};
  unsigned char color_type{};
  std::vector<std::uint8_t> samples;  // raw, unfiltered, row-major
};

inline std::optional<DecodedPng> decode_png(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
  static constexpr unsigned char signature[8] = {137, 80, 78, 71, 13, 10,
                                                 26, 10};
  if (bytes.size() < 8 ||
      !std::equal(signature, signature + 8, bytes.begin())) {
    return std::nullopt;
  }
  DecodedPng png;
  std::string idat;
  std::size_t pos = 8;
  bool saw_ihdr = false;
  while (pos + 12 <= bytes.size()) {
    const auto length = (static_cast<std::uint32_t>(bytes[pos]) << 24U) |
                        (static_cast<std::uint32_t>(bytes[pos + 1]) << 16U) |
                        (static_cast<std::uint32_t>(bytes[pos + 2]) << 8U) |
                        static_cast<std::uint32_t>(bytes[pos + 3]);
    const std::string type(
        reinterpret_cast<const char *>(bytes.data() + pos + 4), 4);
    const auto data_begin = pos + 8;
    const auto data_end = data_begin + length;
    if (data_end + 4 > bytes.size()) {
      return std::nullopt;
    }
    if (type == "IHDR" && length >= 13) {
      png.width = (static_cast<std::uint32_t>(bytes[data_begin]) << 24U) |
                  (static_cast<std::uint32_t>(bytes[data_begin + 1]) << 16U) |
                  (static_cast<std::uint32_t>(bytes[data_begin + 2]) << 8U) |
                  static_cast<std::uint32_t>(bytes[data_begin + 3]);
      png.height = (static_cast<std::uint32_t>(bytes[data_begin + 4]) << 24U) |
                   (static_cast<std::uint32_t>(bytes[data_begin + 5]) << 16U) |
                   (static_cast<std::uint32_t>(bytes[data_begin + 6]) << 8U) |
                   static_cast<std::uint32_t>(bytes[data_begin + 7]);
      png.bit_depth = bytes[data_begin + 8];
      png.color_type = bytes[data_begin + 9];
      saw_ihdr = true;
    } else if (type == "IDAT") {
      idat.append(reinterpret_cast<const char *>(bytes.data() + data_begin),
                  length);
    } else if (type == "IEND") {
      break;
    }
    pos = data_end + 4;  // skip CRC
  }
  if (!saw_ihdr || png.bit_depth != 8 ||
      (png.color_type != 6 && png.color_type != 0)) {
    return std::nullopt;
  }
  const auto channels = png.color_type == 6 ? 4U : 1U;
  std::vector<unsigned char> inflated;
  z_stream stream{};
  stream.next_in = reinterpret_cast<Bytef *>(idat.data());
  stream.avail_in = static_cast<uInt>(idat.size());
  if (inflateInit(&stream) != Z_OK) {
    return std::nullopt;
  }
  std::vector<unsigned char> block(64U * 1024U);
  int ret;
  do {
    stream.next_out = block.data();
    stream.avail_out = static_cast<uInt>(block.size());
    ret = inflate(&stream, Z_NO_FLUSH);
    if (ret != Z_OK && ret != Z_STREAM_END) {
      inflateEnd(&stream);
      return std::nullopt;
    }
    inflated.insert(inflated.end(), block.begin(),
                    block.begin() +
                        static_cast<std::ptrdiff_t>(block.size() -
                                                    stream.avail_out));
  } while (ret != Z_STREAM_END);
  inflateEnd(&stream);

  const auto stride = static_cast<std::size_t>(png.width) * channels;
  png.samples.resize(stride * png.height);
  auto src = inflated.begin();
  for (std::uint32_t row = 0; row < png.height; ++row) {
    if (src == inflated.end()) {
      return std::nullopt;
    }
    const auto filter_type = *src;
    ++src;
    // The writer selects per-row among None/Sub/Up (#489); this decoder
    // reverses exactly that set. Iterator distances are ptrdiff_t; cast
    // explicitly where sizes are size_t so the sign of the result is
    // intentional (-Wsign-conversion).
    const auto copy = std::min<std::size_t>(
        stride, static_cast<std::size_t>(inflated.end() - src));
    const auto row_begin =
        png.samples.begin() + static_cast<std::ptrdiff_t>(stride * row);
    const auto prev_begin = png.samples.begin() +
                            static_cast<std::ptrdiff_t>(
                                stride * (row == 0 ? 0 : row - 1));
    for (std::size_t i = 0; i < copy; ++i) {
      const auto filtered_byte =
          static_cast<std::uint8_t>(*(src + static_cast<std::ptrdiff_t>(i)));
      std::uint8_t raw = filtered_byte;
      switch (filter_type) {
      case 1: // Sub
        raw = static_cast<std::uint8_t>(
            filtered_byte +
            (i >= channels
                 ? static_cast<std::uint8_t>(*(row_begin +
                                               static_cast<std::ptrdiff_t>(
                                                   i - channels)))
                 : 0U));
        break;
      case 2: // Up
        raw = static_cast<std::uint8_t>(
            filtered_byte +
            (row == 0 ? 0U
                      : static_cast<std::uint8_t>(
                            *(prev_begin + static_cast<std::ptrdiff_t>(i)))));
        break;
      default: // None
        break;
      }
      *(row_begin + static_cast<std::ptrdiff_t>(i)) = raw;
    }
    src += static_cast<std::ptrdiff_t>(copy);
  }
  return png;
}

}  // namespace welllog::test
