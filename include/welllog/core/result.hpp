#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <variant>

#include <welllog/core/entity_id.hpp>

namespace welllog {

enum class ErrorCode : std::uint16_t {
  missing_owner,
  invalid_buffer,
  arithmetic_overflow,
  invalid_sampling_axis,
  length_mismatch,
  duplicate_entity_id,
  missing_sampling_axis,
  invalid_document,
  invalid_presentation,
  invalid_viewport,
  document_not_found,
  invalid_manifest,
  unresolved_buffer,
  resource_exhausted,
  internal_error,
  operation_cancelled,
  invalid_font,
  invalid_image,
  invalid_custom_source,
  diagnostic_warning,
  // An UndoCommand or RedoCommand found no entry in the requested history
  // direction (#203, ADR 0025).
  history_empty,
  // A patch's declared base revision does not match the document's current
  // revision (#202/#158, ADR 0025): the patch cannot be applied without
  // guessing by name/position, so it is rejected. Appended so existing stable
  // error-code values remain unchanged.
  patch_conflict,
  // A TimeDepthRelationship is present but not a valid monotonic map.
  // Appended so existing error-code values remain unchanged.
  invalid_time_depth,
  // TST geometry inputs are invalid (non-finite, non-unit, reversed).
  // Appended so existing error-code values remain unchanged.
  invalid_geometry,
};

enum class Severity : std::uint8_t {
  warning,
  error,
  // Appended so warning/error keep their established numeric values. Source
  // adapters use info for auditable automatic normalizations.
  info,
};

enum class MessageKey : std::uint16_t {
  buffer_owner_required,
  buffer_data_required,
  buffer_stride_too_small,
  buffer_extent_overflow,
  buffer_extent_exceeds_capacity,
  null_bitmap_owner_required,
  null_bitmap_too_short,
  null_bitmap_extent_overflow,
  null_bitmap_extent_exceeds_capacity,
  document_structure_invalid,
  entity_identity_duplicated,
  sampling_axis_direction_invalid,
  sampling_axis_missing,
  // A SamplingAxis's nominal_interval is present but not finite/positive.
  sampling_axis_interval_invalid,
  curve_length_mismatch,
  presentation_invalid,
  presentation_document_missing,
  viewport_invalid,
  manifest_invalid,
  manifest_schema_unsupported,
  manifest_resolver_required,
  manifest_buffer_mismatch,
  external_buffer_unresolved,
  resource_exhausted,
  internal_error,
  operation_cancelled,
  interval_depth_order_invalid,
  text_encoding_invalid,
  annotation_anchor_invalid,
  font_load_failed,
  font_glyph_unavailable,
  glyphs_missing_from_fonts,
  font_fallback_used,
  text_engine_unavailable,
  log_scale_values_not_drawn,
  scale_readability_hint,
  image_metadata_invalid,
  image_dimension_exceeds_limit,
  image_pixels_exceed_limit,
  image_compression_ratio_excessive,
  image_tile_unresolved,
  custom_source_empty,
  custom_source_primitives_exceed_limit,
  custom_source_points_exceed_limit,
  // A patch's declared base revision does not match the document's current
  // revision (#202/#158, ADR 0025). Stable message key paired with
  // ErrorCode::patch_conflict.
  patch_base_revision_conflict,
  // An UndoCommand or RedoCommand has no entry to apply (#203, ADR 0025).
  history_empty,
  // A TimeDepthRelationship's points are not a valid monotonic depth↔TWT map.
  // Appended so existing message-key values remain unchanged.
  time_depth_relationship_invalid,
  // TST geometry inputs are invalid (Epic D). Appended so existing
  // message-key values remain unchanged.
  invalid_geometry,
  // A track-data workflow command referenced an entity (track, scale, layer
  // or curve) that does not exist on the current document/presentation
  // (ADR 0055). Paired with ErrorCode::document_not_found.
  track_entity_missing,
  // A track-data workflow command produced an invalid binding (scale does
  // not belong to the layer's track, curve/scale unit mismatch, or no
  // presentation is set). Paired with ErrorCode::invalid_presentation.
  track_binding_invalid,
  // A reorder command's id list is not a complete permutation of the
  // collection it reorders. Paired with ErrorCode::invalid_presentation.
  track_order_incomplete,
  // A scale edit produced an invalid range (non-finite, min >= max, or
  // logarithmic with min <= 0). Paired with ErrorCode::invalid_presentation.
  track_scale_range_invalid,
};

struct ErrorArgument {
  std::array<char, 32> name{};
  std::array<char, 96> value{};
};

struct PropertyMap {
  std::array<ErrorArgument, 4> values{};
  std::uint8_t size{};
};

struct Error {
  ErrorCode code{};
  Severity severity{Severity::error};
  std::optional<EntityId> entity_id;
  MessageKey message{MessageKey::internal_error};
  PropertyMap arguments;
};

template <typename T> class Result {
public:
  Result(T value) : value_(std::move(value)) {}
  Result(Error error) : value_(std::move(error)) {}

  [[nodiscard]] bool has_value() const noexcept {
    return std::holds_alternative<T>(value_);
  }

  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] const T &value() const & { return std::get<T>(value_); }

  [[nodiscard]] T &value() & { return std::get<T>(value_); }

  [[nodiscard]] T &&value() && { return std::get<T>(std::move(value_)); }

  [[nodiscard]] const Error &error() const & { return std::get<Error>(value_); }

private:
  std::variant<T, Error> value_;
};

} // namespace welllog
