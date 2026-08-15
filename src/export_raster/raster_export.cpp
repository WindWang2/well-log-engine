#include <welllog/export/raster.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

#include <zlib.h>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace welllog {
namespace {

constexpr double k_mm_per_inch = 25.4;

[[nodiscard]] Error presentation_error() {
  return Error{.code = ErrorCode::invalid_presentation,
               .severity = Severity::error,
               .entity_id = std::nullopt,
               .message = MessageKey::presentation_invalid,
               .arguments = {}};
}

[[nodiscard]] Error resource_exhausted() {
  return Error{.code = ErrorCode::resource_exhausted,
               .severity = Severity::error,
               .entity_id = std::nullopt,
               .message = MessageKey::resource_exhausted,
               .arguments = {}};
}

[[nodiscard]] Error internal_error() {
  return Error{.code = ErrorCode::internal_error,
               .severity = Severity::error,
               .entity_id = std::nullopt,
               .message = MessageKey::internal_error,
               .arguments = {}};
}

[[nodiscard]] Error cancelled_error() {
  return Error{.code = ErrorCode::operation_cancelled,
               .severity = Severity::error,
               .entity_id = std::nullopt,
               .message = MessageKey::operation_cancelled,
               .arguments = {}};
}

[[nodiscard]] Error overwrite_rejected() {
  return Error{.code = ErrorCode::invalid_document,
               .severity = Severity::error,
               .entity_id = std::nullopt,
               .message = MessageKey::document_structure_invalid,
               .arguments = {}};
}

struct OutputGeometry {
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t dpi{};
  double pixels_per_mm{};
  std::uint32_t tile_height{};
  std::uint32_t channels{4};
};

[[nodiscard]] Result<OutputGeometry>
resolve_geometry(const PreparedScene &scene, const ExportSnapshot &snapshot,
                 const RasterExportRequest &request) {
  if (!std::isfinite(scene.physical_width().value) ||
      !std::isfinite(scene.physical_height().value) ||
      scene.physical_width().value <= 0.0 ||
      scene.physical_height().value <= 0.0) {
    return presentation_error();
  }
  const auto dpi =
      request.dpi_override != 0 ? request.dpi_override : snapshot.page.dpi;
  if (dpi == 0 || request.tile_height_px == 0) {
    return presentation_error();
  }

  const auto ppm = static_cast<double>(dpi) / k_mm_per_inch;
  auto width = request.width_px;
  auto height = request.height_px;
  if (width == 0) {
    const auto w = std::llround(scene.physical_width().value * ppm);
    if (w <= 0 ||
        w > static_cast<long long>(std::numeric_limits<std::uint32_t>::max())) {
      return resource_exhausted();
    }
    width = static_cast<std::uint32_t>(w);
  }
  if (height == 0) {
    const auto h = std::llround(scene.physical_height().value * ppm);
    if (h <= 0 ||
        h > static_cast<long long>(std::numeric_limits<std::uint32_t>::max())) {
      return resource_exhausted();
    }
    height = static_cast<std::uint32_t>(h);
  }

  const auto pixels =
      static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
  if (pixels == 0 || pixels > request.max_output_pixels) {
    return resource_exhausted();
  }

  const auto channels =
      request.color_space == RasterColorSpace::gray ? 1U : 4U;
  const auto tile_h = std::min(request.tile_height_px, height);
  const auto tile_bytes = static_cast<std::uint64_t>(width) *
                          static_cast<std::uint64_t>(tile_h) *
                          static_cast<std::uint64_t>(channels);
  if (tile_bytes == 0 || tile_bytes > request.max_tile_bytes) {
    return resource_exhausted();
  }

  return OutputGeometry{.width = width,
                        .height = height,
                        .dpi = dpi,
                        .pixels_per_mm = ppm,
                        .tile_height = tile_h,
                        .channels = channels};
}

[[nodiscard]] std::uint8_t to_gray(std::uint8_t r, std::uint8_t g,
                                   std::uint8_t b) noexcept {
  const auto y = 0.2126 * static_cast<double>(r) +
                 0.7152 * static_cast<double>(g) +
                 0.0722 * static_cast<double>(b);
  return static_cast<std::uint8_t>(std::clamp(std::lround(y), 0L, 255L));
}

void fill_background(std::vector<std::uint8_t> &tile, std::uint32_t width,
                     std::uint32_t height, std::uint32_t channels,
                     RgbaColor background) {
  if (channels == 1) {
    std::fill(tile.begin(), tile.end(),
              to_gray(background.red, background.green, background.blue));
    return;
  }
  for (std::size_t i = 0; i + 3 < tile.size(); i += 4) {
    tile[i] = background.red;
    tile[i + 1] = background.green;
    tile[i + 2] = background.blue;
    tile[i + 3] = background.alpha;
  }
  (void)width;
  (void)height;
}

void blend_pixel(std::vector<std::uint8_t> &tile, std::uint32_t width,
                 std::uint32_t height, std::uint32_t channels, int x, int y,
                 RgbaColor color) {
  if (x < 0 || y < 0 || static_cast<std::uint32_t>(x) >= width ||
      static_cast<std::uint32_t>(y) >= height) {
    return;
  }
  const auto i =
      (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) *
      channels;
  if (channels == 1) {
    tile[i] = to_gray(color.red, color.green, color.blue);
    return;
  }
  const auto src_a = color.alpha / 255.0;
  const auto dst_a = tile[i + 3] / 255.0;
  const auto out_a = src_a + dst_a * (1.0 - src_a);
  if (out_a <= 0.0) {
    tile[i] = tile[i + 1] = tile[i + 2] = tile[i + 3] = 0;
    return;
  }
  auto channel = [&](std::uint8_t src, std::uint8_t dst) {
    const auto v = (src * src_a + dst * dst_a * (1.0 - src_a)) / out_a;
    return static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L));
  };
  tile[i] = channel(color.red, tile[i]);
  tile[i + 1] = channel(color.green, tile[i + 1]);
  tile[i + 2] = channel(color.blue, tile[i + 2]);
  tile[i + 3] =
      static_cast<std::uint8_t>(std::clamp(std::lround(out_a * 255.0), 0L, 255L));
}

void draw_line(std::vector<std::uint8_t> &tile, std::uint32_t width,
               std::uint32_t height, std::uint32_t channels, int x0, int y0,
               int x1, int y1, RgbaColor color, int thickness) {
  const auto dx = std::abs(x1 - x0);
  const auto sx = x0 < x1 ? 1 : -1;
  const auto dy = -std::abs(y1 - y0);
  const auto sy = y0 < y1 ? 1 : -1;
  auto err = dx + dy;
  const auto radius = std::max(0, thickness / 2);
  while (true) {
    for (int oy = -radius; oy <= radius; ++oy) {
      for (int ox = -radius; ox <= radius; ++ox) {
        blend_pixel(tile, width, height, channels, x0 + ox, y0 + oy, color);
      }
    }
    if (x0 == x1 && y0 == y1) {
      break;
    }
    const auto e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

void fill_rect(std::vector<std::uint8_t> &tile, std::uint32_t width,
               std::uint32_t height, std::uint32_t channels, int left, int top,
               int right, int bottom, RgbaColor color) {
  left = std::max(left, 0);
  top = std::max(top, 0);
  right = std::min(right, static_cast<int>(width));
  bottom = std::min(bottom, static_cast<int>(height));
  for (int y = top; y < bottom; ++y) {
    for (int x = left; x < right; ++x) {
      blend_pixel(tile, width, height, channels, x, y, color);
    }
  }
}

// Even-odd point-in-polygon test on a polygon centred at the origin (scene
// millimetres, y-down).
bool point_in_polygon(const std::vector<PhysicalPoint> &polygon, double px,
                      double py) {
  bool inside = false;
  for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size();
       j = i++) {
    const auto &a = polygon[i];
    const auto &b = polygon[j];
    if ((a.top.value > py) != (b.top.value > py)) {
      const auto x_intersect =
          a.left.value +
          (py - a.top.value) / (b.top.value - a.top.value) *
              (b.left.value - a.left.value);
      if (px < x_intersect) {
        inside = !inside;
      }
    }
  }
  return inside;
}

// Fills a scene-mm polygon (centred at the origin) translated to
// (center_x, center_y) — the raster path's shared symbol geometry consumer.
void fill_polygon(std::vector<std::uint8_t> &tile, std::uint32_t width,
                  std::uint32_t height, std::uint32_t channels,
                  const std::vector<PhysicalPoint> &outline, double center_x,
                  double center_y, double ppm, std::int64_t tile_origin_y,
                  RgbaColor color) {
  if (outline.size() < 3) {
    return;
  }
  double min_x = std::numeric_limits<double>::infinity();
  double max_x = -std::numeric_limits<double>::infinity();
  double min_y = std::numeric_limits<double>::infinity();
  double max_y = -std::numeric_limits<double>::infinity();
  for (const auto &p : outline) {
    min_x = std::min(min_x, p.left.value);
    max_x = std::max(max_x, p.left.value);
    min_y = std::min(min_y, p.top.value);
    max_y = std::max(max_y, p.top.value);
  }
  const auto x0 = std::max(
      0, static_cast<int>(std::floor((center_x + min_x) * ppm)));
  // tile_origin_y is a tile offset (uint32 source), so it fits exactly in
  // double; cast explicitly to keep the mixed int64/double arithmetic honest.
  const auto y0 = std::max(
      0, static_cast<int>(std::floor((center_y + min_y) * ppm) -
                          static_cast<double>(tile_origin_y)));
  const auto x1 = std::min(
      static_cast<int>(width),
      static_cast<int>(std::ceil((center_x + max_x) * ppm)));
  const auto y1 = std::min(
      static_cast<int>(height),
      static_cast<int>(std::ceil((center_y + max_y) * ppm) -
                      static_cast<double>(tile_origin_y)));
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      const auto sx = (static_cast<double>(x) + 0.5) / ppm;
      const auto sy = (static_cast<double>(y) + 0.5 +
                       static_cast<double>(tile_origin_y)) /
                      ppm;
      if (point_in_polygon(outline, sx - center_x, sy - center_y)) {
        blend_pixel(tile, width, height, channels, x, y, color);
      }
    }
  }
}

struct PixelPoint {
  int x{};
  int y{};
};

[[nodiscard]] PixelPoint to_pixel(PhysicalPoint point, double ppm,
                                  std::int64_t tile_origin_y) {
  const auto x = static_cast<int>(std::lround(point.left.value * ppm));
  const auto y_abs =
      static_cast<std::int64_t>(std::llround(point.top.value * ppm));
  return PixelPoint{.x = x, .y = static_cast<int>(y_abs - tile_origin_y)};
}

void rasterize_tile(const PreparedScene &scene, const OutputGeometry &geom,
                    std::uint32_t tile_top, std::uint32_t tile_rows,
                    std::vector<std::uint8_t> &tile, RgbaColor background) {
  fill_background(tile, geom.width, tile_rows, geom.channels, background);
  const auto tile_origin_y = static_cast<std::int64_t>(tile_top);
  const auto ppm = geom.pixels_per_mm;

  for (const auto &layer : scene.interval_layers()) {
    const auto end = layer.first_interval + layer.interval_count;
    for (std::uint64_t index = layer.first_interval; index < end; ++index) {
      const auto &interval = scene.intervals()[index];
      const auto tl =
          to_pixel(PhysicalPoint{interval.rect.left, interval.rect.top}, ppm,
                   tile_origin_y);
      const auto br = to_pixel(
          PhysicalPoint{
              Millimetres{interval.rect.left.value + interval.rect.width.value},
              Millimetres{interval.rect.top.value +
                          interval.rect.height.value}},
          ppm, tile_origin_y);
      fill_rect(tile, geom.width, tile_rows, geom.channels, tl.x, tl.y, br.x,
                br.y, interval.fill_color);
    }
  }

  // Symbol layers: discrete SymbolOccurrence glyphs (shared symbol_glyph
  // geometry) at their prepared scene-mm centers. Drawn below curves to match
  // the SVG/GL channel semantics (markers and symbols sit under curves).
  for (const auto &layer : scene.symbol_layers()) {
    const auto end = layer.first_symbol + layer.symbol_count;
    const auto stroke = std::max(
        1, static_cast<int>(std::lround(layer.symbol_size.value / 6.0 * ppm)));
    for (std::uint64_t index = layer.first_symbol; index < end; ++index) {
      const auto &symbol = scene.symbols()[index];
      if (symbol.kind == SymbolKind::cross) {
        // Stroke-only glyph: the two diagonals (same convention as the
        // marker-symbol branch).
        const auto half = static_cast<int>(
            std::lround(layer.symbol_size.value / 2.0 * ppm));
        const auto center = to_pixel(symbol.center, ppm, tile_origin_y);
        draw_line(tile, geom.width, tile_rows, geom.channels, center.x - half,
                  center.y - half, center.x + half, center.y + half,
                  layer.color, stroke);
        draw_line(tile, geom.width, tile_rows, geom.channels, center.x + half,
                  center.y - half, center.x - half, center.y + half,
                  layer.color, stroke);
      } else {
        fill_polygon(tile, geom.width, tile_rows, geom.channels,
                     symbol_glyph(symbol.kind, layer.symbol_size).outline,
                     symbol.center.left.value, symbol.center.top.value, ppm,
                     tile_origin_y, layer.color);
      }
    }
  }

  const auto segments = scene.curve_segments();
  const auto points = scene.curve_points();
  for (const auto &layer : scene.curve_layers()) {
    if (!layer.visible || layer.segment_count == 0) {
      continue;
    }
    const auto thickness = std::max(
        1, static_cast<int>(std::lround(layer.line_width.value * ppm)));
    const auto seg_end = layer.first_segment + layer.segment_count;
    for (std::uint64_t s = layer.first_segment; s < seg_end; ++s) {
      const auto &segment = segments[s];
      if (segment.point_count < 2) {
        continue;
      }
      for (std::uint64_t p = 0; p + 1 < segment.point_count; ++p) {
        const auto &a = points[segment.first_point + p];
        const auto &b = points[segment.first_point + p + 1];
        const auto pa = to_pixel(a.position, ppm, tile_origin_y);
        const auto pb = to_pixel(b.position, ppm, tile_origin_y);
        draw_line(tile, geom.width, tile_rows, geom.channels, pa.x, pa.y, pb.x,
                  pb.y, layer.color, thickness);
      }
    }
  }

  // Markers: horizontal line across each prepared track at display_top.
  for (const auto &layer : scene.marker_layers()) {
    const auto end = layer.first_marker + layer.marker_count;
    const auto thickness = std::max(
        1, static_cast<int>(std::lround(layer.line_width.value * ppm)));
    for (std::uint64_t index = layer.first_marker; index < end; ++index) {
      const auto &marker = scene.markers()[index];
      // Find a track that contains this marker's depth line; fall back to full
      // scene width.
      int x0 = 0;
      int x1 = static_cast<int>(geom.width);
      for (const auto &track : scene.tracks()) {
        const auto left = static_cast<int>(
            std::lround(track.bounds.left.value * ppm));
        const auto right = static_cast<int>(std::lround(
            (track.bounds.left.value + track.bounds.width.value) * ppm));
        x0 = left;
        x1 = right;
        break;
      }
      const auto y = static_cast<int>(
          std::llround(marker.display_top.value * ppm) - tile_origin_y);
      draw_line(tile, geom.width, tile_rows, geom.channels, x0, y, x1, y,
                layer.line_color, thickness);
      if (layer.draw_symbols) {
        // Marker-semantic symbol glyph at the line's left end (shared shape
        // geometry from scene::symbol_glyph).
        const auto kind = symbol_for_marker_semantic(marker.semantic);
        const auto half = layer.symbol_size.value / 2.0;
        const auto center_x = x0 / ppm + 1.0 + half;
        const auto center_y = marker.display_top.value;
        if (kind == SymbolKind::cross) {
          const auto hpx = static_cast<int>(std::lround(half * ppm));
          const auto cpx = static_cast<int>(std::lround(center_x * ppm));
          const auto cpy = y;
          const auto stroke =
              std::max(1, static_cast<int>(std::lround(
                              layer.symbol_size.value / 6.0 * ppm)));
          draw_line(tile, geom.width, tile_rows, geom.channels, cpx - hpx,
                    cpy - hpx, cpx + hpx, cpy + hpx, layer.line_color, stroke);
          draw_line(tile, geom.width, tile_rows, geom.channels, cpx + hpx,
                    cpy - hpx, cpx - hpx, cpy + hpx, layer.line_color, stroke);
        } else {
          fill_polygon(tile, geom.width, tile_rows, geom.channels,
                       symbol_glyph(kind, layer.symbol_size).outline, center_x,
                       center_y, ppm, tile_origin_y, layer.line_color);
        }
      }
    }
  }
}

// --- PNG streaming ----------------------------------------------------------

void write_be32(std::ostream &out, std::uint32_t value) {
  const unsigned char bytes[4] = {
      static_cast<unsigned char>((value >> 24U) & 0xffU),
      static_cast<unsigned char>((value >> 16U) & 0xffU),
      static_cast<unsigned char>((value >> 8U) & 0xffU),
      static_cast<unsigned char>(value & 0xffU),
  };
  out.write(reinterpret_cast<const char *>(bytes), 4);
}

void write_png_chunk(std::ostream &out, const char type[4],
                     const unsigned char *data, std::size_t size) {
  write_be32(out, static_cast<std::uint32_t>(size));
  out.write(type, 4);
  if (size != 0) {
    out.write(reinterpret_cast<const char *>(data),
              static_cast<std::streamsize>(size));
  }
  auto crc = crc32(0L, Z_NULL, 0);
  crc = crc32(crc, reinterpret_cast<const Bytef *>(type), 4);
  if (size != 0) {
    crc = crc32(crc, data, static_cast<uInt>(size));
  }
  write_be32(out, static_cast<std::uint32_t>(crc));
}

// --- TIFF helpers -----------------------------------------------------------

void write_le16(std::ostream &out, std::uint16_t value) {
  const unsigned char bytes[2] = {
      static_cast<unsigned char>(value & 0xffU),
      static_cast<unsigned char>((value >> 8U) & 0xffU),
  };
  out.write(reinterpret_cast<const char *>(bytes), 2);
}

void write_le32(std::ostream &out, std::uint32_t value) {
  const unsigned char bytes[4] = {
      static_cast<unsigned char>(value & 0xffU),
      static_cast<unsigned char>((value >> 8U) & 0xffU),
      static_cast<unsigned char>((value >> 16U) & 0xffU),
      static_cast<unsigned char>((value >> 24U) & 0xffU),
  };
  out.write(reinterpret_cast<const char *>(bytes), 4);
}

[[nodiscard]] std::vector<std::uint8_t>
packbits_encode(const std::uint8_t *data, std::size_t size) {
  std::vector<std::uint8_t> out;
  out.reserve(size + size / 128U + 8U);
  std::size_t i = 0;
  while (i < size) {
    if (i + 1 < size && data[i] == data[i + 1]) {
      std::size_t repeat = 1;
      while (i + repeat < size && repeat < 128 && data[i + repeat] == data[i]) {
        ++repeat;
      }
      out.push_back(static_cast<std::uint8_t>(257 - repeat));
      out.push_back(data[i]);
      i += repeat;
      continue;
    }
    std::size_t literal = 0;
    while (i + literal < size && literal < 128) {
      if (i + literal + 2 < size &&
          data[i + literal] == data[i + literal + 1] &&
          data[i + literal] == data[i + literal + 2]) {
        break;
      }
      ++literal;
    }
    if (literal == 0) {
      literal = 1;
    }
    out.push_back(static_cast<std::uint8_t>(literal - 1));
    out.insert(out.end(), data + i, data + i + literal);
    i += literal;
  }
  return out;
}

// --- Atomic write -----------------------------------------------------------

[[nodiscard]] std::filesystem::path temp_path_for(const std::filesystem::path &target) {
  auto temp = target;
  temp += ".";
  temp += std::to_string(static_cast<std::uint64_t>(
#if defined(_WIN32)
      ::_getpid()
#else
      ::getpid()
#endif
      ));
  temp += ".tmp";
  return temp;
}

[[nodiscard]] Result<std::filesystem::path>
write_file_atomic(const std::filesystem::path &target,
                  const std::function<bool(std::ostream &)> &producer) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const auto parent = target.parent_path();
  if (!parent.empty()) {
    fs::create_directories(parent, ec);
    if (ec) {
      return internal_error();
    }
  }
  const auto temp = temp_path_for(target);
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
      fs::remove(temp, ec);
      return internal_error();
    }
    try {
      if (!producer(out)) {
        out.close();
        fs::remove(temp, ec);
        return internal_error();
      }
    } catch (const std::bad_alloc &) {
      out.close();
      fs::remove(temp, ec);
      return resource_exhausted();
    } catch (...) {
      out.close();
      fs::remove(temp, ec);
      return internal_error();
    }
    out.flush();
    if (!out) {
      fs::remove(temp, ec);
      return internal_error();
    }
  }
  fs::rename(temp, target, ec);
  if (ec) {
    fs::remove(temp, ec);
    return internal_error();
  }
  return target;
}

struct RenderOutcome {
  RasterExportReport report{};
  Error error{};
  bool ok{false};
  bool cancelled{false};
};

[[nodiscard]] RenderOutcome
render_export(const PreparedScene &scene, const ExportSnapshot &snapshot,
              const RasterExportRequest &request, std::stop_token stop,
              const std::function<void(std::uint32_t, std::uint32_t)>
                  &on_tile_progress) {
  RenderOutcome outcome;
  try {
    auto geometry = resolve_geometry(scene, snapshot, request);
    if (!geometry.has_value()) {
      outcome.error = geometry.error();
      return outcome;
    }
    const auto geom = geometry.value();

    if (std::filesystem::exists(request.path) && !request.overwrite_confirmed) {
      outcome.error = overwrite_rejected();
      return outcome;
    }

    const auto tile_count =
        (geom.height + geom.tile_height - 1U) / geom.tile_height;
    std::vector<std::uint8_t> tile(
        static_cast<std::size_t>(geom.width) * geom.tile_height * geom.channels);
    std::uint64_t peak_tile_bytes = tile.size();

    auto written = write_file_atomic(request.path, [&](std::ostream &out) -> bool {
      if (request.format == RasterImageFormat::png) {
        static constexpr unsigned char signature[8] = {137, 80, 78, 71,
                                                       13,  10, 26, 10};
        out.write(reinterpret_cast<const char *>(signature), 8);
        unsigned char ihdr[13] = {};
        ihdr[0] = static_cast<unsigned char>((geom.width >> 24U) & 0xffU);
        ihdr[1] = static_cast<unsigned char>((geom.width >> 16U) & 0xffU);
        ihdr[2] = static_cast<unsigned char>((geom.width >> 8U) & 0xffU);
        ihdr[3] = static_cast<unsigned char>(geom.width & 0xffU);
        ihdr[4] = static_cast<unsigned char>((geom.height >> 24U) & 0xffU);
        ihdr[5] = static_cast<unsigned char>((geom.height >> 16U) & 0xffU);
        ihdr[6] = static_cast<unsigned char>((geom.height >> 8U) & 0xffU);
        ihdr[7] = static_cast<unsigned char>(geom.height & 0xffU);
        ihdr[8] = 8;
        ihdr[9] = geom.channels == 1 ? 0 : 6;
        write_png_chunk(out, "IHDR", ihdr, 13);

        z_stream stream{};
        if (deflateInit(&stream, Z_DEFAULT_COMPRESSION) != Z_OK) {
          return false;
        }
        std::vector<unsigned char> compressed(64U * 1024U);
        auto flush_deflate = [&](int flush_mode) -> bool {
          int ret;
          do {
            stream.next_out = compressed.data();
            stream.avail_out = static_cast<uInt>(compressed.size());
            ret = deflate(&stream, flush_mode);
            if (ret == Z_STREAM_ERROR) {
              return false;
            }
            const auto have = compressed.size() - stream.avail_out;
            if (have > 0) {
              write_png_chunk(out, "IDAT", compressed.data(), have);
            }
          } while (stream.avail_out == 0);
          return true;
        };

        const std::size_t row_bytes =
            static_cast<std::size_t>(geom.width) * geom.channels;
        std::vector<unsigned char> filtered(1U + row_bytes);
        // Previous row's RAW bytes for the Up filter (zero for the first
        // image row, per the PNG spec).
        std::vector<unsigned char> prev_row(row_bytes, 0U);
        for (std::uint32_t t = 0; t < tile_count; ++t) {
          if (stop.stop_requested()) {
            deflateEnd(&stream);
            return false;
          }
          const auto y0 = t * geom.tile_height;
          const auto rows = std::min(geom.tile_height, geom.height - y0);
          tile.resize(static_cast<std::size_t>(geom.width) * rows *
                      geom.channels);
          rasterize_tile(scene, geom, y0, rows, tile, request.background);
          peak_tile_bytes =
              std::max<std::uint64_t>(peak_tile_bytes, tile.size());
          for (std::uint32_t row = 0; row < rows; ++row) {
            const unsigned char *raw =
                tile.data() + static_cast<std::size_t>(row) * row_bytes;
            // Per-row filter selection among None/Sub/Up (minimum sum of
            // absolute filtered bytes — the spec's recommended heuristic).
            // Well-log rasters have large constant-colour regions where Sub
            // and Up collapse rows to zeros; filter type 0 for every row
            // inflated PNGs well beyond necessary size (#489).
            unsigned long sum_none = 0, sum_sub = 0, sum_up = 0;
            for (std::size_t i = 0; i < row_bytes; ++i) {
              const auto left = i >= geom.channels ? raw[i - geom.channels] : 0U;
              sum_none += raw[i];
              sum_sub += static_cast<unsigned char>(raw[i] - left);
              sum_up += static_cast<unsigned char>(raw[i] - prev_row[i]);
            }
            unsigned char filter_type = 0;
            if (sum_sub < sum_none && sum_sub <= sum_up) {
              filter_type = 1;
            } else if (sum_up < sum_none) {
              filter_type = 2;
            }
            filtered[0] = filter_type;
            for (std::size_t i = 0; i < row_bytes; ++i) {
              const auto left = i >= geom.channels ? raw[i - geom.channels] : 0U;
              switch (filter_type) {
              case 1:
                filtered[1U + i] = static_cast<unsigned char>(raw[i] - left);
                break;
              case 2:
                filtered[1U + i] =
                    static_cast<unsigned char>(raw[i] - prev_row[i]);
                break;
              default:
                filtered[1U + i] = raw[i];
                break;
              }
            }
            std::memcpy(prev_row.data(), raw, row_bytes);
            stream.next_in = filtered.data();
            stream.avail_in = static_cast<uInt>(filtered.size());
            if (!flush_deflate(Z_NO_FLUSH)) {
              deflateEnd(&stream);
              return false;
            }
          }
          if (on_tile_progress) {
            on_tile_progress(t + 1, tile_count);
          }
        }
        if (stop.stop_requested()) {
          deflateEnd(&stream);
          return false;
        }
        stream.next_in = Z_NULL;
        stream.avail_in = 0;
        if (!flush_deflate(Z_FINISH)) {
          deflateEnd(&stream);
          return false;
        }
        deflateEnd(&stream);
        write_png_chunk(out, "IEND", nullptr, 0);
        return static_cast<bool>(out);
      }

      // TIFF: stream strips (one strip per tile) without a full-image buffer.
      out.put('I');
      out.put('I');
      write_le16(out, 42);
      const auto ifd_ptr_pos = out.tellp();
      write_le32(out, 0);

      std::vector<std::uint32_t> strip_offsets;
      std::vector<std::uint32_t> strip_bytes;
      std::vector<std::uint32_t> rows_in_strip;
      strip_offsets.reserve(tile_count);
      strip_bytes.reserve(tile_count);
      rows_in_strip.reserve(tile_count);

      for (std::uint32_t t = 0; t < tile_count; ++t) {
        if (stop.stop_requested()) {
          return false;
        }
        const auto y0 = t * geom.tile_height;
        const auto rows = std::min(geom.tile_height, geom.height - y0);
        tile.resize(static_cast<std::size_t>(geom.width) * rows *
                    geom.channels);
        rasterize_tile(scene, geom, y0, rows, tile, request.background);
        peak_tile_bytes =
            std::max<std::uint64_t>(peak_tile_bytes, tile.size());

        std::vector<std::uint8_t> payload;
        if (request.tiff_compression == TiffCompression::packbits) {
          payload = packbits_encode(tile.data(), tile.size());
        } else {
          payload = tile;
        }
        strip_offsets.push_back(static_cast<std::uint32_t>(out.tellp()));
        out.write(reinterpret_cast<const char *>(payload.data()),
                  static_cast<std::streamsize>(payload.size()));
        strip_bytes.push_back(static_cast<std::uint32_t>(payload.size()));
        rows_in_strip.push_back(rows);
        if (on_tile_progress) {
          on_tile_progress(t + 1, tile_count);
        }
      }
      if (stop.stop_requested()) {
        return false;
      }

      std::uint32_t bps_offset = 0;
      if (geom.channels > 1) {
        bps_offset = static_cast<std::uint32_t>(out.tellp());
        for (std::uint32_t c = 0; c < geom.channels; ++c) {
          write_le16(out, 8);
        }
      }
      std::uint32_t offsets_off = 0;
      std::uint32_t counts_off = 0;
      if (strip_offsets.size() > 1) {
        offsets_off = static_cast<std::uint32_t>(out.tellp());
        for (const auto v : strip_offsets) {
          write_le32(out, v);
        }
        counts_off = static_cast<std::uint32_t>(out.tellp());
        for (const auto v : strip_bytes) {
          write_le32(out, v);
        }
      }
      const auto xres_off = static_cast<std::uint32_t>(out.tellp());
      write_le32(out, geom.dpi);
      write_le32(out, 1);
      const auto yres_off = static_cast<std::uint32_t>(out.tellp());
      write_le32(out, geom.dpi);
      write_le32(out, 1);

      const auto ifd_off = static_cast<std::uint32_t>(out.tellp());
      // 12 base entries; 4-channel files add ExtraSamples(338) (#488).
      write_le16(out, geom.channels == 4 ? 13 : 12);
      auto entry = [&](std::uint16_t tag, std::uint16_t type, std::uint32_t count,
                       std::uint32_t value) {
        write_le16(out, tag);
        write_le16(out, type);
        write_le32(out, count);
        write_le32(out, value);
      };
      const auto strip_count = static_cast<std::uint32_t>(strip_offsets.size());
      const std::uint32_t photometric = geom.channels == 1 ? 1U : 2U;
      const std::uint32_t compression_code =
          request.tiff_compression == TiffCompression::packbits ? 32773U : 1U;
      const std::uint32_t bps_value =
          geom.channels == 1 ? 8U : bps_offset;
      const std::uint32_t strip_off_value =
          strip_count == 1 ? strip_offsets.front() : offsets_off;
      const std::uint32_t strip_count_value =
          strip_count == 1 ? strip_bytes.front() : counts_off;

      entry(256, 4, 1, geom.width);
      entry(257, 4, 1, geom.height);
      entry(258, 3, geom.channels, bps_value);
      entry(259, 3, 1, compression_code);
      entry(262, 3, 1, photometric);
      entry(273, 4, strip_count, strip_off_value);
      entry(277, 3, 1, geom.channels);
      entry(278, 4, 1, geom.tile_height);
      entry(279, 4, strip_count, strip_count_value);
      entry(282, 5, 1, xres_off);
      entry(283, 5, 1, yres_off);
      entry(296, 3, 1, 2); // ResolutionUnit = inch
      if (geom.channels == 4) {
        // ExtraSamples = 2 (unassociated alpha): without tag 338 viewers
        // treat the 4th sample as another colour channel and misrender
        // (#488).
        entry(338, 3, 1, 2);
      }
      write_le32(out, 0);

      const auto end = out.tellp();
      out.seekp(ifd_ptr_pos);
      write_le32(out, ifd_off);
      out.seekp(end);
      return static_cast<bool>(out);
    });

    if (stop.stop_requested()) {
      outcome.cancelled = true;
      outcome.error = cancelled_error();
      std::error_code ec;
      std::filesystem::remove(temp_path_for(request.path), ec);
      return outcome;
    }
    if (!written.has_value()) {
      // Distinguishing cancel-induced false from encode failure: if stop was
      // requested we already returned; otherwise surface internal_error or
      // the Result error.
      outcome.error = written.error();
      std::error_code ec;
      std::filesystem::remove(temp_path_for(request.path), ec);
      return outcome;
    }

    outcome.report = RasterExportReport{
        .path = written.value(),
        .format = request.format,
        .width_px = geom.width,
        .height_px = geom.height,
        .dpi = geom.dpi,
        .color_space = request.color_space,
        .document_revision = snapshot.document_revision,
        .presentation_version = snapshot.presentation_version,
        .document_id = snapshot.document_id,
        .peak_tile_bytes = peak_tile_bytes,
    };
    outcome.ok = true;
    return outcome;
  } catch (const std::bad_alloc &) {
    outcome.error = resource_exhausted();
    return outcome;
  } catch (...) {
    outcome.error = internal_error();
    return outcome;
  }
}

} // namespace

struct RasterExportJob::Impl {
  PreparedScene scene;
  ExportSnapshot snapshot;
  RasterExportRequest request;
  std::jthread worker;
  mutable std::mutex mutex;
  RasterExportState state{RasterExportState::running};
  double fraction{};
  std::vector<RasterExportProgress> pending_progress;
  double last_emitted_fraction{-1.0};
  Result<RasterExportReport> final_result{internal_error()};
};

RasterExportJob::RasterExportJob(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

RasterExportJob::~RasterExportJob() {
  if (impl_ && impl_->worker.joinable()) {
    impl_->worker.request_stop();
    // Unlike WellLogSession, the raster worker captures a raw Impl* (not a
    // shared_ptr), so we cannot detach — the worker must finish before Impl is
    // destroyed. Give it a bounded window to observe the stop_token and exit
    // cooperatively; render_export checks stop at each tile so this is normally
    // fast. After the window, the jthread dtor's implicit join completes the
    // reap. The DLL-teardown loader-lock deadlock (#241) itself is avoided at
    // the test layer via _Exit in fail().
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < deadline) {
      {
        std::lock_guard lock(impl_->mutex);
        if (impl_->state != RasterExportState::running) {
          break; // worker has transitioned to a terminal state
        }
      }
      std::this_thread::yield();
    }
  }
}

Result<std::shared_ptr<RasterExportJob>>
RasterExportJob::start(PreparedScene scene, ExportSnapshot snapshot,
                       RasterExportRequest request) {
  try {
    auto geometry = resolve_geometry(scene, snapshot, request);
    if (!geometry.has_value()) {
      return geometry.error();
    }
    if (request.path.empty()) {
      return presentation_error();
    }
    if (std::filesystem::exists(request.path) && !request.overwrite_confirmed) {
      return overwrite_rejected();
    }

    auto impl = std::make_unique<Impl>();
    impl->scene = std::move(scene);
    impl->snapshot = std::move(snapshot);
    impl->request = std::move(request);

    auto job = std::shared_ptr<RasterExportJob>(
        new RasterExportJob(std::move(impl)));
    auto *state = job->impl_.get();
    state->worker = std::jthread([state](std::stop_token stop) {
      auto emit = [&](std::uint32_t done, std::uint32_t total) {
        std::lock_guard lock(state->mutex);
        const auto fraction =
            total == 0 ? 1.0
                       : static_cast<double>(done) / static_cast<double>(total);
        state->fraction = fraction;
        if (state->last_emitted_fraction < 0.0 || fraction + 1.0e-12 >= 1.0 ||
            fraction - state->last_emitted_fraction >= 0.05 - 1.0e-12) {
          state->pending_progress.push_back(RasterExportProgress{
              .fraction = fraction,
              .tiles_completed = done,
              .tiles_total = total,
          });
          state->last_emitted_fraction = fraction;
        }
      };

      auto outcome = render_export(state->scene, state->snapshot,
                                   state->request, stop, emit);
      std::lock_guard lock(state->mutex);
      if (outcome.cancelled || stop.stop_requested()) {
        state->state = RasterExportState::cancelled;
        state->final_result = cancelled_error();
        std::error_code ec;
        std::filesystem::remove(temp_path_for(state->request.path), ec);
        return;
      }
      if (!outcome.ok) {
        state->state = RasterExportState::failed;
        state->final_result = outcome.error;
        return;
      }
      state->state = RasterExportState::completed;
      state->fraction = 1.0;
      state->final_result = std::move(outcome.report);
    });
    return job;
  } catch (const std::bad_alloc &) {
    return resource_exhausted();
  } catch (...) {
    return internal_error();
  }
}

void RasterExportJob::request_cancel() noexcept {
  if (impl_) {
    impl_->worker.request_stop();
  }
}

RasterExportState RasterExportJob::poll(
    std::vector<RasterExportProgress> *out_progress) noexcept {
  if (!impl_) {
    return RasterExportState::failed;
  }
  std::lock_guard lock(impl_->mutex);
  if (out_progress != nullptr) {
    out_progress->insert(out_progress->end(), impl_->pending_progress.begin(),
                         impl_->pending_progress.end());
    impl_->pending_progress.clear();
  }
  return impl_->state;
}

RasterExportState RasterExportJob::state() const noexcept {
  if (!impl_) {
    return RasterExportState::failed;
  }
  std::lock_guard lock(impl_->mutex);
  return impl_->state;
}

double RasterExportJob::progress_fraction() const noexcept {
  if (!impl_) {
    return 0.0;
  }
  std::lock_guard lock(impl_->mutex);
  return impl_->fraction;
}

Result<RasterExportReport> RasterExportJob::result() const {
  if (!impl_) {
    return internal_error();
  }
  std::lock_guard lock(impl_->mutex);
  if (impl_->state == RasterExportState::running) {
    return internal_error();
  }
  return impl_->final_result;
}

Result<RasterExportReport>
export_raster_sync(const PreparedScene &scene, const ExportSnapshot &snapshot,
                   const RasterExportRequest &request) noexcept {
  std::stop_source source;
  auto outcome =
      render_export(scene, snapshot, request, source.get_token(), {});
  if (outcome.cancelled) {
    return cancelled_error();
  }
  if (!outcome.ok) {
    return outcome.error;
  }
  return outcome.report;
}

} // namespace welllog
