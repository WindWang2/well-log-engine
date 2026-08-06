#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <welllog/render_gl/export.hpp>
#include <welllog/scene/scene.hpp>

namespace welllog::detail {

struct WELLLOG_RENDER_GL_API RasterImage {
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t channels{4};
  std::vector<std::uint8_t> pixels;
};

// Rasterizes one pattern tile (background plus constrained vector
// primitives) into an RGBA image. Rotation is not applied here; the
// shader rotates tile coordinates around the scene anchor, matching the
// vector export's patternTransform.
// Exported so unit tests can link against shared libwelllog-render-gl.
[[nodiscard]] WELLLOG_RENDER_GL_API RasterImage
rasterize_pattern_tile(const PatternDefinition &pattern,
                       double pixels_per_millimetre);

struct WELLLOG_RENDER_GL_API GlyphRaster {
  std::uint32_t width{};
  std::uint32_t height{};
  // Em-space position of the bitmap's left and top edges relative to the
  // glyph origin, plus the density the bitmap was rendered at.
  double left_em{};
  double top_em{};
  double pixels_per_em{1.0};
  std::vector<std::uint8_t> alpha;
};

// Rasterizes a glyph outline (em fractions, y-up) into an 8-bit alpha
// bitmap with a one-pixel pad and 2x2 supersampling.
[[nodiscard]] WELLLOG_RENDER_GL_API GlyphRaster
rasterize_glyph_outline(std::span<const OutlineCommand> commands,
                        double left_em, double bottom_em, double right_em,
                        double top_em, double pixels_per_em);

// Deterministic shelf packer for texture atlases.
class WELLLOG_RENDER_GL_API ShelfAtlasPacker {
public:
  struct Rect {
    std::uint32_t left{};
    std::uint32_t top{};
    std::uint32_t width{};
    std::uint32_t height{};
  };

  ShelfAtlasPacker(std::uint32_t width, std::uint32_t height) noexcept;

  [[nodiscard]] std::optional<Rect> allocate(std::uint32_t width,
                                             std::uint32_t height) noexcept;

private:
  std::uint32_t width_{};
  std::uint32_t height_{};
  std::uint32_t shelf_top_{};
  std::uint32_t shelf_height_{};
  std::uint32_t cursor_{};
};

} // namespace welllog::detail
