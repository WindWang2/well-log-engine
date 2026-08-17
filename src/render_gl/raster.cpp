#include "render_gl/raster.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace welllog::detail {
namespace {

struct RasterPoint {
  double x{};
  double y{};
};

void blend_pixel(RasterImage &image, std::uint32_t column, std::uint32_t row,
                 RgbaColor color, double coverage) {
  if (coverage <= 0.0 || column >= image.width || row >= image.height) {
    return;
  }
  const auto index =
      (static_cast<std::size_t>(row) * image.width + column) * 4;
  const auto source_alpha =
      std::clamp(static_cast<double>(color.alpha) / 255.0 * coverage, 0.0,
                 1.0);
  const auto existing_alpha =
      static_cast<double>(image.pixels[index + 3]) / 255.0;
  const auto out_alpha = source_alpha + existing_alpha * (1.0 - source_alpha);
  if (out_alpha <= 0.0) {
    return;
  }
  for (std::size_t channel = 0; channel < 3; ++channel) {
    const auto source = static_cast<double>(
        channel == 0 ? color.red : channel == 1 ? color.green : color.blue);
    const auto existing = static_cast<double>(image.pixels[index + channel]);
    const auto blended =
        (source * source_alpha +
         existing * existing_alpha * (1.0 - source_alpha)) /
        out_alpha;
    image.pixels[index + channel] =
        static_cast<std::uint8_t>(std::clamp(std::lround(blended), 0l, 255l));
  }
  image.pixels[index + 3] =
      static_cast<std::uint8_t>(std::clamp(std::lround(out_alpha * 255.0), 0l,
                                           255l));
}

void draw_segment(RasterImage &image, RasterPoint from, RasterPoint to,
                  double half_width, RgbaColor color) {
  const auto minimum_x = static_cast<std::int64_t>(std::floor(
      std::min(from.x, to.x) - half_width - 1.0));
  const auto maximum_x = static_cast<std::int64_t>(
      std::ceil(std::max(from.x, to.x) + half_width + 1.0));
  const auto minimum_y = static_cast<std::int64_t>(std::floor(
      std::min(from.y, to.y) - half_width - 1.0));
  const auto maximum_y = static_cast<std::int64_t>(
      std::ceil(std::max(from.y, to.y) + half_width + 1.0));
  const auto delta_x = to.x - from.x;
  const auto delta_y = to.y - from.y;
  const auto length_squared = delta_x * delta_x + delta_y * delta_y;
  for (auto row = minimum_y; row <= maximum_y; ++row) {
    for (auto column = minimum_x; column <= maximum_x; ++column) {
      const auto point_x = static_cast<double>(column) + 0.5;
      const auto point_y = static_cast<double>(row) + 0.5;
      double distance = 0.0;
      if (length_squared == 0.0) {
        distance = std::hypot(point_x - from.x, point_y - from.y);
      } else {
        const auto projection = std::clamp(
            ((point_x - from.x) * delta_x + (point_y - from.y) * delta_y) /
                length_squared,
            0.0, 1.0);
        distance = std::hypot(point_x - (from.x + projection * delta_x),
                              point_y - (from.y + projection * delta_y));
      }
      blend_pixel(image, static_cast<std::uint32_t>(column),
                  static_cast<std::uint32_t>(row), color,
                  std::clamp(half_width + 0.5 - distance, 0.0, 1.0));
    }
  }
}

void draw_circle(RasterImage &image, RasterPoint center, double radius,
                 double half_width, bool filled, RgbaColor color) {
  const auto extent = filled ? radius : radius + half_width;
  const auto minimum_x = static_cast<std::int64_t>(
      std::floor(center.x - extent - 1.0));
  const auto maximum_x =
      static_cast<std::int64_t>(std::ceil(center.x + extent + 1.0));
  const auto minimum_y = static_cast<std::int64_t>(
      std::floor(center.y - extent - 1.0));
  const auto maximum_y =
      static_cast<std::int64_t>(std::ceil(center.y + extent + 1.0));
  for (auto row = minimum_y; row <= maximum_y; ++row) {
    for (auto column = minimum_x; column <= maximum_x; ++column) {
      const auto distance =
          std::hypot(static_cast<double>(column) + 0.5 - center.x,
                     static_cast<double>(row) + 0.5 - center.y);
      const auto coverage =
          filled ? std::clamp(radius + 0.5 - distance, 0.0, 1.0)
                 : std::clamp(half_width + 0.5 - std::abs(distance - radius),
                              0.0, 1.0);
      blend_pixel(image, static_cast<std::uint32_t>(column),
                  static_cast<std::uint32_t>(row), color, coverage);
    }
  }
}

// Flattens an outline into polylines (y-up em space) for scanline fill.
[[nodiscard]] std::vector<std::vector<RasterPoint>>
flatten_outline(std::span<const OutlineCommand> commands) {
  std::vector<std::vector<RasterPoint>> contours;
  RasterPoint current{};
  RasterPoint contour_start{};
  const auto push_vertex = [&](RasterPoint point) {
    if (contours.empty()) {
      return;
    }
    contours.back().push_back(point);
    current = point;
  };
  constexpr auto subdivisions = 8;
  for (const auto &command : commands) {
    switch (command.verb) {
    case OutlineVerb::move_to:
      contours.push_back({});
      contour_start = RasterPoint{command.coordinates[0],
                                  command.coordinates[1]};
      push_vertex(contour_start);
      break;
    case OutlineVerb::line_to:
      push_vertex(
          RasterPoint{command.coordinates[0], command.coordinates[1]});
      break;
    case OutlineVerb::quadratic_to: {
      const RasterPoint control{command.coordinates[0],
                                command.coordinates[1]};
      const RasterPoint end{command.coordinates[2], command.coordinates[3]};
      for (auto step = 1; step <= subdivisions; ++step) {
        const auto t = static_cast<double>(step) / subdivisions;
        const auto u = 1.0 - t;
        push_vertex(RasterPoint{
            u * u * current.x + 2.0 * u * t * control.x + t * t * end.x,
            u * u * current.y + 2.0 * u * t * control.y + t * t * end.y,
        });
      }
      break;
    }
    case OutlineVerb::cubic_to: {
      const RasterPoint first{command.coordinates[0], command.coordinates[1]};
      const RasterPoint second{command.coordinates[2],
                               command.coordinates[3]};
      const RasterPoint end{command.coordinates[4], command.coordinates[5]};
      for (auto step = 1; step <= subdivisions; ++step) {
        const auto t = static_cast<double>(step) / subdivisions;
        const auto u = 1.0 - t;
        push_vertex(RasterPoint{
            u * u * u * current.x + 3.0 * u * u * t * first.x +
                3.0 * u * t * t * second.x + t * t * t * end.x,
            u * u * u * current.y + 3.0 * u * u * t * first.y +
                3.0 * u * t * t * second.y + t * t * t * end.y,
        });
      }
      break;
    }
    case OutlineVerb::close:
      push_vertex(contour_start);
      break;
    }
  }
  return contours;
}

} // namespace

RasterImage rasterize_pattern_tile(const PatternDefinition &pattern,
                                   double pixels_per_millimetre) {
  const auto density = std::clamp(pixels_per_millimetre, 1.0, 128.0);
  const auto width = std::max(
      std::uint32_t{1},
      static_cast<std::uint32_t>(
          std::clamp(std::lround(pattern.tile_width.value * density), 1l,
                     512l)));
  const auto height = std::max(
      std::uint32_t{1},
      static_cast<std::uint32_t>(
          std::clamp(std::lround(pattern.tile_height.value * density), 1l,
                     512l)));
  RasterImage image{
      .width = width,
      .height = height,
      .channels = 4,
      .pixels = std::vector<std::uint8_t>(
          static_cast<std::size_t>(width) * height * 4, 0),
  };
  const auto scale_x = static_cast<double>(width) / pattern.tile_width.value;
  const auto scale_y = static_cast<double>(height) / pattern.tile_height.value;
  const auto density_x = scale_x;
  if (pattern.background.alpha > 0) {
    for (std::uint32_t row = 0; row < height; ++row) {
      for (std::uint32_t column = 0; column < width; ++column) {
        blend_pixel(image, column, row, pattern.background, 1.0);
      }
    }
  }
  const auto to_pixel = [&](const PhysicalPoint &point) {
    return RasterPoint{point.left.value * scale_x,
                       point.top.value * scale_y};
  };
  const auto half_width = std::max(
      0.5, pattern.stroke_width.value * density_x * 0.5);
  for (const auto &primitive : pattern.primitives) {
    if (const auto *line = std::get_if<PatternLine>(&primitive)) {
      draw_segment(image, to_pixel(line->from), to_pixel(line->to),
                   half_width, pattern.foreground);
    } else if (const auto *polyline =
                   std::get_if<PatternPolyline>(&primitive)) {
      for (std::size_t index = 0; index + 1 < polyline->points.size();
           ++index) {
        draw_segment(image, to_pixel(polyline->points[index]),
                     to_pixel(polyline->points[index + 1]), half_width,
                     pattern.foreground);
      }
      if (polyline->closed && polyline->points.size() > 2) {
        draw_segment(image, to_pixel(polyline->points.back()),
                     to_pixel(polyline->points.front()), half_width,
                     pattern.foreground);
      }
    } else {
      const auto &circle = std::get<PatternCircle>(primitive);
      draw_circle(image, to_pixel(circle.center),
                  circle.radius.value * density_x, half_width, circle.filled,
                  pattern.foreground);
    }
  }
  return image;
}

GlyphRaster rasterize_glyph_outline(std::span<const OutlineCommand> commands,
                                    double left_em, double bottom_em,
                                    double right_em, double top_em,
                                    double pixels_per_em) {
  const auto density = std::clamp(pixels_per_em, 1.0, 4096.0);
  const auto pad_em = 1.0 / density;
  const auto origin_left = left_em - pad_em;
  const auto origin_top = top_em + pad_em;
  const auto width = std::max(
      std::uint32_t{1},
      static_cast<std::uint32_t>(
          std::lround((right_em - left_em) * density) + 2));
  const auto height = std::max(
      std::uint32_t{1},
      static_cast<std::uint32_t>(
          std::lround((top_em - bottom_em) * density) + 2));
  GlyphRaster raster{
      .width = width,
      .height = height,
      .left_em = origin_left,
      .top_em = origin_top,
      .pixels_per_em = density,
      .alpha = std::vector<std::uint8_t>(
          static_cast<std::size_t>(width) * height, 0),
  };
  if (commands.empty()) {
    return raster;
  }
  const auto contours = flatten_outline(commands);
  const auto to_pixel = [&](const RasterPoint &point) {
    return RasterPoint{(point.x - origin_left) * density,
                       (origin_top - point.y) * density};
  };
  std::vector<std::vector<RasterPoint>> polygons;
  polygons.reserve(contours.size());
  for (const auto &contour : contours) {
    auto &polygon = polygons.emplace_back();
    polygon.reserve(contour.size());
    for (const auto &point : contour) {
      polygon.push_back(to_pixel(point));
    }
  }
  // Nonzero winding fill with 2x2 supersampling. Per-row active crossings
  // replace the previous O(W*H*4*E) per-pixel edge walk (issue #607) while
  // using the same half-open y-test and intersection-left-of-sample rule,
  // so occupancy matches the old winding fill for simple fixtures.
  constexpr std::array<double, 2> offsets{0.25, 0.75};
  struct Crossing {
    double x{};
    int delta{};
  };
  std::vector<Crossing> crossings;
  std::vector<std::uint8_t> covered(width, 0);
  for (std::uint32_t row = 0; row < height; ++row) {
    std::fill(covered.begin(), covered.end(), 0);
    for (const auto offset_y : offsets) {
      const auto y = static_cast<double>(row) + offset_y;
      crossings.clear();
      for (const auto &polygon : polygons) {
        for (std::size_t index = 0; index + 1 < polygon.size(); ++index) {
          const auto &from = polygon[index];
          const auto &to = polygon[index + 1];
          if ((from.y <= y) == (to.y <= y)) {
            continue;
          }
          const auto intersection =
              from.x + (y - from.y) / (to.y - from.y) * (to.x - from.x);
          crossings.push_back(Crossing{
              .x = intersection,
              .delta = to.y > from.y ? 1 : -1,
          });
        }
      }
      std::sort(crossings.begin(), crossings.end(),
                [](const Crossing &a, const Crossing &b) { return a.x < b.x; });
      auto winding = 0;
      std::size_t next = 0;
      for (std::uint32_t column = 0; column < width; ++column) {
        for (const auto offset_x : offsets) {
          const auto x = static_cast<double>(column) + offset_x;
          while (next < crossings.size() && crossings[next].x < x) {
            winding += crossings[next].delta;
            ++next;
          }
          if (winding != 0) {
            ++covered[column];
          }
        }
      }
    }
    for (std::uint32_t column = 0; column < width; ++column) {
      raster.alpha[static_cast<std::size_t>(row) * width + column] =
          static_cast<std::uint8_t>(std::lround(
              static_cast<double>(covered[column]) / 4.0 * 255.0));
    }
  }
  return raster;
}

ShelfAtlasPacker::ShelfAtlasPacker(std::uint32_t width,
                                   std::uint32_t height) noexcept
    : width_(width), height_(height) {}

std::optional<ShelfAtlasPacker::Rect>
ShelfAtlasPacker::allocate(std::uint32_t width, std::uint32_t height) noexcept {
  if (width == 0 || height == 0 || width > width_ || height > height_) {
    return std::nullopt;
  }
  if (cursor_ + width > width_) {
    shelf_top_ += shelf_height_;
    shelf_height_ = 0;
    cursor_ = 0;
  }
  if (shelf_top_ + height > height_) {
    return std::nullopt;
  }
  const auto rect = Rect{
      .left = cursor_,
      .top = shelf_top_,
      .width = width,
      .height = height,
  };
  cursor_ += width;
  shelf_height_ = std::max(shelf_height_, height);
  return rect;
}

} // namespace welllog::detail
