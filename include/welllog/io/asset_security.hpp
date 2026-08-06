#pragma once

// Untrusted asset URI / ImageSource / CustomLayerSource policy (#172, ADR 0042).
// Pattern and Custom Layer are declarative-only; URIs may not carry scripts,
// shaders, commands, or network schemes.

#include <cstdint>
#include <optional>
#include <string_view>

#include <welllog/core/document.hpp>
#include <welllog/core/result.hpp>
#include <welllog/io/export.hpp>

namespace welllog {

// Rejects javascript:/vbscript:/data: (non-file), network schemes, and
// obvious script/shader tokens. Relative file paths and bare ids are allowed.
[[nodiscard]] WELLLOG_IO_API bool
is_safe_untrusted_asset_uri(std::string_view uri) noexcept;

// ImageSource structural limits (mirrors scene/manifest ADR 0042 ceilings).
struct ImageAssetLimits {
  std::uint64_t max_dimension_px{65536};
  std::uint64_t max_pixels{512ULL * 1024ULL * 1024ULL};
  std::uint32_t min_dpi{1};
  std::uint32_t max_dpi{1'000'000};
};

[[nodiscard]] WELLLOG_IO_API std::optional<Error>
validate_image_source_limits(const ImageSource &source,
                             ImageAssetLimits limits = {}) noexcept;

// Custom layer: non-empty primitives, point/primitive ceilings, safe optional
// string fields if present on nested structures (none today — structural).
struct CustomLayerAssetLimits {
  std::size_t max_primitives{4096};
  std::size_t max_vertices{1ULL << 20};
  std::size_t max_polyline_points{8192};
  std::size_t max_clip_points{8192};
};

[[nodiscard]] WELLLOG_IO_API std::optional<Error>
validate_custom_layer_source(const CustomLayerSource &source,
                             CustomLayerAssetLimits limits = {}) noexcept;

// Pattern: positive tile size, finite coords, primitive count ceiling.
struct PatternAssetLimits {
  std::size_t max_primitives{4096};
  std::size_t max_polyline_points{8192};
  double max_tile_millimetres{10'000.0};
};

// Forward-declared in scene; validation lives here so IO/scene tests share it
// without pulling GL. Implemented against PatternDefinition fields by value
// copy of the essential checks.
struct PatternDefinition;

[[nodiscard]] WELLLOG_IO_API std::optional<Error>
validate_pattern_definition_limits(
    std::size_t primitive_count, std::size_t max_polyline_points_seen,
    double tile_width_mm, double tile_height_mm,
    PatternAssetLimits limits = {}) noexcept;

} // namespace welllog
