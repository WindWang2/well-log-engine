#include "render_gl/raster.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

using namespace welllog;
using namespace welllog::detail;

[[noreturn]] void fail(std::string_view message) {
  std::cerr << "FAIL: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void require(bool condition, std::string_view message) {
  if (!condition) {
    fail(message);
  }
}

EntityId id(std::string_view text) {
  const auto parsed = EntityId::parse(text);
  require(parsed.has_value(), "test UUID must be valid");
  return *parsed;
}

[[nodiscard]] std::uint8_t alpha_at(const RasterImage &image,
                                    std::uint32_t column, std::uint32_t row) {
  return image.pixels[(static_cast<std::size_t>(row) * image.width + column) *
                          4 +
                      3];
}

void pattern_tiles_rasterize_background_and_lines() {
  const auto pattern = PatternDefinition{
      .id = id("50000000-0000-4000-8000-000000000001"),
      .tile_width = Millimetres{4.0},
      .tile_height = Millimetres{4.0},
      .rotation_degrees = 0.0,
      .foreground = RgbaColor{255, 0, 0, 255},
      .background = RgbaColor{0, 0, 255, 255},
      .stroke_width = Millimetres{0.5},
      .scene_anchor = {},
      .primitives =
          {
              PatternLine{PhysicalPoint{Millimetres{0.0}, Millimetres{0.0}},
                          PhysicalPoint{Millimetres{4.0}, Millimetres{4.0}}},
          },
  };
  const auto tile = rasterize_pattern_tile(pattern, 16.0);
  require(tile.width == 64 && tile.height == 64,
          "4mm tiles at 16px/mm must rasterize to 64px");
  require(tile.channels == 4, "pattern tiles must be RGBA");
  // A pixel on the diagonal must be foreground red; a pixel far from it
  // must be background blue.
  const auto on_line = (static_cast<std::size_t>(32) * tile.width + 32) * 4;
  require(tile.pixels[on_line + 0] > 200 && tile.pixels[on_line + 2] < 100,
          "diagonal pixels must carry the foreground color");
  const auto off_line = (static_cast<std::size_t>(4) * tile.width + 60) * 4;
  require(tile.pixels[off_line + 2] > 200 && tile.pixels[off_line + 0] < 100,
          "off-line pixels must carry the background color");
  require(alpha_at(tile, 32, 32) == 255 && alpha_at(tile, 60, 4) == 255,
          "opaque backgrounds must stay opaque");

  const auto repeated = rasterize_pattern_tile(pattern, 16.0);
  require(repeated.pixels == tile.pixels,
          "pattern rasterization must be deterministic");
}

void transparent_backgrounds_stay_transparent() {
  const auto pattern = PatternDefinition{
      .id = id("50000000-0000-4000-8000-000000000002"),
      .tile_width = Millimetres{2.0},
      .tile_height = Millimetres{2.0},
      .rotation_degrees = 0.0,
      .foreground = RgbaColor{0, 0, 0, 255},
      .background = RgbaColor{0, 0, 0, 0},
      .stroke_width = Millimetres{0.2},
      .scene_anchor = {},
      .primitives =
          {
              PatternCircle{PhysicalPoint{Millimetres{1.0}, Millimetres{1.0}},
                            Millimetres{0.5}, true},
          },
  };
  const auto tile = rasterize_pattern_tile(pattern, 16.0);
  require(alpha_at(tile, 16, 16) == 255,
          "filled circle centers must be opaque");
  require(alpha_at(tile, 2, 2) == 0,
          "transparent tile regions must keep zero alpha");
}

void glyph_outlines_rasterize_with_winding_fill() {
  // A 1x1 em square outline.
  const OutlineCommand commands[] = {
      OutlineCommand{.verb = OutlineVerb::move_to,
                     .coordinates = {0.0, 0.0}},
      OutlineCommand{.verb = OutlineVerb::line_to,
                     .coordinates = {1.0, 0.0}},
      OutlineCommand{.verb = OutlineVerb::line_to,
                     .coordinates = {1.0, 1.0}},
      OutlineCommand{.verb = OutlineVerb::line_to,
                     .coordinates = {0.0, 1.0}},
      OutlineCommand{.verb = OutlineVerb::close, .coordinates = {}},
  };
  const auto raster = rasterize_glyph_outline(commands, 0.0, 0.0, 1.0, 1.0,
                                              32.0);
  require(raster.width == 34 && raster.height == 34,
          "raster size must cover the bounds plus padding");
  require(raster.pixels_per_em == 32.0, "density must round-trip");
  const auto center =
      raster.alpha[(static_cast<std::size_t>(17) * raster.width + 17)];
  require(center == 255, "the square interior must be fully covered");
  const auto outside = raster.alpha[0];
  require(outside == 0, "padding outside the outline must be empty");

  // A square with a reversed inner square (a ring) must leave a hole.
  const OutlineCommand ring[] = {
      OutlineCommand{.verb = OutlineVerb::move_to,
                     .coordinates = {0.0, 0.0}},
      OutlineCommand{.verb = OutlineVerb::line_to,
                     .coordinates = {1.0, 0.0}},
      OutlineCommand{.verb = OutlineVerb::line_to,
                     .coordinates = {1.0, 1.0}},
      OutlineCommand{.verb = OutlineVerb::line_to,
                     .coordinates = {0.0, 1.0}},
      OutlineCommand{.verb = OutlineVerb::close, .coordinates = {}},
      OutlineCommand{.verb = OutlineVerb::move_to,
                     .coordinates = {0.25, 0.25}},
      OutlineCommand{.verb = OutlineVerb::line_to,
                     .coordinates = {0.25, 0.75}},
      OutlineCommand{.verb = OutlineVerb::line_to,
                     .coordinates = {0.75, 0.75}},
      OutlineCommand{.verb = OutlineVerb::line_to,
                     .coordinates = {0.75, 0.25}},
      OutlineCommand{.verb = OutlineVerb::close, .coordinates = {}},
  };
  const auto ring_raster =
      rasterize_glyph_outline(ring, 0.0, 0.0, 1.0, 1.0, 32.0);
  const auto hole_x = static_cast<std::uint32_t>(
      std::lround((0.5 - ring_raster.left_em) * 32.0));
  const auto hole_y = static_cast<std::uint32_t>(
      std::lround((ring_raster.top_em - 0.5) * 32.0));
  require(ring_raster.alpha[static_cast<std::size_t>(hole_y) *
                                ring_raster.width +
                            hole_x] == 0,
          "counter-wound contours must punch holes");
  const auto wall_x = static_cast<std::uint32_t>(
      std::lround((0.125 - ring_raster.left_em) * 32.0));
  require(ring_raster.alpha[static_cast<std::size_t>(hole_y) *
                                ring_raster.width +
                            wall_x] == 255,
          "the ring wall must stay covered");
}

void atlas_packing_is_deterministic_and_bounded() {
  ShelfAtlasPacker packer(64, 32);
  const auto first = packer.allocate(16, 8);
  const auto second = packer.allocate(16, 8);
  require(first.has_value() && second.has_value(),
          "small rects must allocate");
  require(first->left == 0 && first->top == 0, "first rect starts at origin");
  require(second->left == 16 && second->top == 0,
          "rects pack along the shelf");
  const auto third = packer.allocate(48, 8);
  require(!third.has_value() || third->top >= 8,
          "overflowing shelves must wrap to a new shelf");
  ShelfAtlasPacker tight(16, 16);
  require(!tight.allocate(32, 8).has_value(),
          "oversized rects must be rejected");
  require(!tight.allocate(0, 8).has_value(), "empty rects must be rejected");
  const auto fill = tight.allocate(16, 16);
  require(fill.has_value(), "an exactly fitting rect must allocate");
  require(!tight.allocate(1, 1).has_value(),
          "a full atlas must reject further allocations");
}

} // namespace

int main() {
  pattern_tiles_rasterize_background_and_lines();
  transparent_backgrounds_stay_transparent();
  glyph_outlines_rasterize_with_winding_fill();
  atlas_packing_is_deterministic_and_bounded();
  std::cout << "PASS: pattern and glyph rasterization\n";
  return EXIT_SUCCESS;
}
