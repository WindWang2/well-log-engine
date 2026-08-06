#include <welllog/scene/image_pyramid.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace welllog {
namespace {

[[nodiscard]] Error image_error(EntityId entity_id, ErrorCode code,
                                MessageKey message) {
  return Error{
      .code = code,
      .severity = Severity::error,
      .entity_id = entity_id.is_nil() ? std::nullopt
                                      : std::optional<EntityId>{entity_id},
      .message = message,
      .arguments = {},
  };
}

// Number of levels for an image of `edge_px` at `tile_size`: keep halving
// until a single tile covers the whole image. Always >= 1.
[[nodiscard]] std::uint32_t
level_count_for(std::uint64_t edge_px, std::uint32_t tile_size) {
  std::uint32_t count = 1;
  auto edge = edge_px;
  while (edge > tile_size) {
    edge = (edge + 1) / 2;
    ++count;
  }
  return count;
}

// Tile-grid dimensions of one level (tiles along each axis).
[[nodiscard]] std::uint32_t tiles_along(std::uint64_t edge_px,
                                        std::uint32_t tile_size,
                                        std::uint32_t level) {
  auto edge = edge_px;
  for (std::uint32_t l = 0; l < level; ++l) {
    edge = (edge + 1) / 2;
  }
  return static_cast<std::uint32_t>((edge + tile_size - 1) / tile_size);
}

// Pixel width of the image edge at one level (after halving).
[[nodiscard]] std::uint64_t edge_at_level(std::uint64_t edge_px,
                                          std::uint32_t level) {
  auto edge = edge_px;
  for (std::uint32_t l = 0; l < level; ++l) {
    edge = (edge + 1) / 2;
  }
  return edge;
}

// Pixel extent of one tile along an axis (last tile may be smaller).
[[nodiscard]] std::uint64_t edge_pixels(std::uint64_t edge_px,
                                        std::uint32_t tile_size,
                                        std::uint32_t level,
                                        std::uint32_t tile_index) {
  const auto edge = edge_at_level(edge_px, level);
  const auto start = static_cast<std::uint64_t>(tile_index) * tile_size;
  if (start >= edge) {
    return 0;
  }
  const auto remaining = edge - start;
  return std::min<std::uint64_t>(tile_size, remaining);
}

} // namespace

struct ImagePyramid::Impl {
  std::uint64_t width_px{};
  std::uint64_t height_px{};
  double reference_depth_top{};
  double reference_depth_bottom{};
  std::uint32_t tile_size{};
  std::uint32_t level_count{};
  std::uint64_t derived_bytes{};
  std::uint64_t maximum_derived_bytes{};
  bool budget_limited{};
};

ImagePyramid::ImagePyramid() = default;
ImagePyramid::~ImagePyramid() = default;
ImagePyramid::ImagePyramid(const ImagePyramid &) = default;
ImagePyramid &ImagePyramid::operator=(const ImagePyramid &) = default;
ImagePyramid::ImagePyramid(ImagePyramid &&) noexcept = default;
ImagePyramid &ImagePyramid::operator=(ImagePyramid &&) noexcept = default;

ImagePyramid::ImagePyramid(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

Result<ImagePyramid>
ImagePyramid::build(const ImageSource &source, ImagePyramidOptions options,
                    std::stop_token stop_token) noexcept {
  static_cast<void>(stop_token);
  constexpr std::uint64_t minimum_tile_size = 64;
  if (options.tile_size < minimum_tile_size || (options.tile_size & (options.tile_size - 1)) != 0) {
    return image_error(source.id, ErrorCode::invalid_image,
                       MessageKey::image_metadata_invalid);
  }
  if (source.width_px == 0 || source.height_px == 0 ||
      source.reference_depth_bottom <= source.reference_depth_top ||
      !std::isfinite(source.reference_depth_top) ||
      !std::isfinite(source.reference_depth_bottom)) {
    return image_error(source.id, ErrorCode::invalid_image,
                       MessageKey::image_metadata_invalid);
  }
  try {
    auto impl = std::make_shared<Impl>();
    impl->width_px = source.width_px;
    impl->height_px = source.height_px;
    impl->reference_depth_top = source.reference_depth_top;
    impl->reference_depth_bottom = source.reference_depth_bottom;
    impl->tile_size = options.tile_size;
    impl->maximum_derived_bytes = options.maximum_derived_bytes;

    // Compute level count, capping by the derived-byte budget. Each level
    // costs ~tiles*sizeof(Tile) of metadata; stop adding coarser levels when
    // the budget is exhausted (degrade, don't fail — ADR 0034).
    const auto full_levels =
        std::max(level_count_for(source.width_px, options.tile_size),
                 level_count_for(source.height_px, options.tile_size));
    std::uint32_t levels = 0;
    std::uint64_t derived = 0;
    for (std::uint32_t level = 0; level < full_levels; ++level) {
      const auto tiles_x =
          tiles_along(source.width_px, options.tile_size, level);
      const auto tiles_y =
          tiles_along(source.height_px, options.tile_size, level);
      const auto level_bytes =
          static_cast<std::uint64_t>(tiles_x) * tiles_y * sizeof(ImagePyramidTile);
      if (derived + level_bytes > options.maximum_derived_bytes && levels > 0) {
        impl->budget_limited = true;
        break;
      }
      derived += level_bytes;
      ++levels;
    }
    if (levels == 0) {
      levels = 1;
    }
    impl->level_count = levels;
    impl->derived_bytes = derived;
    return ImagePyramid{std::move(impl)};
  } catch (const std::bad_alloc &) {
    return image_error(source.id, ErrorCode::resource_exhausted,
                       MessageKey::resource_exhausted);
  } catch (...) {
    return image_error(source.id, ErrorCode::internal_error,
                       MessageKey::internal_error);
  }
}

Result<ImagePyramidSelection>
ImagePyramid::query(const ImagePyramidQuery &query,
                    std::stop_token stop_token) const noexcept {
  static_cast<void>(stop_token);
  ImagePyramidSelection selection;
  if (impl_ == nullptr) {
    return selection;
  }
  if (!std::isfinite(query.viewport_top) ||
      !std::isfinite(query.viewport_bottom) ||
      query.viewport_bottom <= query.viewport_top ||
      query.pixel_height <= 0.0) {
    return selection;
  }
  const auto depth_span =
      impl_->reference_depth_bottom - impl_->reference_depth_top;
  if (depth_span <= 0.0) {
    return selection;
  }

  // Expand the viewport by the prefetch margin (symmetric).
  const auto vh = query.viewport_bottom - query.viewport_top;
  const auto prefetch = vh * std::max(0.0, query.prefetch_viewports);
  const auto q_top = query.viewport_top - prefetch;
  const auto q_bottom = query.viewport_bottom + prefetch;

  // Intersect with the image's depth range.
  const auto top = std::max(q_top, impl_->reference_depth_top);
  const auto bottom = std::min(q_bottom, impl_->reference_depth_bottom);
  if (bottom <= top) {
    return selection; // viewport does not overlap the image
  }

  // Pick the coarsest level whose pixel density still covers ~1 screen pixel
  // per source pixel vertically. samples_per_pixel = (image px over the
  // visible depth span) / viewport pixels.
  const auto visible_depth = query.viewport_bottom - query.viewport_top;
  const auto px_over_span =
      (visible_depth / depth_span) * static_cast<double>(impl_->height_px);
  const auto samples_per_pixel = px_over_span / std::max(1.0, query.pixel_height);
  // Start at level 0 and halve until a tile (~tile_size px) covers >= 1 pixel.
  std::uint32_t level = 0;
  auto density = samples_per_pixel;
  while (level + 1 < impl_->level_count &&
         density > static_cast<double>(impl_->tile_size)) {
    density *= 0.5;
    ++level;
  }

  const auto tiles_y = tiles_along(impl_->height_px, impl_->tile_size, level);
  // Map the overlapping depth span to tile rows.
  const auto first_row_d = top - impl_->reference_depth_top;
  const auto last_row_d = bottom - impl_->reference_depth_top;
  const auto row_top = static_cast<std::uint32_t>(
      std::min<double>(static_cast<double>(tiles_y),
                       (first_row_d / depth_span) * tiles_y));
  const auto row_bottom_exclusive = static_cast<std::uint32_t>(
      std::min<double>(static_cast<double>(tiles_y),
                       std::ceil((last_row_d / depth_span) * tiles_y)));

  const auto tiles_x = tiles_along(impl_->width_px, impl_->tile_size, level);
  const auto px_per_tile_top =
      depth_span / static_cast<double>(tiles_y);
  try {
    for (std::uint32_t row = row_top; row < row_bottom_exclusive; ++row) {
      for (std::uint32_t col = 0; col < tiles_x; ++col) {
        const auto tile_top_d =
            impl_->reference_depth_top + row * px_per_tile_top;
        const auto tile_bottom_d =
            impl_->reference_depth_top + (row + 1) * px_per_tile_top;
        const auto tile_w =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(
                impl_->tile_size,
                edge_pixels(impl_->width_px, impl_->tile_size, level, col)));
        const auto tile_h =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(
                impl_->tile_size,
                edge_pixels(impl_->height_px, impl_->tile_size, level, row)));
        selection.tiles.push_back(ImagePyramidTile{
            .level = level,
            .row = row,
            .col = col,
            .width_px = tile_w,
            .height_px = tile_h,
            .top_reference_depth = tile_top_d,
            .bottom_reference_depth = tile_bottom_d,
        });
      }
    }
  } catch (const std::bad_alloc &) {
    return image_error(EntityId{}, ErrorCode::resource_exhausted,
                       MessageKey::resource_exhausted);
  } catch (...) {
    return image_error(EntityId{}, ErrorCode::internal_error,
                       MessageKey::internal_error);
  }
  return selection;
}

ImagePyramidStatistics ImagePyramid::statistics() const noexcept {
  if (impl_ == nullptr) {
    return {};
  }
  return ImagePyramidStatistics{
      .width_px = impl_->width_px,
      .height_px = impl_->height_px,
      .level_count = impl_->level_count,
      .tile_size = impl_->tile_size,
      .derived_bytes = impl_->derived_bytes,
      .maximum_derived_bytes = impl_->maximum_derived_bytes,
      .budget_limited = impl_->budget_limited,
  };
}

} // namespace welllog
