#include <welllog/session/session.hpp>

#include "scene/prepare.hpp"

#include <welllog/core/utf8.hpp>
#include <welllog/scene/curve_lod.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <queue>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace welllog {
namespace {

constexpr std::uint32_t default_frame_pixel_height = 2160;

[[nodiscard]] Result<std::uint64_t> required_bytes(const BufferView &buffer) {
  if (!buffer.has_owner()) {
    return Error{
        .code = ErrorCode::missing_owner,
        .entity_id = std::nullopt,
        .message = MessageKey::buffer_owner_required,
        .arguments = {},
    };
  }
  if (buffer.length() == 0 || buffer.data() == nullptr) {
    return Error{
        .code = ErrorCode::invalid_buffer,
        .entity_id = std::nullopt,
        .message = MessageKey::buffer_data_required,
        .arguments = {},
    };
  }
  const auto element_size = scalar_size_bytes(buffer.scalar_type());
  if (buffer.stride_bytes() < element_size) {
    return Error{
        .code = ErrorCode::invalid_buffer,
        .entity_id = std::nullopt,
        .message = MessageKey::buffer_stride_too_small,
        .arguments = {},
    };
  }
  const auto steps = buffer.length() - 1;
  if (steps > (std::numeric_limits<std::uint64_t>::max() - element_size) /
                  buffer.stride_bytes()) {
    return Error{
        .code = ErrorCode::arithmetic_overflow,
        .entity_id = std::nullopt,
        .message = MessageKey::buffer_extent_overflow,
        .arguments = {},
    };
  }
  const auto required = steps * buffer.stride_bytes() + element_size;
  if (required > buffer.byte_capacity()) {
    return Error{
        .code = ErrorCode::invalid_buffer,
        .entity_id = std::nullopt,
        .message = MessageKey::buffer_extent_exceeds_capacity,
        .arguments = {},
    };
  }
  return required;
}

// CurveBuffer overload (#197): sums the required bytes across the single
// block or each composite segment. Each segment is validated independently.
[[nodiscard]] Result<std::uint64_t> required_bytes(const CurveBuffer &buffer) {
  if (buffer.is_composite()) {
    std::uint64_t total = 0;
    for (const auto &segment : buffer.segments()) {
      const auto r = required_bytes(segment);
      if (!r) {
        return r.error();
      }
      total += r.value();
    }
    return total;
  }
  return required_bytes(buffer.as_single());
}

[[nodiscard]] std::optional<Error>
validate_null_bitmap(const NullBitmapView &nulls,
                     std::uint64_t expected_length) {
  if (nulls.empty()) {
    return std::nullopt;
  }
  if (!nulls.has_owner()) {
    return Error{
        .code = ErrorCode::missing_owner,
        .entity_id = std::nullopt,
        .message = MessageKey::null_bitmap_owner_required,
        .arguments = {},
    };
  }
  if (nulls.data() == nullptr || nulls.bit_length() < expected_length) {
    return Error{
        .code = ErrorCode::invalid_buffer,
        .entity_id = std::nullopt,
        .message = MessageKey::null_bitmap_too_short,
        .arguments = {},
    };
  }
  if (nulls.bit_length() >
      std::numeric_limits<std::uint64_t>::max() - std::uint64_t{7}) {
    return Error{
        .code = ErrorCode::arithmetic_overflow,
        .entity_id = std::nullopt,
        .message = MessageKey::null_bitmap_extent_overflow,
        .arguments = {},
    };
  }
  if ((nulls.bit_length() + 7) / 8 > nulls.byte_capacity()) {
    return Error{
        .code = ErrorCode::invalid_buffer,
        .entity_id = std::nullopt,
        .message = MessageKey::null_bitmap_extent_exceeds_capacity,
        .arguments = {},
    };
  }
  return std::nullopt;
}

// Reads element `index` from a single-block or composite curve buffer. Returns
// NaN for an out-of-range/null cell, matching the non-finite → missing-sample
// semantics used by the missing-sample scan and the selection row mappers.
[[nodiscard]] double load_as_double(const CurveBuffer &buffer,
                                    std::uint64_t index) noexcept {
  const auto v = buffer.value_as_double(index);
  return v.value_or(std::numeric_limits<double>::quiet_NaN());
}

// Type-exact monotone check on a single contiguous coordinate block. Compares
// the raw scalar values (not doubles) so an integer axis whose values differ
// only outside double precision is still checked exactly — e.g. uint64 values
// 2^53+1 and 2^53 are distinct integers but equal as doubles; the double path
// would hide that disorder (regression-tested in session_submission_test).
template <typename T>
[[nodiscard]] bool axis_is_ordered(const BufferView &coordinates,
                                   AxisDirection direction) noexcept {
  T previous{};
  std::memcpy(&previous, coordinates.data(), sizeof(T));
  if constexpr (std::is_floating_point_v<T>) {
    if (!std::isfinite(previous)) {
      return false;
    }
  }

  for (std::uint64_t index = 1; index < coordinates.length(); ++index) {
    T current{};
    std::memcpy(&current,
                coordinates.data() + index * coordinates.stride_bytes(),
                sizeof(T));
    if constexpr (std::is_floating_point_v<T>) {
      if (!std::isfinite(current)) {
        return false;
      }
    }
    const auto ordered = direction == AxisDirection::increasing
                             ? current >= previous
                             : current <= previous;
    if (!ordered) {
      return false;
    }
    previous = current;
  }
  return true;
}

// Checks the axis coordinates are monotone in the declared direction. A
// single-block axis (the common case) is checked type-exactly via the template
// above so integer precision is preserved. A composite (multi-segment) axis —
// the append case (#198) — is checked by walking the concatenation through
// `value_as_double`: coordinates are overwhelmingly floating-point, and the
// append's own validation guarantees tail continuity against the existing
// direction, so double precision across the segment boundary is acceptable.
[[nodiscard]] bool axis_is_ordered(const SamplingAxis &axis) noexcept {
  const auto &coordinates = axis.coordinates;
  if (coordinates.is_composite()) {
    const auto length = coordinates.length();
    if (length == 0) {
      return false;
    }
    auto previous = coordinates.value_as_double(0);
    if (!previous.has_value() || !std::isfinite(*previous)) {
      return false;
    }
    for (std::uint64_t index = 1; index < length; ++index) {
      const auto current = coordinates.value_as_double(index);
      if (!current.has_value() || !std::isfinite(*current)) {
        return false;
      }
      const auto ordered = axis.direction == AxisDirection::increasing
                               ? *current >= *previous
                               : *current <= *previous;
      if (!ordered) {
        return false;
      }
      previous = current;
    }
    return true;
  }
  const auto &block = coordinates.as_single();
  switch (block.scalar_type()) {
  case ScalarType::float32:
    return axis_is_ordered<float>(block, axis.direction);
  case ScalarType::float64:
    return axis_is_ordered<double>(block, axis.direction);
  case ScalarType::int16:
    return axis_is_ordered<std::int16_t>(block, axis.direction);
  case ScalarType::int32:
    return axis_is_ordered<std::int32_t>(block, axis.direction);
  case ScalarType::int64:
    return axis_is_ordered<std::int64_t>(block, axis.direction);
  case ScalarType::uint8:
    return axis_is_ordered<std::uint8_t>(block, axis.direction);
  case ScalarType::uint16:
    return axis_is_ordered<std::uint16_t>(block, axis.direction);
  case ScalarType::uint32:
    return axis_is_ordered<std::uint32_t>(block, axis.direction);
  case ScalarType::uint64:
    return axis_is_ordered<std::uint64_t>(block, axis.direction);
  }
  return false;
}

[[nodiscard]] std::optional<Error>
validate_document(const WellLogDocument &document) {
  if (document.id().is_nil() || document.revision().value == 0 ||
      document.sampling_axes().empty() || document.curves().empty()) {
    return Error{
        .code = ErrorCode::invalid_document,
        .entity_id = document.id(),
        .message = MessageKey::document_structure_invalid,
        .arguments = {},
    };
  }

  std::unordered_set<EntityId, EntityIdHash> ids;
  ids.insert(document.id());
  std::unordered_map<EntityId, const SamplingAxis *, EntityIdHash> axes;
  for (const auto &axis : document.sampling_axes()) {
    if (axis.id.is_nil() || !ids.insert(axis.id).second) {
      return Error{
          .code = ErrorCode::duplicate_entity_id,
          .entity_id = axis.id,
          .message = MessageKey::entity_identity_duplicated,
          .arguments = {},
      };
    }
    if (auto result = required_bytes(axis.coordinates); !result) {
      auto error = result.error();
      error.entity_id = axis.id;
      return error;
    }
    if (!axis_is_ordered(axis)) {
      return Error{
          .code = ErrorCode::invalid_sampling_axis,
          .entity_id = axis.id,
          .message = MessageKey::sampling_axis_direction_invalid,
          .arguments = {},
      };
    }
    if (axis.nominal_interval.has_value() &&
        (!std::isfinite(*axis.nominal_interval) ||
         *axis.nominal_interval <= 0.0)) {
      return Error{
          .code = ErrorCode::invalid_sampling_axis,
          .entity_id = axis.id,
          .message = MessageKey::sampling_axis_interval_invalid,
          .arguments = {},
      };
    }
    axes.emplace(axis.id, &axis);
  }

  for (const auto &curve : document.curves()) {
    if (curve.id.is_nil() || !ids.insert(curve.id).second) {
      return Error{
          .code = ErrorCode::duplicate_entity_id,
          .entity_id = curve.id,
          .message = MessageKey::entity_identity_duplicated,
          .arguments = {},
      };
    }
    if (auto result = required_bytes(curve.values); !result) {
      auto error = result.error();
      error.entity_id = curve.id;
      return error;
    }
    const auto axis = axes.find(curve.sampling_axis_id);
    if (axis == axes.end()) {
      return Error{
          .code = ErrorCode::missing_sampling_axis,
          .entity_id = curve.id,
          .message = MessageKey::sampling_axis_missing,
          .arguments = {},
      };
    }
    if (curve.values.length() != axis->second->coordinates.length()) {
      return Error{
          .code = ErrorCode::length_mismatch,
          .entity_id = curve.id,
          .message = MessageKey::curve_length_mismatch,
          .arguments = {},
      };
    }
    if (auto error = validate_null_bitmap(curve.nulls, curve.values.length())) {
      error->entity_id = curve.id;
      return error;
    }
  }

  const auto encoding_error = [](EntityId entity_id) {
    return Error{
        .code = ErrorCode::invalid_document,
        .entity_id = entity_id,
        .message = MessageKey::text_encoding_invalid,
        .arguments = {},
    };
  };

  for (const auto &interval : document.intervals()) {
    if (interval.id.is_nil() || !ids.insert(interval.id).second) {
      return Error{
          .code = ErrorCode::duplicate_entity_id,
          .entity_id = interval.id,
          .message = MessageKey::entity_identity_duplicated,
          .arguments = {},
      };
    }
    if (!std::isfinite(interval.top_reference_depth) ||
        !std::isfinite(interval.bottom_reference_depth) ||
        interval.top_reference_depth >= interval.bottom_reference_depth) {
      return Error{
          .code = ErrorCode::invalid_document,
          .entity_id = interval.id,
          .message = MessageKey::interval_depth_order_invalid,
          .arguments = {},
      };
    }
    if (!is_valid_utf8(interval.label)) {
      return encoding_error(interval.id);
    }
  }

  for (const auto &marker : document.markers()) {
    if (marker.id.is_nil() || !ids.insert(marker.id).second) {
      return Error{
          .code = ErrorCode::duplicate_entity_id,
          .entity_id = marker.id,
          .message = MessageKey::entity_identity_duplicated,
          .arguments = {},
      };
    }
    if (!std::isfinite(marker.reference_depth)) {
      return Error{
          .code = ErrorCode::invalid_document,
          .entity_id = marker.id,
          .message = MessageKey::document_structure_invalid,
          .arguments = {},
      };
    }
    if (!is_valid_utf8(marker.label)) {
      return encoding_error(marker.id);
    }
  }

  for (const auto &symbol : document.symbols()) {
    if (symbol.id.is_nil() || !ids.insert(symbol.id).second) {
      return Error{
          .code = ErrorCode::duplicate_entity_id,
          .entity_id = symbol.id,
          .message = MessageKey::entity_identity_duplicated,
          .arguments = {},
      };
    }
    if (!std::isfinite(symbol.reference_depth) ||
        !std::isfinite(symbol.track_fraction) || symbol.track_fraction < 0.0 ||
        symbol.track_fraction > 1.0) {
      return Error{
          .code = ErrorCode::invalid_document,
          .entity_id = symbol.id,
          .message = MessageKey::document_structure_invalid,
          .arguments = {},
      };
    }
    if (!is_valid_utf8(symbol.label)) {
      return encoding_error(symbol.id);
    }
  }

  for (const auto &annotation : document.annotations()) {
    if (annotation.id.is_nil() || !ids.insert(annotation.id).second) {
      return Error{
          .code = ErrorCode::duplicate_entity_id,
          .entity_id = annotation.id,
          .message = MessageKey::entity_identity_duplicated,
          .arguments = {},
      };
    }
    const auto valid_fraction = [](double fraction) {
      return std::isfinite(fraction) && fraction >= 0.0 && fraction <= 1.0;
    };
    bool anchor_valid = false;
    switch (annotation.anchor) {
    case AnnotationAnchor::reference_depth:
      anchor_valid = std::isfinite(annotation.reference_depth) &&
                     valid_fraction(annotation.track_fraction);
      break;
    case AnnotationAnchor::track:
      anchor_valid = !annotation.track_id.is_nil() &&
                     valid_fraction(annotation.depth_fraction) &&
                     valid_fraction(annotation.horizontal_fraction);
      break;
    case AnnotationAnchor::scene_point:
      anchor_valid = std::isfinite(annotation.scene_point.left.value) &&
                     std::isfinite(annotation.scene_point.top.value);
      break;
    }
    if (!anchor_valid || !std::isfinite(annotation.rotation_degrees) ||
        !std::isfinite(annotation.font_size.value) ||
        annotation.font_size.value <= 0.0 || annotation.text.empty()) {
      return Error{
          .code = ErrorCode::invalid_document,
          .entity_id = annotation.id,
          .message = MessageKey::annotation_anchor_invalid,
          .arguments = {},
      };
    }
    if (!is_valid_utf8(annotation.text)) {
      return encoding_error(annotation.id);
    }
  }
  return std::nullopt;
}

[[nodiscard]] bool same_source_reference(const BufferSourceReference &left,
                                         const BufferSourceReference &right) {
  return left.uri == right.uri && left.checksum == right.checksum &&
         left.byte_offset == right.byte_offset;
}

[[nodiscard]] bool same_buffer_view(const BufferView &left,
                                    const BufferView &right) {
  return left.data() == right.data() && left.length() == right.length() &&
         left.stride_bytes() == right.stride_bytes() &&
         left.scalar_type() == right.scalar_type() &&
         left.byte_capacity() == right.byte_capacity() &&
         left.access_mode() == right.access_mode() &&
         same_source_reference(left.source(), right.source());
}

[[nodiscard]] bool same_curve_buffer(const CurveBuffer &left,
                                      const CurveBuffer &right) {
  if (left.is_composite() != right.is_composite()) {
    return false;
  }
  if (!left.is_composite()) {
    return same_buffer_view(left.as_single(), right.as_single());
  }
  const auto left_segments = left.segments();
  const auto right_segments = right.segments();
  if (left_segments.size() != right_segments.size()) {
    return false;
  }
  return std::equal(left_segments.begin(), left_segments.end(),
                    right_segments.begin(),
                    [](const BufferView &left_segment,
                       const BufferView &right_segment) {
                      return same_buffer_view(left_segment, right_segment);
                    });
}

[[nodiscard]] bool same_null_bitmap(const NullBitmapView &left,
                                    const NullBitmapView &right) {
  return left.data() == right.data() && left.bit_length() == right.bit_length() &&
         left.byte_capacity() == right.byte_capacity() &&
         same_source_reference(left.source(), right.source());
}

// A ready LOD cache may be retagged only if every buffer/metadata input it
// reads is identical. ApplyPatchCommand satisfies this by construction (ADR
// 0025); this defensive check keeps a stale internal hint from corrupting a
// future SetDocumentCommand should that invariant ever change.
[[nodiscard]] bool same_lod_inputs(const WellLogDocument &previous,
                                   const WellLogDocument &replacement) {
  if (previous.sampling_axes().size() != replacement.sampling_axes().size() ||
      previous.curves().size() != replacement.curves().size() ||
      previous.image_sources().size() != replacement.image_sources().size()) {
    return false;
  }
  for (std::size_t index = 0; index < previous.sampling_axes().size(); ++index) {
    const auto &left = previous.sampling_axes()[index];
    const auto &right = replacement.sampling_axes()[index];
    if (left.id != right.id || left.domain != right.domain ||
        left.unit != right.unit || left.direction != right.direction ||
        !same_curve_buffer(left.coordinates, right.coordinates)) {
      return false;
    }
  }
  for (std::size_t index = 0; index < previous.curves().size(); ++index) {
    const auto &left = previous.curves()[index];
    const auto &right = replacement.curves()[index];
    if (left.id != right.id ||
        left.sampling_axis_id != right.sampling_axis_id ||
        !same_curve_buffer(left.values, right.values) ||
        !same_null_bitmap(left.nulls, right.nulls)) {
      return false;
    }
  }
  for (std::size_t index = 0; index < previous.image_sources().size();
       ++index) {
    const auto &left = previous.image_sources()[index];
    const auto &right = replacement.image_sources()[index];
    if (left.id != right.id || left.width_px != right.width_px ||
        left.height_px != right.height_px ||
        left.pixel_format != right.pixel_format ||
        left.reference_depth_top != right.reference_depth_top ||
        left.reference_depth_bottom != right.reference_depth_bottom ||
        left.dpi != right.dpi ||
        !same_source_reference(left.source, right.source)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::uint64_t missing_sample_count(const Curve &curve) noexcept {
  std::uint64_t count{};
  for (std::uint64_t index = 0; index < curve.values.length(); ++index) {
    if ((!curve.nulls.empty() && curve.nulls.is_null(index)) ||
        !std::isfinite(load_as_double(curve.values, index))) {
      ++count;
    }
  }
  return count;
}

[[nodiscard]] Error viewport_error(EntityId document_id) {
  return Error{
      .code = ErrorCode::invalid_viewport,
      .severity = Severity::error,
      .entity_id = document_id,
      .message = MessageKey::viewport_invalid,
      .arguments = {},
  };
}

[[nodiscard]] bool valid_viewport(DepthViewport viewport) noexcept {
  return std::isfinite(viewport.top) && std::isfinite(viewport.bottom) &&
         viewport.top < viewport.bottom &&
         std::isfinite(viewport.bottom - viewport.top);
}

[[nodiscard]] bool valid_crosshair(CrosshairState crosshair) noexcept {
  return std::isfinite(crosshair.track_fraction) &&
         crosshair.track_fraction >= 0.0 && crosshair.track_fraction <= 1.0 &&
         std::isfinite(crosshair.display_depth);
}

[[nodiscard]] bool valid_selection_range(SelectionDepthRange range) noexcept {
  return std::isfinite(range.top) && std::isfinite(range.bottom) &&
         range.top <= range.bottom;
}

// Lower/upper bound of a Reference Depth Range in axis index space. A selection
// range maps to a half-open `[first_row, last_row)` span of axis rows. For an
// increasing axis, `top` (smaller depth) is the lower index; for a decreasing
// axis it is the higher index. The mapping is index-projection: it reads the
// raw axis coordinates (no LOD, no interpolation) and clamps to the axis
// length.

// Finds the first index whose coordinate is >= `depth` (increasing axis) or <=
// `depth` (decreasing axis). Returns `length` when `depth` is beyond the last
// coordinate. Used for the selection's `first_row`.
[[nodiscard]] std::uint64_t first_row_at_depth(const CurveBuffer &coordinates,
                                               AxisDirection direction,
                                               double depth) noexcept {
  const auto length = coordinates.length();
  if (length == 0) {
    return 0;
  }
  if (direction == AxisDirection::increasing) {
    for (std::uint64_t i = 0; i < length; ++i) {
      if (load_as_double(coordinates, i) >= depth) {
        return i;
      }
    }
    return length;
  }
  for (std::uint64_t i = 0; i < length; ++i) {
    if (load_as_double(coordinates, i) <= depth) {
      return i;
    }
  }
  return length;
}

// Finds the first index whose coordinate is > `depth` (increasing axis) or <
// `depth` (decreasing axis). Returns `length` when `depth` is at/ beyond the
// last coordinate. Used for the selection's exclusive `last_row`.
[[nodiscard]] std::uint64_t last_row_after_depth(const CurveBuffer &coordinates,
                                                 AxisDirection direction,
                                                 double depth) noexcept {
  const auto length = coordinates.length();
  if (length == 0) {
    return 0;
  }
  if (direction == AxisDirection::increasing) {
    for (std::uint64_t i = 0; i < length; ++i) {
      if (load_as_double(coordinates, i) > depth) {
        return i;
      }
    }
    return length;
  }
  for (std::uint64_t i = 0; i < length; ++i) {
    if (load_as_double(coordinates, i) < depth) {
      return i;
    }
  }
  return length;
}

// Resolves a SelectionDepthRange on an axis to a half-open `[first, last)` row
// span. The span is clamped to `[0, length]`; an empty/wholely-out-of-range
// selection yields `first == last` (zero rows). `increasing` axis: top is the
// lower index, bottom the upper; `decreasing`: inverted.
struct RowSpan {
  std::uint64_t first{};
  std::uint64_t last{};
};
[[nodiscard]] RowSpan rows_for_range(const CurveBuffer &coordinates,
                                     AxisDirection direction,
                                     SelectionDepthRange range) noexcept {
  const auto length = coordinates.length();
  if (length == 0) {
    return {0, 0};
  }
  if (direction == AxisDirection::increasing) {
    const auto first = first_row_at_depth(coordinates, direction, range.top);
    const auto last =
        last_row_after_depth(coordinates, direction, range.bottom);
    return {first, std::max(first, last)};
  }
  const auto first = first_row_at_depth(coordinates, direction, range.bottom);
  const auto last = last_row_after_depth(coordinates, direction, range.top);
  return {first, std::max(first, last)};
}

// Resolves a half-open row span to the Reference Depth Range it covers by
// reading the raw axis coordinate at the boundary rows (no LOD). Direction is
// immaterial here — the range is min/max of the two boundary coordinates, so a
// decreasing axis produces the same `[top, bottom]` as an increasing one. A
// zero-length span (`first == last`) yields the coordinate at `first` for both
// ends.
[[nodiscard]] SelectionDepthRange
range_for_rows(const CurveBuffer &coordinates, std::uint64_t first_row,
               std::uint64_t last_row) noexcept {
  const auto length = coordinates.length();
  const auto clamped_first =
      first_row >= length ? (length == 0 ? 0 : length - 1) : first_row;
  const auto clamped_last_idx =
      last_row == 0 ? 0 : (last_row - 1 >= length ? length - 1 : last_row - 1);
  if (length == 0) {
    return {};
  }
  const auto a = load_as_double(coordinates, clamped_first);
  const auto b = load_as_double(coordinates, clamped_last_idx);
  return {.top = std::min(a, b), .bottom = std::max(a, b)};
}

// Locates a Sampling Axis on a document by id; returns nullptr when absent.
[[nodiscard]] const SamplingAxis *find_axis(const WellLogDocument &document,
                                            EntityId axis_id) noexcept {
  for (const auto &axis : document.sampling_axes()) {
    if (axis.id == axis_id) {
      return &axis;
    }
  }
  return nullptr;
}

// Selection-failure error builders. Each maps to the SAME code/message the
// document/viewport paths already use for that failure mode, so a caller can
// distinguish an unknown document from an unknown axis from a bad range — the
// single invalid_viewport used before was a Mysterious Name that hid the cause
// (architecture.md §2 Result/Error model; quality-security-performance.md §7
// "稳定码").
[[nodiscard]] Error selection_document_missing(EntityId document_id) {
  return Error{
      .code = ErrorCode::document_not_found,
      .severity = Severity::error,
      .entity_id = document_id,
      .message = MessageKey::presentation_document_missing,
      .arguments = {},
  };
}

[[nodiscard]] Error selection_axis_missing(EntityId axis_id) {
  return Error{
      .code = ErrorCode::missing_sampling_axis,
      .severity = Severity::error,
      .entity_id = axis_id,
      .message = MessageKey::sampling_axis_missing,
      .arguments = {},
  };
}

// A bad range/span value or a version overflow — the existing viewport pair is
// the closest "invalid value" code; the session has no selection-specific code.
[[nodiscard]] Error selection_invalid(EntityId document_id) {
  return Error{
      .code = ErrorCode::invalid_viewport,
      .severity = Severity::error,
      .entity_id = document_id,
      .message = MessageKey::viewport_invalid,
      .arguments = {},
  };
}

struct LodBuildOutput {
  bool cancelled{};
  std::optional<Error> error;
  std::uint64_t derived_bytes{};
  std::unordered_map<EntityId, CurveLodPyramid, EntityIdHash> pyramids;
  // Image pyramids built from ImageSource entities (#184). metadata-only
  // (no pixel decode — ADR 0045); a missing/empty map means image layers
  // produce no tiles (non-fatal degradation).
  detail::ScenePreparer::ImagePyramidMap image_pyramids;
  std::uint64_t image_derived_bytes{};
  // ImageSource ids whose pyramid build failed (non-cancelled) and were
  // skipped. poll_async publishes a Diagnostic per id (qsp §7: degradation
  // must be observable), then degrades — the scene emits no layer for them.
  std::vector<EntityId> skipped_images;
};

// Long-lived worker pool for LOD/frame build tasks (issue #492): every
// viewport pan/zoom used to create and join a fresh std::jthread per mouse
// event (~50-100us of thread churn at 60 Hz drag). Tasks now run on
// persistent pool threads; cancellation moved from the jthread's implicit
// stop token into the task state's stop_source, so request_stop()/reap/drain
// keep their semantics (generation discard unchanged).
class TaskExecutor {
public:
  explicit TaskExecutor(unsigned int thread_count) {
    for (unsigned int index = 0; index < thread_count; ++index) {
      workers_.emplace_back([this] { worker_loop(); });
    }
  }

  ~TaskExecutor() {
    std::vector<QueuedJob> dropped;
    {
      const auto lock = std::lock_guard{mutex_};
      shutdown_ = true;
      while (!pending_.empty()) {
        dropped.push_back(std::move(pending_.front()));
        pending_.pop();
      }
    }
    cv_.notify_all();
    // Jobs still queued at shutdown will never run on a pool thread; run
    // them inline with an already-stopped token so their task states observe
    // cancellation and waiters (drain/reap) are not left blocking on a job
    // that will not run. The workers_' jthread dtors then join any in-flight
    // pool threads (same join-on-destruction contract as the per-task jthread
    // this class replaces).
    for (auto &job : dropped) {
      run_cancelled(*job.source, std::move(job.job));
    }
  }

  // Runs job(source.get_token()) on a pool thread. The stop_source lives in
  // the task state (shared_ptr-kept alive by the task record), so a
  // request_stop() racing with queueing is still observed by the job.
  void submit(std::stop_source &source,
              std::function<void(std::stop_token)> job) {
    {
      const auto lock = std::lock_guard{mutex_};
      if (!shutdown_) {
        pending_.push(QueuedJob{&source, std::move(job)});
        cv_.notify_one();
        return;
      }
    }
    run_cancelled(source, std::move(job));
  }

private:
  struct QueuedJob {
    std::stop_source *source;
    std::function<void(std::stop_token)> job;
  };

  void worker_loop() {
    for (;;) {
      auto job = QueuedJob{};
      {
        auto lock = std::unique_lock{mutex_};
        cv_.wait(lock, [this] { return shutdown_ || !pending_.empty(); });
        if (shutdown_ && pending_.empty()) {
          return;
        }
        job = std::move(pending_.front());
        pending_.pop();
      }
      job.job(job.source->get_token());
    }
  }

  static void run_cancelled(std::stop_source &source,
                            std::function<void(std::stop_token)> job) {
    source.request_stop();
    job(source.get_token());
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::queue<QueuedJob> pending_;
  bool shutdown_{};
  std::vector<std::jthread> workers_;
};

struct LodTaskState {
  std::mutex mutex;
  std::stop_source stop_source;
  bool finished{};
  LodBuildOutput output;
};

struct LodTask {
  EntityId document_id;
  DocumentRevision revision;
  std::uint64_t generation{};
  std::shared_ptr<LodTaskState> state;
};

struct FrameBuildOutput {
  bool cancelled{};
  std::optional<Error> error;
  std::shared_ptr<const PreparedScene> scene;
};

struct FrameTaskState {
  std::mutex mutex;
  std::stop_source stop_source;
  bool finished{};
  FrameBuildOutput output;
};

struct FrameTask {
  EntityId document_id;
  DocumentRevision revision;
  std::uint64_t generation{};
  std::shared_ptr<FrameTaskState> state;
};

[[nodiscard]] std::unique_ptr<FrameTask> make_frame_task(
    TaskExecutor &executor, EntityId document_id, DocumentRevision revision,
    std::uint64_t generation, std::shared_ptr<const WellLogDocument> document,
    ScenePresentation presentation,
    std::shared_ptr<const detail::ScenePreparer::CurveLodMap> pyramids,
    CurveLodQuery query,
    std::shared_ptr<const detail::ScenePreparer::ImagePyramidMap>
        image_pyramids,
    ImagePyramidQuery image_query, std::shared_ptr<TextEngine> text_engine,
    std::mutex *text_engine_mutex) {
  auto state = std::make_shared<FrameTaskState>();
  auto task = std::make_unique<FrameTask>();
  task->document_id = document_id;
  task->revision = revision;
  task->generation = generation;
  task->state = state;
  // Bind the reference BEFORE the lambda capture moves the shared_ptr:
  // argument evaluation order is unspecified, so evaluating
  // `state->stop_source` after the move would dereference a null state.
  auto &stop_source = state->stop_source;
  executor.submit(stop_source,
      [document = std::move(document), presentation = std::move(presentation),
       pyramids = std::move(pyramids), query,
       image_pyramids = std::move(image_pyramids), image_query,
       text_engine = std::move(text_engine), text_engine_mutex,
       state = std::move(state)](std::stop_token stop_token) {
        auto output = FrameBuildOutput{};
        if (stop_token.stop_requested()) {
          output.cancelled = true;
        } else {
          try {
            Result<PreparedScene> prepared = Error{
                .code = ErrorCode::internal_error,
                .severity = Severity::error,
                .entity_id = std::nullopt,
                .message = MessageKey::internal_error,
                .arguments = {},
            };
            {
              // Text engines are single-threaded; serialize shaping across
              // concurrent frame preparations.
              const auto text_guard =
                  text_engine == nullptr
                      ? std::unique_lock<std::mutex>{}
                      : std::unique_lock<std::mutex>{*text_engine_mutex};
              prepared = detail::ScenePreparer::prepare(
                  *document, presentation, *pyramids, query,
                  image_pyramids ? *image_pyramids
                                 : detail::ScenePreparer::ImagePyramidMap{},
                  image_query, stop_token, text_engine.get());
            }
            if (stop_token.stop_requested()) {
              output.cancelled = true;
            } else if (prepared.has_value()) {
              output.scene = std::make_shared<const PreparedScene>(
                  std::move(prepared).value());
            } else {
              output.cancelled =
                  prepared.error().code == ErrorCode::operation_cancelled;
              if (!output.cancelled) {
                output.error = prepared.error();
              }
            }
          } catch (const std::bad_alloc &) {
            output.error = Error{
                .code = ErrorCode::resource_exhausted,
                .severity = Severity::error,
                .entity_id = document->id(),
                .message = MessageKey::resource_exhausted,
                .arguments = {},
            };
          } catch (...) {
            output.error = Error{
                .code = ErrorCode::internal_error,
                .severity = Severity::error,
                .entity_id = document->id(),
                .message = MessageKey::internal_error,
                .arguments = {},
            };
          }
        }
        const auto guard = std::lock_guard{state->mutex};
        state->output = std::move(output);
        state->finished = true;
      });
  return task;
}

struct CurvePreparation {
  DocumentRevision revision;
  std::uint64_t generation{};
  PreparationState state{PreparationState::unavailable};
  std::uint64_t derived_bytes{};
  std::uint64_t maximum_derived_bytes{};
  std::shared_ptr<const detail::ScenePreparer::CurveLodMap> pyramids;
  // Image pyramids built alongside the curve LOD (#184); empty when the
  // document has no ImageSource entities.
  std::shared_ptr<const detail::ScenePreparer::ImagePyramidMap> image_pyramids;
};

// The prepared-scene issue enums (ValueIssueCode / TextIssueCode) map 1:1 onto
// the session-domain DiagnosticCode and the error-domain MessageKey. These
// resolvers keep that mapping in one place per family rather than restating it
// inside each publish loop's switch (ADR 0038 domain separation is preserved:
// the enums themselves stay distinct across scene/session/error layers).

struct ValueIssueMapping {
  DiagnosticCode code{DiagnosticCode::nonpositive_log_values};
  MessageKey message{MessageKey::log_scale_values_not_drawn};
};

[[nodiscard]] ValueIssueMapping resolve(ValueIssueCode code) noexcept {
  switch (code) {
  case ValueIssueCode::nonpositive_log_values:
    return {.code = DiagnosticCode::nonpositive_log_values,
            .message = MessageKey::log_scale_values_not_drawn};
  case ValueIssueCode::scale_readability_hint:
    return {.code = DiagnosticCode::scale_readability_hint,
            .message = MessageKey::scale_readability_hint};
  }
  return {};
}

struct TextIssueMapping {
  DiagnosticCode code{DiagnosticCode::missing_glyphs};
  MessageKey message{MessageKey::glyphs_missing_from_fonts};
};

[[nodiscard]] TextIssueMapping resolve(TextIssueCode code) noexcept {
  switch (code) {
  case TextIssueCode::missing_glyphs:
    return {.code = DiagnosticCode::missing_glyphs,
            .message = MessageKey::glyphs_missing_from_fonts};
  case TextIssueCode::fallback_font_used:
    return {.code = DiagnosticCode::fallback_font_used,
            .message = MessageKey::font_fallback_used};
  case TextIssueCode::text_engine_unavailable:
    return {.code = DiagnosticCode::text_engine_unavailable,
            .message = MessageKey::text_engine_unavailable};
  }
  return {};
}

} // namespace

struct WellLogSession::Impl {
  // The state a history transition restores. Raw document buffers are immutable
  // shared owners, so retaining a document snapshot does not duplicate sample
  // data. Presentation layout and Selection Set are semantic state; viewports,
  // crosshairs, and prepared pixels remain transient widget/session state.
  struct SemanticState {
    std::shared_ptr<const WellLogDocument> document;
    std::optional<ScenePresentation> presentation;
    std::optional<SelectionState> selection;
  };

  // A patch carries its explicit inverse edit list, as required by #203;
  // snapshots make append undo exact because immutable curve buffers cannot be
  // expressed as patch edits.
  struct HistoryEntry {
    SemanticState before;
    SemanticState after;
    std::vector<EntityEdit> inverse_edits;
  };

  struct DocumentHistory {
    std::vector<HistoryEntry> undo;
    std::vector<HistoryEntry> redo;
  };

  PerformanceBudgets budgets;
  std::uint64_t state_version{};
  std::uint64_t next_diagnostic_id{1};
  std::uint64_t next_lod_generation{1};
  std::uint64_t next_frame_generation{1};
  std::uint64_t completed_lod_tasks{};
  std::uint64_t cancelled_lod_tasks{};
  std::uint64_t discarded_lod_tasks{};
  std::unordered_map<EntityId, std::shared_ptr<const WellLogDocument>,
                     EntityIdHash>
      documents;
  std::unordered_map<EntityId, std::shared_ptr<const PreparedScene>,
                     EntityIdHash>
      prepared_scenes;
  std::unordered_map<EntityId, ScenePresentation, EntityIdHash> presentations;
  std::unordered_map<EntityId, DepthViewport, EntityIdHash> viewports;
  std::unordered_map<EntityId, std::uint32_t, EntityIdHash>
      viewport_pixel_heights;
  std::unordered_map<EntityId, DepthViewport, EntityIdHash> viewport_defaults;
  std::unordered_map<EntityId, CrosshairState, EntityIdHash> crosshairs;
  std::unordered_map<EntityId, SelectionState, EntityIdHash> selections;
  std::unordered_map<EntityId, DocumentHistory, EntityIdHash> histories;
  // execute_history stages the semantic state that SetDocumentCommand must
  // restore before its notifications become observable. This keeps a history
  // transition atomic for event observers.
  std::unordered_map<EntityId, SemanticState, EntityIdHash>
      pending_history_restores;
  // Per-document append viewport mode (#200): whether an AppendBatchCommand
  // preserves the current viewport (fixed, the absence/default) or advances its
  // bottom to the new tail depth (follow_latest).
  std::unordered_map<EntityId, AppendViewportMode, EntityIdHash>
      append_viewport_modes;
  // Multi-well surface layout (#160, ADR 0012). Empty = independent single-well
  // mode (prepared_scene(doc) only). Non-empty wells share Display Depth via
  // shared_depth_viewport when set.
  std::vector<WellPlacement> well_layout;
  Millimetres well_layout_gap{4.0};
  std::optional<DepthViewport> shared_depth_viewport;
  std::optional<std::uint32_t> shared_pixel_height;
  std::optional<std::pair<double, double>> surface_horizontal_view;
  // Per-document Depth Transform (#161). Applied via presentation rebuild.
  std::unordered_map<EntityId, DepthTransform, EntityIdHash> depth_transforms;
  std::vector<CrossWellOverlay> cross_well_overlays;
  // High-frequency append coalescing state (#201, ADR 0031). Per-document
  // staged tail-blocks awaiting a visible revision, plus the time the last
  // visible revision was produced (for the refresh-rate cap). Empty/disabled
  // when append_refresh_rate_hz == 0.
  struct AppendCoalescer {
    std::vector<CurveTailBlock> staged_blocks;
    std::chrono::steady_clock::time_point last_flush{};
    bool has_last_flush{false};
  };
  std::unordered_map<EntityId, AppendCoalescer, EntityIdHash> append_coalescers;
  std::unordered_map<EntityId, CurvePreparation, EntityIdHash> preparations;
  enum class LodReuseKind : std::uint8_t {
    append_tail,
    unchanged_document,
  };
  // Commands that delegate to SetDocumentCommand may retain derived data when
  // their source buffers are known compatible. Appends extend curve pyramids;
  // patches retain all raw curves/images and reuse a ready preparation as-is.
  struct PendingLodReuse {
    LodReuseKind kind{LodReuseKind::append_tail};
    std::shared_ptr<const detail::ScenePreparer::CurveLodMap> pyramids;
    std::shared_ptr<const detail::ScenePreparer::ImagePyramidMap> image_pyramids;
    std::uint64_t derived_bytes{};
    std::uint64_t maximum_derived_bytes{};
    std::shared_ptr<const WellLogDocument> source_document;
    DocumentRevision source_revision{};
  };
  std::unordered_map<EntityId, PendingLodReuse, EntityIdHash>
      pending_lod_reuse;
  std::unordered_map<EntityId, std::uint64_t, EntityIdHash> frame_generations;
  TaskExecutor task_executor{2};
  std::vector<std::unique_ptr<LodTask>> lod_tasks;
  std::vector<std::unique_ptr<FrameTask>> frame_tasks;
  std::vector<ViewEvent> events;
  std::vector<Diagnostic> diagnostics;
  std::unordered_map<std::uint64_t, Error> diagnostic_errors;
  std::shared_ptr<TextEngine> text_engine;
  std::mutex text_engine_mutex;
  ViewEventObserverId next_observer_id{1};
  std::unordered_map<ViewEventObserverId, ViewEventObserver> observers;

  [[nodiscard]] SemanticState semantic_state(EntityId document_id) const {
    SemanticState state;
    if (const auto document = documents.find(document_id);
        document != documents.end()) {
      state.document = document->second;
    }
    if (const auto presentation = presentations.find(document_id);
        presentation != presentations.end()) {
      state.presentation = presentation->second;
    }
    if (const auto selection = selections.find(document_id);
        selection != selections.end()) {
      state.selection = selection->second;
    }
    return state;
  }

  void publish_history_changed(EntityId document_id,
                               DocumentRevision revision) {
    const auto event = ViewEvent{
        .kind = ViewEventKind::history_changed,
        .state_version = state_version,
        .document_id = document_id,
        .document_revision = revision,
    };
    events.push_back(event);
    notify_observers(event);
  }

  void record_new_history(EntityId document_id, HistoryEntry entry,
                          DocumentRevision revision) {
    auto &history = histories.at(document_id);
    history.redo.clear();
    history.undo.push_back(std::move(entry));
    publish_history_changed(document_id, revision);
  }

  void notify_observers(const ViewEvent &event) const noexcept {
    try {
      std::vector<ViewEventObserver> observer_snapshot;
      observer_snapshot.reserve(observers.size());
      for (const auto &[observer_id, observer] : observers) {
        static_cast<void>(observer_id);
        observer_snapshot.push_back(observer);
      }
      for (const auto &observer : observer_snapshot) {
        try {
          observer(event);
        } catch (...) {
        }
      }
    } catch (...) {
    }
  }

  void publish_async_failure(EntityId document_id, DocumentRevision revision,
                             const Error &error,
                             std::vector<ViewEvent> &notifications) {
    if (state_version == std::numeric_limits<std::uint64_t>::max() ||
        next_diagnostic_id == std::numeric_limits<std::uint64_t>::max()) {
      return;
    }
    const auto diagnostic_id = next_diagnostic_id;
    diagnostics.reserve(diagnostics.size() + 1);
    diagnostic_errors.reserve(diagnostic_errors.size() + 1);
    events.reserve(events.size() + 1);
    notifications.reserve(notifications.size() + 1);
    diagnostic_errors.emplace(diagnostic_id, error);
    ++state_version;
    diagnostics.push_back(Diagnostic{
        .id = diagnostic_id,
        .code = DiagnosticCode::asynchronous_preparation_failed,
        .severity = error.severity,
        .document_id = document_id,
        .entity_id = error.entity_id.value_or(document_id),
        .document_revision = revision,
        .occurrence_count = 1,
    });
    ++next_diagnostic_id;
    const auto event = ViewEvent{
        .kind = ViewEventKind::diagnostic_published,
        .state_version = state_version,
        .document_id = document_id,
        .document_revision = revision,
    };
    events.push_back(event);
    notifications.push_back(event);
  }

  // Publishes one diagnostic derived from a prepared-scene issue: reserves
  // space across the four diagnostic sinks, emplaces the error + Diagnostic,
  // bumps the version/next-id, and fans out the ViewEvent to both event
  // sinks. Shared by publish_value_issues and publish_text_issues so the
  // reserve/emplace/push/event sequence lives in one place. Returns the id
  // assigned to the published diagnostic.
  std::uint64_t publish_one_diagnostic(
      EntityId document_id, DocumentRevision revision, EntityId entity_id,
      std::uint32_t occurrence_count, DiagnosticCode code, MessageKey message,
      ErrorCode error_code, std::vector<ViewEvent> &notifications) noexcept {
    const auto diagnostic_id = next_diagnostic_id;
    diagnostics.reserve(diagnostics.size() + 1);
    diagnostic_errors.reserve(diagnostic_errors.size() + 1);
    events.reserve(events.size() + 1);
    notifications.reserve(notifications.size() + 1);
    diagnostic_errors.emplace(diagnostic_id, Error{
                                                 .code = error_code,
                                                 .severity = Severity::warning,
                                                 .entity_id = entity_id,
                                                 .message = message,
                                                 .arguments = {},
                                             });
    ++state_version;
    diagnostics.push_back(Diagnostic{
        .id = diagnostic_id,
        .code = code,
        .severity = Severity::warning,
        .document_id = document_id,
        .entity_id = entity_id,
        .document_revision = revision,
        .occurrence_count = occurrence_count,
    });
    ++next_diagnostic_id;
    const auto event = ViewEvent{
        .kind = ViewEventKind::diagnostic_published,
        .state_version = state_version,
        .document_id = document_id,
        .document_revision = revision,
    };
    events.push_back(event);
    notifications.push_back(event);
    return diagnostic_id;
  }

  // Publishes prepared-scene value issues (non-positive log-scale
  // samples, scale readability hints) into the diagnostic stream.
  void publish_value_issues(EntityId document_id, DocumentRevision revision,
                            const PreparedScene &scene,
                            std::vector<ViewEvent> &notifications) noexcept {
    try {
      for (const auto &issue : scene.value_issues()) {
        if (state_version == std::numeric_limits<std::uint64_t>::max() ||
            next_diagnostic_id == std::numeric_limits<std::uint64_t>::max()) {
          break;
        }
        const auto mapping = resolve(issue.code);
        publish_one_diagnostic(document_id, revision, issue.entity_id,
                               issue.occurrence_count, mapping.code,
                               mapping.message, ErrorCode::diagnostic_warning,
                               notifications);
      }
    } catch (...) {
    }
  }

  // Publishes prepared-scene text issues (missing glyphs, fallback fonts,
  // unavailable engines) into the diagnostic stream. Returns the first
  // published diagnostic identity, if any.
  [[nodiscard]] std::optional<std::uint64_t>
  publish_text_issues(EntityId document_id, DocumentRevision revision,
                      const PreparedScene &scene,
                      std::vector<ViewEvent> &notifications) noexcept {
    std::optional<std::uint64_t> first_diagnostic;
    try {
      for (const auto &issue : scene.text_issues()) {
        if (state_version == std::numeric_limits<std::uint64_t>::max() ||
            next_diagnostic_id == std::numeric_limits<std::uint64_t>::max()) {
          break;
        }
        const auto mapping = resolve(issue.code);
        const auto published = publish_one_diagnostic(
            document_id, revision, issue.entity_id, issue.occurrence_count,
            mapping.code, mapping.message, ErrorCode::invalid_font,
            notifications);
        if (!first_diagnostic.has_value()) {
          first_diagnostic = published;
        }
      }
    } catch (...) {
    }
    return first_diagnostic;
  }

  // Cooperatively cancel and reap all LOD/frame worker jthreads with a bounded
  // wait. Called from ~WellLogSession so that normal teardown (workers already
  // finished, or finishing within milliseconds of a stop request) reaps them
  // promptly via try_lock, and only starved workers fall through to the jthread
  // dtor's implicit join. std::jthread has no detach() — once request_stop() +
  // a yield window has been offered, remaining workers are left for the dtor,
  // which will join (the stop_token cooperation is fine-grained: LOD workers
  // check every 4096 samples, scene prepare checks per track/layer). The real
  // loader-lock deadlock (#241) is avoided at the test layer by using _Exit in
  // fail() so DLL_PROCESS_DETACH never runs while workers are mid-flight; this
  // drain ensures the non-exit teardown path also reaps promptly.
  void drain_workers() {
    drain_task_vector(lod_tasks);
    drain_task_vector(frame_tasks);
  }

  template <typename TaskPtr>
  void drain_task_vector(std::vector<TaskPtr> &tasks) {
    if (tasks.empty()) {
      return;
    }
    // Request cooperative cancellation on every outstanding worker first, so
    // they all observe the stop_token concurrently rather than one-at-a-time.
    for (auto &task : tasks) {
      task->state->stop_source.request_stop();
    }
    // Bounded wait: give workers a short window to observe the token and
    // finish. Normal teardown (workers already done) reaps immediately.
    constexpr auto drain_deadline = std::chrono::seconds{2};
    const auto deadline = std::chrono::steady_clock::now() + drain_deadline;
    while (!tasks.empty() &&
           std::chrono::steady_clock::now() < deadline) {
      const auto task_iter =
          std::find_if(tasks.begin(), tasks.end(), [](const auto &task) {
            // try_lock mirrors poll_async: never block the drainer on a
            // worker's publish lock (#241). finished is set under that lock.
            auto lock =
                std::unique_lock{task->state->mutex, std::try_to_lock};
            return lock.owns_lock() && task->state->finished;
          });
      if (task_iter == tasks.end()) {
        // No worker has finished yet; yield the timeslice so starved workers
        // get CPU to observe the stop_token and proceed.
        std::this_thread::yield();
        continue;
      }
      // Worker is done — erasing the task drops the task record; the pool
      // thread it ran on stays alive for the next job (TaskExecutor, #492).
      tasks.erase(task_iter);
    }
    // Workers still running after the deadline remain in `tasks`; their jthread
    // dtors (triggered by Impl destruction) will request_stop + join. The stop
    // token cooperation guarantees eventual completion — the bounded drain just
    // fast-paths the common case so well-behaved teardown doesn't wait.
  }
};

WellLogSession::WellLogSession() : WellLogSession(PerformanceBudgets{}) {}
WellLogSession::WellLogSession(PerformanceBudgets budgets)
    : impl_(std::make_unique<Impl>()) {
  impl_->budgets = budgets;
}
WellLogSession::~WellLogSession() {
  // Cooperatively cancel + bounded-wait all LOD/frame workers before the Impl
  // is destroyed. Without this, the vector-of-unique_ptr<LodTask> destruction
  // triggers each jthread dtor's unconditional request_stop + join, which can
  // block indefinitely under Windows CPU starvation or inside the loader lock
  // during DLL_PROCESS_DETACH (#241).
  if (impl_) {
    try {
      impl_->drain_workers();
    } catch (...) {
      // drain_workers is noexcept in spirit (try_lock + yield + erase); swallow
      // any exception so the destructor never throws.
    }
  }
}
WellLogSession::WellLogSession(WellLogSession &&) noexcept = default;
WellLogSession &WellLogSession::operator=(WellLogSession &&other) noexcept {
  // Drain the outgoing session's workers before the default unique_ptr move
  // destroys Impl (which would otherwise join unconditionally, #241).
  if (this != &other && impl_) {
    try {
      impl_->drain_workers();
    } catch (...) {
    }
  }
  impl_ = std::move(other.impl_);
  return *this;
}

void WellLogSession::set_text_engine(
    std::shared_ptr<TextEngine> text_engine) noexcept {
  try {
    const auto guard = std::lock_guard{impl_->text_engine_mutex};
    impl_->text_engine = std::move(text_engine);
  } catch (...) {
  }
}

std::shared_ptr<TextEngine> WellLogSession::text_engine() const noexcept {
  const auto guard = std::lock_guard{impl_->text_engine_mutex};
  return impl_->text_engine;
}

Result<CommandReceipt> WellLogSession::execute(SetDocumentCommand command) {
  try {
    if (const auto error = validate_document(command.document)) {
      return *error;
    }
    if (impl_->state_version == std::numeric_limits<std::uint64_t>::max()) {
      return Error{
          .code = ErrorCode::internal_error,
          .severity = Severity::error,
          .entity_id = command.document.id(),
          .message = MessageKey::internal_error,
          .arguments = {},
      };
    }

    auto document =
        std::make_shared<const WellLogDocument>(std::move(command.document));
    const auto document_id = document->id();
    const auto revision = document->revision();
    const auto asynchronous =
        std::any_of(document->curves().begin(), document->curves().end(),
                    [&](const Curve &curve) {
                      return curve.values.length() >=
                             impl_->budgets.asynchronous_sample_threshold;
                    });
    if (asynchronous && impl_->next_lod_generation ==
                            std::numeric_limits<std::uint64_t>::max()) {
      return Error{
          .code = ErrorCode::internal_error,
          .severity = Severity::error,
          .entity_id = document_id,
          .message = MessageKey::internal_error,
          .arguments = {},
      };
    }
    // Consume a trusted derived-cache hint on either path so it cannot leak
    // into a later replacement. A patch never changes raw buffers (ADR 0025),
    // whereas an append only has an extend-tail starting point.
    std::optional<Impl::PendingLodReuse> reuse;
    if (const auto pending = impl_->pending_lod_reuse.find(document_id);
        pending != impl_->pending_lod_reuse.end()) {
      reuse = pending->second;
      impl_->pending_lod_reuse.erase(pending);
    }
    const auto current_document = impl_->documents.find(document_id);
    const auto reuse_matches_current_document =
        reuse.has_value() && reuse->source_document != nullptr &&
        current_document != impl_->documents.end() &&
        reuse->source_document == current_document->second &&
        reuse->source_revision == current_document->second->revision();
    const auto reuse_preparation =
        asynchronous && reuse_matches_current_document &&
        reuse->kind == Impl::LodReuseKind::unchanged_document &&
        reuse->pyramids != nullptr &&
        same_lod_inputs(*current_document->second, *document);
    const auto generation = asynchronous && !reuse_preparation
                                ? impl_->next_lod_generation
                                : std::uint64_t{};
    const auto reuse_map = reuse_matches_current_document &&
                                   reuse->kind == Impl::LodReuseKind::append_tail
                               ? reuse->pyramids
                               : nullptr;
    auto task = std::unique_ptr<LodTask>{};
    auto maximum_derived_bytes = reuse_preparation
                                     ? reuse->maximum_derived_bytes
                                     : impl_->budgets.maximum_cpu_derived_bytes;
    if (asynchronous && !reuse_preparation) {
      if (maximum_derived_bytes == 0) {
        const auto add_budget = [&](std::uint64_t increment) {
          maximum_derived_bytes =
              increment > std::numeric_limits<std::uint64_t>::max() -
                              maximum_derived_bytes
                  ? std::numeric_limits<std::uint64_t>::max()
                  : maximum_derived_bytes + increment;
        };
        for (const auto &axis : document->sampling_axes()) {
          add_budget(axis.coordinates.length() *
                     scalar_size_bytes(axis.coordinates.scalar_type()) / 4);
        }
        for (const auto &curve : document->curves()) {
          add_budget(curve.values.length() *
                     scalar_size_bytes(curve.values.scalar_type()) / 4);
        }
      }
      auto state = std::make_shared<LodTaskState>();
      task = std::make_unique<LodTask>();
      task->document_id = document_id;
      task->revision = revision;
      task->generation = generation;
      task->state = state;
      const auto curve_count =
          static_cast<std::uint64_t>(document->curves().size());
      const auto per_curve_budget =
          std::max(std::uint64_t{1}, maximum_derived_bytes / curve_count);
      const auto image_pyramid_options = impl_->budgets.image_pyramid_options;
      // Append-incremental LOD reuse (#199): reuse_map only holds the prior
      // curve pyramids for an AppendBatchCommand. This worker extends a tail or
      // falls back to a full build; unchanged-document reuse bypasses it.
      const auto build_options = CurveLodBuildOptions{
          .algorithm = CurveLodAlgorithm::hierarchical,
          .base_bucket_samples = 16,
          .maximum_derived_bytes = per_curve_budget,
      };
      impl_->task_executor.submit(
          state->stop_source,
          [document, state, image_pyramid_options, reuse_map,
           build_options](std::stop_token stop_token) {
        auto output = LodBuildOutput{};
        try {
          for (const auto &curve : document->curves()) {
            if (stop_token.stop_requested()) {
              output.cancelled = true;
              break;
            }
            const auto axis =
                std::find_if(document->sampling_axes().begin(),
                             document->sampling_axes().end(),
                             [&](const SamplingAxis &candidate) {
                               return candidate.id == curve.sampling_axis_id;
                             });
            if (axis == document->sampling_axes().end()) {
              output.error = Error{
                  .code = ErrorCode::missing_sampling_axis,
                  .severity = Severity::error,
                  .entity_id = curve.id,
                  .message = MessageKey::sampling_axis_missing,
                  .arguments = {},
              };
              break;
            }
            // Incremental path: if a previous pyramid for this curve is staged
            // for reuse (#199), extend its tail onto the appended curve. A
            // structural rejection (edited prefix, option change, shrink) falls
            // back to a full build so the result is always correct.
            auto pyramid = Result<CurveLodPyramid>{Error{
                .code = ErrorCode::invalid_document,
                .severity = Severity::error,
                .entity_id = curve.id,
                .message = MessageKey::document_structure_invalid,
                .arguments = {},
            }};
            if (reuse_map != nullptr) {
              const auto previous = reuse_map->find(curve.id);
              if (previous != reuse_map->end()) {
                pyramid = CurveLodPyramid::extend_tail(
                    previous->second, *axis, curve, build_options, stop_token);
              }
            }
            if (!pyramid.has_value() &&
                pyramid.error().code != ErrorCode::operation_cancelled) {
              pyramid = CurveLodPyramid::build(*axis, curve, build_options,
                                               stop_token);
            }
            if (!pyramid.has_value()) {
              output.cancelled =
                  pyramid.error().code == ErrorCode::operation_cancelled;
              if (!output.cancelled) {
                output.error = pyramid.error();
              }
              break;
            }
            output.derived_bytes += pyramid.value().statistics().derived_bytes;
            output.pyramids.emplace(curve.id, std::move(pyramid).value());
          }
          // Image pyramids (#184): build the level/tile grid for each
          // ImageSource. metadata-only (no pixel decode — ADR 0045); a build
          // failure degrades (no tiles for that image) rather than failing the
          // whole LOD task, mirroring how the scene tolerates a missing map.
          for (const auto &image : document->image_sources()) {
            if (stop_token.stop_requested()) {
              output.cancelled = true;
              break;
            }
            auto image_pyramid =
                ImagePyramid::build(image, image_pyramid_options, stop_token);
            if (!image_pyramid.has_value()) {
              output.cancelled =
                  image_pyramid.error().code == ErrorCode::operation_cancelled;
              if (!output.cancelled) {
                // Non-fatal: record the skipped image so poll_async publishes a
                // Diagnostic (qsp §7: degradation must be observable), then
                // degrade — the scene emits no layer for this source.
                output.skipped_images.push_back(image.id);
                continue;
              }
              break;
            }
            output.image_derived_bytes +=
                image_pyramid.value().statistics().derived_bytes;
            // Fold into the aggregate so the budget envelope (ADR 0034) and
            // performance_snapshot report the total derived bytes (curve +
            // image), not curve-only.
            output.derived_bytes +=
                image_pyramid.value().statistics().derived_bytes;
            output.image_pyramids.emplace(image.id,
                                          std::move(image_pyramid).value());
          }
        } catch (const std::bad_alloc &) {
          output.error = Error{
              .code = ErrorCode::resource_exhausted,
              .severity = Severity::error,
              .entity_id = document->id(),
              .message = MessageKey::resource_exhausted,
              .arguments = {},
          };
        } catch (...) {
          output.error = Error{
              .code = ErrorCode::internal_error,
              .severity = Severity::error,
              .entity_id = document->id(),
              .message = MessageKey::internal_error,
              .arguments = {},
          };
        }
        const auto guard = std::lock_guard{state->mutex};
        state->output = std::move(output);
        state->finished = true;
      });
    }
    const auto next_state_version = impl_->state_version + 1;
    std::vector<ViewEvent> pending_events;
    std::vector<Diagnostic> pending_diagnostics;
    pending_events.reserve(1 + document->curves().size());
    pending_diagnostics.reserve(document->curves().size());
    pending_events.push_back(ViewEvent{
        .kind = ViewEventKind::documents_changed,
        .state_version = next_state_version,
        .document_id = document_id,
        .document_revision = revision,
    });

    std::optional<std::uint64_t> first_diagnostic_id;
    for (const auto &curve : document->curves()) {
      const auto count = missing_sample_count(curve);
      if (count == 0) {
        continue;
      }
      if (impl_->next_diagnostic_id >
          std::numeric_limits<std::uint64_t>::max() -
              pending_diagnostics.size()) {
        return Error{
            .code = ErrorCode::internal_error,
            .severity = Severity::error,
            .entity_id = document_id,
            .message = MessageKey::internal_error,
            .arguments = {},
        };
      }
      const auto new_diagnostic_id =
          impl_->next_diagnostic_id + pending_diagnostics.size();
      if (!first_diagnostic_id.has_value()) {
        first_diagnostic_id = new_diagnostic_id;
      }
      pending_diagnostics.push_back(Diagnostic{
          .id = new_diagnostic_id,
          .code = DiagnosticCode::missing_samples,
          .severity = Severity::warning,
          .document_id = document_id,
          .entity_id = curve.id,
          .document_revision = revision,
          .occurrence_count = count,
      });
      pending_events.push_back(ViewEvent{
          .kind = ViewEventKind::diagnostic_published,
          .state_version = next_state_version,
          .document_id = document_id,
          .document_revision = revision,
      });
    }

    impl_->events.reserve(impl_->events.size() + pending_events.size());
    impl_->diagnostics.reserve(impl_->diagnostics.size() +
                               pending_diagnostics.size());
    if (asynchronous) {
      impl_->preparations.reserve(impl_->preparations.size() + 1);
    }
    if (asynchronous && !reuse_preparation) {
      impl_->lod_tasks.reserve(impl_->lod_tasks.size() + 1);
    }
    for (auto &existing_task : impl_->lod_tasks) {
      if (existing_task->document_id == document_id) {
        existing_task->state->stop_source.request_stop();
      }
    }
    for (auto &existing_task : impl_->frame_tasks) {
      if (existing_task->document_id == document_id) {
        existing_task->state->stop_source.request_stop();
      }
    }
    impl_->documents.insert_or_assign(document_id, document);
    impl_->prepared_scenes.erase(document_id);
    impl_->presentations.erase(document_id);
    impl_->viewports.erase(document_id);
    impl_->viewport_pixel_heights.erase(document_id);
    impl_->viewport_defaults.erase(document_id);
    impl_->crosshairs.erase(document_id);
    impl_->frame_generations.erase(document_id);
    const auto history_restore =
        impl_->pending_history_restores.find(document_id);
    if (history_restore != impl_->pending_history_restores.end()) {
      // History transitions restore layout and selection before any observer is
      // told about the replacement. SetDocumentCommand remains the single
      // revision/invalidation path, while these saved semantic values prevent
      // a transient cleared presentation or remapped selection from escaping.
      if (history_restore->second.presentation.has_value()) {
        impl_->presentations.insert_or_assign(
            document_id, *history_restore->second.presentation);
        pending_events.push_back(ViewEvent{
            .kind = ViewEventKind::presentation_changed,
            .state_version = next_state_version,
            .document_id = document_id,
            .document_revision = revision,
        });
      }
      if (history_restore->second.selection.has_value()) {
        impl_->selections.insert_or_assign(document_id,
                                           *history_restore->second.selection);
        pending_events.push_back(ViewEvent{
            .kind = history_restore->second.selection->valid
                        ? ViewEventKind::selection_changed
                        : ViewEventKind::selection_invalidated,
            .state_version = next_state_version,
            .document_id = document_id,
            .document_revision = revision,
        });
      } else if (impl_->selections.erase(document_id) != 0) {
        // ClearSelectionCommand uses selection_changed for an empty Selection
        // Set too; match that observable convention for a restored empty state.
        pending_events.push_back(ViewEvent{
            .kind = ViewEventKind::selection_changed,
            .state_version = next_state_version,
            .document_id = document_id,
            .document_revision = revision,
        });
      }
    } else {
      // ADR 0024: a document replacement attempts to safely remap an existing
      // selection onto the new revision's axis coordinates. If the selected
      // axis survived and the depth range still falls within the new axis
      // extent, the row span is recomputed against the new revision and the
      // selection stays valid. Otherwise the selection is explicitly
      // invalidated and a selection_invalidated event is published (the host
      // must stop using it). The outcome event folds into the pending events at
      // next_state_version.
      if (const auto sel = impl_->selections.find(document_id);
          sel != impl_->selections.end()) {
        const auto axis = find_axis(*document, sel->second.sampling_axis_id);
        if (axis != nullptr) {
          const auto span = rows_for_range(axis->coordinates, axis->direction,
                                           sel->second.reference_depth_range);
          const auto axis_extent =
              range_for_rows(axis->coordinates, 0, axis->coordinates.length());
          // Keep the selection if it resolves to a non-empty span within the
          // new axis extent; otherwise invalidate.
          const auto within = span.last > span.first &&
                              sel->second.reference_depth_range.top >=
                                  axis_extent.top - 1.0e-9 &&
                              sel->second.reference_depth_range.bottom <=
                                  axis_extent.bottom + 1.0e-9;
          if (within) {
            sel->second.first_row = span.first;
            sel->second.last_row = span.last;
            sel->second.document_revision = revision;
            sel->second.valid = true;
          } else {
            sel->second.valid = false;
            sel->second.document_revision = revision;
          }
        } else {
          sel->second.valid = false;
          sel->second.document_revision = revision;
        }
        pending_events.push_back(ViewEvent{
            .kind = sel->second.valid ? ViewEventKind::selection_changed
                                      : ViewEventKind::selection_invalidated,
            .state_version = next_state_version,
            .document_id = document_id,
            .document_revision = revision,
        });
      }
    }
    if (asynchronous) {
      if (reuse_preparation) {
        impl_->preparations.insert_or_assign(
            document_id, CurvePreparation{
                             .revision = revision,
                             .generation = 0,
                             .state = PreparationState::ready,
                             .derived_bytes = reuse->derived_bytes,
                             .maximum_derived_bytes = maximum_derived_bytes,
                             .pyramids = reuse->pyramids,
                             .image_pyramids = reuse->image_pyramids,
                         });
      } else {
        impl_->preparations.insert_or_assign(
            document_id, CurvePreparation{
                             .revision = revision,
                             .generation = generation,
                             .state = PreparationState::pending,
                             .derived_bytes = 0,
                             .maximum_derived_bytes = maximum_derived_bytes,
                             .pyramids = {},
                             .image_pyramids = {},
                         });
        impl_->lod_tasks.push_back(std::move(task));
        ++impl_->next_lod_generation;
      }
    } else {
      impl_->preparations.erase(document_id);
    }
    impl_->state_version = next_state_version;
    impl_->next_diagnostic_id += pending_diagnostics.size();
    impl_->events.insert(impl_->events.end(), pending_events.begin(),
                         pending_events.end());
    impl_->diagnostics.insert(impl_->diagnostics.end(),
                              pending_diagnostics.begin(),
                              pending_diagnostics.end());
    for (const auto &event : pending_events) {
      impl_->notify_observers(event);
    }

    return CommandReceipt{
        .state_version = next_state_version,
        .document_id = document_id,
        .document_revision = revision,
        .asynchronous_preparation_started = asynchronous,
        .diagnostic_id = first_diagnostic_id,
    };
  } catch (const std::bad_alloc &) {
    return Error{
        .code = ErrorCode::resource_exhausted,
        .severity = Severity::error,
        .entity_id = std::nullopt,
        .message = MessageKey::resource_exhausted,
        .arguments = {},
    };
  } catch (...) {
    return Error{
        .code = ErrorCode::internal_error,
        .severity = Severity::error,
        .entity_id = std::nullopt,
        .message = MessageKey::internal_error,
        .arguments = {},
    };
  }
}

Result<CommandReceipt>
WellLogSession::execute(const SetPresentationCommand &command) {
  try {
    const auto document_id = command.presentation.document_id();
    const auto document = impl_->documents.find(document_id);
    if (document == impl_->documents.end()) {
      return Error{
          .code = ErrorCode::document_not_found,
          .severity = Severity::error,
          .entity_id = document_id,
          .message = MessageKey::presentation_document_missing,
          .arguments = {},
      };
    }
    if (impl_->state_version == std::numeric_limits<std::uint64_t>::max()) {
      return Error{
          .code = ErrorCode::internal_error,
          .severity = Severity::error,
          .entity_id = document_id,
          .message = MessageKey::internal_error,
          .arguments = {},
      };
    }
    const auto revision = document->second->revision();
    const auto depth_range = command.presentation.reference_depth_range();
    const auto initial_viewport =
        DepthViewport{.top = depth_range.top, .bottom = depth_range.bottom};
    const auto preparation = impl_->preparations.find(document_id);
    if (preparation != impl_->preparations.end()) {
      if (preparation->second.state == PreparationState::unavailable) {
        return Error{
            .code = ErrorCode::internal_error,
            .severity = Severity::error,
            .entity_id = document_id,
            .message = MessageKey::internal_error,
            .arguments = {},
        };
      }
      auto frame_task = std::unique_ptr<FrameTask>{};
      auto frame_generation = std::uint64_t{};
      if (preparation->second.state == PreparationState::ready) {
        if (impl_->next_frame_generation ==
                std::numeric_limits<std::uint64_t>::max() ||
            preparation->second.pyramids == nullptr) {
          return Error{
              .code = ErrorCode::internal_error,
              .severity = Severity::error,
              .entity_id = document_id,
              .message = MessageKey::internal_error,
              .arguments = {},
          };
        }
        frame_generation = impl_->next_frame_generation;
        frame_task = make_frame_task(
            impl_->task_executor,
            document_id, revision, frame_generation, document->second,
            command.presentation, preparation->second.pyramids,
            CurveLodQuery{
                .viewport_top = initial_viewport.top,
                .viewport_bottom = initial_viewport.bottom,
                .pixel_height = default_frame_pixel_height,
                .prefetch_viewports = impl_->budgets.prefetch_viewports,
            },
            preparation->second.image_pyramids,
            ImagePyramidQuery{
                .viewport_top = initial_viewport.top,
                .viewport_bottom = initial_viewport.bottom,
                .pixel_height = static_cast<double>(default_frame_pixel_height),
                .prefetch_viewports = impl_->budgets.prefetch_viewports,
            },
            impl_->text_engine, &impl_->text_engine_mutex);
      }
      const auto next_state_version = impl_->state_version + 1;
      std::vector<ViewEvent> pending_events{
          ViewEvent{
              .kind = ViewEventKind::presentation_changed,
              .state_version = next_state_version,
              .document_id = document_id,
              .document_revision = revision,
          },
          ViewEvent{
              .kind = ViewEventKind::viewport_changed,
              .state_version = next_state_version,
              .document_id = document_id,
              .document_revision = revision,
          },
      };
      impl_->events.reserve(impl_->events.size() + pending_events.size());
      impl_->presentations.reserve(impl_->presentations.size() + 1);
      impl_->viewports.reserve(impl_->viewports.size() + 1);
      impl_->viewport_pixel_heights.reserve(
          impl_->viewport_pixel_heights.size() + 1);
      impl_->viewport_defaults.reserve(impl_->viewport_defaults.size() + 1);
      if (frame_task != nullptr) {
        impl_->frame_tasks.reserve(impl_->frame_tasks.size() + 1);
        impl_->frame_generations.reserve(impl_->frame_generations.size() + 1);
      }
      for (auto &existing_task : impl_->frame_tasks) {
        if (existing_task->document_id == document_id) {
          existing_task->state->stop_source.request_stop();
        }
      }
      if (frame_task != nullptr) {
        impl_->frame_generations.insert_or_assign(document_id,
                                                  frame_generation);
        impl_->frame_tasks.push_back(std::move(frame_task));
        ++impl_->next_frame_generation;
      } else {
        impl_->frame_generations.erase(document_id);
      }
      impl_->presentations.insert_or_assign(document_id, command.presentation);
      impl_->viewports.insert_or_assign(document_id, initial_viewport);
      impl_->viewport_pixel_heights.insert_or_assign(
          document_id, default_frame_pixel_height);
      impl_->viewport_defaults.insert_or_assign(document_id, initial_viewport);
      impl_->crosshairs.erase(document_id);
      impl_->state_version = next_state_version;
      impl_->events.insert(impl_->events.end(), pending_events.begin(),
                           pending_events.end());
      for (const auto &event : pending_events) {
        impl_->notify_observers(event);
      }
      return CommandReceipt{
          .state_version = next_state_version,
          .document_id = document_id,
          .document_revision = revision,
          .asynchronous_preparation_started =
              preparation->second.state == PreparationState::pending ||
              frame_generation != 0,
          .diagnostic_id = std::nullopt,
      };
    }

    Result<PreparedScene> prepared = Error{
        .code = ErrorCode::internal_error,
        .severity = Severity::error,
        .entity_id = std::nullopt,
        .message = MessageKey::internal_error,
        .arguments = {},
    };
    {
      const auto text_guard =
          impl_->text_engine == nullptr
              ? std::unique_lock<std::mutex>{}
              : std::unique_lock<std::mutex>{impl_->text_engine_mutex};
      prepared = detail::ScenePreparer::prepare(
          *document->second, command.presentation, impl_->text_engine.get());
    }
    if (!prepared) {
      return prepared.error();
    }

    auto scene =
        std::make_shared<const PreparedScene>(std::move(prepared).value());
    std::vector<ViewEvent> text_notifications;
    impl_->publish_value_issues(document_id, revision, *scene,
                                text_notifications);
    const auto text_diagnostic = impl_->publish_text_issues(
        document_id, revision, *scene, text_notifications);
    const auto next_state_version = impl_->state_version + 1;
    std::vector<ViewEvent> pending_events{
        ViewEvent{
            .kind = ViewEventKind::presentation_changed,
            .state_version = next_state_version,
            .document_id = document_id,
            .document_revision = revision,
        },
        ViewEvent{
            .kind = ViewEventKind::viewport_changed,
            .state_version = next_state_version,
            .document_id = document_id,
            .document_revision = revision,
        },
        ViewEvent{
            .kind = ViewEventKind::frame_ready,
            .state_version = next_state_version,
            .document_id = document_id,
            .document_revision = revision,
        },
    };
    impl_->events.reserve(impl_->events.size() + pending_events.size());
    impl_->prepared_scenes.reserve(impl_->prepared_scenes.size() + 1);
    impl_->presentations.reserve(impl_->presentations.size() + 1);
    impl_->viewports.reserve(impl_->viewports.size() + 1);
    impl_->viewport_pixel_heights.reserve(impl_->viewport_pixel_heights.size() +
                                          1);
    impl_->viewport_defaults.reserve(impl_->viewport_defaults.size() + 1);
    for (auto &existing_task : impl_->frame_tasks) {
      if (existing_task->document_id == document_id) {
        existing_task->state->stop_source.request_stop();
      }
    }
    impl_->frame_generations.erase(document_id);
    impl_->prepared_scenes.insert_or_assign(document_id, std::move(scene));
    impl_->presentations.insert_or_assign(document_id, command.presentation);
    impl_->viewports.insert_or_assign(document_id, initial_viewport);
    impl_->viewport_pixel_heights.insert_or_assign(document_id,
                                                   default_frame_pixel_height);
    impl_->viewport_defaults.insert_or_assign(document_id, initial_viewport);
    impl_->crosshairs.erase(document_id);
    impl_->state_version = next_state_version;
    impl_->events.insert(impl_->events.end(), pending_events.begin(),
                         pending_events.end());
    for (const auto &event : pending_events) {
      impl_->notify_observers(event);
    }
    for (const auto &event : text_notifications) {
      impl_->notify_observers(event);
    }
    return CommandReceipt{
        .state_version = next_state_version,
        .document_id = document_id,
        .document_revision = revision,
        .asynchronous_preparation_started = false,
        .diagnostic_id = text_diagnostic,
    };
  } catch (const std::bad_alloc &) {
    return Error{
        .code = ErrorCode::resource_exhausted,
        .severity = Severity::error,
        .entity_id = std::nullopt,
        .message = MessageKey::resource_exhausted,
        .arguments = {},
    };
  } catch (...) {
    return Error{
        .code = ErrorCode::internal_error,
        .severity = Severity::error,
        .entity_id = std::nullopt,
        .message = MessageKey::internal_error,
        .arguments = {},
    };
  }
}

namespace {

[[nodiscard]] Millimetres
presentation_column_width(const ScenePresentation &presentation) noexcept {
  double width = 0.0;
  for (const auto &track : presentation.tracks()) {
    if (std::isfinite(track.width.value) && track.width.value > 0.0) {
      width += track.width.value;
    }
  }
  return Millimetres{width > 0.0 ? width : 40.0};
}

[[nodiscard]] bool layout_contains(const std::vector<WellPlacement> &layout,
                                   EntityId document_id) noexcept {
  return std::any_of(layout.begin(), layout.end(),
                     [document_id](const WellPlacement &well) {
                       return well.document_id == document_id;
                     });
}

} // namespace

Result<CommandReceipt>
WellLogSession::execute(const SetWellLayoutCommand &command) {
  try {
    if (command.wells.empty()) {
      return execute(ClearWellLayoutCommand{});
    }
    std::vector<WellPlacement> packed;
    packed.reserve(command.wells.size());
    double cursor = 0.0;
    for (const auto &well : command.wells) {
      if (well.document_id.is_nil() ||
          !impl_->documents.contains(well.document_id)) {
        return Error{
            .code = ErrorCode::document_not_found,
            .severity = Severity::error,
            .entity_id = well.document_id,
            .message = MessageKey::document_structure_invalid,
            .arguments = {},
        };
      }
      WellPlacement next = well;
      if (command.pack_left_to_right) {
        Millimetres width = well.width;
        if (width.value <= 0.0) {
          if (const auto scene = prepared_scene(well.document_id);
              scene != nullptr && scene->physical_width().value > 0.0) {
            width = scene->physical_width();
          } else if (const auto pres =
                         impl_->presentations.find(well.document_id);
                     pres != impl_->presentations.end()) {
            width = presentation_column_width(pres->second);
          } else {
            width = Millimetres{40.0};
          }
        }
        next.left = Millimetres{cursor};
        next.width = width;
        cursor += width.value + command.gap.value;
      }
      packed.push_back(next);
    }
    impl_->well_layout = std::move(packed);
    impl_->well_layout_gap = command.gap;
    if (impl_->state_version == std::numeric_limits<std::uint64_t>::max()) {
      return Error{.code = ErrorCode::internal_error,
                   .severity = Severity::error,
                   .entity_id = std::nullopt,
                   .message = MessageKey::internal_error,
                   .arguments = {}};
    }
    ++impl_->state_version;
    return CommandReceipt{
        .state_version = impl_->state_version,
        .document_id = impl_->well_layout.front().document_id,
        .document_revision =
            impl_->documents.at(impl_->well_layout.front().document_id)
                ->revision(),
        .asynchronous_preparation_started = false,
        .diagnostic_id = std::nullopt,
    };
  } catch (const std::bad_alloc &) {
    return Error{.code = ErrorCode::resource_exhausted,
                 .severity = Severity::error,
                 .entity_id = std::nullopt,
                 .message = MessageKey::resource_exhausted,
                 .arguments = {}};
  } catch (...) {
    return Error{.code = ErrorCode::internal_error,
                 .severity = Severity::error,
                 .entity_id = std::nullopt,
                 .message = MessageKey::internal_error,
                 .arguments = {}};
  }
}

Result<CommandReceipt>
WellLogSession::execute(const ClearWellLayoutCommand &) {
  impl_->well_layout.clear();
  impl_->shared_depth_viewport.reset();
  impl_->shared_pixel_height.reset();
  impl_->surface_horizontal_view.reset();
  if (impl_->state_version != std::numeric_limits<std::uint64_t>::max()) {
    ++impl_->state_version;
  }
  return CommandReceipt{
      .state_version = impl_->state_version,
      .document_id = EntityId{},
      .document_revision = DocumentRevision{},
      .asynchronous_preparation_started = false,
      .diagnostic_id = std::nullopt,
  };
}

Result<CommandReceipt>
WellLogSession::execute(const SetSharedDepthViewportCommand &command) {
  if (!valid_viewport(command.viewport)) {
    return viewport_error(EntityId{});
  }
  if (impl_->well_layout.empty()) {
    return Error{.code = ErrorCode::invalid_presentation,
                 .severity = Severity::error,
                 .entity_id = std::nullopt,
                 .message = MessageKey::presentation_invalid,
                 .arguments = {}};
  }
  impl_->shared_depth_viewport = command.viewport;
  if (command.pixel_height != 0) {
    impl_->shared_pixel_height = command.pixel_height;
  }
  // Broadcast to every well so per-doc prepared scenes stay in sync.
  Result<CommandReceipt> last = viewport_error(EntityId{});
  for (const auto &well : impl_->well_layout) {
    if (!well.visible) {
      continue;
    }
    const auto pixel =
        command.pixel_height != 0
            ? command.pixel_height
            : viewport_pixel_height(well.document_id).value_or(std::uint32_t{1});
    last = execute(SetViewportMetricsCommand{
        .document_id = well.document_id,
        .viewport = command.viewport,
        .pixel_height = pixel == 0 ? 1U : pixel,
    });
    if (!last.has_value()) {
      return last;
    }
  }
  return last;
}

Result<CommandReceipt>
WellLogSession::execute(const SetSurfaceHorizontalViewCommand &command) {
  if (!std::isfinite(command.left_mm) || !std::isfinite(command.right_mm) ||
      command.right_mm <= command.left_mm) {
    return Error{.code = ErrorCode::invalid_presentation,
                 .severity = Severity::error,
                 .entity_id = std::nullopt,
                 .message = MessageKey::presentation_invalid,
                 .arguments = {}};
  }
  impl_->surface_horizontal_view =
      std::pair<double, double>{command.left_mm, command.right_mm};
  if (impl_->state_version != std::numeric_limits<std::uint64_t>::max()) {
    ++impl_->state_version;
  }
  return CommandReceipt{
      .state_version = impl_->state_version,
      .document_id = impl_->well_layout.empty()
                         ? EntityId{}
                         : impl_->well_layout.front().document_id,
      .document_revision = DocumentRevision{},
      .asynchronous_preparation_started = false,
      .diagnostic_id = std::nullopt,
  };
}

Result<CommandReceipt>
WellLogSession::execute(const SetViewportCommand &command) {
  // Multi-well surface: viewport changes on any layout well are shared.
  if (!impl_->well_layout.empty() &&
      layout_contains(impl_->well_layout, command.document_id)) {
    return execute(SetSharedDepthViewportCommand{
        .viewport = command.viewport,
        .pixel_height =
            viewport_pixel_height(command.document_id).value_or(std::uint32_t{}),
    });
  }
  return execute(SetViewportMetricsCommand{
      .document_id = command.document_id,
      .viewport = command.viewport,
      .pixel_height =
          viewport_pixel_height(command.document_id).value_or(std::uint32_t{}),
  });
}

namespace {

[[nodiscard]] Result<ScenePresentation>
rebuild_presentation_with_transform(const ScenePresentation &pres,
                                    const DepthTransform &transform) {
  ScenePresentationBuilder builder(
      pres.document_id(), pres.reference_depth_range(), pres.physical_height(),
      pres.font_asset_fingerprint());
  builder.set_presentation_version(pres.presentation_version());
  builder.set_depth_transform(transform);
  for (const auto &track : pres.tracks()) {
    builder.add_track(track);
  }
  for (const auto &scale : pres.scales()) {
    builder.add_scale(scale);
  }
  for (const auto &layer : pres.curve_layers()) {
    builder.add_curve_layer(layer);
  }
  for (const auto &pattern : pres.patterns()) {
    builder.add_pattern(pattern);
  }
  for (const auto &layer : pres.interval_layers()) {
    builder.add_interval_layer(layer);
  }
  for (const auto &layer : pres.crossover_fill_layers()) {
    builder.add_crossover_fill_layer(layer);
  }
  for (const auto &layer : pres.image_layers()) {
    builder.add_image_layer(layer);
  }
  for (const auto &layer : pres.marker_layers()) {
    builder.add_marker_layer(layer);
  }
  for (const auto &layer : pres.symbol_layers()) {
    builder.add_symbol_layer(layer);
  }
  for (const auto &layer : pres.text_layers()) {
    builder.add_text_layer(layer);
  }
  for (const auto &layer : pres.custom_layers()) {
    builder.add_custom_layer(layer);
  }
  auto built = builder.build();
  if (built.document_id().is_nil()) {
    return Error{.code = ErrorCode::resource_exhausted,
                 .severity = Severity::error,
                 .entity_id = pres.document_id(),
                 .message = MessageKey::resource_exhausted,
                 .arguments = {}};
  }
  return built;
}

[[nodiscard]] std::optional<double>
marker_reference_depth(const WellLogDocument &doc, EntityId marker_id) {
  for (const auto &marker : doc.markers()) {
    if (marker.id == marker_id) {
      return marker.reference_depth;
    }
  }
  return std::nullopt;
}

} // namespace

Result<CommandReceipt>
WellLogSession::execute(const SetDepthTransformCommand &command) {
  try {
    if (const auto validated = validate_depth_transform(command.transform);
        validated.has_value()) {
      return *validated;
    }
    if (!impl_->documents.contains(command.document_id)) {
      return Error{.code = ErrorCode::document_not_found,
                   .severity = Severity::error,
                   .entity_id = command.document_id,
                   .message = MessageKey::document_structure_invalid,
                   .arguments = {}};
    }
    impl_->depth_transforms[command.document_id] = command.transform;
    const auto pres_it = impl_->presentations.find(command.document_id);
    if (pres_it == impl_->presentations.end()) {
      // Transform stored; applied when a presentation is next set.
      if (impl_->state_version != std::numeric_limits<std::uint64_t>::max()) {
        ++impl_->state_version;
      }
      return CommandReceipt{
          .state_version = impl_->state_version,
          .document_id = command.document_id,
          .document_revision =
              impl_->documents.at(command.document_id)->revision(),
          .asynchronous_preparation_started = false,
          .diagnostic_id = std::nullopt,
      };
    }
    auto rebuilt =
        rebuild_presentation_with_transform(pres_it->second, command.transform);
    if (!rebuilt.has_value()) {
      return rebuilt.error();
    }
    return execute(SetPresentationCommand{std::move(rebuilt.value())});
  } catch (const std::bad_alloc &) {
    return Error{.code = ErrorCode::resource_exhausted,
                 .severity = Severity::error,
                 .entity_id = command.document_id,
                 .message = MessageKey::resource_exhausted,
                 .arguments = {}};
  } catch (...) {
    return Error{.code = ErrorCode::internal_error,
                 .severity = Severity::error,
                 .entity_id = command.document_id,
                 .message = MessageKey::internal_error,
                 .arguments = {}};
  }
}

Result<CommandReceipt>
WellLogSession::execute(const AlignWellsToMarkersCommand &command) {
  try {
    if (command.target_marker_ids.size() < 2 ||
        command.wells.empty()) {
      return Error{.code = ErrorCode::invalid_presentation,
                   .severity = Severity::error,
                   .entity_id = command.target_document_id,
                   .message = MessageKey::presentation_invalid,
                   .arguments = {}};
    }
    const auto target_doc = document(command.target_document_id);
    if (target_doc == nullptr) {
      return Error{.code = ErrorCode::document_not_found,
                   .severity = Severity::error,
                   .entity_id = command.target_document_id,
                   .message = MessageKey::document_structure_invalid,
                   .arguments = {}};
    }
    std::vector<double> target_depths;
    target_depths.reserve(command.target_marker_ids.size());
    for (const auto mid : command.target_marker_ids) {
      const auto d = marker_reference_depth(*target_doc, mid);
      if (!d.has_value()) {
        return Error{.code = ErrorCode::invalid_document,
                     .severity = Severity::error,
                     .entity_id = mid,
                     .message = MessageKey::document_structure_invalid,
                     .arguments = {}};
      }
      // Target uses identity transform: display == reference.
      target_depths.push_back(*d);
    }
    // Target well: identity transform.
    {
      auto r = execute(SetDepthTransformCommand{
          .document_id = command.target_document_id,
          .transform = DepthTransform{},
      });
      if (!r.has_value()) {
        return r;
      }
    }
    for (const auto &well : command.wells) {
      if (well.marker_ids.size() != command.target_marker_ids.size()) {
        return Error{.code = ErrorCode::invalid_presentation,
                     .severity = Severity::error,
                     .entity_id = well.document_id,
                     .message = MessageKey::presentation_invalid,
                     .arguments = {}};
      }
      const auto doc = document(well.document_id);
      if (doc == nullptr) {
        return Error{.code = ErrorCode::document_not_found,
                     .severity = Severity::error,
                     .entity_id = well.document_id,
                     .message = MessageKey::document_structure_invalid,
                     .arguments = {}};
      }
      std::vector<double> source_depths;
      source_depths.reserve(well.marker_ids.size());
      for (const auto mid : well.marker_ids) {
        const auto d = marker_reference_depth(*doc, mid);
        if (!d.has_value()) {
          return Error{.code = ErrorCode::invalid_document,
                       .severity = Severity::error,
                       .entity_id = mid,
                       .message = MessageKey::document_structure_invalid,
                       .arguments = {}};
        }
        source_depths.push_back(*d);
      }
      auto xform = depth_transform_aligning_markers(source_depths, target_depths);
      if (!xform.has_value()) {
        return xform.error();
      }
      auto r = execute(SetDepthTransformCommand{
          .document_id = well.document_id,
          .transform = std::move(xform.value()),
      });
      if (!r.has_value()) {
        return r;
      }
    }
    if (valid_viewport(command.shared_viewport)) {
      // Ensure layout includes all wells if not already.
      if (impl_->well_layout.empty()) {
        std::vector<WellPlacement> wells{
            WellPlacement{.document_id = command.target_document_id}};
        for (const auto &w : command.wells) {
          wells.push_back(WellPlacement{.document_id = w.document_id});
        }
        auto layout = execute(SetWellLayoutCommand{
            .wells = std::move(wells),
            .pack_left_to_right = true,
        });
        if (!layout.has_value()) {
          return layout;
        }
      }
      return execute(SetSharedDepthViewportCommand{
          .viewport = command.shared_viewport,
          .pixel_height = command.pixel_height,
      });
    }
    if (impl_->state_version != std::numeric_limits<std::uint64_t>::max()) {
      ++impl_->state_version;
    }
    return CommandReceipt{
        .state_version = impl_->state_version,
        .document_id = command.target_document_id,
        .document_revision = target_doc->revision(),
        .asynchronous_preparation_started = false,
        .diagnostic_id = std::nullopt,
    };
  } catch (const std::bad_alloc &) {
    return Error{.code = ErrorCode::resource_exhausted,
                 .severity = Severity::error,
                 .entity_id = command.target_document_id,
                 .message = MessageKey::resource_exhausted,
                 .arguments = {}};
  } catch (...) {
    return Error{.code = ErrorCode::internal_error,
                 .severity = Severity::error,
                 .entity_id = command.target_document_id,
                 .message = MessageKey::internal_error,
                 .arguments = {}};
  }
}

Result<CommandReceipt>
WellLogSession::execute(const SetCrossWellOverlaysCommand &command) {
  try {
    for (const auto &overlay : command.overlays) {
      if (overlay.id.is_nil() || overlay.left_document_id.is_nil() ||
          overlay.right_document_id.is_nil() ||
          overlay.left_marker_id.is_nil() ||
          overlay.right_marker_id.is_nil()) {
        return Error{.code = ErrorCode::invalid_document,
                     .severity = Severity::error,
                     .entity_id = overlay.id,
                     .message = MessageKey::document_structure_invalid,
                     .arguments = {}};
      }
      if (!impl_->documents.contains(overlay.left_document_id) ||
          !impl_->documents.contains(overlay.right_document_id)) {
        return Error{.code = ErrorCode::document_not_found,
                     .severity = Severity::error,
                     .entity_id = overlay.id,
                     .message = MessageKey::document_structure_invalid,
                     .arguments = {}};
      }
      if (overlay.kind == CrossWellOverlay::Kind::correlation_band &&
          (overlay.left_bottom_marker_id.is_nil() ||
           overlay.right_bottom_marker_id.is_nil())) {
        return Error{.code = ErrorCode::invalid_document,
                     .severity = Severity::error,
                     .entity_id = overlay.id,
                     .message = MessageKey::document_structure_invalid,
                     .arguments = {}};
      }
    }
    impl_->cross_well_overlays = command.overlays;
    if (impl_->state_version != std::numeric_limits<std::uint64_t>::max()) {
      ++impl_->state_version;
    }
    return CommandReceipt{
        .state_version = impl_->state_version,
        .document_id = impl_->well_layout.empty()
                           ? EntityId{}
                           : impl_->well_layout.front().document_id,
        .document_revision = DocumentRevision{},
        .asynchronous_preparation_started = false,
        .diagnostic_id = std::nullopt,
    };
  } catch (const std::bad_alloc &) {
    return Error{.code = ErrorCode::resource_exhausted,
                 .severity = Severity::error,
                 .entity_id = std::nullopt,
                 .message = MessageKey::resource_exhausted,
                 .arguments = {}};
  } catch (...) {
    return Error{.code = ErrorCode::internal_error,
                 .severity = Severity::error,
                 .entity_id = std::nullopt,
                 .message = MessageKey::internal_error,
                 .arguments = {}};
  }
}

Result<CommandReceipt>
WellLogSession::execute(const SetViewportMetricsCommand &command) {
  try {
    if (!valid_viewport(command.viewport)) {
      return viewport_error(command.document_id);
    }
    const auto document = impl_->documents.find(command.document_id);
    const auto viewport = impl_->viewports.find(command.document_id);
    const auto viewport_pixel_height =
        impl_->viewport_pixel_heights.find(command.document_id);
    if (document == impl_->documents.end() ||
        viewport == impl_->viewports.end() ||
        viewport_pixel_height == impl_->viewport_pixel_heights.end()) {
      return viewport_error(command.document_id);
    }
    if (command.pixel_height == 0) {
      return viewport_error(command.document_id);
    }
    if (impl_->state_version == std::numeric_limits<std::uint64_t>::max()) {
      return viewport_error(command.document_id);
    }
    auto frame_task = std::unique_ptr<FrameTask>{};
    auto frame_generation = std::uint64_t{};
    const auto preparation = impl_->preparations.find(command.document_id);
    const auto presentation = impl_->presentations.find(command.document_id);
    if (preparation != impl_->preparations.end() &&
        preparation->second.state == PreparationState::ready &&
        presentation != impl_->presentations.end()) {
      if (impl_->next_frame_generation ==
          std::numeric_limits<std::uint64_t>::max()) {
        return viewport_error(command.document_id);
      }
      frame_generation = impl_->next_frame_generation;
      frame_task = make_frame_task(
          impl_->task_executor,
          command.document_id, document->second->revision(), frame_generation,
          document->second, presentation->second, preparation->second.pyramids,
          CurveLodQuery{
              .viewport_top = command.viewport.top,
              .viewport_bottom = command.viewport.bottom,
              .pixel_height = command.pixel_height,
              .prefetch_viewports = impl_->budgets.prefetch_viewports,
          },
          preparation->second.image_pyramids,
          ImagePyramidQuery{
              .viewport_top = command.viewport.top,
              .viewport_bottom = command.viewport.bottom,
              .pixel_height = static_cast<double>(command.pixel_height),
              .prefetch_viewports = impl_->budgets.prefetch_viewports,
          },
          impl_->text_engine, &impl_->text_engine_mutex);
    }
    const auto next_state_version = impl_->state_version + 1;
    const auto revision = document->second->revision();
    impl_->events.reserve(impl_->events.size() + 1);
    if (frame_task != nullptr) {
      impl_->frame_tasks.reserve(impl_->frame_tasks.size() + 1);
      impl_->frame_generations.reserve(impl_->frame_generations.size() + 1);
      for (auto &existing_task : impl_->frame_tasks) {
        if (existing_task->document_id == command.document_id) {
          existing_task->state->stop_source.request_stop();
        }
      }
      impl_->frame_generations.insert_or_assign(command.document_id,
                                                frame_generation);
      impl_->frame_tasks.push_back(std::move(frame_task));
      ++impl_->next_frame_generation;
    }
    viewport->second = command.viewport;
    viewport_pixel_height->second = command.pixel_height;
    impl_->state_version = next_state_version;
    const auto event = ViewEvent{
        .kind = ViewEventKind::viewport_changed,
        .state_version = next_state_version,
        .document_id = command.document_id,
        .document_revision = revision,
    };
    impl_->events.push_back(event);
    impl_->notify_observers(event);
    return CommandReceipt{
        .state_version = next_state_version,
        .document_id = command.document_id,
        .document_revision = revision,
        .asynchronous_preparation_started = frame_generation != 0,
        .diagnostic_id = std::nullopt,
    };
  } catch (const std::bad_alloc &) {
    return Error{
        .code = ErrorCode::resource_exhausted,
        .severity = Severity::error,
        .entity_id = command.document_id,
        .message = MessageKey::resource_exhausted,
        .arguments = {},
    };
  } catch (...) {
    return Error{
        .code = ErrorCode::internal_error,
        .severity = Severity::error,
        .entity_id = command.document_id,
        .message = MessageKey::internal_error,
        .arguments = {},
    };
  }
}

Result<CommandReceipt> WellLogSession::execute(const PanDepthCommand &command) {
  const auto current = viewport(command.document_id);
  if (!current.has_value() || !std::isfinite(command.display_depth_delta)) {
    return viewport_error(command.document_id);
  }
  const auto next = DepthViewport{
      .top = current->top + command.display_depth_delta,
      .bottom = current->bottom + command.display_depth_delta,
  };
  if (!valid_viewport(next)) {
    return viewport_error(command.document_id);
  }
  return execute(SetViewportCommand{
      .document_id = command.document_id,
      .viewport = next,
  });
}

Result<CommandReceipt>
WellLogSession::execute(const ZoomDepthAtCommand &command) {
  const auto current = viewport(command.document_id);
  if (!current.has_value() || !std::isfinite(command.anchor_display_depth) ||
      !std::isfinite(command.span_factor) || command.span_factor <= 0.0) {
    return viewport_error(command.document_id);
  }
  const auto next = DepthViewport{
      .top =
          command.anchor_display_depth +
          (current->top - command.anchor_display_depth) * command.span_factor,
      .bottom = command.anchor_display_depth +
                (current->bottom - command.anchor_display_depth) *
                    command.span_factor,
  };
  if (!valid_viewport(next)) {
    return viewport_error(command.document_id);
  }
  return execute(SetViewportCommand{
      .document_id = command.document_id,
      .viewport = next,
  });
}

Result<CommandReceipt>
WellLogSession::execute(const ResetViewportCommand &command) {
  const auto default_viewport =
      impl_->viewport_defaults.find(command.document_id);
  if (default_viewport == impl_->viewport_defaults.end()) {
    return viewport_error(command.document_id);
  }
  return execute(SetViewportCommand{
      .document_id = command.document_id,
      .viewport = default_viewport->second,
  });
}

Result<CommandReceipt>
WellLogSession::execute(const SetCrosshairCommand &command) {
  try {
    if (command.crosshair.has_value() && !valid_crosshair(*command.crosshair)) {
      return viewport_error(command.document_id);
    }
    const auto document = impl_->documents.find(command.document_id);
    if (document == impl_->documents.end() ||
        !impl_->viewports.contains(command.document_id)) {
      return viewport_error(command.document_id);
    }
    if (impl_->state_version == std::numeric_limits<std::uint64_t>::max()) {
      return viewport_error(command.document_id);
    }
    const auto next_state_version = impl_->state_version + 1;
    const auto revision = document->second->revision();
    // Multi-well surface: shared Display Depth cursor across layout wells.
    std::vector<EntityId> targets{command.document_id};
    if (!impl_->well_layout.empty() &&
        layout_contains(impl_->well_layout, command.document_id)) {
      targets.clear();
      for (const auto &well : impl_->well_layout) {
        if (well.visible && impl_->documents.contains(well.document_id)) {
          targets.push_back(well.document_id);
        }
      }
    }
    impl_->events.reserve(impl_->events.size() + targets.size());
    for (const auto target : targets) {
      if (command.crosshair.has_value()) {
        impl_->crosshairs.insert_or_assign(target, *command.crosshair);
      } else {
        impl_->crosshairs.erase(target);
      }
    }
    impl_->state_version = next_state_version;
    const auto event = ViewEvent{
        .kind = ViewEventKind::crosshair_changed,
        .state_version = next_state_version,
        .document_id = command.document_id,
        .document_revision = revision,
    };
    impl_->events.push_back(event);
    impl_->notify_observers(event);
    return CommandReceipt{
        .state_version = next_state_version,
        .document_id = command.document_id,
        .document_revision = revision,
        .asynchronous_preparation_started = false,
        .diagnostic_id = std::nullopt,
    };
  } catch (const std::bad_alloc &) {
    return Error{
        .code = ErrorCode::resource_exhausted,
        .severity = Severity::error,
        .entity_id = command.document_id,
        .message = MessageKey::resource_exhausted,
        .arguments = {},
    };
  } catch (...) {
    return Error{
        .code = ErrorCode::internal_error,
        .severity = Severity::error,
        .entity_id = command.document_id,
        .message = MessageKey::internal_error,
        .arguments = {},
    };
  }
}

// Shared apply path for the selection commands. Resolves a SelectionState for
// `document_id` over `axis_id` from either a depth range or a row span, stores
// it, bumps the version, and publishes a selection_changed event. Rejects when
// the document or axis is unknown, or the range/span is invalid.
[[nodiscard]] Result<CommandReceipt> WellLogSession::apply_selection(
    EntityId document_id, EntityId axis_id, SelectionDepthRange range,
    std::uint64_t first_row, std::uint64_t last_row, bool from_rows) {
  try {
    const auto document = impl_->documents.find(document_id);
    if (document == impl_->documents.end()) {
      return selection_document_missing(document_id);
    }
    const auto axis = find_axis(*document->second, axis_id);
    if (axis == nullptr) {
      return selection_axis_missing(axis_id);
    }
    const auto revision = document->second->revision();
    if (from_rows) {
      // Resolve rows → range, then recompute the row span from that range so
      // the stored span is canonical (clamped, monotone).
      range = range_for_rows(axis->coordinates, first_row, last_row);
    }
    if (!valid_selection_range(range)) {
      return selection_invalid(document_id);
    }
    const auto span = rows_for_range(axis->coordinates, axis->direction, range);
    if (impl_->state_version == std::numeric_limits<std::uint64_t>::max()) {
      return selection_invalid(document_id);
    }
    const auto next_state_version = impl_->state_version + 1;
    impl_->events.reserve(impl_->events.size() + 1);
    impl_->selections.reserve(impl_->selections.size() + 1);
    impl_->selections.insert_or_assign(document_id,
                                       SelectionState{
                                           .document_id = document_id,
                                           .sampling_axis_id = axis_id,
                                           .reference_depth_range = range,
                                           .first_row = span.first,
                                           .last_row = span.last,
                                           .document_revision = revision,
                                           .valid = true,
                                       });
    impl_->state_version = next_state_version;
    const auto event = ViewEvent{
        .kind = ViewEventKind::selection_changed,
        .state_version = next_state_version,
        .document_id = document_id,
        .document_revision = revision,
    };
    impl_->events.push_back(event);
    impl_->notify_observers(event);
    return CommandReceipt{
        .state_version = next_state_version,
        .document_id = document_id,
        .document_revision = revision,
        .asynchronous_preparation_started = false,
        .diagnostic_id = std::nullopt,
    };
  } catch (const std::bad_alloc &) {
    return Error{
        .code = ErrorCode::resource_exhausted,
        .severity = Severity::error,
        .entity_id = document_id,
        .message = MessageKey::resource_exhausted,
        .arguments = {},
    };
  } catch (...) {
    return Error{
        .code = ErrorCode::internal_error,
        .severity = Severity::error,
        .entity_id = document_id,
        .message = MessageKey::internal_error,
        .arguments = {},
    };
  }
}

Result<CommandReceipt>
WellLogSession::execute(const SetSelectionCommand &command) {
  return apply_selection(command.document_id, command.sampling_axis_id,
                         command.reference_depth_range, 0, 0,
                         /*from_rows=*/false);
}

Result<CommandReceipt>
WellLogSession::execute(const SetRowSelectionCommand &command) {
  if (command.last_row < command.first_row) {
    return selection_invalid(command.document_id);
  }
  return apply_selection(command.document_id, command.sampling_axis_id,
                         SelectionDepthRange{}, command.first_row,
                         command.last_row, /*from_rows=*/true);
}

Result<CommandReceipt>
WellLogSession::execute(const ClearSelectionCommand &command) {
  try {
    const auto document = impl_->documents.find(command.document_id);
    if (document == impl_->documents.end()) {
      return selection_document_missing(command.document_id);
    }
    if (!impl_->selections.contains(command.document_id)) {
      // Nothing to clear: still succeed, no event.
      return CommandReceipt{
          .state_version = impl_->state_version,
          .document_id = command.document_id,
          .document_revision = document->second->revision(),
          .asynchronous_preparation_started = false,
          .diagnostic_id = std::nullopt,
      };
    }
    if (impl_->state_version == std::numeric_limits<std::uint64_t>::max()) {
      return selection_invalid(command.document_id);
    }
    const auto next_state_version = impl_->state_version + 1;
    const auto revision = document->second->revision();
    impl_->events.reserve(impl_->events.size() + 1);
    impl_->selections.erase(command.document_id);
    impl_->state_version = next_state_version;
    const auto event = ViewEvent{
        .kind = ViewEventKind::selection_changed,
        .state_version = next_state_version,
        .document_id = command.document_id,
        .document_revision = revision,
    };
    impl_->events.push_back(event);
    impl_->notify_observers(event);
    return CommandReceipt{
        .state_version = next_state_version,
        .document_id = command.document_id,
        .document_revision = revision,
        .asynchronous_preparation_started = false,
        .diagnostic_id = std::nullopt,
    };
  } catch (...) {
    return Error{
        .code = ErrorCode::internal_error,
        .severity = Severity::error,
        .entity_id = command.document_id,
        .message = MessageKey::internal_error,
        .arguments = {},
    };
  }
}

// --- AppendBatchCommand (#198, ADR 0031) ------------------------------------
//
// Atomically appends a batch of curve tail-blocks to an existing document,
// producing one new Document Revision from the whole batch (or failing the
// whole batch). Old data blocks stay immutable and are NOT re-copied: each
// appended tail becomes a new segment on the curve's/axis's composite buffer,
// the existing segments retained via their SharedOwners. The session rejects
// an append whose declared revision is not strictly greater than the current
// (monotonic revision gate). Out-of-order and historical backfill are rejected
// — those require an explicit Replace/Patch.

// Gathers the existing physical segments of a CurveBuffer in order: the single
// block, or each composite segment. Used to rebuild a composite spanning the
// existing data plus a new tail, with no contiguous copy of the old data.
[[nodiscard]] std::vector<BufferView>
existing_segments(const CurveBuffer &buffer) {
  if (buffer.is_composite()) {
    const auto segs = buffer.segments();
    return {segs.begin(), segs.end()};
  }
  return {buffer.as_single()};
}

// Tail-continuity + monotonicity check for an append. The tail coordinates must
// (a) be monotone in the axis direction with no non-finite values, and (b)
// continue the existing axis: the tail's first coordinate must stand in the
// declared direction relative to the existing last coordinate (increasing →
// tail.first >= existing.last; decreasing → tail.first <= existing.last). An
// out-of-order tail (the next sample would step backward) or a historical
// backfill (tail starts before the existing end) fails here. Coordinates are
// compared as doubles — append coordinates are depths (floating-point); integer
// precision across the segment boundary is not a concern for an append.
[[nodiscard]] bool tail_continues_axis(const CurveBuffer &existing_coords,
                                       const BufferView &tail_coordinates,
                                       AxisDirection direction) noexcept {
  const auto existing_length = existing_coords.length();
  const auto tail_length = tail_coordinates.length();
  if (tail_length == 0) {
    return false;
  }
  // Tail must itself be monotone + finite in the declared direction.
  auto previous = tail_coordinates.value_as_double(0);
  if (!previous.has_value() || !std::isfinite(*previous)) {
    return false;
  }
  for (std::uint64_t index = 1; index < tail_length; ++index) {
    const auto current = tail_coordinates.value_as_double(index);
    if (!current.has_value() || !std::isfinite(*current)) {
      return false;
    }
    const auto ordered = direction == AxisDirection::increasing
                             ? *current >= *previous
                             : *current <= *previous;
    if (!ordered) {
      return false;
    }
    previous = current;
  }
  // Continuity against the existing axis end (only when the axis is non-empty;
  // an empty axis — impossible for a valid document — would accept any tail).
  if (existing_length == 0) {
    return true;
  }
  const auto existing_last =
      existing_coords.value_as_double(existing_length - 1);
  const auto tail_first = tail_coordinates.value_as_double(0);
  if (!existing_last.has_value() || !tail_first.has_value()) {
    return false;
  }
  return direction == AxisDirection::increasing ? *tail_first >= *existing_last
                                                : *tail_first <= *existing_last;
}

// Append-failure error builders. Each reuses the closest existing stable
// code/message so a caller distinguishes a missing document, a monotonic
// revision clash, a structural tail mismatch, and a direction/continuity
// violation — never a single opaque code (architecture.md §2 Result/Error).
[[nodiscard]] Error append_document_missing(EntityId document_id) {
  return Error{
      .code = ErrorCode::document_not_found,
      .severity = Severity::error,
      .entity_id = document_id,
      .message = MessageKey::presentation_document_missing,
      .arguments = {},
  };
}

[[nodiscard]] Error append_revision_not_monotonic(EntityId document_id) {
  // A revision clash is a document-structure violation (the host raced or
  // mis-stated the base revision); invalid_document is the closest stable code.
  return Error{
      .code = ErrorCode::invalid_document,
      .severity = Severity::error,
      .entity_id = document_id,
      .message = MessageKey::document_structure_invalid,
      .arguments = {},
  };
}

// A block names a curve id that does not exist on the document — the closest
// stable code is invalid_document (the referenced entity is absent), distinct
// from a missing sampling axis below so a caller can tell the two apart.
[[nodiscard]] Error append_curve_missing(EntityId curve_id) {
  return Error{
      .code = ErrorCode::invalid_document,
      .severity = Severity::error,
      .entity_id = curve_id,
      .message = MessageKey::document_structure_invalid,
      .arguments = {},
  };
}

// A block names a sampling axis id that does not exist on the document (or the
// curve's axis id disagrees with the block's). Reuses the same code/message
// validate_document uses when a curve references an absent axis.
[[nodiscard]] Error append_axis_missing(EntityId axis_id) {
  return Error{
      .code = ErrorCode::missing_sampling_axis,
      .severity = Severity::error,
      .entity_id = axis_id,
      .message = MessageKey::sampling_axis_missing,
      .arguments = {},
  };
}

[[nodiscard]] Error append_tail_mismatch(EntityId entity_id) {
  return Error{
      .code = ErrorCode::length_mismatch,
      .severity = Severity::error,
      .entity_id = entity_id,
      .message = MessageKey::curve_length_mismatch,
      .arguments = {},
  };
}

[[nodiscard]] Error append_tail_direction(EntityId axis_id) {
  return Error{
      .code = ErrorCode::invalid_sampling_axis,
      .severity = Severity::error,
      .entity_id = axis_id,
      .message = MessageKey::sampling_axis_direction_invalid,
      .arguments = {},
  };
}

// The minimum spacing between two visible append revisions at `rate_hz` (#201).
// 0 disables coalescing (handled by the caller). Capped at 1 ns (≥1000 Hz) to
// avoid sub-nanosecond periods; a host setting >1000 Hz gets 1000 Hz. Defined
// once here and shared by execute()'s gate and poll_async()'s overdue scan so
// the two cannot drift.
[[nodiscard]] std::chrono::nanoseconds
coalesce_interval(std::uint32_t rate_hz) noexcept {
  if (rate_hz >= 1000) {
    return std::chrono::nanoseconds{1};
  }
  return std::chrono::nanoseconds{std::int64_t{1'000'000'000} /
                                  std::int64_t{rate_hz}};
}

// --- Document Patch helpers (#202/#158, ADR 0025) ---------------------------

// Extracts the EntityId from any patchable entity (every PatchableEntity
// alternative has an `id` member named identically).
[[nodiscard]] EntityId patch_entity_id(const PatchableEntity &entity) noexcept {
  return std::visit([](const auto &e) -> EntityId { return e.id; }, entity);
}

// Returns the current value for one patchable entity, whether it lives in the
// document's interpretation collections or the presentation's layout
// collections. The history path copies that value into an inverse edit before
// an ApplyPatchCommand replaces or removes it.
[[nodiscard]] std::optional<PatchableEntity>
patchable_entity_at(const WellLogDocument &document,
                    const ScenePresentation *presentation, EntityId id) {
  const auto in_document =
      [id](const auto &entities) -> std::optional<PatchableEntity> {
    const auto found =
        std::find_if(entities.begin(), entities.end(),
                     [id](const auto &entity) { return entity.id == id; });
    return found == entities.end() ? std::nullopt
                                   : std::optional<PatchableEntity>{*found};
  };
  if (const auto found = in_document(document.intervals()); found.has_value()) {
    return found;
  }
  if (const auto found = in_document(document.markers()); found.has_value()) {
    return found;
  }
  if (const auto found = in_document(document.symbols()); found.has_value()) {
    return found;
  }
  if (const auto found = in_document(document.annotations());
      found.has_value()) {
    return found;
  }
  if (const auto found = in_document(document.qc_masks()); found.has_value()) {
    return found;
  }
  if (const auto found = in_document(document.curves()); found.has_value()) {
    // Only derived curves participate in the patch vocabulary; raw source
    // curves remain immutable via patch (ADR 0025 / #159).
    if (std::holds_alternative<Curve>(*found) &&
        !std::get<Curve>(*found).derived.has_value()) {
      return std::nullopt;
    }
    return found;
  }
  if (presentation == nullptr) {
    return std::nullopt;
  }
  if (const auto found = in_document(presentation->tracks());
      found.has_value()) {
    return found;
  }
  if (const auto found = in_document(presentation->scales());
      found.has_value()) {
    return found;
  }
  return in_document(presentation->curve_layers());
}

// Produces the inverse of a validated patch. An upsert that created an entity
// reverses to Remove; one that modified an entity restores its previous value;
// a Remove reverses to an upsert of the previous entity. Edits are reversed to
// preserve the normal command-inversion ordering even though #202 forbids
// duplicate ids in a patch.
[[nodiscard]] std::optional<std::vector<EntityEdit>>
inverse_edits_for_patch(const DocumentPatch &patch,
                        const WellLogDocument &document,
                        const ScenePresentation *presentation) {
  std::vector<EntityEdit> inverse;
  inverse.reserve(patch.edits.size());
  for (const auto &edit : patch.edits) {
    if (std::holds_alternative<UpsertEntity>(edit)) {
      const auto &upsert = std::get<UpsertEntity>(edit);
      const auto previous = patchable_entity_at(document, presentation,
                                                patch_entity_id(upsert.entity));
      if (previous.has_value()) {
        inverse.emplace_back(UpsertEntity{.entity = *previous});
      } else {
        inverse.emplace_back(
            RemoveEntity{.id = patch_entity_id(upsert.entity)});
      }
      continue;
    }
    const auto id = std::get<RemoveEntity>(edit).id;
    const auto previous = patchable_entity_at(document, presentation, id);
    if (!previous.has_value()) {
      return std::nullopt;
    }
    inverse.emplace_back(UpsertEntity{.entity = *previous});
  }
  std::reverse(inverse.begin(), inverse.end());
  return inverse;
}

// A patch-conflict error: the patch's base revision does not match the current
// revision (#202, ADR 0025). Stable code so a host detects a stale-base patch
// specifically (never applied by name/position guessing).
[[nodiscard]] Error patch_base_conflict(EntityId document_id) {
  return Error{
      .code = ErrorCode::patch_conflict,
      .severity = Severity::error,
      .entity_id = document_id,
      .message = MessageKey::patch_base_revision_conflict,
      .arguments = {},
  };
}

[[nodiscard]] Error history_empty(EntityId document_id) {
  return Error{
      .code = ErrorCode::history_empty,
      .severity = Severity::error,
      .entity_id = document_id,
      .message = MessageKey::history_empty,
      .arguments = {},
  };
}

// Generic upsert into a builder collection: visit the entity variant and call
// the matching add_* on the document builder. Returns true if the entity type
// is a document entity (was routed), false if it is a presentation entity (the
// caller routes it to the presentation builder instead).
[[nodiscard]] bool upsert_document_entity(WellLogDocumentBuilder &builder,
                                          const PatchableEntity &entity) {
  return std::visit(
      [&builder](const auto &e) -> bool {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, Interval>) {
          builder.add_interval(e);
          return true;
        } else if constexpr (std::is_same_v<T, Marker>) {
          builder.add_marker(e);
          return true;
        } else if constexpr (std::is_same_v<T, SymbolOccurrence>) {
          builder.add_symbol(e);
          return true;
        } else if constexpr (std::is_same_v<T, TextAnnotation>) {
          builder.add_annotation(e);
          return true;
        } else if constexpr (std::is_same_v<T, QcMask>) {
          builder.add_qc_mask(e);
          return true;
        } else if constexpr (std::is_same_v<T, Curve>) {
          // Derived curves only — raw source curves are never patch-upserted.
          if (e.derived.has_value()) {
            builder.add_curve(e);
            return true;
          }
          return false;
        }
        return false;
      },
      entity);
}

[[nodiscard]] bool upsert_presentation_entity(ScenePresentationBuilder &builder,
                                              const PatchableEntity &entity) {
  return std::visit(
      [&builder](const auto &e) -> bool {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, TrackSpec>) {
          builder.add_track(e);
          return true;
        } else if constexpr (std::is_same_v<T, TrackScaleSpec>) {
          builder.add_scale(e);
          return true;
        } else if constexpr (std::is_same_v<T, CurveLayerSpec>) {
          builder.add_curve_layer(e);
          return true;
        }
        return false;
      },
      entity);
}

Result<CommandReceipt>
WellLogSession::execute(const AppendBatchCommand &command) {
  try {
    // Coalescing disabled (the default): commit immediately, one revision per
    // AppendBatchCommand — backward-compatible with #198/#199/#200.
    const auto rate_hz = impl_->budgets.append_refresh_rate_hz;
    if (rate_hz == 0) {
      return commit_append_batch(command);
    }
    // Coalescing enabled (#201, ADR 0031): the document must exist.
    if (!impl_->documents.contains(command.document_id)) {
      return append_document_missing(command.document_id);
    }
    // Stage this command's tail-blocks into the per-document coalescer.
    auto &coalescer = impl_->append_coalescers[command.document_id];
    for (const auto &block : command.blocks) {
      coalescer.staged_blocks.push_back(block);
    }
    // Flush when the refresh interval has elapsed since the last visible
    // revision (or this is the first staged batch). Otherwise return a
    // "coalesced, no new revision yet" receipt so the host sees its append was
    // accepted but not yet made visible.
    const auto interval = coalesce_interval(rate_hz);
    const auto now = std::chrono::steady_clock::now();
    const auto due =
        !coalescer.has_last_flush || now - coalescer.last_flush >= interval;
    if (due) {
      // Flush the staged batch as a single visible revision. Propagate the
      // Result directly — a validation failure (Error) is surfaced to the host,
      // never dressed up as success. Flush always has staged blocks here (the
      // command's blocks were staged just above), so the "nothing staged"
      // success-at-current-revision path does not apply.
      return flush_append_coalesce(command.document_id);
    }
    return CommandReceipt{
        .state_version = impl_->state_version,
        .document_id = command.document_id,
        .document_revision =
            impl_->documents.at(command.document_id)->revision(),
        .asynchronous_preparation_started = false,
        .diagnostic_id = std::nullopt,
    };
  } catch (const std::bad_alloc &) {
    return Error{
        .code = ErrorCode::resource_exhausted,
        .severity = Severity::error,
        .entity_id = command.document_id,
        .message = MessageKey::resource_exhausted,
        .arguments = {},
    };
  } catch (...) {
    return Error{
        .code = ErrorCode::internal_error,
        .severity = Severity::error,
        .entity_id = command.document_id,
        .message = MessageKey::internal_error,
        .arguments = {},
    };
  }
}

// NOTE: this method must not insert/erase entries of append_coalescers — it is
// called from within poll_async()'s iteration over that map (which only
// mutates each entry's staged_blocks/last_flush, keeping unordered_map
// iterators valid).
Result<CommandReceipt>
WellLogSession::flush_append_coalesce(EntityId document_id) noexcept {
  try {
    const auto it = impl_->append_coalescers.find(document_id);
    if (it == impl_->append_coalescers.end() ||
        it->second.staged_blocks.empty()) {
      // Nothing staged: succeed at the current revision without a state change
      // (the host asked to flush; there was nothing to flush).
      const auto current_revision =
          impl_->documents.contains(document_id)
              ? impl_->documents.at(document_id)->revision()
              : DocumentRevision{};
      return CommandReceipt{
          .state_version = impl_->state_version,
          .document_id = document_id,
          .document_revision = current_revision,
          .asynchronous_preparation_started = false,
          .diagnostic_id = std::nullopt,
      };
    }
    // Move the staged blocks out before committing so a validation failure
    // (commit_append_batch returns an error) still clears the buffer — the
    // staged batch is rejected wholesale (atomic), never half-flushed.
    auto blocks = std::move(it->second.staged_blocks);
    it->second.staged_blocks.clear();
    const auto current_revision =
        impl_->documents.contains(document_id)
            ? impl_->documents.at(document_id)->revision()
            : DocumentRevision{};
    auto result = commit_append_batch(AppendBatchCommand{
        .document_id = document_id,
        .target_revision = DocumentRevision{current_revision.value + 1},
        .blocks = std::move(blocks),
    });
    if (!result.has_value()) {
      // Validation failed: the staged batch is dropped (atomic) AND the Error
      // is propagated so a host flushing on unload can detect the rejected
      // batch rather than losing it silently (architecture.md §2 Result/Error;
      // qsp §7 "dropped/coalesced append" must be observable).
      return result.error();
    }
    it->second.last_flush = std::chrono::steady_clock::now();
    it->second.has_last_flush = true;
    return result.value();
  } catch (const std::bad_alloc &) {
    return Error{
        .code = ErrorCode::resource_exhausted,
        .severity = Severity::error,
        .entity_id = document_id,
        .message = MessageKey::resource_exhausted,
        .arguments = {},
    };
  } catch (...) {
    return Error{
        .code = ErrorCode::internal_error,
        .severity = Severity::error,
        .entity_id = document_id,
        .message = MessageKey::internal_error,
        .arguments = {},
    };
  }
}

Result<CommandReceipt>
WellLogSession::execute(const ApplyPatchCommand &command) {
  try {
    const auto document_entry = impl_->documents.find(command.document_id);
    if (document_entry == impl_->documents.end()) {
      return append_document_missing(command.document_id);
    }
    const auto &current_doc = *document_entry->second;
    const auto current_revision = current_doc.revision();
    // Patch-conflict gate (AC #6): every patch, including an empty no-op, must
    // acknowledge the exact document revision it was built against.
    if (command.patch.base_revision.value != current_revision.value) {
      return patch_base_conflict(command.document_id);
    }
    if (command.patch.edits.empty()) {
      // An empty patch is a no-op at the current revision.
      return CommandReceipt{
          .state_version = impl_->state_version,
          .document_id = command.document_id,
          .document_revision = current_revision,
          .asynchronous_preparation_started = false,
          .diagnostic_id = std::nullopt,
      };
    }

    // Resolve the presentation (optional — a patch may target only document
    // entities on a document that has no presentation yet).
    const auto pres_entry = impl_->presentations.find(command.document_id);
    const bool has_presentation = pres_entry != impl_->presentations.end();

    // --- Validate the whole patch before touching any state (atomicity). ---
    // Track the set of ids this patch references so remove-targets-exist and
    // no-duplicate-id checks see the patch's cumulative effect, and so the
    // rebuild can skip/replace entities by id.
    std::unordered_set<EntityId, EntityIdHash> patched_ids;
    for (const auto &edit : command.patch.edits) {
      if (std::holds_alternative<UpsertEntity>(edit)) {
        const auto &upsert = std::get<UpsertEntity>(edit);
        const auto id = patch_entity_id(upsert.entity);
        if (id.is_nil()) {
          return Error{
              .code = ErrorCode::invalid_document,
              .severity = Severity::error,
              .entity_id = id,
              .message = MessageKey::document_structure_invalid,
              .arguments = {},
          };
        }
        if (!patched_ids.insert(id).second) {
          return Error{
              .code = ErrorCode::duplicate_entity_id,
              .severity = Severity::error,
              .entity_id = id,
              .message = MessageKey::entity_identity_duplicated,
              .arguments = {},
          };
        }
      } else {
        const auto id = std::get<RemoveEntity>(edit).id;
        if (id.is_nil()) {
          return Error{
              .code = ErrorCode::invalid_document,
              .severity = Severity::error,
              .entity_id = id,
              .message = MessageKey::document_structure_invalid,
              .arguments = {},
          };
        }
        if (!patched_ids.insert(id).second) {
          return Error{
              .code = ErrorCode::duplicate_entity_id,
              .severity = Severity::error,
              .entity_id = id,
              .message = MessageKey::entity_identity_duplicated,
              .arguments = {},
          };
        }
        // Use the same central lookup as inverse generation so removal and
        // history agree on which document/presentation entities are patchable.
        const auto existing = patchable_entity_at(
            current_doc, has_presentation ? &pres_entry->second : nullptr, id);
        if (!existing.has_value()) {
          // The remove targets no existing entity. document_not_found is the
          // closest stable code; document_structure_invalid message (not the
          // presentation->document message) - the entity referenced by the
          // patch is absent from the document/presentation structure.
          return Error{
              .code = ErrorCode::document_not_found,
              .severity = Severity::error,
              .entity_id = id,
              .message = MessageKey::document_structure_invalid,
              .arguments = {},
          };
        }
      }
    }

    // --- Build the patched document (interpretation entities edited). ---
    // Copy every untouched collection verbatim; for the patchable document
    // collections (intervals/markers/symbols/annotations), copy each entity
    // unless it is removed, and append upserts.
    WellLogDocumentBuilder doc_builder(
        current_doc.id(), DocumentRevision{current_revision.value + 1});
    for (const auto &axis : current_doc.sampling_axes()) {
      doc_builder.add_sampling_axis(axis);
    }
    for (const auto &image : current_doc.image_sources()) {
      doc_builder.add_image_source(image);
    }
    for (const auto &custom : current_doc.custom_sources()) {
      doc_builder.add_custom_source(custom);
    }
    // Helper: copy an entity collection, skipping any id this patch touches —
    // both removed ids (gone) and upserted ids (replaced by the upsert's new
    // value, applied below). Copying the original then appending the upsert
    // would duplicate the id.
    const auto is_patched = [&patched_ids](EntityId id) {
      return patched_ids.contains(id);
    };
    // Curves (raw + derived) and QC masks are patchable collections (#159).
    for (const auto &curve : current_doc.curves()) {
      if (!is_patched(curve.id)) {
        doc_builder.add_curve(curve);
      }
    }
    for (const auto &mask : current_doc.qc_masks()) {
      if (!is_patched(mask.id)) {
        doc_builder.add_qc_mask(mask);
      }
    }
    for (const auto &interval : current_doc.intervals()) {
      if (!is_patched(interval.id)) {
        doc_builder.add_interval(interval);
      }
    }
    for (const auto &marker : current_doc.markers()) {
      if (!is_patched(marker.id)) {
        doc_builder.add_marker(marker);
      }
    }
    for (const auto &symbol : current_doc.symbols()) {
      if (!is_patched(symbol.id)) {
        doc_builder.add_symbol(symbol);
      }
    }
    for (const auto &annotation : current_doc.annotations()) {
      if (!is_patched(annotation.id)) {
        doc_builder.add_annotation(annotation);
      }
    }
    // Apply upserts — document entities go to the document builder,
    // presentation entities to the presentation builder (built next).
    bool presentation_upserts_present = false;
    for (const auto &edit : command.patch.edits) {
      if (std::holds_alternative<UpsertEntity>(edit)) {
        const auto &upsert = std::get<UpsertEntity>(edit);
        if (!upsert_document_entity(doc_builder, upsert.entity)) {
          presentation_upserts_present = true;
        }
      }
    }
    auto patched_doc = doc_builder.build();
    if (patched_doc.id().is_nil()) {
      return Error{
          .code = ErrorCode::resource_exhausted,
          .severity = Severity::error,
          .entity_id = command.document_id,
          .message = MessageKey::resource_exhausted,
          .arguments = {},
      };
    }

    // --- Validate QC Mask / Derived Curve invariants (#159). ---
    for (const auto &edit : command.patch.edits) {
      if (!std::holds_alternative<UpsertEntity>(edit)) {
        continue;
      }
      const auto &entity = std::get<UpsertEntity>(edit).entity;
      if (std::holds_alternative<Curve>(entity)) {
        const auto &curve = std::get<Curve>(entity);
        if (!curve.derived.has_value()) {
          return Error{
              .code = ErrorCode::invalid_document,
              .severity = Severity::error,
              .entity_id = curve.id,
              .message = MessageKey::document_structure_invalid,
              .arguments = {},
          };
        }
        const auto &prov = *curve.derived;
        bool input_found = false;
        for (const auto &candidate : patched_doc.curves()) {
          if (candidate.id == prov.input_curve_id) {
            input_found = true;
            break;
          }
        }
        if (!input_found) {
          return Error{
              .code = ErrorCode::invalid_document,
              .severity = Severity::error,
              .entity_id = curve.id,
              .message = MessageKey::document_structure_invalid,
              .arguments = {},
          };
        }
      }
      if (std::holds_alternative<QcMask>(entity)) {
        const auto &mask = std::get<QcMask>(entity);
        if (mask.states.scalar_type() != ScalarType::uint8) {
          return Error{
              .code = ErrorCode::invalid_document,
              .severity = Severity::error,
              .entity_id = mask.id,
              .message = MessageKey::document_structure_invalid,
              .arguments = {},
          };
        }
        const auto curves = patched_doc.curves();
        const auto curve = std::find_if(
            curves.begin(), curves.end(),
            [&](const Curve &c) { return c.id == mask.curve_id; });
        if (curve == curves.end() ||
            curve->values.length() != mask.states.length()) {
          return Error{
              .code = ErrorCode::invalid_document,
              .severity = Severity::error,
              .entity_id = mask.id,
              .message = MessageKey::document_structure_invalid,
              .arguments = {},
          };
        }
      }
    }

    // --- Build the patched presentation (layout entities edited), if any. ---
    std::optional<ScenePresentation> patched_presentation;
    if (has_presentation) {
      const auto &pres = pres_entry->second;
      ScenePresentationBuilder pres_builder(
          pres.document_id(), pres.reference_depth_range(),
          pres.physical_height(), pres.font_asset_fingerprint());
      pres_builder.set_presentation_version(pres.presentation_version());
      // Depth-transform version round-trips via the descriptor's version.
      pres_builder.set_depth_transform_version(pres.depth_transform().version);
      for (const auto &pattern : pres.patterns()) {
        pres_builder.add_pattern(pattern);
      }
      for (const auto &layer : pres.interval_layers()) {
        pres_builder.add_interval_layer(layer);
      }
      for (const auto &layer : pres.crossover_fill_layers()) {
        pres_builder.add_crossover_fill_layer(layer);
      }
      for (const auto &layer : pres.image_layers()) {
        pres_builder.add_image_layer(layer);
      }
      for (const auto &layer : pres.marker_layers()) {
        pres_builder.add_marker_layer(layer);
      }
      for (const auto &layer : pres.symbol_layers()) {
        pres_builder.add_symbol_layer(layer);
      }
      for (const auto &layer : pres.text_layers()) {
        pres_builder.add_text_layer(layer);
      }
      for (const auto &layer : pres.custom_layers()) {
        pres_builder.add_custom_layer(layer);
      }
      // Copy the patchable layout collections, skipping removed ids.
      for (const auto &track : pres.tracks()) {
        if (!is_patched(track.id)) {
          pres_builder.add_track(track);
        }
      }
      for (const auto &scale : pres.scales()) {
        if (!is_patched(scale.id)) {
          pres_builder.add_scale(scale);
        }
      }
      for (const auto &layer : pres.curve_layers()) {
        if (!is_patched(layer.id)) {
          pres_builder.add_curve_layer(layer);
        }
      }
      // Apply presentation upserts.
      for (const auto &edit : command.patch.edits) {
        if (std::holds_alternative<UpsertEntity>(edit)) {
          (void)upsert_presentation_entity(pres_builder,
                                           std::get<UpsertEntity>(edit).entity);
        }
      }
      patched_presentation = pres_builder.build();
      // SetPresentationCommand defers validation while a LOD task is pending.
      // A patch must instead preflight the complete patched graph before
      // SetDocumentCommand changes a revision or emits any event. The scene
      // preflight performs no text shaping or curve-geometry preparation, so a
      // presentation-only patch retains the minimal asynchronous work closure.
      if (const auto validation = detail::ScenePreparer::preflight(
              patched_doc, *patched_presentation);
          validation.has_value()) {
        return *validation;
      }
    } else if (presentation_upserts_present) {
      // A patch upserts a presentation entity but the document has no
      // presentation to edit — a presentation entity needs a presentation.
      return Error{
          .code = ErrorCode::invalid_presentation,
          .severity = Severity::error,
          .entity_id = command.document_id,
          .message = MessageKey::presentation_document_missing,
          .arguments = {},
      };
    }

    // Capture the exact semantic state and the declarative inverse only after
    // every patch validation has succeeded. The snapshots make revision
    // restoration exact; inverse_edits records how each validated patch edit
    // reverses.
    const auto inverse_edits = inverse_edits_for_patch(
        command.patch, current_doc,
        has_presentation ? &pres_entry->second : nullptr);
    if (!inverse_edits.has_value()) {
      return Error{
          .code = ErrorCode::internal_error,
          .severity = Severity::error,
          .entity_id = command.document_id,
          .message = MessageKey::internal_error,
          .arguments = {},
      };
    }
    auto history_entry = Impl::HistoryEntry{
        .before = impl_->semantic_state(command.document_id),
        .after = {},
        .inverse_edits = std::move(*inverse_edits),
    };
    auto [history, inserted] =
        impl_->histories.try_emplace(command.document_id);
    static_cast<void>(inserted);
    history->second.undo.reserve(history->second.undo.size() + 1);
    impl_->events.reserve(impl_->events.size() + 1);

    // --- Capture viewport state to restore after the commit (like append). ---
    struct CapturedViewport {
      DepthViewport viewport;
      std::uint32_t pixel_height;
    };
    std::optional<CapturedViewport> captured_viewport;
    if (const auto vp = impl_->viewports.find(command.document_id);
        vp != impl_->viewports.end()) {
      const auto ph = impl_->viewport_pixel_heights.find(command.document_id);
      if (ph != impl_->viewport_pixel_heights.end() && ph->second != 0) {
        captured_viewport = CapturedViewport{.viewport = vp->second,
                                             .pixel_height = ph->second};
      }
    }
    const auto captured_default =
        impl_->viewport_defaults.find(command.document_id) !=
                impl_->viewport_defaults.end()
            ? std::optional<DepthViewport>{impl_->viewport_defaults.at(
                  command.document_id)}
            : std::nullopt;

    // --- Commit the patched document (reuses validation/LOD/selection remap).
    // ---
    // A patch edits interpretation/layout entities and leaves raw curves and
    // images byte-identical (ADR 0025). Retag a ready cache at the new revision
    // instead of rebuilding its LOD; SetPresentationCommand only needs a new
    // frame from that cache (architecture.md §7 minimal-closure rule).
    if (const auto prep = impl_->preparations.find(command.document_id);
        prep != impl_->preparations.end() &&
        prep->second.state == PreparationState::ready &&
        prep->second.revision == current_revision &&
        prep->second.pyramids != nullptr) {
      impl_->pending_lod_reuse.insert_or_assign(
          command.document_id,
          Impl::PendingLodReuse{
              .kind = Impl::LodReuseKind::unchanged_document,
              .pyramids = prep->second.pyramids,
              .image_pyramids = prep->second.image_pyramids,
              .derived_bytes = prep->second.derived_bytes,
              .maximum_derived_bytes = prep->second.maximum_derived_bytes,
              .source_document = document_entry->second,
              .source_revision = current_revision,
          });
    }
    const auto commit = execute(SetDocumentCommand{std::move(patched_doc)});
    if (!commit.has_value()) {
      // SetDocumentCommand can reject before consuming the cache hint (for
      // example, an invalid patched document). Do not let that ready cache be
      // reused by a later, unrelated replacement of this document id.
      impl_->pending_lod_reuse.erase(command.document_id);
      return commit;
    }

    // Restore the patched presentation through its normal command path. The
    // document commit cleared the presentation and any prepared frame; merely
    // repopulating Impl::presentations would leave a pending LOD task with no
    // way to schedule its replacement PreparedScene. SetPresentationCommand
    // reconnects the edited layout to the current revision's preparation.
    // The viewport is restored below because a patch does not move the depth
    // window — unlike an append, the edited depths are interpretation entities,
    // not new samples.
    std::optional<CommandReceipt> presentation_commit;
    if (patched_presentation.has_value()) {
      const auto presentation_result =
          execute(SetPresentationCommand{*patched_presentation});
      if (!presentation_result.has_value()) {
        return presentation_result;
      }
      presentation_commit = presentation_result.value();
    }
    if (captured_default.has_value()) {
      impl_->viewport_defaults.insert_or_assign(command.document_id,
                                                *captured_default);
    }
    if (captured_viewport.has_value()) {
      impl_->viewports.insert_or_assign(command.document_id,
                                        captured_viewport->viewport);
      impl_->viewport_pixel_heights.insert_or_assign(
          command.document_id, captured_viewport->pixel_height);
      if (impl_->state_version != std::numeric_limits<std::uint64_t>::max()) {
        ++impl_->state_version;
        impl_->events.reserve(impl_->events.size() + 1);
        const auto event = ViewEvent{
            .kind = ViewEventKind::viewport_changed,
            .state_version = impl_->state_version,
            .document_id = command.document_id,
            .document_revision = commit.value().document_revision,
        };
        impl_->events.push_back(event);
        impl_->notify_observers(event);
      }
    }
    history_entry.after = impl_->semantic_state(command.document_id);
    impl_->record_new_history(command.document_id, std::move(history_entry),
                              commit.value().document_revision);
    auto receipt = presentation_commit.has_value() ? *presentation_commit
                                                   : commit.value();
    receipt.state_version = impl_->state_version;
    return receipt;
  } catch (const std::bad_alloc &) {
    return Error{
        .code = ErrorCode::resource_exhausted,
        .severity = Severity::error,
        .entity_id = command.document_id,
        .message = MessageKey::resource_exhausted,
        .arguments = {},
    };
  } catch (...) {
    return Error{
        .code = ErrorCode::internal_error,
        .severity = Severity::error,
        .entity_id = command.document_id,
        .message = MessageKey::internal_error,
        .arguments = {},
    };
  }
}

Result<CommandReceipt> WellLogSession::execute(const UndoCommand &command) {
  return execute_history(command.document_id, HistoryDirection::undo);
}

Result<CommandReceipt> WellLogSession::execute(const RedoCommand &command) {
  return execute_history(command.document_id, HistoryDirection::redo);
}

Result<CommandReceipt>
WellLogSession::execute_history(EntityId document_id,
                                HistoryDirection direction) {
  try {
    if (!impl_->documents.contains(document_id)) {
      return append_document_missing(document_id);
    }
    const auto history_it = impl_->histories.find(document_id);
    if (history_it == impl_->histories.end()) {
      return history_empty(document_id);
    }
    auto &history = history_it->second;
    auto &source =
        direction == HistoryDirection::undo ? history.undo : history.redo;
    auto &destination =
        direction == HistoryDirection::undo ? history.redo : history.undo;
    if (source.empty()) {
      return history_empty(document_id);
    }
    // Reserve the only allocations needed after the document commit before
    // making that commit. A failed reserve therefore leaves document and
    // history unchanged instead of producing an unobservable transition.
    destination.reserve(destination.size() + 1);
    impl_->events.reserve(impl_->events.size() + 1);
    const auto &target = direction == HistoryDirection::undo
                             ? source.back().before
                             : source.back().after;
    if (target.document == nullptr) {
      return Error{
          .code = ErrorCode::internal_error,
          .severity = Severity::error,
          .entity_id = document_id,
          .message = MessageKey::internal_error,
          .arguments = {},
      };
    }

    // Re-enter through SetDocumentCommand so revision-scoped prepared work is
    // cancelled/rebuilt through the normal path. Stage the captured semantic
    // state first; SetDocumentCommand restores it before publishing its events,
    // so observers see one coherent history transition.
    impl_->pending_history_restores.insert_or_assign(document_id, target);
    const auto restored = execute(SetDocumentCommand{*target.document});
    impl_->pending_history_restores.erase(document_id);
    if (!restored.has_value()) {
      return restored;
    }

    // SetDocumentCommand restores the retained presentation so document and
    // selection observers see one coherent semantic snapshot. It deliberately
    // clears the revision-scoped PreparedScene, however, so history restores
    // must also re-enter through SetPresentationCommand. That normal path
    // rebuilds (or schedules) the graphics scene that SVG and screen adapters
    // consume; merely retaining the presentation would leave every graphics
    // seam empty after undo/redo.
    auto receipt = restored.value();
    if (target.presentation.has_value()) {
      const auto presentation =
          execute(SetPresentationCommand{*target.presentation});
      if (!presentation.has_value()) {
        return presentation;
      }
      receipt = presentation.value();
    }

    auto entry = std::move(source.back());
    source.pop_back();
    destination.push_back(std::move(entry));
    impl_->publish_history_changed(document_id, receipt.document_revision);
    receipt.state_version = impl_->state_version;
    return receipt;
  } catch (const std::bad_alloc &) {
    return Error{
        .code = ErrorCode::resource_exhausted,
        .severity = Severity::error,
        .entity_id = document_id,
        .message = MessageKey::resource_exhausted,
        .arguments = {},
    };
  } catch (...) {
    return Error{
        .code = ErrorCode::internal_error,
        .severity = Severity::error,
        .entity_id = document_id,
        .message = MessageKey::internal_error,
        .arguments = {},
    };
  }
}

Result<CommandReceipt>
WellLogSession::commit_append_batch(const AppendBatchCommand &command) {
  try {
    if (command.blocks.empty()) {
      // An empty batch is a no-op: succeed at the current revision without
      // producing a new one (no state change, no event).
      const auto document = impl_->documents.find(command.document_id);
      if (document == impl_->documents.end()) {
        return append_document_missing(command.document_id);
      }
      return CommandReceipt{
          .state_version = impl_->state_version,
          .document_id = command.document_id,
          .document_revision = document->second->revision(),
          .asynchronous_preparation_started = false,
          .diagnostic_id = std::nullopt,
      };
    }

    const auto document_entry = impl_->documents.find(command.document_id);
    if (document_entry == impl_->documents.end()) {
      return append_document_missing(command.document_id);
    }
    const auto &current = *document_entry->second;
    const auto current_revision = current.revision();
    // Monotonic revision gate: the declared target must be strictly greater
    // than the document's current revision. SetDocumentCommand blindly
    // replaces; append refuses a stale/equal revision so a racing host cannot
    // silently clobber a newer append.
    if (command.target_revision.value <= current_revision.value) {
      return append_revision_not_monotonic(command.document_id);
    }

    // --- Validate the whole batch before touching any state (atomicity). ---
    // Stage the rebuilt curves/axes keyed by id so the commit rebuild is a
    // lookup. A failure here returns an error and leaves the session unchanged.
    struct RebuiltCurve {
      Curve curve;
    };
    struct RebuiltAxis {
      SamplingAxis axis;
    };
    std::unordered_map<EntityId, RebuiltAxis, EntityIdHash> rebuilt_axes;
    std::unordered_map<EntityId, RebuiltCurve, EntityIdHash> rebuilt_curves;

    for (const auto &block : command.blocks) {
      // Resolve the existing curve + its sampling axis on the current document.
      const Curve *existing_curve = nullptr;
      for (const auto &curve : current.curves()) {
        if (curve.id == block.curve_id) {
          existing_curve = &curve;
          break;
        }
      }
      if (existing_curve == nullptr) {
        return append_curve_missing(block.curve_id);
      }
      if (existing_curve->sampling_axis_id != block.sampling_axis_id) {
        return append_tail_mismatch(block.curve_id);
      }
      const auto *axis = find_axis(current, block.sampling_axis_id);
      if (axis == nullptr) {
        return append_axis_missing(block.sampling_axis_id);
      }

      // Tail buffers must each be valid (owner + non-empty data + stride).
      if (const auto r = required_bytes(block.tail_coordinates); !r) {
        return r.error();
      }
      if (const auto r = required_bytes(block.tail_values); !r) {
        return r.error();
      }
      // Tail coordinate/value lengths must match each other.
      if (block.tail_coordinates.length() != block.tail_values.length()) {
        return append_tail_mismatch(block.curve_id);
      }
      // Tail coordinate scalar type must match the existing axis (a mixed-type
      // composite is rejected at CompositeBufferView build; catch it here with
      // a structural error before composing).
      if (block.tail_coordinates.scalar_type() !=
          axis->coordinates.scalar_type()) {
        return append_tail_mismatch(block.sampling_axis_id);
      }
      // Tail value scalar type must match the existing curve values.
      if (block.tail_values.scalar_type() !=
          existing_curve->values.scalar_type()) {
        return append_tail_mismatch(block.curve_id);
      }

      // Tail continuity + monotonicity in the axis direction (rejects
      // out-of-order and historical backfill). Compose against the staged
      // composite when an earlier block in this batch already rebuilt this
      // axis/curve, so a multi-block batch on one curve appends in order.
      const auto staged_axis = rebuilt_axes.find(axis->id);
      const auto &axis_coords_so_far =
          staged_axis == rebuilt_axes.end()
              ? axis->coordinates
              : staged_axis->second.axis.coordinates;
      if (!tail_continues_axis(axis_coords_so_far, block.tail_coordinates,
                               axis->direction)) {
        return append_tail_direction(axis->id);
      }

      // --- Compose the no-copy composite buffers. ---
      // Axis coordinates: existing segments + tail coordinate block.
      auto coord_segments = existing_segments(axis_coords_so_far);
      coord_segments.push_back(block.tail_coordinates);
      auto coord_composite =
          CompositeBufferView::from_segments(std::move(coord_segments));
      if (coord_composite.empty()) {
        return append_tail_mismatch(block.sampling_axis_id);
      }

      // Curve values: existing segments + tail value block.
      const auto staged_curve = rebuilt_curves.find(existing_curve->id);
      const auto &curve_values_so_far = staged_curve == rebuilt_curves.end()
                                            ? existing_curve->values
                                            : staged_curve->second.curve.values;
      auto value_segments = existing_segments(curve_values_so_far);
      value_segments.push_back(block.tail_values);
      auto value_composite =
          CompositeBufferView::from_segments(std::move(value_segments));
      if (value_composite.empty()) {
        return append_tail_mismatch(block.curve_id);
      }

      // Stage the rebuilt axis + curve (preserving all metadata; only the
      // buffers change). A second block on the same axis/curve composes against
      // the staged composite, so a multi-block batch on one curve appends in
      // order.
      SamplingAxis rebuilt_axis = *axis;
      rebuilt_axis.coordinates = CurveBuffer(coord_composite);
      rebuilt_axes[axis->id] = RebuiltAxis{.axis = std::move(rebuilt_axis)};

      Curve rebuilt_curve = *existing_curve;
      rebuilt_curve.values = CurveBuffer(value_composite);
      rebuilt_curves[existing_curve->id] =
          RebuiltCurve{.curve = std::move(rebuilt_curve)};
    }

    // --- Atomic commit: rebuild the document at the target revision. ---
    // Copy every entity from the current document into a fresh builder at the
    // new revision, substituting the rebuilt (composite-buffer) axes/curves.
    // No old data block is re-copied — the composite buffers reference them
    // in place via their SharedOwners.
    WellLogDocumentBuilder builder(current.id(), command.target_revision);
    for (const auto &axis : current.sampling_axes()) {
      const auto it = rebuilt_axes.find(axis.id);
      builder.add_sampling_axis(it == rebuilt_axes.end() ? axis
                                                         : it->second.axis);
    }
    for (const auto &curve : current.curves()) {
      const auto it = rebuilt_curves.find(curve.id);
      builder.add_curve(it == rebuilt_curves.end() ? curve : it->second.curve);
    }
    for (const auto &interval : current.intervals()) {
      builder.add_interval(interval);
    }
    for (const auto &marker : current.markers()) {
      builder.add_marker(marker);
    }
    for (const auto &symbol : current.symbols()) {
      builder.add_symbol(symbol);
    }
    for (const auto &image : current.image_sources()) {
      builder.add_image_source(image);
    }
    for (const auto &annotation : current.annotations()) {
      builder.add_annotation(annotation);
    }
    for (const auto &custom : current.custom_sources()) {
      builder.add_custom_source(custom);
    }

    // Delegate to the existing SetDocumentCommand commit path: it re-validates
    // the rebuilt document (catching any inconsistency), rebuilds the LOD,
    // remaps/invalidates the selection, clears stale viewports/scenes, and
    // publishes the documents_changed event at the new revision.
    auto appended = builder.build();
    if (appended.id().is_nil()) {
      // Builder allocation failure (allocation_failed flag is internal to the
      // builder; build() returns a default document on failure).
      return Error{
          .code = ErrorCode::resource_exhausted,
          .severity = Severity::error,
          .entity_id = command.document_id,
          .message = MessageKey::resource_exhausted,
          .arguments = {},
      };
    }
    // Appends have no patch representation for their immutable curve-buffer
    // segments, so their before/after semantic snapshots are the undo record.
    // Reserve every throwing history/event allocation before the document
    // commit so a successful visible append is never left unrecorded.
    auto history_entry = Impl::HistoryEntry{
        .before = impl_->semantic_state(command.document_id),
        .after = {},
        .inverse_edits = {},
    };
    auto [history, inserted] =
        impl_->histories.try_emplace(command.document_id);
    static_cast<void>(inserted);
    history->second.undo.reserve(history->second.undo.size() + 1);
    impl_->events.reserve(impl_->events.size() + 1);
    // Stage the document's previously-built per-curve pyramids for incremental
    // LOD reuse (#199): when the previous preparation is ready at the current
    // revision, the SetDocumentCommand LOD worker will extend_tail each curve's
    // pyramid instead of full-rebuilding. Unchanged curves reuse the previous
    // pyramid directly (zero work); appended curves extend it. A missing/stale
    // preparation (none built, or a revision mismatch) leaves the reuse map
    // empty and the worker full-builds — always correct.
    if (const auto prep = impl_->preparations.find(command.document_id);
        prep != impl_->preparations.end() &&
        prep->second.state == PreparationState::ready &&
        prep->second.revision == current_revision &&
        prep->second.pyramids != nullptr) {
      impl_->pending_lod_reuse.insert_or_assign(
          command.document_id,
          Impl::PendingLodReuse{
              .kind = Impl::LodReuseKind::append_tail,
              .pyramids = prep->second.pyramids,
              .image_pyramids = {},
              .derived_bytes = 0,
              .maximum_derived_bytes = 0,
              .source_document = document_entry->second,
              .source_revision = current_revision,
          });
    }

    // Capture the viewport interaction state to restore after the append commit
    // (#200). SetDocumentCommand clears the viewport/presentation/defaults (it
    // is a full document replacement); an append must instead preserve (Fixed)
    // or advance (Follow-Latest) the viewport. Captured BEFORE the delegate so
    // the post-commit restore can re-insert it. The presentation + pixel height
    // are restored verbatim so the LOD-completion path can rebuild the scene
    // against the chosen viewport.
    struct CapturedViewport {
      DepthViewport viewport;
      std::uint32_t pixel_height;
    };
    std::optional<CapturedViewport> captured_viewport;
    if (const auto vp = impl_->viewports.find(command.document_id);
        vp != impl_->viewports.end()) {
      const auto ph = impl_->viewport_pixel_heights.find(command.document_id);
      if (ph != impl_->viewport_pixel_heights.end() && ph->second != 0) {
        captured_viewport = CapturedViewport{.viewport = vp->second,
                                             .pixel_height = ph->second};
      }
    }
    const auto captured_presentation =
        impl_->presentations.find(command.document_id) !=
                impl_->presentations.end()
            ? std::optional<ScenePresentation>{impl_->presentations.at(
                  command.document_id)}
            : std::nullopt;
    const auto captured_default =
        impl_->viewport_defaults.find(command.document_id) !=
                impl_->viewport_defaults.end()
            ? std::optional<DepthViewport>{impl_->viewport_defaults.at(
                  command.document_id)}
            : std::nullopt;
    const auto mode = impl_->append_viewport_modes.count(command.document_id)
                          ? impl_->append_viewport_modes.at(command.document_id)
                          : AppendViewportMode::fixed;

    const auto commit = execute(SetDocumentCommand{std::move(appended)});
    if (!commit.has_value()) {
      // A rejected delegated replacement must not leave an append-tail cache
      // hint for a later document with different source buffers.
      impl_->pending_lod_reuse.erase(command.document_id);
      return commit;
    }

    // Restore the presentation + viewport defaults so the LOD-completion frame
    // task can build a scene (it needs both present). These were cleared by the
    // SetDocumentCommand commit.
    if (captured_presentation.has_value()) {
      impl_->presentations.insert_or_assign(command.document_id,
                                            *captured_presentation);
    }
    if (captured_default.has_value()) {
      impl_->viewport_defaults.insert_or_assign(command.document_id,
                                                *captured_default);
    }
    // Apply the configured viewport mode. Fixed: restore the prior window
    // unchanged. Follow-Latest: advance the bottom to the new tail's last
    // reference depth, preserving the span (top = new_bottom - span). Only
    // applied when a prior viewport was captured; otherwise the viewport stays
    // cleared (matching a plain document replacement with no prior viewport).
    if (captured_viewport.has_value()) {
      auto result_viewport = captured_viewport->viewport;
      if (mode == AppendViewportMode::follow_latest) {
        // The appended tail's newest reference depth: the last coordinate of
        // the first sampling axis on the extended document (the typical
        // single-axis case; a multi-axis document uses the primary axis's
        // extent). A DepthViewport is always normalized top<bottom
        // (valid_viewport / range_for_rows), independent of axis direction, so
        // the bound the tail lands on depends on direction: an increasing
        // axis's tail is the deepest sample (→ bottom), a decreasing axis's
        // tail is the shallowest (→ top). The opposite bound shifts to
        // preserve the span.
        const auto extended_doc = impl_->documents.find(command.document_id);
        if (extended_doc != impl_->documents.end() &&
            !extended_doc->second->sampling_axes().empty()) {
          const auto &axis = extended_doc->second->sampling_axes().front();
          const auto length = axis.coordinates.length();
          if (length > 0) {
            if (const auto last = axis.coordinates.value_as_double(length - 1);
                last.has_value() && std::isfinite(*last)) {
              const auto span = result_viewport.bottom - result_viewport.top;
              if (std::isfinite(span) && span > 0.0) {
                if (axis.direction == AxisDirection::increasing) {
                  // Tail is the deepest sample → new bottom; top = bottom-span.
                  if (std::isfinite(*last - span)) {
                    result_viewport.bottom = *last;
                    result_viewport.top = *last - span;
                  }
                } else {
                  // Tail is the shallowest sample → new top; bottom = top+span.
                  if (std::isfinite(*last + span)) {
                    result_viewport.top = *last;
                    result_viewport.bottom = *last + span;
                  }
                }
              }
            }
          }
        }
      }
      impl_->viewports.insert_or_assign(command.document_id, result_viewport);
      impl_->viewport_pixel_heights.insert_or_assign(
          command.document_id, captured_viewport->pixel_height);
      // Publish a viewport_changed event so the host/view observes the
      // post-append viewport (Fixed: unchanged; Follow-Latest: advanced).
      if (impl_->state_version != std::numeric_limits<std::uint64_t>::max()) {
        ++impl_->state_version;
        const auto event = ViewEvent{
            .kind = ViewEventKind::viewport_changed,
            .state_version = impl_->state_version,
            .document_id = command.document_id,
            .document_revision = commit.value().document_revision,
        };
        impl_->events.reserve(impl_->events.size() + 1);
        impl_->events.push_back(event);
        impl_->notify_observers(event);
      }
    }
    history_entry.after = impl_->semantic_state(command.document_id);
    impl_->record_new_history(command.document_id, std::move(history_entry),
                              commit.value().document_revision);
    auto receipt = commit.value();
    receipt.state_version = impl_->state_version;
    return receipt;
  } catch (const std::bad_alloc &) {
    return Error{
        .code = ErrorCode::resource_exhausted,
        .severity = Severity::error,
        .entity_id = command.document_id,
        .message = MessageKey::resource_exhausted,
        .arguments = {},
    };
  } catch (...) {
    return Error{
        .code = ErrorCode::internal_error,
        .severity = Severity::error,
        .entity_id = command.document_id,
        .message = MessageKey::internal_error,
        .arguments = {},
    };
  }
}

void WellLogSession::poll_async() noexcept {
  try {
    // Flush any append coalescers whose refresh interval has elapsed (#201), so
    // a delayed visible revision appears even without a new AppendBatchCommand.
    // flush_append_coalesce only mutates each entry's staged_blocks/last_flush
    // (no map insert/erase), so iterating append_coalescers here is safe.
    if (impl_->budgets.append_refresh_rate_hz != 0) {
      const auto interval =
          coalesce_interval(impl_->budgets.append_refresh_rate_hz);
      const auto now = std::chrono::steady_clock::now();
      for (auto &[doc_id, coalescer] : impl_->append_coalescers) {
        if (!coalescer.staged_blocks.empty() && coalescer.has_last_flush &&
            now - coalescer.last_flush >= interval) {
          // A validation failure on the merged batch surfaces here as a Result
          // error; the staged batch is dropped atomically inside flush. The
          // error is intentionally not re-published from poll_async (the host
          // observes it via the next execute()/flush return); discard quietly.
          (void)flush_append_coalesce(doc_id);
        }
      }
    }
    std::vector<ViewEvent> notifications;
    notifications.reserve(impl_->lod_tasks.size() + impl_->frame_tasks.size());
    auto task = impl_->lod_tasks.begin();
    while (task != impl_->lod_tasks.end()) {
      auto output = LodBuildOutput{};
      {
        // try_lock (not lock_guard): the poller must never block on a worker's
        // mutex. A worker holds this lock only to publish its output, but under
        // CPU starvation it can be preempted mid-critical-section — a blocking
        // poll then stalls the caller's deadline loop forever (#241). If the
        // lock isn't free, leave the task for the next poll.
        auto lock = std::unique_lock{(*task)->state->mutex, std::try_to_lock};
        if (!lock.owns_lock() || !(*task)->state->finished) {
          ++task;
          continue;
        }
        output = std::move((*task)->state->output);
      }
      auto completed_task = std::move(*task);
      task = impl_->lod_tasks.erase(task);

      try {
        const auto preparation =
            impl_->preparations.find(completed_task->document_id);
        const auto current =
            preparation != impl_->preparations.end() &&
            preparation->second.revision == completed_task->revision &&
            preparation->second.generation == completed_task->generation;
        if (output.cancelled) {
          ++impl_->cancelled_lod_tasks;
        } else if (!current) {
          ++impl_->discarded_lod_tasks;
        } else if (output.error.has_value()) {
          preparation->second.state = PreparationState::unavailable;
          impl_->publish_async_failure(completed_task->document_id,
                                       completed_task->revision, *output.error,
                                       notifications);
        } else {
          preparation->second.state = PreparationState::ready;
          preparation->second.derived_bytes = output.derived_bytes;
          preparation->second.pyramids =
              std::make_shared<const detail::ScenePreparer::CurveLodMap>(
                  std::move(output.pyramids));
          preparation->second.image_pyramids =
              std::make_shared<const detail::ScenePreparer::ImagePyramidMap>(
                  std::move(output.image_pyramids));
          // Publish a Diagnostic for each ImageSource whose pyramid build
          // failed (non-cancelled), so the degradation is observable (qsp §7)
          // before the scene emits no layer for it.
          for (const auto &skipped : output.skipped_images) {
            impl_->publish_one_diagnostic(
                completed_task->document_id, completed_task->revision, skipped,
                1, DiagnosticCode::image_pyramid_unavailable,
                MessageKey::image_metadata_invalid, ErrorCode::invalid_image,
                notifications);
          }
          const auto document =
              impl_->documents.find(completed_task->document_id);
          const auto presentation =
              impl_->presentations.find(completed_task->document_id);
          const auto viewport =
              impl_->viewports.find(completed_task->document_id);
          const auto viewport_pixel_height =
              impl_->viewport_pixel_heights.find(completed_task->document_id);
          if (document != impl_->documents.end() &&
              presentation != impl_->presentations.end() &&
              viewport != impl_->viewports.end() &&
              viewport_pixel_height != impl_->viewport_pixel_heights.end()) {
            if (impl_->next_frame_generation ==
                std::numeric_limits<std::uint64_t>::max()) {
              preparation->second.state = PreparationState::unavailable;
              impl_->publish_async_failure(
                  completed_task->document_id, completed_task->revision,
                  Error{
                      .code = ErrorCode::internal_error,
                      .severity = Severity::error,
                      .entity_id = completed_task->document_id,
                      .message = MessageKey::internal_error,
                      .arguments = {},
                  },
                  notifications);
            } else {
              const auto frame_generation = impl_->next_frame_generation;
              auto pending_frame = make_frame_task(
                  impl_->task_executor,
                  completed_task->document_id, completed_task->revision,
                  frame_generation, document->second, presentation->second,
                  preparation->second.pyramids,
                  CurveLodQuery{
                      .viewport_top = viewport->second.top,
                      .viewport_bottom = viewport->second.bottom,
                      .pixel_height = viewport_pixel_height->second,
                      .prefetch_viewports = impl_->budgets.prefetch_viewports,
                  },
                  preparation->second.image_pyramids,
                  ImagePyramidQuery{
                      .viewport_top = viewport->second.top,
                      .viewport_bottom = viewport->second.bottom,
                      .pixel_height =
                          static_cast<double>(viewport_pixel_height->second),
                      .prefetch_viewports = impl_->budgets.prefetch_viewports,
                  },
                  impl_->text_engine, &impl_->text_engine_mutex);
              impl_->frame_tasks.reserve(impl_->frame_tasks.size() + 1);
              impl_->frame_generations.reserve(impl_->frame_generations.size() +
                                               1);
              for (auto &existing_task : impl_->frame_tasks) {
                if (existing_task->document_id == completed_task->document_id) {
                  existing_task->state->stop_source.request_stop();
                }
              }
              impl_->frame_generations.insert_or_assign(
                  completed_task->document_id, frame_generation);
              impl_->frame_tasks.push_back(std::move(pending_frame));
              ++impl_->next_frame_generation;
            }
          }
          ++impl_->completed_lod_tasks;
        }
      } catch (...) {
        const auto preparation =
            impl_->preparations.find(completed_task->document_id);
        if (preparation != impl_->preparations.end() &&
            preparation->second.revision == completed_task->revision &&
            preparation->second.generation == completed_task->generation) {
          preparation->second.state = PreparationState::unavailable;
        }
        ++impl_->discarded_lod_tasks;
        try {
          impl_->publish_async_failure(
              completed_task->document_id, completed_task->revision,
              Error{
                  .code = ErrorCode::internal_error,
                  .severity = Severity::error,
                  .entity_id = completed_task->document_id,
                  .message = MessageKey::internal_error,
                  .arguments = {},
              },
              notifications);
        } catch (...) {
        }
      }
    }

    auto frame_task = impl_->frame_tasks.begin();
    while (frame_task != impl_->frame_tasks.end()) {
      auto output = FrameBuildOutput{};
      {
        // try_lock for the same reason as the LOD loop above (#241): never
        // block the poller on a worker's publish lock.
        auto lock =
            std::unique_lock{(*frame_task)->state->mutex, std::try_to_lock};
        if (!lock.owns_lock() || !(*frame_task)->state->finished) {
          ++frame_task;
          continue;
        }
        output = std::move((*frame_task)->state->output);
      }
      auto completed_task = std::move(*frame_task);
      frame_task = impl_->frame_tasks.erase(frame_task);
      try {
        const auto generation =
            impl_->frame_generations.find(completed_task->document_id);
        const auto document =
            impl_->documents.find(completed_task->document_id);
        const auto current =
            generation != impl_->frame_generations.end() &&
            generation->second == completed_task->generation &&
            document != impl_->documents.end() &&
            document->second->revision() == completed_task->revision;
        if (output.cancelled) {
          ++impl_->cancelled_lod_tasks;
        } else if (!current) {
          ++impl_->discarded_lod_tasks;
        } else if (output.error.has_value() || output.scene == nullptr) {
          impl_->frame_generations.erase(generation);
          const auto error = output.error.value_or(Error{
              .code = ErrorCode::internal_error,
              .severity = Severity::error,
              .entity_id = completed_task->document_id,
              .message = MessageKey::internal_error,
              .arguments = {},
          });
          impl_->publish_async_failure(completed_task->document_id,
                                       completed_task->revision, error,
                                       notifications);
        } else {
          impl_->prepared_scenes.insert_or_assign(completed_task->document_id,
                                                  std::move(output.scene));
          impl_->frame_generations.erase(generation);
          ++impl_->completed_lod_tasks;
          impl_->publish_value_issues(
              completed_task->document_id, completed_task->revision,
              *impl_->prepared_scenes.at(completed_task->document_id),
              notifications);
          static_cast<void>(impl_->publish_text_issues(
              completed_task->document_id, completed_task->revision,
              *impl_->prepared_scenes.at(completed_task->document_id),
              notifications));
          if (impl_->state_version <
              std::numeric_limits<std::uint64_t>::max()) {
            ++impl_->state_version;
            const auto event = ViewEvent{
                .kind = ViewEventKind::frame_ready,
                .state_version = impl_->state_version,
                .document_id = completed_task->document_id,
                .document_revision = completed_task->revision,
            };
            impl_->events.push_back(event);
            notifications.push_back(event);
          }
        }
      } catch (...) {
        const auto generation =
            impl_->frame_generations.find(completed_task->document_id);
        if (generation != impl_->frame_generations.end() &&
            generation->second == completed_task->generation) {
          impl_->frame_generations.erase(generation);
        }
        ++impl_->discarded_lod_tasks;
        try {
          impl_->publish_async_failure(
              completed_task->document_id, completed_task->revision,
              Error{
                  .code = ErrorCode::internal_error,
                  .severity = Severity::error,
                  .entity_id = completed_task->document_id,
                  .message = MessageKey::internal_error,
                  .arguments = {},
              },
              notifications);
        } catch (...) {
        }
      }
    }
    for (const auto &event : notifications) {
      impl_->notify_observers(event);
    }
  } catch (...) {
  }
}

std::optional<PerformanceSnapshot>
WellLogSession::performance_snapshot(EntityId document_id) const noexcept {
  try {
    const auto preparation = impl_->preparations.find(document_id);
    if (preparation == impl_->preparations.end()) {
      return std::nullopt;
    }
    return PerformanceSnapshot{
        .document_revision = preparation->second.revision,
        .preparation_state = preparation->second.state,
        .cpu_derived_bytes = preparation->second.derived_bytes,
        .maximum_cpu_derived_bytes = preparation->second.maximum_derived_bytes,
        .maximum_gpu_cache_bytes = impl_->budgets.maximum_gpu_cache_bytes,
        .maximum_upload_bytes_per_frame =
            impl_->budgets.maximum_upload_bytes_per_frame,
        .completed_tasks = impl_->completed_lod_tasks,
        .cancelled_tasks = impl_->cancelled_lod_tasks,
        .discarded_tasks = impl_->discarded_lod_tasks,
        .frame_preparation_pending =
            impl_->frame_generations.contains(document_id),
    };
  } catch (...) {
    return std::nullopt;
  }
}

PerformanceBudgets WellLogSession::performance_budgets() const noexcept {
  return impl_->budgets;
}

void WellLogSession::set_performance_budgets(
    PerformanceBudgets budgets) noexcept {
  impl_->budgets = std::move(budgets);
}

std::span<const ViewEvent> WellLogSession::events() const noexcept {
  return impl_->events;
}

void WellLogSession::clear_events() noexcept { impl_->events.clear(); }

std::span<const Diagnostic> WellLogSession::diagnostics() const noexcept {
  return impl_->diagnostics;
}

std::optional<Error>
WellLogSession::diagnostic_error(std::uint64_t diagnostic_id) const noexcept {
  try {
    const auto found = impl_->diagnostic_errors.find(diagnostic_id);
    return found == impl_->diagnostic_errors.end()
               ? std::nullopt
               : std::optional<Error>{found->second};
  } catch (...) {
    return std::nullopt;
  }
}

std::shared_ptr<const WellLogDocument>
WellLogSession::document(EntityId id) const noexcept {
  const auto found = impl_->documents.find(id);
  return found == impl_->documents.end() ? nullptr : found->second;
}

std::shared_ptr<const PreparedScene>
WellLogSession::prepared_scene(EntityId document_id) const noexcept {
  const auto found = impl_->prepared_scenes.find(document_id);
  return found == impl_->prepared_scenes.end() ? nullptr : found->second;
}

Result<PreparedScene> WellLogSession::prepare_for_export(
    EntityId document_id, std::uint64_t aggregate_pixel_height) const noexcept {
  try {
    // Reject absurd export densities up front (review D-007): a host passing
    // a huge value (e.g. from a Python int >= 2^32 that survived the binding
    // as unsigned long long) would otherwise truncate into CurveLodQuery and
    // silently produce a wrong-density scene. 2^32 px is far beyond any real
    // export; required_aggregate_pixel_height returns values orders of
    // magnitude smaller.
    if (aggregate_pixel_height > std::numeric_limits<std::uint32_t>::max()) {
      return Error{
          .code = ErrorCode::invalid_viewport,
          .severity = Severity::error,
          .entity_id = document_id,
          .message = MessageKey::internal_error,
          .arguments = {},
      };
    }
    const auto density =
        static_cast<std::uint32_t>(aggregate_pixel_height);
    const auto document = impl_->documents.find(document_id);
    if (document == impl_->documents.end()) {
      return Error{
          .code = ErrorCode::document_not_found,
          .severity = Severity::error,
          .entity_id = document_id,
          .message = MessageKey::presentation_document_missing,
          .arguments = {},
      };
    }
    const auto presentation = impl_->presentations.find(document_id);
    if (presentation == impl_->presentations.end()) {
      return Error{
          .code = ErrorCode::invalid_presentation,
          .severity = Severity::error,
          .entity_id = document_id,
          .message = MessageKey::presentation_document_missing,
          .arguments = {},
      };
    }
    const auto depth_range = presentation->second.reference_depth_range();
    const auto preparation = impl_->preparations.find(document_id);
    const bool lods_ready =
        preparation != impl_->preparations.end() &&
        preparation->second.state == PreparationState::ready &&
        preparation->second.pyramids != nullptr;
    {
      const auto text_guard =
          impl_->text_engine == nullptr
              ? std::unique_lock<std::mutex>{}
              : std::unique_lock<std::mutex>{impl_->text_engine_mutex};
      if (lods_ready) {
        // Prepare at the requested export density using the document's LOD
        // pyramids so fixed-page pagination resolves the correct per-page
        // curve detail (T3 / #275). Full document depth, no prefetch.
        const CurveLodQuery query{
            .viewport_top = depth_range.top,
            .viewport_bottom = depth_range.bottom,
            .pixel_height = density,
            .prefetch_viewports = 0.0,
        };
        const ImagePyramidQuery image_query{
            .viewport_top = depth_range.top,
            .viewport_bottom = depth_range.bottom,
            .pixel_height = static_cast<double>(density),
            .prefetch_viewports = 0.0,
        };
        const auto &image_pyramids =
            preparation->second.image_pyramids
                ? *preparation->second.image_pyramids
                : detail::ScenePreparer::ImagePyramidMap{};
        return detail::ScenePreparer::prepare(
            *document->second, presentation->second,
            *preparation->second.pyramids, query, image_pyramids, image_query,
            {}, impl_->text_engine.get());
      }
      // No LOD pyramids yet: density has no effect on raw-sample emission,
      // so fall back to the no-query prepare (mirrors the interactive sync
      // path). The export is still valid; curve detail is just unconstrained.
      return detail::ScenePreparer::prepare(*document->second,
                                            presentation->second,
                                            impl_->text_engine.get());
    }
  } catch (const std::bad_alloc &) {
    return Error{.code = ErrorCode::resource_exhausted,
                 .severity = Severity::error,
                 .entity_id = std::nullopt,
                 .message = MessageKey::internal_error,
                 .arguments = {}};
  } catch (...) {
    return Error{.code = ErrorCode::internal_error,
                 .severity = Severity::error,
                 .entity_id = std::nullopt,
                 .message = MessageKey::internal_error,
                 .arguments = {}};
  }
}

std::span<const WellPlacement>
WellLogSession::well_layout() const noexcept {
  return impl_ == nullptr ? std::span<const WellPlacement>{}
                          : std::span<const WellPlacement>{impl_->well_layout};
}

std::span<const CrossWellOverlay>
WellLogSession::cross_well_overlays() const noexcept {
  return impl_ == nullptr
             ? std::span<const CrossWellOverlay>{}
             : std::span<const CrossWellOverlay>{impl_->cross_well_overlays};
}

DepthTransform
WellLogSession::depth_transform(EntityId document_id) const noexcept {
  if (impl_ == nullptr) {
    return {};
  }
  const auto found = impl_->depth_transforms.find(document_id);
  return found == impl_->depth_transforms.end() ? DepthTransform{}
                                                : found->second;
}

std::optional<DepthViewport>
WellLogSession::shared_depth_viewport() const noexcept {
  return impl_ == nullptr ? std::nullopt : impl_->shared_depth_viewport;
}

std::shared_ptr<const PreparedScene>
WellLogSession::prepared_surface_scene() const noexcept {
  try {
    if (impl_ == nullptr || impl_->well_layout.empty()) {
      return nullptr;
    }
    std::vector<WellScenePlacement> placements;
    const auto view = impl_->surface_horizontal_view;
    struct WellSpan {
      EntityId document_id{};
      double left{};
      double right{};
    };
    std::vector<WellSpan> spans;
    for (const auto &well : impl_->well_layout) {
      if (!well.visible) {
        continue;
      }
      const auto scene_it = impl_->prepared_scenes.find(well.document_id);
      if (scene_it == impl_->prepared_scenes.end() ||
          scene_it->second == nullptr) {
        continue;
      }
      const auto width =
          well.width.value > 0.0 ? well.width.value
                                 : scene_it->second->physical_width().value;
      const auto right = well.left.value + width;
      if (view.has_value()) {
        if (right <= view->first || well.left.value >= view->second) {
          continue;
        }
      }
      placements.push_back(WellScenePlacement{
          .document_id = well.document_id,
          .left = well.left,
          .scene = scene_it->second,
      });
      spans.push_back(WellSpan{.document_id = well.document_id,
                               .left = well.left.value,
                               .right = right});
    }
    if (placements.empty()) {
      return nullptr;
    }
    Millimetres height = placements.front().scene->physical_height();
    for (const auto &placement : placements) {
      if (placement.scene->physical_height().value > height.value) {
        height = placement.scene->physical_height();
      }
    }
    auto composed = compose_multi_well_scene(placements, height);
    if (!composed.has_value()) {
      return nullptr;
    }
    if (impl_->cross_well_overlays.empty()) {
      return std::make_shared<const PreparedScene>(std::move(composed.value()));
    }

    // Resolve Marker EntityIds → Display Depth → surface millimetres, then
    // decorate the composed surface with overlay polylines/bands (#161).
    const auto depth_to_top =
        [&](EntityId document_id, double reference_depth) -> std::optional<double> {
      const auto scene_it = impl_->prepared_scenes.find(document_id);
      if (scene_it == impl_->prepared_scenes.end() ||
          scene_it->second == nullptr) {
        return std::nullopt;
      }
      const auto range = scene_it->second->reference_depth_range();
      const auto transform = depth_transform(document_id);
      const auto display =
          map_reference_to_display(transform, reference_depth);
      const auto span = range.bottom - range.top;
      if (!(span > 0.0) || !std::isfinite(display)) {
        return std::nullopt;
      }
      const auto h = scene_it->second->physical_height().value;
      return (display - range.top) / span * h;
    };
    const auto well_span = [&](EntityId document_id) -> std::optional<WellSpan> {
      for (const auto &span : spans) {
        if (span.document_id == document_id) {
          return span;
        }
      }
      return std::nullopt;
    };
    const auto marker_ref = [&](EntityId document_id,
                                EntityId marker_id) -> std::optional<double> {
      const auto doc = document(document_id);
      if (doc == nullptr) {
        return std::nullopt;
      }
      return marker_reference_depth(*doc, marker_id);
    };

    std::vector<SurfaceOverlayGeometry> geometry;
    geometry.reserve(impl_->cross_well_overlays.size());
    for (const auto &overlay : impl_->cross_well_overlays) {
      const auto left_span = well_span(overlay.left_document_id);
      const auto right_span = well_span(overlay.right_document_id);
      if (!left_span.has_value() || !right_span.has_value()) {
        continue; // culled or missing well
      }
      const auto left_ref =
          marker_ref(overlay.left_document_id, overlay.left_marker_id);
      const auto right_ref =
          marker_ref(overlay.right_document_id, overlay.right_marker_id);
      if (!left_ref.has_value() || !right_ref.has_value()) {
        continue;
      }
      const auto left_top =
          depth_to_top(overlay.left_document_id, *left_ref);
      const auto right_top =
          depth_to_top(overlay.right_document_id, *right_ref);
      if (!left_top.has_value() || !right_top.has_value()) {
        continue;
      }
      SurfaceOverlayGeometry geo{
          .id = overlay.id,
          .kind = overlay.kind == CrossWellOverlay::Kind::correlation_band
                      ? SurfaceOverlayGeometry::Kind::correlation_band
                      : SurfaceOverlayGeometry::Kind::horizon_line,
          .left_top =
              PhysicalPoint{Millimetres{left_span->right}, Millimetres{*left_top}},
          .right_top =
              PhysicalPoint{Millimetres{right_span->left}, Millimetres{*right_top}},
          .color = overlay.color,
          .line_width = overlay.line_width,
          .z_order = overlay.z_order,
      };
      if (geo.kind == SurfaceOverlayGeometry::Kind::correlation_band) {
        const auto left_bottom_ref = marker_ref(overlay.left_document_id,
                                                overlay.left_bottom_marker_id);
        const auto right_bottom_ref = marker_ref(
            overlay.right_document_id, overlay.right_bottom_marker_id);
        if (!left_bottom_ref.has_value() || !right_bottom_ref.has_value()) {
          continue;
        }
        const auto left_bottom =
            depth_to_top(overlay.left_document_id, *left_bottom_ref);
        const auto right_bottom =
            depth_to_top(overlay.right_document_id, *right_bottom_ref);
        if (!left_bottom.has_value() || !right_bottom.has_value()) {
          continue;
        }
        geo.left_bottom = PhysicalPoint{Millimetres{left_span->right},
                                        Millimetres{*left_bottom}};
        geo.right_bottom = PhysicalPoint{Millimetres{right_span->left},
                                         Millimetres{*right_bottom}};
      }
      geometry.push_back(geo);
    }

    if (geometry.empty()) {
      return std::make_shared<const PreparedScene>(std::move(composed.value()));
    }
    auto decorated =
        append_surface_overlay_geometry(std::move(composed.value()), geometry);
    if (!decorated.has_value()) {
      return nullptr;
    }
    return std::make_shared<const PreparedScene>(std::move(decorated.value()));
  } catch (...) {
    return nullptr;
  }
}

std::optional<CurvePick>
WellLogSession::pick_surface_curve(const CurvePickQuery &query) const noexcept {
  try {
    if (impl_ == nullptr || impl_->well_layout.empty()) {
      return std::nullopt;
    }
    std::vector<WellScenePlacement> placements;
    const auto view = impl_->surface_horizontal_view;
    for (const auto &well : impl_->well_layout) {
      if (!well.visible) {
        continue;
      }
      const auto scene_it = impl_->prepared_scenes.find(well.document_id);
      if (scene_it == impl_->prepared_scenes.end() ||
          scene_it->second == nullptr) {
        continue;
      }
      const auto width =
          well.width.value > 0.0 ? well.width.value
                                 : scene_it->second->physical_width().value;
      if (view.has_value()) {
        const auto right = well.left.value + width;
        if (right <= view->first || well.left.value >= view->second) {
          continue;
        }
      }
      placements.push_back(WellScenePlacement{
          .document_id = well.document_id,
          .left = well.left,
          .scene = scene_it->second,
      });
    }
    return pick_curve_multi_well(placements, query);
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<DepthViewport>
WellLogSession::viewport(EntityId document_id) const noexcept {
  const auto found = impl_->viewports.find(document_id);
  return found == impl_->viewports.end()
             ? std::nullopt
             : std::optional<DepthViewport>{found->second};
}

std::optional<std::uint32_t>
WellLogSession::viewport_pixel_height(EntityId document_id) const noexcept {
  const auto found = impl_->viewport_pixel_heights.find(document_id);
  return found == impl_->viewport_pixel_heights.end()
             ? std::nullopt
             : std::optional<std::uint32_t>{found->second};
}

std::optional<CrosshairState>
WellLogSession::crosshair(EntityId document_id) const noexcept {
  const auto found = impl_->crosshairs.find(document_id);
  return found == impl_->crosshairs.end()
             ? std::nullopt
             : std::optional<CrosshairState>{found->second};
}

std::optional<SelectionState>
WellLogSession::selection(EntityId document_id) const noexcept {
  const auto found = impl_->selections.find(document_id);
  return found == impl_->selections.end()
             ? std::nullopt
             : std::optional<SelectionState>{found->second};
}

bool WellLogSession::can_undo(EntityId document_id) const noexcept {
  try {
    const auto history = impl_->histories.find(document_id);
    return history != impl_->histories.end() && !history->second.undo.empty();
  } catch (...) {
    return false;
  }
}

bool WellLogSession::can_redo(EntityId document_id) const noexcept {
  try {
    const auto history = impl_->histories.find(document_id);
    return history != impl_->histories.end() && !history->second.redo.empty();
  } catch (...) {
    return false;
  }
}

AppendViewportMode
WellLogSession::append_viewport_mode(EntityId document_id) const noexcept {
  const auto found = impl_->append_viewport_modes.find(document_id);
  return found == impl_->append_viewport_modes.end() ? AppendViewportMode::fixed
                                                     : found->second;
}

void WellLogSession::set_append_viewport_mode(
    EntityId document_id, AppendViewportMode mode) noexcept {
  impl_->append_viewport_modes.insert_or_assign(document_id, mode);
}

ViewEventObserverId
WellLogSession::subscribe_view_events(ViewEventObserver observer) noexcept {
  if (!observer) {
    return 0;
  }
  try {
    if (impl_->next_observer_id == 0 ||
        impl_->next_observer_id ==
            std::numeric_limits<ViewEventObserverId>::max()) {
      return 0;
    }
    const auto observer_id = impl_->next_observer_id++;
    impl_->observers.emplace(observer_id, std::move(observer));
    return observer_id;
  } catch (...) {
    return 0;
  }
}

void WellLogSession::unsubscribe_view_events(
    ViewEventObserverId observer_id) noexcept {
  impl_->observers.erase(observer_id);
}

} // namespace welllog
