#pragma once

// Multi-resolution image pyramid for a raster ImageSource (rendering.md
// section 10). Unlike CurveLodPyramid (a 1-D scalar envelope), this describes
// a 2-D tiling structure: the full-resolution image is divided into square
// tiles, and each coarser level halves both dimensions. The pyramid does NOT
// decode pixels — build() only computes the level/tile grid from the source
// metadata; query() selects which (level, row, col) tiles are visible (+ a
// prefetch margin) at a resolution matching the viewport density. The host
// supplies decoded tile bytes via ManifestResolvers::image_tile (ADR 0042).

#include <cstdint>
#include <stop_token>
#include <vector>

#include <welllog/core/document.hpp>
#include <welllog/core/result.hpp>
#include <welllog/scene/export.hpp>

namespace welllog {

struct ImagePyramidOptions {
  // Tile edge length in pixels (square tiles). Must be a power of two >= 64.
  std::uint32_t tile_size{256};
  // Hard cap on derived (level/tile metadata) bytes; building degrades to a
  // coarser pyramid rather than failing (ADR 0034).
  std::uint64_t maximum_derived_bytes{64ULL * 1024ULL * 1024ULL};
};

struct ImagePyramidQuery {
  // Visible depth window in reference-depth units.
  double viewport_top{};
  double viewport_bottom{};
  // Vertical pixel height of the viewport (drives level selection so a tile
  // covers roughly one screen pixel).
  double pixel_height{};
  // Symmetric prefetch expansion of the viewport, in viewport-heights.
  double prefetch_viewports{2.0};
};

// One selected tile: its pyramid coordinates and the depth span it covers in
// the source image (reference-depth units), used to place it in the scene.
struct ImagePyramidTile {
  std::uint32_t level{};
  std::uint32_t row{};
  std::uint32_t col{};
  std::uint32_t width_px{};
  std::uint32_t height_px{};
  double top_reference_depth{};
  double bottom_reference_depth{};
};

struct ImagePyramidSelection {
  std::vector<ImagePyramidTile> tiles;
};

struct ImagePyramidStatistics {
  std::uint64_t width_px{};
  std::uint64_t height_px{};
  std::uint32_t level_count{};
  std::uint32_t tile_size{};
  std::uint64_t derived_bytes{};
  std::uint64_t maximum_derived_bytes{};
  bool budget_limited{};
};

class WELLLOG_SCENE_API ImagePyramid {
public:
  ImagePyramid();
  ~ImagePyramid();
  ImagePyramid(const ImagePyramid &);
  ImagePyramid &operator=(const ImagePyramid &);
  ImagePyramid(ImagePyramid &&) noexcept;
  ImagePyramid &operator=(ImagePyramid &&) noexcept;

  [[nodiscard]] static Result<ImagePyramid>
  build(const ImageSource &source, ImagePyramidOptions options = {},
        std::stop_token stop_token = {}) noexcept;

  [[nodiscard]] Result<ImagePyramidSelection>
  query(const ImagePyramidQuery &query,
        std::stop_token stop_token = {}) const noexcept;

  [[nodiscard]] ImagePyramidStatistics statistics() const noexcept;

private:
  struct Impl;
  std::shared_ptr<const Impl> impl_;
  explicit ImagePyramid(std::shared_ptr<const Impl> impl);
};

} // namespace welllog
