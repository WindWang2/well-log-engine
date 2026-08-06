#pragma once

#include <cstdint>

namespace welllog {

struct Millimetres {
  double value{};
  friend constexpr bool operator==(Millimetres, Millimetres) = default;
};

struct PhysicalRect {
  Millimetres left;
  Millimetres top;
  Millimetres width;
  Millimetres height;
};

struct PhysicalPoint {
  Millimetres left;
  Millimetres top;
};

struct RgbaColor {
  std::uint8_t red{};
  std::uint8_t green{};
  std::uint8_t blue{};
  std::uint8_t alpha{255};
  friend constexpr bool operator==(RgbaColor, RgbaColor) = default;
};

} // namespace welllog
