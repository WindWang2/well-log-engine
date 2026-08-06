#include <welllog/io/asset_security.hpp>

#include <welllog/core/checked_math.hpp>

#include <cctype>
#include <cmath>

namespace welllog {
namespace {

[[nodiscard]] bool starts_with_ci(std::string_view s,
                                  std::string_view prefix) noexcept {
  if (s.size() < prefix.size()) {
    return false;
  }
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(s[i])) !=
        std::tolower(static_cast<unsigned char>(prefix[i]))) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool contains_ci(std::string_view hay,
                               std::string_view needle) noexcept {
  if (needle.empty() || hay.size() < needle.size()) {
    return false;
  }
  for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
    bool match = true;
    for (std::size_t j = 0; j < needle.size(); ++j) {
      if (std::tolower(static_cast<unsigned char>(hay[i + j])) !=
          std::tolower(static_cast<unsigned char>(needle[j]))) {
        match = false;
        break;
      }
    }
    if (match) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] Error asset_error(ErrorCode code, MessageKey message) {
  return Error{.code = code,
               .severity = Severity::error,
               .entity_id = std::nullopt,
               .message = message,
               .arguments = {}};
}

} // namespace

bool is_safe_untrusted_asset_uri(std::string_view uri) noexcept {
  if (uri.empty() || uri.size() > 4096) {
    return false;
  }
  for (const auto c : uri) {
    if (static_cast<unsigned char>(c) < 0x20) {
      return false;
    }
  }
  // Executable / network schemes never accepted for untrusted assets.
  constexpr std::string_view blocked_schemes[] = {
      "javascript:", "vbscript:", "data:", "http://", "https://", "ftp://",
      "ws://",       "wss://",    "file://",
  };
  for (const auto scheme : blocked_schemes) {
    if (starts_with_ci(uri, scheme)) {
      return false;
    }
  }
  // Script / shader / command markers that must never appear in URIs.
  constexpr std::string_view blocked_tokens[] = {
      "<script",  "onload=",  "eval(",     "glCreateShader",
      "glShader", "#version", "void main", "system(",
      "exec(",    "cmd.exe",  "/bin/sh",
  };
  for (const auto token : blocked_tokens) {
    if (contains_ci(uri, token)) {
      return false;
    }
  }
  return true;
}

std::optional<Error>
validate_image_source_limits(const ImageSource &source,
                             ImageAssetLimits limits) noexcept {
  if (source.id.is_nil()) {
    return asset_error(ErrorCode::invalid_image, MessageKey::image_metadata_invalid);
  }
  if (source.width_px == 0 || source.height_px == 0 ||
      source.width_px > limits.max_dimension_px ||
      source.height_px > limits.max_dimension_px) {
    return asset_error(ErrorCode::invalid_image,
                       MessageKey::image_dimension_exceeds_limit);
  }
  const auto pixels = checked_mul_u64(source.width_px, source.height_px);
  if (!pixels.has_value() || *pixels > limits.max_pixels) {
    return asset_error(ErrorCode::invalid_image,
                       MessageKey::image_pixels_exceed_limit);
  }
  if (source.dpi < limits.min_dpi || source.dpi > limits.max_dpi) {
    return asset_error(ErrorCode::invalid_image, MessageKey::image_metadata_invalid);
  }
  if (!(source.reference_depth_bottom > source.reference_depth_top) ||
      !std::isfinite(source.reference_depth_top) ||
      !std::isfinite(source.reference_depth_bottom)) {
    return asset_error(ErrorCode::invalid_image, MessageKey::image_metadata_invalid);
  }
  if (!is_safe_untrusted_asset_uri(source.source.uri)) {
    return asset_error(ErrorCode::invalid_image, MessageKey::image_metadata_invalid);
  }
  return std::nullopt;
}

std::optional<Error>
validate_custom_layer_source(const CustomLayerSource &source,
                             CustomLayerAssetLimits limits) noexcept {
  if (source.id.is_nil()) {
    return asset_error(ErrorCode::invalid_custom_source,
                       MessageKey::custom_source_empty);
  }
  if (source.primitives.empty()) {
    return asset_error(ErrorCode::invalid_custom_source,
                       MessageKey::custom_source_empty);
  }
  if (source.primitives.size() > limits.max_primitives) {
    return asset_error(ErrorCode::invalid_custom_source,
                       MessageKey::custom_source_primitives_exceed_limit);
  }
  std::size_t vertices = 0;
  for (const auto &primitive : source.primitives) {
    if (const auto *poly = std::get_if<CustomPolyline>(&primitive)) {
      if (poly->points.size() < 2 ||
          poly->points.size() > limits.max_polyline_points) {
        return asset_error(ErrorCode::invalid_custom_source,
                           MessageKey::custom_source_points_exceed_limit);
      }
      vertices += poly->points.size();
    } else if (std::holds_alternative<CustomTriangle>(primitive)) {
      vertices += 3;
    } else if (std::holds_alternative<CustomQuad>(primitive)) {
      vertices += 6; // tessellated count (mirrors scene)
    } else if (std::holds_alternative<CustomSymbolOccurrence>(primitive)) {
      vertices += 24;
    }
    if (vertices > limits.max_vertices) {
      return asset_error(ErrorCode::invalid_custom_source,
                         MessageKey::custom_source_points_exceed_limit);
    }
  }
  if (source.clip.has_value()) {
    if (source.clip->points.size() < 3 ||
        source.clip->points.size() > limits.max_clip_points) {
      return asset_error(ErrorCode::invalid_custom_source,
                         MessageKey::custom_source_points_exceed_limit);
    }
  }
  return std::nullopt;
}

std::optional<Error> validate_pattern_definition_limits(
    std::size_t primitive_count, std::size_t max_polyline_points_seen,
    double tile_width_mm, double tile_height_mm,
    PatternAssetLimits limits) noexcept {
  if (primitive_count == 0 || primitive_count > limits.max_primitives) {
    return asset_error(ErrorCode::invalid_presentation,
                       MessageKey::presentation_invalid);
  }
  if (max_polyline_points_seen > limits.max_polyline_points) {
    return asset_error(ErrorCode::invalid_presentation,
                       MessageKey::presentation_invalid);
  }
  if (!std::isfinite(tile_width_mm) || !std::isfinite(tile_height_mm) ||
      tile_width_mm <= 0.0 || tile_height_mm <= 0.0 ||
      tile_width_mm > limits.max_tile_millimetres ||
      tile_height_mm > limits.max_tile_millimetres) {
    return asset_error(ErrorCode::invalid_presentation,
                       MessageKey::presentation_invalid);
  }
  return std::nullopt;
}

} // namespace welllog
