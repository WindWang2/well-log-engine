#include <welllog/scene/curve_lod.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace welllog {
namespace {

[[nodiscard]] Error lod_error(ErrorCode code, MessageKey message) {
  return Error{
      .code = code,
      .severity = Severity::error,
      .entity_id = std::nullopt,
      .message = message,
      .arguments = {},
  };
}

[[nodiscard]] std::optional<Error> validate_buffer(const BufferView &buffer) {
  if (!buffer.has_owner()) {
    return lod_error(ErrorCode::missing_owner,
                     MessageKey::buffer_owner_required);
  }
  if (buffer.length() == 0 || buffer.data() == nullptr) {
    return lod_error(ErrorCode::invalid_buffer,
                     MessageKey::buffer_data_required);
  }
  const auto element_size = scalar_size_bytes(buffer.scalar_type());
  if (buffer.stride_bytes() < element_size) {
    return lod_error(ErrorCode::invalid_buffer,
                     MessageKey::buffer_stride_too_small);
  }
  const auto steps = buffer.length() - 1;
  if (steps > (std::numeric_limits<std::uint64_t>::max() - element_size) /
                  buffer.stride_bytes()) {
    return lod_error(ErrorCode::arithmetic_overflow,
                     MessageKey::buffer_extent_overflow);
  }
  if (steps * buffer.stride_bytes() + element_size > buffer.byte_capacity()) {
    return lod_error(ErrorCode::invalid_buffer,
                     MessageKey::buffer_extent_exceeds_capacity);
  }
  return std::nullopt;
}

// Validates a curve's value buffer whether it is a single block or a composite
// of N segments (#197). The single-block path reuses validate_buffer above; the
// composite path validates each segment (the same per-block invariants apply —
// each segment must carry an owner, non-empty data, valid stride/capacity).
[[nodiscard]] std::optional<Error>
validate_curve_buffer(const CurveBuffer &buffer) {
  if (buffer.is_composite()) {
    for (const auto &segment : buffer.segments()) {
      if (const auto error = validate_buffer(segment)) {
        return error;
      }
    }
    return std::nullopt;
  }
  return validate_buffer(buffer.as_single());
}

[[nodiscard]] std::optional<Error>
validate_lod_inputs(const SamplingAxis &axis, const Curve &curve,
                    std::stop_token stop_token) {
  if (const auto error = validate_curve_buffer(axis.coordinates)) {
    return error;
  }
  if (const auto error = validate_curve_buffer(curve.values)) {
    return error;
  }
  if (!curve.nulls.empty() &&
      (!curve.nulls.has_owner() || curve.nulls.data() == nullptr ||
       curve.nulls.bit_length() < curve.values.length() ||
       curve.nulls.bit_length() >
           std::numeric_limits<std::uint64_t>::max() - 7 ||
       (curve.nulls.bit_length() + 7) / 8 > curve.nulls.byte_capacity())) {
    return lod_error(ErrorCode::invalid_buffer,
                     MessageKey::null_bitmap_extent_exceeds_capacity);
  }
  const auto length = axis.coordinates.length();
  auto previous = axis.coordinates.value_as_double(0);
  if (!previous.has_value() || !std::isfinite(*previous)) {
    return lod_error(ErrorCode::invalid_sampling_axis,
                     MessageKey::sampling_axis_direction_invalid);
  }
  for (std::uint64_t index = 1; index < length; ++index) {
    if ((index & std::uint64_t{4095}) == 0 && stop_token.stop_requested()) {
      return lod_error(ErrorCode::operation_cancelled,
                       MessageKey::operation_cancelled);
    }
    const auto current = axis.coordinates.value_as_double(index);
    if (!current.has_value() || !std::isfinite(*current) ||
        (axis.direction == AxisDirection::increasing ? *current < *previous
                                                     : *current > *previous)) {
      return lod_error(ErrorCode::invalid_sampling_axis,
                       MessageKey::sampling_axis_direction_invalid);
    }
    previous = current;
  }
  return std::nullopt;
}

[[nodiscard]] bool valid_sample(const SamplingAxis &axis, const Curve &curve,
                                std::uint64_t index) noexcept {
  const auto depth = axis.coordinates.value_as_double(index);
  const auto value = curve.values.value_as_double(index);
  return (curve.nulls.empty() || !curve.nulls.is_null(index)) &&
         depth.has_value() && value.has_value() && std::isfinite(*depth) &&
         std::isfinite(*value);
}

struct Summary {
  std::uint32_t source_begin{};
  std::uint32_t source_end{};
  std::array<std::uint32_t, 4> indices{};
  std::uint8_t count{};
};

struct Level {
  std::uint64_t bucket_samples{};
  std::vector<Summary> summaries;
};

struct SourceRun {
  std::uint32_t begin{};
  std::uint32_t end{};
  std::vector<Level> levels;
};

struct SourceRange {
  std::uint64_t begin{};
  std::uint64_t end{};
};

[[nodiscard]] Summary summarize(const Curve &curve, std::uint64_t begin,
                                std::uint64_t end) {
  auto minimum_index = begin;
  auto maximum_index = begin;
  auto minimum = curve.values.value_as_double(begin).value();
  auto maximum = minimum;
  for (auto index = begin + 1; index < end; ++index) {
    const auto value = curve.values.value_as_double(index).value();
    if (value < minimum) {
      minimum = value;
      minimum_index = index;
    }
    if (value > maximum) {
      maximum = value;
      maximum_index = index;
    }
  }
  std::array<std::uint64_t, 4> candidates{
      begin,
      minimum_index,
      maximum_index,
      end - 1,
  };
  std::sort(candidates.begin(), candidates.end());

  Summary result{
      .source_begin = static_cast<std::uint32_t>(begin),
      .source_end = static_cast<std::uint32_t>(end),
      .indices = {},
      .count = 0,
  };
  for (const auto candidate : candidates) {
    if (result.count == 0 ||
        result.indices[static_cast<std::size_t>(result.count - 1)] !=
            candidate) {
      result.indices[static_cast<std::size_t>(result.count)] =
          static_cast<std::uint32_t>(candidate);
      ++result.count;
    }
  }
  return result;
}

[[nodiscard]] Summary summarize_children(const Curve &curve,
                                         std::span<const Summary> children) {
  std::array<std::uint32_t, 16> candidates{};
  std::size_t candidate_count{};
  for (const auto &child :
       children.first(std::min(children.size(), std::size_t{4}))) {
    const auto retained =
        std::min<std::size_t>(child.count, child.indices.size());
    for (std::size_t offset = 0; offset < retained; ++offset) {
      if (candidate_count == candidates.size()) {
        break;
      }
      candidates[candidate_count] = child.indices[offset];
      ++candidate_count;
    }
  }
  for (std::size_t index = 1; index < candidate_count; ++index) {
    const auto value = candidates[index];
    auto insertion = index;
    while (insertion > 0 && candidates[insertion - 1] > value) {
      candidates[insertion] = candidates[insertion - 1];
      --insertion;
    }
    candidates[insertion] = value;
  }
  auto unique_count = std::size_t{};
  for (std::size_t index = 0; index < candidate_count; ++index) {
    if (unique_count == 0 ||
        candidates[unique_count - 1] != candidates[index]) {
      candidates[unique_count] = candidates[index];
      ++unique_count;
    }
  }
  candidate_count = unique_count;

  auto minimum_index = candidates[0];
  auto maximum_index = candidates[0];
  auto minimum = curve.values.value_as_double(minimum_index).value();
  auto maximum = minimum;
  for (std::size_t offset = 1; offset < candidate_count; ++offset) {
    const auto index = candidates[offset];
    const auto value = curve.values.value_as_double(index).value();
    if (value < minimum) {
      minimum = value;
      minimum_index = index;
    }
    if (value > maximum) {
      maximum = value;
      maximum_index = index;
    }
  }
  std::array<std::uint32_t, 4> selected{
      candidates[0],
      minimum_index,
      maximum_index,
      candidates[candidate_count - 1],
  };
  std::sort(selected.begin(), selected.end());
  Summary result{
      .source_begin = children.front().source_begin,
      .source_end = children.back().source_end,
      .indices = {},
      .count = 0,
  };
  for (const auto index : selected) {
    if (result.count == 0 ||
        result.indices[static_cast<std::size_t>(result.count - 1)] != index) {
      result.indices[static_cast<std::size_t>(result.count)] = index;
      ++result.count;
    }
  }
  return result;
}

[[nodiscard]] SourceRange source_range(const SamplingAxis &axis, double top,
                                       double bottom) {
  const auto length = axis.coordinates.length();
  const auto first_matching = [&](auto before_range) {
    std::uint64_t low{};
    auto high = length;
    while (low < high) {
      const auto middle = low + (high - low) / 2;
      const auto depth = axis.coordinates.value_as_double(middle).value();
      if (before_range(depth)) {
        low = middle + 1;
      } else {
        high = middle;
      }
    }
    return low;
  };
  if (axis.direction == AxisDirection::increasing) {
    return SourceRange{
        .begin = first_matching([&](double depth) { return depth < top; }),
        .end = first_matching([&](double depth) { return depth <= bottom; }),
    };
  }
  return SourceRange{
      .begin = first_matching([&](double depth) { return depth > bottom; }),
      .end = first_matching([&](double depth) { return depth >= top; }),
  };
}

[[nodiscard]] std::uint64_t default_budget(const SamplingAxis &axis,
                                           const Curve &curve) noexcept {
  const auto axis_bytes = axis.coordinates.length() *
                          scalar_size_bytes(axis.coordinates.scalar_type());
  const auto curve_bytes =
      curve.values.length() * scalar_size_bytes(curve.values.scalar_type());
  return axis_bytes / 4 + curve_bytes / 4;
}

// Derives the hierarchical levels for one SourceRun `[run_begin, run_end)` over
// `curve`, appending levels until either the run is exhausted or the budget is
// reached. Returns the bytes consumed (sizeof(Level) + summaries for each level
// added); sets `budget_limited` when the budget cut the run short before its
// natural top level. This is the shared per-run build step used by both the
// full `CurveLodPyramid::build` and the incremental `extend_tail` (#199), so
// the two produce byte-identical levels for any run they both derive — the
// foundation of the append parity guarantee (a tail-extended pyramid equals a
// full rebuild over the extended curve).
struct RunBuildResult {
  std::vector<Level> levels;
  std::uint64_t derived_bytes{};
  std::uint32_t level_count{};
  bool budget_limited{};
};
[[nodiscard]] RunBuildResult
build_run_levels(const Curve &curve, std::uint32_t run_begin,
                 std::uint32_t run_end, std::uint64_t base_bucket_samples,
                 std::uint64_t maximum_derived_bytes,
                 std::uint64_t derived_bytes_so_far,
                 std::stop_token stop_token) {
  RunBuildResult result;
  auto bucket_samples = base_bucket_samples;
  while (bucket_samples <= static_cast<std::uint64_t>(run_end) - run_begin) {
    if (stop_token.stop_requested()) {
      result.budget_limited = true;
      return result;
    }
    Level level{
        .bucket_samples = bucket_samples,
        .summaries = {},
    };
    const auto bucket_count =
        (static_cast<std::uint64_t>(run_end) - run_begin + bucket_samples - 1) /
        bucket_samples;
    const auto level_bytes = sizeof(Level) + bucket_count * sizeof(Summary);
    // Compare as used+need > max so an already-over-budget
    // `derived_bytes_so_far` cannot unsigned-wrap the remaining budget to a
    // huge value and admit another level (#750).
    if (derived_bytes_so_far > maximum_derived_bytes ||
        result.derived_bytes > maximum_derived_bytes - derived_bytes_so_far ||
        level_bytes > maximum_derived_bytes - derived_bytes_so_far -
                          result.derived_bytes) {
      result.budget_limited = true;
      break;
    }
    level.summaries.reserve(static_cast<std::size_t>(bucket_count));
    if (result.levels.empty()) {
      for (auto bucket_begin = static_cast<std::uint64_t>(run_begin);
           bucket_begin < static_cast<std::uint64_t>(run_end);
           bucket_begin += bucket_samples) {
        if (stop_token.stop_requested()) {
          result.budget_limited = true;
          return result;
        }
        const auto bucket_end =
            std::min(static_cast<std::uint64_t>(run_end),
                     bucket_begin + bucket_samples);
        level.summaries.push_back(summarize(curve, bucket_begin, bucket_end));
      }
    } else {
      const auto &children = result.levels.back().summaries;
      for (std::size_t child_begin = 0; child_begin < children.size();
           child_begin += 4) {
        if (stop_token.stop_requested()) {
          result.budget_limited = true;
          return result;
        }
        const auto child_end =
            std::min(children.size(), child_begin + std::size_t{4});
        level.summaries.push_back(summarize_children(
            curve, std::span<const Summary>{children}.subspan(
                       child_begin, child_end - child_begin)));
      }
    }
    result.derived_bytes += level_bytes;
    ++result.level_count;
    result.levels.reserve(result.levels.size() + 1);
    result.levels.push_back(std::move(level));
    if (bucket_samples > std::numeric_limits<std::uint64_t>::max() / 4) {
      break;
    }
    bucket_samples *= 4;
  }
  return result;
}

} // namespace

struct CurveLodSelection::Impl {
  bool uses_raw_samples{};
  std::uint64_t bucket_samples{};
  std::vector<CurveLodPoint> points;
  std::vector<CurveLodSegment> segments;
};

CurveLodSelection::CurveLodSelection() = default;
CurveLodSelection::~CurveLodSelection() = default;
CurveLodSelection::CurveLodSelection(const CurveLodSelection &) = default;
CurveLodSelection &
CurveLodSelection::operator=(const CurveLodSelection &) = default;
CurveLodSelection::CurveLodSelection(CurveLodSelection &&) noexcept = default;
CurveLodSelection &
CurveLodSelection::operator=(CurveLodSelection &&) noexcept = default;

CurveLodSelection::CurveLodSelection(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

bool CurveLodSelection::uses_raw_samples() const noexcept {
  return impl_ == nullptr || impl_->uses_raw_samples;
}

std::uint64_t CurveLodSelection::bucket_samples() const noexcept {
  return impl_ == nullptr ? 1 : impl_->bucket_samples;
}

std::span<const CurveLodPoint> CurveLodSelection::points() const noexcept {
  return impl_ == nullptr ? std::span<const CurveLodPoint>{}
                          : std::span<const CurveLodPoint>{impl_->points};
}

std::span<const CurveLodSegment> CurveLodSelection::segments() const noexcept {
  return impl_ == nullptr ? std::span<const CurveLodSegment>{}
                          : std::span<const CurveLodSegment>{impl_->segments};
}

struct CurveLodPyramid::Impl {
  SamplingAxis axis;
  Curve curve;
  CurveLodBuildOptions options;
  std::vector<SourceRun> runs;
  CurveLodStatistics statistics;
};

CurveLodPyramid::CurveLodPyramid() = default;
CurveLodPyramid::~CurveLodPyramid() = default;
CurveLodPyramid::CurveLodPyramid(const CurveLodPyramid &) = default;
CurveLodPyramid &CurveLodPyramid::operator=(const CurveLodPyramid &) = default;
CurveLodPyramid::CurveLodPyramid(CurveLodPyramid &&) noexcept = default;
CurveLodPyramid &
CurveLodPyramid::operator=(CurveLodPyramid &&) noexcept = default;

CurveLodPyramid::CurveLodPyramid(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

Result<CurveLodPyramid>
CurveLodPyramid::build(const SamplingAxis &axis, const Curve &curve,
                       CurveLodBuildOptions options,
                       std::stop_token stop_token) noexcept {
  try {
    if (stop_token.stop_requested()) {
      return lod_error(ErrorCode::operation_cancelled,
                       MessageKey::operation_cancelled);
    }
    if (curve.sampling_axis_id != axis.id ||
        axis.coordinates.length() != curve.values.length() ||
        axis.coordinates.length() == 0 ||
        axis.coordinates.length() > std::numeric_limits<std::uint32_t>::max() ||
        options.base_bucket_samples < 2) {
      return lod_error(ErrorCode::invalid_document,
                       MessageKey::document_structure_invalid);
    }
    if (const auto error = validate_lod_inputs(axis, curve, stop_token)) {
      return *error;
    }
    if (options.maximum_derived_bytes == 0) {
      options.maximum_derived_bytes = default_budget(axis, curve);
    }
    auto impl = std::make_shared<Impl>(Impl{
        .axis = axis,
        .curve = curve,
        .options = options,
        .runs = {},
        .statistics =
            CurveLodStatistics{
                .source_samples = curve.values.length(),
                .source_bytes =
                    axis.coordinates.length() *
                        scalar_size_bytes(axis.coordinates.scalar_type()) +
                    curve.values.length() *
                        scalar_size_bytes(curve.values.scalar_type()) +
                    curve.nulls.byte_capacity(),
                .derived_bytes = 0,
                .maximum_derived_bytes = options.maximum_derived_bytes,
                .level_count = 0,
                .budget_limited = false,
            },
    });

    const auto sample_count = curve.values.length();
    std::uint64_t run_count{};
    auto in_run = false;
    for (std::uint64_t source_index = 0; source_index < sample_count;
         ++source_index) {
      if ((source_index & std::uint64_t{4095}) == 0 &&
          stop_token.stop_requested()) {
        return lod_error(ErrorCode::operation_cancelled,
                         MessageKey::operation_cancelled);
      }
      const auto valid = valid_sample(axis, curve, source_index);
      if (valid && !in_run) {
        ++run_count;
      }
      in_run = valid;
    }
    if (run_count > options.maximum_derived_bytes / sizeof(SourceRun)) {
      return lod_error(ErrorCode::resource_exhausted,
                       MessageKey::resource_exhausted);
    }
    impl->runs.reserve(static_cast<std::size_t>(run_count));
    std::uint64_t index{};
    std::uint64_t derived_bytes = run_count * sizeof(SourceRun);
    while (index < sample_count) {
      if (stop_token.stop_requested()) {
        return lod_error(ErrorCode::operation_cancelled,
                         MessageKey::operation_cancelled);
      }
      while (index < sample_count && !valid_sample(axis, curve, index)) {
        if ((index & std::uint64_t{4095}) == 0 && stop_token.stop_requested()) {
          return lod_error(ErrorCode::operation_cancelled,
                           MessageKey::operation_cancelled);
        }
        ++index;
      }
      if (index == sample_count) {
        break;
      }
      const auto run_begin = index;
      while (index < sample_count && valid_sample(axis, curve, index)) {
        if ((index & std::uint64_t{4095}) == 0 && stop_token.stop_requested()) {
          return lod_error(ErrorCode::operation_cancelled,
                           MessageKey::operation_cancelled);
        }
        ++index;
      }
      auto run = SourceRun{
          .begin = static_cast<std::uint32_t>(run_begin),
          .end = static_cast<std::uint32_t>(index),
          .levels = {},
      };
      if (options.algorithm == CurveLodAlgorithm::hierarchical) {
        const auto built = build_run_levels(
            curve, run.begin, run.end, options.base_bucket_samples,
            options.maximum_derived_bytes, derived_bytes, stop_token);
        if (stop_token.stop_requested()) {
          return lod_error(ErrorCode::operation_cancelled,
                           MessageKey::operation_cancelled);
        }
        derived_bytes += built.derived_bytes;
        impl->statistics.level_count += built.level_count;
        if (built.budget_limited) {
          impl->statistics.budget_limited = true;
        }
        run.levels = std::move(built.levels);
      }
      impl->runs.push_back(std::move(run));
    }
    impl->statistics.derived_bytes = derived_bytes;
    return CurveLodPyramid{std::move(impl)};
  } catch (const std::bad_alloc &) {
    return lod_error(ErrorCode::resource_exhausted,
                     MessageKey::resource_exhausted);
  } catch (...) {
    return lod_error(ErrorCode::internal_error, MessageKey::internal_error);
  }
}

// True when two CurveBuffers wrap the same physical memory segment-for-
// segment (data pointer, element count, stride, scalar type). Copying a
// CurveBuffer copies the shared-ownership views, not the samples, so an
// unchanged curve carried into a new document revision still compares
// equal here — this is the input identity the unchanged-curve fast path
// below keys on (issue #471 / audit: unchanged curves were full-rebuilt
// because the append-parity length precondition rejected same-length
// inputs).
[[nodiscard]] static bool
same_buffer_identity(const CurveBuffer &a, const CurveBuffer &b) noexcept {
  if (a.is_composite() != b.is_composite()) {
    return false;
  }
  if (a.is_composite()) {
    const auto as = a.segments();
    const auto bs = b.segments();
    if (as.size() != bs.size()) {
      return false;
    }
    for (std::size_t index = 0; index < as.size(); ++index) {
      if (as[index].data() != bs[index].data() ||
          as[index].length() != bs[index].length() ||
          as[index].stride_bytes() != bs[index].stride_bytes() ||
          as[index].scalar_type() != bs[index].scalar_type()) {
        return false;
      }
    }
    return true;
  }
  const auto &x = a.as_single();
  const auto &y = b.as_single();
  return x.data() == y.data() && x.length() == y.length() &&
         x.stride_bytes() == y.stride_bytes() &&
         x.scalar_type() == y.scalar_type();
}

Result<CurveLodPyramid>
CurveLodPyramid::extend_tail(const CurveLodPyramid &previous,
                             const SamplingAxis &extended_axis,
                             const Curve &extended_curve,
                             CurveLodBuildOptions options,
                             std::stop_token stop_token) noexcept {
  try {
    if (stop_token.stop_requested()) {
      return lod_error(ErrorCode::operation_cancelled,
                       MessageKey::operation_cancelled);
    }
    // The previous pyramid must exist and carry the curve it was built over.
    if (previous.impl_ == nullptr || previous.impl_->runs.empty()) {
      return lod_error(ErrorCode::invalid_document,
                       MessageKey::document_structure_invalid);
    }
    const auto &prev = *previous.impl_;
    const auto prev_length = prev.statistics.source_samples;
    const auto extended_length = extended_curve.values.length();
    // Resolve a default budget the same way `build` does, so the parity check
    // below compares like-for-like (the previous build's resolved budget against
    // the extended curve's resolved budget).
    if (options.maximum_derived_bytes == 0) {
      options.maximum_derived_bytes =
          default_budget(extended_axis, extended_curve);
    }
    // Unchanged-input fast path: when the extended curve and axis wrap the
    // same physical segments the previous pyramid was built over (and the
    // build options match), `previous` already IS the pyramid of this exact
    // input — return it with zero derived work. Without this branch the
    // length-growth precondition below rejected same-length curves and the
    // caller full-rebuilt every unchanged curve, contradicting the staging
    // comment's "unchanged curves reuse the previous pyramid directly".
    if (same_buffer_identity(extended_curve.values, prev.curve.values) &&
        same_buffer_identity(extended_axis.coordinates,
                             prev.axis.coordinates) &&
        extended_curve.id == prev.curve.id &&
        extended_axis.id == prev.axis.id &&
        options.algorithm == prev.options.algorithm &&
        options.base_bucket_samples == prev.options.base_bucket_samples &&
        options.maximum_derived_bytes ==
            (prev.options.maximum_derived_bytes != 0
                 ? prev.options.maximum_derived_bytes
                 : options.maximum_derived_bytes)) {
      return previous;
    }
    const auto prev_resolved_budget =
        prev.options.maximum_derived_bytes != 0
            ? prev.options.maximum_derived_bytes
            : default_budget(prev.axis, prev.curve);
    // Structural preconditions: matching ids, the extended curve is longer, the
    // build options match (a different algorithm/base bucket/budget would
    // produce a different level grid or a different budget envelope, so reuse
    // is invalid — caller must full-build). The budget must match exactly: a
    // reused run was sized under the previous budget, so a different budget
    // would let extend_tail emit levels a full rebuild would truncate (or vice
    // versa), breaking the parity guarantee.
    if (extended_curve.sampling_axis_id != extended_axis.id ||
        extended_axis.id != prev.axis.id ||
        extended_curve.id != prev.curve.id ||
        extended_length <= prev_length ||
        extended_axis.coordinates.length() != extended_length ||
        options.algorithm != prev.options.algorithm ||
        options.base_bucket_samples != prev.options.base_bucket_samples ||
        options.maximum_derived_bytes != prev_resolved_budget ||
        extended_length > std::numeric_limits<std::uint32_t>::max() ||
        options.base_bucket_samples < 2) {
      return lod_error(ErrorCode::invalid_document,
                       MessageKey::document_structure_invalid);
    }
    if (const auto error =
            validate_lod_inputs(extended_axis, extended_curve, stop_token)) {
      return *error;
    }
    // Append (not edit) precondition: the first `prev_length` curve VALUES of
    // the extended curve must equal the previous curve numerically. The reused
    // level summaries are a pure function of the curve values in their sample
    // range, so an edited value would make them wrong — refuse (the caller must
    // issue a full build). The axis coordinates are not re-checked here: a
    // finite→finite axis edit in the prefix does not corrupt summaries (run
    // boundaries depend only on sample validity), and source_range query
    // semantics read the extended axis directly.
    for (std::uint64_t index = 0; index < prev_length; ++index) {
      if ((index & std::uint64_t{4095}) == 0 && stop_token.stop_requested()) {
        return lod_error(ErrorCode::operation_cancelled,
                         MessageKey::operation_cancelled);
      }
      const auto prev_value = prev.curve.values.value_as_double(index);
      const auto ext_value = extended_curve.values.value_as_double(index);
      if (!prev_value.has_value() || !ext_value.has_value() ||
          *prev_value != *ext_value) {
        return lod_error(ErrorCode::invalid_document,
                         MessageKey::document_structure_invalid);
      }
    }

    if (options.maximum_derived_bytes == 0) {
      options.maximum_derived_bytes =
          default_budget(extended_axis, extended_curve);
    }

    auto impl = std::make_shared<Impl>(Impl{
        .axis = extended_axis,
        .curve = extended_curve,
        .options =
            CurveLodBuildOptions{
                .algorithm = prev.options.algorithm,
                .base_bucket_samples = prev.options.base_bucket_samples,
                .maximum_derived_bytes = options.maximum_derived_bytes,
            },
        .runs = {},
        .statistics =
            CurveLodStatistics{
                .source_samples = extended_length,
                .source_bytes =
                    extended_axis.coordinates.length() *
                        scalar_size_bytes(extended_axis.coordinates.scalar_type()) +
                    extended_length *
                        scalar_size_bytes(extended_curve.values.scalar_type()) +
                    extended_curve.nulls.byte_capacity(),
                .derived_bytes = 0,
                .maximum_derived_bytes = options.maximum_derived_bytes,
                .level_count = 0,
                .budget_limited = false,
            },
    });

    // All runs except the last are reused byte-for-byte: their `[begin, end)`
    // sample ranges fall entirely within the unchanged earlier region, so their
    // level summaries are identical to a full rebuild. Their bytes and level
    // counts carry over unchanged.
    std::uint64_t derived_bytes{};
    if (prev.runs.size() > 1) {
      impl->runs.reserve(prev.runs.size());
      for (std::size_t run_index = 0; run_index + 1 < prev.runs.size();
           ++run_index) {
        const auto &reused = prev.runs[run_index];
        derived_bytes += sizeof(SourceRun);
        for (const auto &level : reused.levels) {
          derived_bytes += sizeof(Level) + level.summaries.size() * sizeof(Summary);
          impl->statistics.level_count += 1;
        }
        impl->runs.push_back(reused);
      }
    }

    // Re-derive from the last run's begin to the extended end. This re-derives
    // the (now longer) last run and any new tail runs the appended region
    // introduces (null gaps splitting runs). Re-scanning run boundaries here —
    // rather than naively extending the last run's end — guarantees the run
    // structure matches a full rebuild exactly.
    const auto resume_begin =
        static_cast<std::uint64_t>(prev.runs.back().begin);
    // First pass: discover the re-derive region's run boundaries (begin,end) so
    // the SourceRun overhead can be pre-charged exactly as `build` does
    // (`run_count * sizeof(SourceRun)` up front). Charging per-run AFTER
    // derivation would hand build_run_levels more budget than a full rebuild
    // sees, letting extend_tail emit a level build truncates — breaking the
    // parity guarantee under a binding budget.
    struct RunRange {
      std::uint32_t begin;
      std::uint32_t end;
    };
    std::vector<RunRange> redesign_runs;
    {
      std::uint64_t scan = resume_begin;
      const auto extended_count = extended_length;
      while (scan < extended_count) {
        if (stop_token.stop_requested()) {
          return lod_error(ErrorCode::operation_cancelled,
                           MessageKey::operation_cancelled);
        }
        while (scan < extended_count &&
               !valid_sample(extended_axis, extended_curve, scan)) {
          if ((scan & std::uint64_t{4095}) == 0 &&
              stop_token.stop_requested()) {
            return lod_error(ErrorCode::operation_cancelled,
                             MessageKey::operation_cancelled);
          }
          ++scan;
        }
        if (scan == extended_count) {
          break;
        }
        const auto run_begin = scan;
        while (scan < extended_count &&
               valid_sample(extended_axis, extended_curve, scan)) {
          if ((scan & std::uint64_t{4095}) == 0 &&
              stop_token.stop_requested()) {
            return lod_error(ErrorCode::operation_cancelled,
                             MessageKey::operation_cancelled);
          }
          ++scan;
        }
        redesign_runs.push_back(RunRange{
            .begin = static_cast<std::uint32_t>(run_begin),
            .end = static_cast<std::uint32_t>(scan),
        });
      }
    }
    // Pre-charge every re-derive run's SourceRun overhead (matches build's
    // `run_count * sizeof(SourceRun)` initial charge over the same run set).
    // build() refuses when the *total* run count cannot fit in the budget;
    // apply the same check here so a null-split tail cannot slip past it
    // and then wrap the unsigned remaining-budget subtraction (#750).
    const auto total_runs = impl->runs.size() + redesign_runs.size();
    if (total_runs > options.maximum_derived_bytes / sizeof(SourceRun)) {
      return lod_error(ErrorCode::resource_exhausted,
                       MessageKey::resource_exhausted);
    }
    derived_bytes += redesign_runs.size() * sizeof(SourceRun);
    if (derived_bytes > options.maximum_derived_bytes) {
      impl->statistics.budget_limited = true;
    }
    // Second pass: derive the levels for each re-derive run under the now-full
    // budget envelope, identical to a full rebuild's per-run derivation.
    for (const auto &range : redesign_runs) {
      auto run = SourceRun{
          .begin = range.begin,
          .end = range.end,
          .levels = {},
      };
      if (prev.options.algorithm == CurveLodAlgorithm::hierarchical) {
        const auto built = build_run_levels(
            extended_curve, run.begin, run.end, prev.options.base_bucket_samples,
            options.maximum_derived_bytes, derived_bytes, stop_token);
        if (stop_token.stop_requested()) {
          return lod_error(ErrorCode::operation_cancelled,
                           MessageKey::operation_cancelled);
        }
        derived_bytes += built.derived_bytes;
        impl->statistics.level_count += built.level_count;
        if (built.budget_limited) {
          impl->statistics.budget_limited = true;
        }
        run.levels = std::move(built.levels);
      }
      impl->runs.push_back(std::move(run));
    }
    impl->statistics.derived_bytes = derived_bytes;
    return CurveLodPyramid{std::move(impl)};
  } catch (const std::bad_alloc &) {
    return lod_error(ErrorCode::resource_exhausted,
                     MessageKey::resource_exhausted);
  } catch (...) {
    return lod_error(ErrorCode::internal_error, MessageKey::internal_error);
  }
}

Result<CurveLodSelection>
CurveLodPyramid::query(const CurveLodQuery &query,
                       std::stop_token stop_token) const noexcept {
  try {
    if (stop_token.stop_requested()) {
      return lod_error(ErrorCode::operation_cancelled,
                       MessageKey::operation_cancelled);
    }
    if (impl_ == nullptr || !std::isfinite(query.viewport_top) ||
        !std::isfinite(query.viewport_bottom) ||
        query.viewport_top >= query.viewport_bottom ||
        query.pixel_height == 0 || !std::isfinite(query.prefetch_viewports) ||
        query.prefetch_viewports < 0.0) {
      return lod_error(ErrorCode::invalid_viewport,
                       MessageKey::viewport_invalid);
    }

    const auto span = query.viewport_bottom - query.viewport_top;
    const auto query_top = query.viewport_top - span * query.prefetch_viewports;
    const auto query_bottom =
        query.viewport_bottom + span * query.prefetch_viewports;
    if (!std::isfinite(query_top) || !std::isfinite(query_bottom)) {
      return lod_error(ErrorCode::invalid_viewport,
                       MessageKey::viewport_invalid);
    }
    const auto visible =
        source_range(impl_->axis, query.viewport_top, query.viewport_bottom);
    const auto prefetched = source_range(impl_->axis, query_top, query_bottom);

    auto selection = std::make_shared<CurveLodSelection::Impl>();
    if (visible.begin >= visible.end || prefetched.begin >= prefetched.end) {
      return CurveLodSelection{std::move(selection)};
    }
    const auto visible_samples = visible.end - visible.begin;
    const auto samples_per_pixel = static_cast<double>(visible_samples) /
                                   static_cast<double>(query.pixel_height);
    const auto use_raw = samples_per_pixel <= 1.0;
    selection->uses_raw_samples = use_raw;
    selection->bucket_samples = 1;

    for (const auto &run : impl_->runs) {
      if (stop_token.stop_requested()) {
        return lod_error(ErrorCode::operation_cancelled,
                         MessageKey::operation_cancelled);
      }
      const auto begin =
          std::max(static_cast<std::uint64_t>(run.begin), prefetched.begin);
      const auto end =
          std::min(static_cast<std::uint64_t>(run.end), prefetched.end);
      if (begin >= end) {
        continue;
      }
      const auto first_point =
          static_cast<std::uint64_t>(selection->points.size());
      auto target_bucket_samples = impl_->options.base_bucket_samples;
      while (static_cast<double>(target_bucket_samples) < samples_per_pixel &&
             target_bucket_samples <=
                 std::numeric_limits<std::uint64_t>::max() / 4) {
        target_bucket_samples *= 4;
      }
      if (use_raw) {
        for (auto index = begin; index < end; ++index) {
          if ((index & std::uint64_t{4095}) == 0 &&
              stop_token.stop_requested()) {
            return lod_error(ErrorCode::operation_cancelled,
                             MessageKey::operation_cancelled);
          }
          selection->points.push_back(CurveLodPoint{
              .sample_index = index,
              .reference_depth =
                  impl_->axis.coordinates.value_as_double(index).value(),
              .value = impl_->curve.values.value_as_double(index).value(),
          });
        }
      } else if (impl_->options.algorithm ==
                     CurveLodAlgorithm::scalar_reference ||
                 run.levels.empty() ||
                 run.levels.back().bucket_samples < target_bucket_samples) {
        const auto bucket_samples = target_bucket_samples;
        selection->bucket_samples =
            std::max(selection->bucket_samples, bucket_samples);
        const auto first_bucket =
            run.begin + ((begin - run.begin) / bucket_samples) * bucket_samples;
        for (auto bucket_begin = first_bucket; bucket_begin < end;
             bucket_begin += bucket_samples) {
          if (stop_token.stop_requested()) {
            return lod_error(ErrorCode::operation_cancelled,
                             MessageKey::operation_cancelled);
          }
          const auto bucket_end = std::min(static_cast<std::uint64_t>(run.end),
                                           bucket_begin + bucket_samples);
          const auto summary =
              summarize(impl_->curve, bucket_begin, bucket_end);
          for (std::uint8_t offset = 0; offset < summary.count; ++offset) {
            const auto index =
                summary.indices[static_cast<std::size_t>(offset)];
            selection->points.push_back(CurveLodPoint{
                .sample_index = index,
                .reference_depth =
                    impl_->axis.coordinates.value_as_double(index).value(),
                .value = impl_->curve.values.value_as_double(index).value(),
            });
          }
        }
      } else {
        auto level = run.levels.end() - 1;
        for (auto candidate = run.levels.begin(); candidate != run.levels.end();
             ++candidate) {
          if (static_cast<double>(candidate->bucket_samples) >=
              samples_per_pixel) {
            level = candidate;
            break;
          }
        }
        selection->bucket_samples =
            std::max(selection->bucket_samples, level->bucket_samples);
        for (const auto &summary : level->summaries) {
          if (stop_token.stop_requested()) {
            return lod_error(ErrorCode::operation_cancelled,
                             MessageKey::operation_cancelled);
          }
          if (summary.source_end <= begin || summary.source_begin >= end) {
            continue;
          }
          for (std::uint8_t offset = 0; offset < summary.count; ++offset) {
            const auto index =
                summary.indices[static_cast<std::size_t>(offset)];
            selection->points.push_back(CurveLodPoint{
                .sample_index = index,
                .reference_depth =
                    impl_->axis.coordinates.value_as_double(index).value(),
                .value = impl_->curve.values.value_as_double(index).value(),
            });
          }
        }
      }
      selection->segments.push_back(CurveLodSegment{
          .first_point = first_point,
          .point_count = static_cast<std::uint64_t>(selection->points.size()) -
                         first_point,
      });
    }
    return CurveLodSelection{std::move(selection)};
  } catch (const std::bad_alloc &) {
    return lod_error(ErrorCode::resource_exhausted,
                     MessageKey::resource_exhausted);
  } catch (...) {
    return lod_error(ErrorCode::internal_error, MessageKey::internal_error);
  }
}

CurveLodStatistics CurveLodPyramid::statistics() const noexcept {
  return impl_ == nullptr ? CurveLodStatistics{} : impl_->statistics;
}

} // namespace welllog
