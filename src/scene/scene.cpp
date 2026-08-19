#include "scene/prepare.hpp"
#include "scene/triangulate.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <set>
#include <stop_token>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace welllog {

namespace {

[[nodiscard]] double interpolate_segment(double x, double x0, double x1,
                                         double y0, double y1) noexcept {
  if (x1 == x0) {
    return y0;
  }
  const auto t = (x - x0) / (x1 - x0);
  return y0 + t * (y1 - y0);
}

} // namespace

std::optional<Error>
validate_depth_transform(const DepthTransform &transform) noexcept {
  const auto &pts = transform.control_points;
  if (pts.empty()) {
    return std::nullopt;
  }
  if (pts.size() == 1) {
    return Error{.code = ErrorCode::invalid_presentation,
                 .severity = Severity::error,
                 .entity_id = std::nullopt,
                 .message = MessageKey::presentation_invalid,
                 .arguments = {}};
  }
  // Display direction is fixed by the first pair and must hold for every
  // subsequent step (strictly monotonic in ONE direction).
  const auto increasing_display = pts[1].display_depth > pts[0].display_depth;
  for (std::size_t i = 0; i < pts.size(); ++i) {
    if (!std::isfinite(pts[i].reference_depth) ||
        !std::isfinite(pts[i].display_depth)) {
      return Error{.code = ErrorCode::invalid_presentation,
                   .severity = Severity::error,
                   .entity_id = std::nullopt,
                   .message = MessageKey::presentation_invalid,
                   .arguments = {}};
    }
    if (i > 0) {
      // Reference must be strictly increasing; display must be strictly
      // monotonic in EITHER direction — a decreasing display is the TVDSS
      // display domain (deeper MD → smaller subsea depth), which the scene
      // mm layout maps to a deeper position on the page.
      if (!(pts[i].reference_depth > pts[i - 1].reference_depth) ||
          pts[i].display_depth == pts[i - 1].display_depth ||
          (pts[i].display_depth > pts[i - 1].display_depth) !=
              increasing_display) {
        return Error{.code = ErrorCode::invalid_presentation,
                     .severity = Severity::error,
                     .entity_id = std::nullopt,
                     .message = MessageKey::presentation_invalid,
                     .arguments = {}};
      }
    }
  }
  return std::nullopt;
}

double map_reference_to_display(const DepthTransform &transform,
                                double reference_depth) noexcept {
  const auto &pts = transform.control_points;
  if (pts.empty() || !std::isfinite(reference_depth)) {
    return reference_depth;
  }
  if (reference_depth <= pts.front().reference_depth) {
    if (transform.extrapolate == DepthExtrapolatePolicy::clamp ||
        pts.size() < 2) {
      return pts.front().display_depth;
    }
    return interpolate_segment(reference_depth, pts[0].reference_depth,
                               pts[1].reference_depth, pts[0].display_depth,
                               pts[1].display_depth);
  }
  if (reference_depth >= pts.back().reference_depth) {
    if (transform.extrapolate == DepthExtrapolatePolicy::clamp ||
        pts.size() < 2) {
      return pts.back().display_depth;
    }
    const auto n = pts.size();
    return interpolate_segment(reference_depth, pts[n - 2].reference_depth,
                               pts[n - 1].reference_depth,
                               pts[n - 2].display_depth,
                               pts[n - 1].display_depth);
  }
  for (std::size_t i = 1; i < pts.size(); ++i) {
    if (reference_depth <= pts[i].reference_depth) {
      return interpolate_segment(reference_depth, pts[i - 1].reference_depth,
                                 pts[i].reference_depth,
                                 pts[i - 1].display_depth,
                                 pts[i].display_depth);
    }
  }
  return pts.back().display_depth;
}

double map_display_to_reference(const DepthTransform &transform,
                                double display_depth) noexcept {
  const auto &pts = transform.control_points;
  if (pts.empty() || !std::isfinite(display_depth)) {
    return display_depth;
  }
  const auto n = pts.size();
  // Display is strictly monotonic in either direction (TVDSS display
  // decreases with MD); bracket the value over consecutive control points.
  for (std::size_t i = 1; i < n; ++i) {
    const auto a = pts[i - 1].display_depth;
    const auto b = pts[i].display_depth;
    if ((display_depth >= a && display_depth <= b) ||
        (display_depth <= a && display_depth >= b)) {
      return interpolate_segment(display_depth, a, b,
                                 pts[i - 1].reference_depth,
                                 pts[i].reference_depth);
    }
  }
  // Outside the control span: extrapolate from the endpoint segment (or
  // clamp to the endpoint when requested).
  const auto decreasing = pts.back().display_depth < pts.front().display_depth;
  const auto beyond_top = decreasing
                              ? display_depth > pts.front().display_depth
                              : display_depth <= pts.front().display_depth;
  if (beyond_top) {
    if (transform.extrapolate == DepthExtrapolatePolicy::clamp || n < 2) {
      return pts.front().reference_depth;
    }
    return interpolate_segment(display_depth, pts[0].display_depth,
                               pts[1].display_depth, pts[0].reference_depth,
                               pts[1].reference_depth);
  }
  if (transform.extrapolate == DepthExtrapolatePolicy::clamp || n < 2) {
    return pts.back().reference_depth;
  }
  return interpolate_segment(display_depth, pts[n - 2].display_depth,
                             pts[n - 1].display_depth,
                             pts[n - 2].reference_depth,
                             pts[n - 1].reference_depth);
}

Result<DepthTransform> depth_transform_aligning_markers(
    std::span<const double> source_reference_depths,
    std::span<const double> target_display_depths) noexcept {
  if (source_reference_depths.size() != target_display_depths.size() ||
      source_reference_depths.size() < 2) {
    return Error{.code = ErrorCode::invalid_presentation,
                 .severity = Severity::error,
                 .entity_id = std::nullopt,
                 .message = MessageKey::presentation_invalid,
                 .arguments = {}};
  }
  DepthTransform transform;
  transform.control_points.reserve(source_reference_depths.size());
  for (std::size_t i = 0; i < source_reference_depths.size(); ++i) {
    transform.control_points.push_back(DepthControlPoint{
        .reference_depth = source_reference_depths[i],
        .display_depth = target_display_depths[i],
    });
  }
  // Sort by source reference depth.
  std::sort(transform.control_points.begin(), transform.control_points.end(),
            [](const DepthControlPoint &a, const DepthControlPoint &b) {
              return a.reference_depth < b.reference_depth;
            });
  if (const auto err = validate_depth_transform(transform); err.has_value()) {
    return *err;
  }
  transform.version = 1;
  return transform;
}

SymbolKind symbol_for_marker_semantic(MarkerSemantic semantic) noexcept {
  switch (semantic) {
  case MarkerSemantic::formation_top:
    return SymbolKind::triangle_down;
  case MarkerSemantic::fault:
    return SymbolKind::cross;
  case MarkerSemantic::fluid_contact:
    return SymbolKind::diamond;
  case MarkerSemantic::casing_shoe:
    return SymbolKind::shoe;
  case MarkerSemantic::custom:
    break;
  }
  return SymbolKind::circle;
}

SymbolGlyph symbol_glyph(SymbolKind kind, Millimetres size) noexcept {
  const auto half = size.value / 2.0;
  SymbolGlyph glyph;
  glyph.kind = kind;
  glyph.size = size;
  glyph.stroke_width = size.value / 6.0;
  const auto point = [](double x, double y) {
    return PhysicalPoint{.left = Millimetres{x}, .top = Millimetres{y}};
  };
  switch (kind) {
  case SymbolKind::circle:
    // 16-gon approximation — vector backends still emit exact arcs via
    // `kind`; this outline serves the raster path and geometry consumers.
    for (int i = 0; i < 16; ++i) {
      const double theta = 2.0 * 3.14159265358979323846 * i / 16.0;
      glyph.outline.push_back(point(half * std::cos(theta),
                                    half * std::sin(theta)));
    }
    break;
  case SymbolKind::cross:
    break;  // stroke-only; backends draw the two diagonals.
  case SymbolKind::square:
    glyph.outline = {point(-half, -half), point(half, -half),
                     point(half, half), point(-half, half)};
    break;
  case SymbolKind::triangle_up:
    glyph.outline = {point(0.0, -half), point(half, half), point(-half, half)};
    break;
  case SymbolKind::triangle_down:
    glyph.outline = {point(0.0, half), point(half, -half), point(-half, -half)};
    break;
  case SymbolKind::diamond:
    glyph.outline = {point(0.0, -half), point(half, 0.0), point(0.0, half),
                     point(-half, 0.0)};
    break;
  case SymbolKind::shoe:
    // Casing-shoe glyph: filled arch (flat side up, bulge down) — a
    // horseshoe/shoe profile pointing down the well.
    for (int i = 0; i <= 16; ++i) {
      const double theta = 3.14159265358979323846 * i / 16.0;
      glyph.outline.push_back(point(half * std::cos(theta),
                                    half * std::sin(theta)));
    }
    break;
  }
  return glyph;
}

namespace {

[[nodiscard]] Error presentation_error(EntityId entity_id = {}) {
  return Error{
      .code = ErrorCode::invalid_presentation,
      .severity = Severity::error,
      .entity_id = entity_id.is_nil() ? std::nullopt
                                      : std::optional<EntityId>{entity_id},
      .message = MessageKey::presentation_invalid,
      .arguments = {},
  };
}

constexpr double header_line_height_factor = 1.25;
constexpr double header_baseline_factor = 0.85;
constexpr double header_padding_millimetres = 0.5;
constexpr double header_left_padding_millimetres = 1.0;

template <typename Layer>
void order_layers_by_z(std::vector<const Layer *> &layers) {
  std::stable_sort(layers.begin(), layers.end(),
                   [](const Layer *left_layer, const Layer *right_layer) {
                     if (left_layer->z_order != right_layer->z_order) {
                       return left_layer->z_order < right_layer->z_order;
                     }
                     return left_layer->id < right_layer->id;
                   });
}

[[nodiscard]] Error cancellation_error() {
  return Error{
      .code = ErrorCode::operation_cancelled,
      .severity = Severity::error,
      .entity_id = std::nullopt,
      .message = MessageKey::operation_cancelled,
      .arguments = {},
  };
}

} // namespace

struct ScenePresentation::Impl {
  EntityId document_id;
  DepthDomain reference_depth_domain;
  std::string reference_depth_unit;
  double reference_depth_top{};
  double reference_depth_bottom{};
  Millimetres physical_height;
  std::string font_asset_fingerprint;
  PresentationVersion presentation_version;
  std::uint64_t depth_transform_version{};
  DepthTransform depth_transform_map{};
  std::vector<TrackSpec> tracks;
  std::vector<TrackScaleSpec> scales;
  std::vector<CurveLayerSpec> curve_layers;
  std::vector<PatternDefinition> patterns;
  std::vector<IntervalLayerSpec> interval_layers;
  std::vector<CrossoverFillLayerSpec> crossover_fill_layers;
  std::vector<ImageLayerSpec> image_layers;
  std::vector<MarkerLayerSpec> marker_layers;
  std::vector<SymbolLayerSpec> symbol_layers;
  std::vector<TextLayerSpec> text_layers;
  std::vector<CustomLayerSpec> custom_layers;
};

ScenePresentation::ScenePresentation() = default;
ScenePresentation::~ScenePresentation() = default;
ScenePresentation::ScenePresentation(const ScenePresentation &) = default;
ScenePresentation &
ScenePresentation::operator=(const ScenePresentation &) = default;
ScenePresentation::ScenePresentation(ScenePresentation &&) noexcept = default;
ScenePresentation &
ScenePresentation::operator=(ScenePresentation &&) noexcept = default;

ScenePresentation::ScenePresentation(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

EntityId ScenePresentation::document_id() const noexcept {
  return impl_ == nullptr ? EntityId{} : impl_->document_id;
}

ReferenceDepthRange ScenePresentation::reference_depth_range() const noexcept {
  return impl_ == nullptr ? ReferenceDepthRange{}
                          : ReferenceDepthRange{
                                .domain = impl_->reference_depth_domain,
                                .unit = impl_->reference_depth_unit,
                                .top = impl_->reference_depth_top,
                                .bottom = impl_->reference_depth_bottom,
                            };
}

Millimetres ScenePresentation::physical_height() const noexcept {
  return impl_ == nullptr ? Millimetres{} : impl_->physical_height;
}

std::string_view ScenePresentation::font_asset_fingerprint() const noexcept {
  return impl_ == nullptr ? std::string_view{}
                          : std::string_view{impl_->font_asset_fingerprint};
}

PresentationVersion
ScenePresentation::presentation_version() const noexcept {
  return impl_ == nullptr ? PresentationVersion{}
                          : impl_->presentation_version;
}

DepthTransformDescriptor
ScenePresentation::depth_transform() const noexcept {
  return impl_ == nullptr
             ? DepthTransformDescriptor{}
             : DepthTransformDescriptor{
                   .domain = impl_->reference_depth_domain,
                   .unit = impl_->reference_depth_unit,
                   .reference_top = impl_->reference_depth_top,
                   .reference_bottom = impl_->reference_depth_bottom,
                   .version = impl_->depth_transform_version,
               };
}

const DepthTransform &
ScenePresentation::depth_transform_map() const noexcept {
  static const DepthTransform k_identity{};
  return impl_ == nullptr ? k_identity : impl_->depth_transform_map;
}

std::span<const TrackSpec> ScenePresentation::tracks() const noexcept {
  return impl_ == nullptr ? std::span<const TrackSpec>{}
                          : std::span<const TrackSpec>{impl_->tracks};
}

std::span<const TrackScaleSpec> ScenePresentation::scales() const noexcept {
  return impl_ == nullptr ? std::span<const TrackScaleSpec>{}
                          : std::span<const TrackScaleSpec>{impl_->scales};
}

std::span<const CurveLayerSpec>
ScenePresentation::curve_layers() const noexcept {
  return impl_ == nullptr
             ? std::span<const CurveLayerSpec>{}
             : std::span<const CurveLayerSpec>{impl_->curve_layers};
}

std::span<const PatternDefinition>
ScenePresentation::patterns() const noexcept {
  return impl_ == nullptr
             ? std::span<const PatternDefinition>{}
             : std::span<const PatternDefinition>{impl_->patterns};
}

std::span<const IntervalLayerSpec>
ScenePresentation::interval_layers() const noexcept {
  return impl_ == nullptr
             ? std::span<const IntervalLayerSpec>{}
             : std::span<const IntervalLayerSpec>{impl_->interval_layers};
}

std::span<const CrossoverFillLayerSpec>
ScenePresentation::crossover_fill_layers() const noexcept {
  return impl_ == nullptr
             ? std::span<const CrossoverFillLayerSpec>{}
             : std::span<const CrossoverFillLayerSpec>{
                   impl_->crossover_fill_layers};
}

std::span<const ImageLayerSpec>
ScenePresentation::image_layers() const noexcept {
  return impl_ == nullptr ? std::span<const ImageLayerSpec>{}
                          : std::span<const ImageLayerSpec>{impl_->image_layers};
}

std::span<const MarkerLayerSpec>
ScenePresentation::marker_layers() const noexcept {
  return impl_ == nullptr
             ? std::span<const MarkerLayerSpec>{}
             : std::span<const MarkerLayerSpec>{impl_->marker_layers};
}

std::span<const SymbolLayerSpec>
ScenePresentation::symbol_layers() const noexcept {
  return impl_ == nullptr
             ? std::span<const SymbolLayerSpec>{}
             : std::span<const SymbolLayerSpec>{impl_->symbol_layers};
}

std::span<const TextLayerSpec>
ScenePresentation::text_layers() const noexcept {
  return impl_ == nullptr
             ? std::span<const TextLayerSpec>{}
             : std::span<const TextLayerSpec>{impl_->text_layers};
}

std::span<const CustomLayerSpec>
ScenePresentation::custom_layers() const noexcept {
  return impl_ == nullptr
             ? std::span<const CustomLayerSpec>{}
             : std::span<const CustomLayerSpec>{impl_->custom_layers};
}

struct ScenePresentationBuilder::Impl {
  ScenePresentation::Impl presentation;
  bool allocation_failed{};
};

ScenePresentationBuilder::ScenePresentationBuilder(
    EntityId document_id, ReferenceDepthRange reference_depth_range,
    Millimetres physical_height,
    std::string_view font_asset_fingerprint) noexcept {
  try {
    impl_ = std::make_unique<Impl>(Impl{
        .presentation =
            ScenePresentation::Impl{
                .document_id = document_id,
                .reference_depth_domain = reference_depth_range.domain,
                .reference_depth_unit = std::string{reference_depth_range.unit},
                .reference_depth_top = reference_depth_range.top,
                .reference_depth_bottom = reference_depth_range.bottom,
                .physical_height = physical_height,
                .font_asset_fingerprint = std::string{font_asset_fingerprint},
                .presentation_version = PresentationVersion{},
                .depth_transform_version = 0,
                .depth_transform_map = DepthTransform{},
                .tracks = {},
                .scales = {},
                .curve_layers = {},
                .patterns = {},
                .interval_layers = {},
                .crossover_fill_layers = {},
                .image_layers = {},
                .marker_layers = {},
                .symbol_layers = {},
                .text_layers = {},
                .custom_layers = {},
            },
        .allocation_failed = false,
    });
  } catch (...) {
    impl_.reset();
  }
}

ScenePresentationBuilder::~ScenePresentationBuilder() = default;
ScenePresentationBuilder::ScenePresentationBuilder(
    ScenePresentationBuilder &&) noexcept = default;
ScenePresentationBuilder &ScenePresentationBuilder::operator=(
    ScenePresentationBuilder &&) noexcept = default;

ScenePresentationBuilder &
ScenePresentationBuilder::add_track(const TrackSpec &track) noexcept {
  if (impl_ == nullptr || impl_->allocation_failed) {
    return *this;
  }
  try {
    impl_->presentation.tracks.push_back(track);
  } catch (...) {
    impl_->allocation_failed = true;
  }
  return *this;
}

ScenePresentationBuilder &
ScenePresentationBuilder::add_scale(const TrackScaleSpec &scale) noexcept {
  if (impl_ == nullptr || impl_->allocation_failed) {
    return *this;
  }
  try {
    impl_->presentation.scales.push_back(scale);
  } catch (...) {
    impl_->allocation_failed = true;
  }
  return *this;
}

ScenePresentationBuilder &ScenePresentationBuilder::set_presentation_version(
    PresentationVersion version) noexcept {
  if (impl_ != nullptr && !impl_->allocation_failed) {
    impl_->presentation.presentation_version = version;
  }
  return *this;
}

ScenePresentationBuilder &ScenePresentationBuilder::set_depth_transform_version(
    std::uint64_t version) noexcept {
  if (impl_ != nullptr && !impl_->allocation_failed) {
    impl_->presentation.depth_transform_version = version;
    impl_->presentation.depth_transform_map.version = version;
  }
  return *this;
}

ScenePresentationBuilder &ScenePresentationBuilder::set_depth_transform(
    const DepthTransform &transform) noexcept {
  if (impl_ != nullptr && !impl_->allocation_failed) {
    impl_->presentation.depth_transform_map = transform;
    if (transform.version != 0) {
      impl_->presentation.depth_transform_version = transform.version;
    } else if (!transform.control_points.empty()) {
      impl_->presentation.depth_transform_version = std::max<std::uint64_t>(
          1, impl_->presentation.depth_transform_version);
      impl_->presentation.depth_transform_map.version =
          impl_->presentation.depth_transform_version;
    }
  }
  return *this;
}

ScenePresentationBuilder &ScenePresentationBuilder::add_curve_layer(
    const CurveLayerSpec &layer) noexcept {
  if (impl_ == nullptr || impl_->allocation_failed) {
    return *this;
  }
  try {
    impl_->presentation.curve_layers.push_back(layer);
  } catch (...) {
    impl_->allocation_failed = true;
  }
  return *this;
}

ScenePresentationBuilder &
ScenePresentationBuilder::add_pattern(const PatternDefinition &pattern) noexcept {
  if (impl_ == nullptr || impl_->allocation_failed) {
    return *this;
  }
  try {
    impl_->presentation.patterns.push_back(pattern);
  } catch (...) {
    impl_->allocation_failed = true;
  }
  return *this;
}

ScenePresentationBuilder &ScenePresentationBuilder::add_interval_layer(
    const IntervalLayerSpec &layer) noexcept {
  if (impl_ == nullptr || impl_->allocation_failed) {
    return *this;
  }
  try {
    impl_->presentation.interval_layers.push_back(layer);
  } catch (...) {
    impl_->allocation_failed = true;
  }
  return *this;
}

ScenePresentationBuilder &
ScenePresentationBuilder::add_crossover_fill_layer(
    const CrossoverFillLayerSpec &layer) noexcept {
  if (impl_ == nullptr || impl_->allocation_failed) {
    return *this;
  }
  try {
    impl_->presentation.crossover_fill_layers.push_back(layer);
  } catch (...) {
    impl_->allocation_failed = true;
  }
  return *this;
}

ScenePresentationBuilder &
ScenePresentationBuilder::add_image_layer(const ImageLayerSpec &layer) noexcept {
  if (impl_ == nullptr || impl_->allocation_failed) {
    return *this;
  }
  try {
    impl_->presentation.image_layers.push_back(layer);
  } catch (...) {
    impl_->allocation_failed = true;
  }
  return *this;
}

ScenePresentationBuilder &ScenePresentationBuilder::add_marker_layer(
    const MarkerLayerSpec &layer) noexcept {
  if (impl_ == nullptr || impl_->allocation_failed) {
    return *this;
  }
  try {
    impl_->presentation.marker_layers.push_back(layer);
  } catch (...) {
    impl_->allocation_failed = true;
  }
  return *this;
}

ScenePresentationBuilder &ScenePresentationBuilder::add_symbol_layer(
    const SymbolLayerSpec &layer) noexcept {
  if (impl_ == nullptr || impl_->allocation_failed) {
    return *this;
  }
  try {
    impl_->presentation.symbol_layers.push_back(layer);
  } catch (...) {
    impl_->allocation_failed = true;
  }
  return *this;
}

ScenePresentationBuilder &ScenePresentationBuilder::add_text_layer(
    const TextLayerSpec &layer) noexcept {
  if (impl_ == nullptr || impl_->allocation_failed) {
    return *this;
  }
  try {
    impl_->presentation.text_layers.push_back(layer);
  } catch (...) {
    impl_->allocation_failed = true;
  }
  return *this;
}

ScenePresentationBuilder &ScenePresentationBuilder::add_custom_layer(
    const CustomLayerSpec &layer) noexcept {
  if (impl_ == nullptr || impl_->allocation_failed) {
    return *this;
  }
  try {
    impl_->presentation.custom_layers.push_back(layer);
  } catch (...) {
    impl_->allocation_failed = true;
  }
  return *this;
}

ScenePresentation ScenePresentationBuilder::build() const noexcept {
  if (impl_ == nullptr || impl_->allocation_failed) {
    return {};
  }
  try {
    return ScenePresentation{
        std::make_shared<ScenePresentation::Impl>(impl_->presentation)};
  } catch (...) {
    return {};
  }
}

struct PreparedScene::Impl {
  struct PickPrimitive {
    std::uint64_t first_point{};
    std::uint64_t second_point{};
  };

  struct CurvePickIndex {
    std::uint64_t layer_index{};
    double bin_height{};
    std::vector<PickPrimitive> primitives;
    std::vector<std::vector<std::uint64_t>> bins;
  };

  EntityId document_id;
  DocumentRevision document_revision;
  Millimetres physical_width;
  Millimetres physical_height;
  DepthDomain reference_depth_domain;
  std::string reference_depth_unit;
  double reference_depth_top{};
  double reference_depth_bottom{};
  std::string font_asset_fingerprint;
  PresentationVersion presentation_version;
  std::uint64_t depth_transform_version{};
  std::vector<PreparedTrack> tracks;
  std::vector<PreparedCurveLayer> curve_layers;
  std::vector<PreparedCurveSegment> curve_segments;
  std::vector<PreparedCurvePoint> curve_points;
  std::vector<CurvePickIndex> curve_pick_indices;
  std::vector<PatternDefinition> patterns;
  std::vector<PreparedIntervalLayer> interval_layers;
  std::vector<PreparedInterval> intervals;
  std::vector<PreparedFillLayer> fill_layers;
  std::vector<PreparedFillRegion> fill_regions;
  std::vector<PreparedFillVertex> fill_vertices;
  std::vector<PreparedFillTriangle> fill_triangles;
  std::vector<PreparedImageLayer> image_layers;
  std::vector<PreparedImageTile> image_tiles;
  std::vector<PreparedMarkerLayer> marker_layers;
  std::vector<PreparedMarker> markers;
  std::vector<PreparedSymbolLayer> symbol_layers;
  std::vector<PreparedSymbol> symbols;
  std::vector<PreparedTextLayer> text_layers;
  std::vector<PreparedTextRun> text_runs;
  std::vector<PreparedGlyph> glyphs;
  std::vector<PreparedTextFont> text_fonts;
  std::vector<PreparedGlyphOutline> glyph_outlines;
  std::vector<OutlineCommand> outline_commands;
  std::vector<SceneTextIssue> text_issues;
  std::vector<SceneValueIssue> value_issues;
  std::vector<PreparedTrackHeaderEntry> track_header_entries;
  std::vector<PreparedCustomLayer> custom_layers;
  std::vector<PreparedCustomPrimitive> custom_primitives;
  std::vector<PhysicalPoint> custom_vertices;
  std::vector<PreparedCustomClipPath> custom_clip_paths;
};

// ---- Crossover fill geometry (rendering.md section 6) ---------------------
namespace {

// Two curve layers map to track-x via their own TrackScaleSpec. A crossover
// fill encloses the region between them where one is to the right of the
// other, between consecutive crossings of their mapped-x polylines. The
// boundary is computed from mapped x-coordinates only (never raw values),
// and breaks wherever either curve is missing a sample.

// One mapped sample of a curve, as needed for crossover math: the scene
// position and the reference depth it came from.
struct AlignedSample {
  double left{};
  double top{};
  double reference_depth{};
  // Which contiguous valid run (prepared segment) this sample belongs to.
  // Two samples with different runs straddle a missing-value gap and must
  // NOT be interpolated across (criterion 3: no crossing Null/QC Invalid).
  std::uint32_t run{};
};

// Collects the valid mapped points of one prepared curve layer, in depth
// order, as AlignedSamples tagged with their source run. Each prepared
// segment is one run; concatenating runs preserves the gaps between them.
[[nodiscard]] std::vector<AlignedSample>
collect_layer_samples(std::span<const PreparedCurveSegment> segments,
                      std::span<const PreparedCurvePoint> points,
                      const PreparedCurveLayer &layer) {
  std::vector<AlignedSample> samples;
  for (std::uint64_t s = 0; s < layer.segment_count; ++s) {
    const auto &segment =
        segments[static_cast<std::size_t>(layer.first_segment + s)];
    for (std::uint64_t p = 0; p < segment.point_count; ++p) {
      const auto &point =
          points[static_cast<std::size_t>(segment.first_point + p)];
      samples.push_back(AlignedSample{
          .left = point.position.left.value,
          .top = point.position.top.value,
          .reference_depth = point.reference_depth,
          .run = static_cast<std::uint32_t>(s),
      });
    }
  }
  std::sort(samples.begin(), samples.end(),
            [](const AlignedSample &a, const AlignedSample &b) {
              return a.reference_depth < b.reference_depth;
            });
  return samples;
}

// Linear interpolation of the lower curve's mapped left at a given depth,
// within one contiguous valid run of lower samples. Returns nullopt when
// the depth lies outside the run OR between two runs (i.e. across a missing
// gap), so the caller breaks the region instead of bridging the gap.
[[nodiscard]] std::optional<double>
interpolate_left_at_depth(const std::vector<AlignedSample> &lower,
                          std::size_t run_begin, std::size_t run_end,
                          std::size_t &cursor, double depth) {
  if (run_begin >= run_end || run_end > lower.size()) {
    return std::nullopt;
  }
  if (depth < lower[run_begin].reference_depth ||
      depth > lower[run_end - 1].reference_depth) {
    return std::nullopt;
  }
  if (cursor < run_begin) {
    cursor = run_begin;
  }
  // Both arrays are depth-sorted: advance monotonically to the last sample
  // whose next neighbour is still strictly shallower than `depth`.
  while (cursor + 1 < run_end &&
         lower[cursor + 1].reference_depth < depth) {
    ++cursor;
  }
  if (cursor + 1 >= run_end) {
    // A single-sample run has no bracketing pair (same as the linear scan).
    return std::nullopt;
  }
  const auto &a = lower[cursor];
  const auto &b = lower[cursor + 1];
  if (depth < a.reference_depth || depth > b.reference_depth) {
    return std::nullopt;
  }
  // Do not interpolate across a missing-value gap: the bracketing pair
  // must belong to the same run.
  if (a.run != b.run) {
    return std::nullopt;
  }
  const auto span = b.reference_depth - a.reference_depth;
  if (span <= 0.0) {
    return a.left; // repeated depth: take the lower sample
  }
  const auto t = (depth - a.reference_depth) / span;
  return a.left + t * (b.left - a.left);
}

// One closed region between two crossings: the upper polyline samples
// (upper-to-the-right of lower) then the lower edge reversed.
struct CrossoverRegion {
  std::vector<PhysicalPoint> ring;
  double top_reference_depth{};
  double bottom_reference_depth{};
};

// Build the regions for one crossover fill layer. `upper` provides the
// depth grid; `lower` is interpolated onto it. A region is emitted wherever
// the upper-left-minus-lower-left stays non-negative between crossings
// (CrossoverFillRule::upper_minus_lower).
[[nodiscard]] std::optional<std::vector<CrossoverRegion>>
build_crossover_regions(const std::vector<AlignedSample> &upper,
                        const std::vector<AlignedSample> &lower,
                        std::stop_token stop_token) {
  std::vector<CrossoverRegion> regions;
  if (upper.empty() || lower.empty()) {
    return regions;
  }
  const auto lower_run_begin = std::size_t{0};
  const auto lower_run_end = lower.size();
  auto lower_cursor = lower_run_begin;

  struct RingPoint {
    PhysicalPoint position{};
    double reference_depth{};
  };
  std::vector<RingPoint> upper_run;
  std::vector<RingPoint> lower_run;
  std::optional<double> last_diff_sign;

  const auto flush_region = [&](double region_top_depth,
                                double region_bottom_depth) {
    if (upper_run.size() < 2 || lower_run.size() < 2) {
      upper_run.clear();
      lower_run.clear();
      return;
    }
    CrossoverRegion region;
    region.top_reference_depth = region_top_depth;
    region.bottom_reference_depth = region_bottom_depth;
    region.ring.reserve(upper_run.size() + lower_run.size());
    for (const auto &p : upper_run) {
      region.ring.push_back(p.position);
    }
    for (auto it = lower_run.rbegin(); it != lower_run.rend(); ++it) {
      region.ring.push_back(it->position);
    }
    if (region.ring.size() >= 3) {
      regions.push_back(std::move(region));
    }
    upper_run.clear();
    lower_run.clear();
  };

  for (std::size_t ui = 0; ui < upper.size(); ++ui) {
    if ((ui & 4095U) == 0U && stop_token.stop_requested()) {
      return std::nullopt;
    }
    const auto &u = upper[ui];
    const auto lower_left = interpolate_left_at_depth(lower, lower_run_begin,
                                                      lower_run_end,
                                                      lower_cursor,
                                                      u.reference_depth);
    if (!lower_left.has_value()) {
      if (!upper_run.empty()) {
        flush_region(upper_run.front().reference_depth,
                     upper_run.back().reference_depth);
      }
      last_diff_sign.reset();
      continue;
    }
    const auto diff = u.left - *lower_left;
    const auto sign = diff > 0.0 ? 1.0 : (diff < 0.0 ? -1.0 : 0.0);
    if (!last_diff_sign.has_value()) {
      if (sign > 0.0) {
        upper_run.push_back(RingPoint{PhysicalPoint{.left = Millimetres{u.left},
                                                    .top = Millimetres{u.top}},
                                      u.reference_depth});
        lower_run.push_back(
            RingPoint{PhysicalPoint{.left = Millimetres{*lower_left},
                                    .top = Millimetres{u.top}},
                      u.reference_depth});
        last_diff_sign = sign;
      }
      continue;
    }
    if (sign >= 0.0) {
      upper_run.push_back(RingPoint{PhysicalPoint{.left = Millimetres{u.left},
                                                  .top = Millimetres{u.top}},
                                    u.reference_depth});
      lower_run.push_back(
          RingPoint{PhysicalPoint{.left = Millimetres{*lower_left},
                                  .top = Millimetres{u.top}},
                    u.reference_depth});
      last_diff_sign = sign > 0.0 ? sign : *last_diff_sign;
    } else {
      // Sign flipped: a crossing closed the region.
      flush_region(upper_run.empty() ? u.reference_depth
                                     : upper_run.front().reference_depth,
                   u.reference_depth);
      last_diff_sign.reset();
    }
  }
  if (!upper_run.empty()) {
    flush_region(upper_run.front().reference_depth,
                 upper_run.back().reference_depth);
  }
  return regions;
}

} // namespace

PreparedScene::PreparedScene() = default;
PreparedScene::~PreparedScene() = default;
PreparedScene::PreparedScene(const PreparedScene &) = default;
PreparedScene &PreparedScene::operator=(const PreparedScene &) = default;
PreparedScene::PreparedScene(PreparedScene &&) noexcept = default;
PreparedScene &PreparedScene::operator=(PreparedScene &&) noexcept = default;

PreparedScene::PreparedScene(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

EntityId PreparedScene::document_id() const noexcept {
  return impl_ == nullptr ? EntityId{} : impl_->document_id;
}

DocumentRevision PreparedScene::document_revision() const noexcept {
  return impl_ == nullptr ? DocumentRevision{} : impl_->document_revision;
}

Millimetres PreparedScene::physical_width() const noexcept {
  return impl_ == nullptr ? Millimetres{} : impl_->physical_width;
}

Millimetres PreparedScene::physical_height() const noexcept {
  return impl_ == nullptr ? Millimetres{} : impl_->physical_height;
}

ReferenceDepthRange PreparedScene::reference_depth_range() const noexcept {
  return impl_ == nullptr ? ReferenceDepthRange{}
                          : ReferenceDepthRange{
                                .domain = impl_->reference_depth_domain,
                                .unit = impl_->reference_depth_unit,
                                .top = impl_->reference_depth_top,
                                .bottom = impl_->reference_depth_bottom,
                            };
}

std::string_view PreparedScene::font_asset_fingerprint() const noexcept {
  return impl_ == nullptr ? std::string_view{}
                          : std::string_view{impl_->font_asset_fingerprint};
}

PresentationVersion PreparedScene::presentation_version() const noexcept {
  return impl_ == nullptr ? PresentationVersion{}
                          : impl_->presentation_version;
}

DepthTransformDescriptor
PreparedScene::depth_transform() const noexcept {
  return impl_ == nullptr
             ? DepthTransformDescriptor{}
             : DepthTransformDescriptor{
                   .domain = impl_->reference_depth_domain,
                   .unit = impl_->reference_depth_unit,
                   .reference_top = impl_->reference_depth_top,
                   .reference_bottom = impl_->reference_depth_bottom,
                   .version = impl_->depth_transform_version,
               };
}

std::span<const PreparedTrack> PreparedScene::tracks() const noexcept {
  return impl_ == nullptr ? std::span<const PreparedTrack>{}
                          : std::span<const PreparedTrack>{impl_->tracks};
}

std::span<const PreparedCurveLayer>
PreparedScene::curve_layers() const noexcept {
  return impl_ == nullptr
             ? std::span<const PreparedCurveLayer>{}
             : std::span<const PreparedCurveLayer>{impl_->curve_layers};
}

std::span<const PreparedCurveSegment>
PreparedScene::curve_segments() const noexcept {
  return impl_ == nullptr
             ? std::span<const PreparedCurveSegment>{}
             : std::span<const PreparedCurveSegment>{impl_->curve_segments};
}

std::span<const PreparedCurvePoint>
PreparedScene::curve_points() const noexcept {
  return impl_ == nullptr
             ? std::span<const PreparedCurvePoint>{}
             : std::span<const PreparedCurvePoint>{impl_->curve_points};
}

std::span<const PatternDefinition>
PreparedScene::patterns() const noexcept {
  return impl_ == nullptr
             ? std::span<const PatternDefinition>{}
             : std::span<const PatternDefinition>{impl_->patterns};
}

std::span<const PreparedIntervalLayer>
PreparedScene::interval_layers() const noexcept {
  return impl_ == nullptr
             ? std::span<const PreparedIntervalLayer>{}
             : std::span<const PreparedIntervalLayer>{impl_->interval_layers};
}

std::span<const PreparedInterval> PreparedScene::intervals() const noexcept {
  return impl_ == nullptr
             ? std::span<const PreparedInterval>{}
             : std::span<const PreparedInterval>{impl_->intervals};
}

std::span<const PreparedFillLayer>
PreparedScene::fill_layers() const noexcept {
  return impl_ == nullptr
             ? std::span<const PreparedFillLayer>{}
             : std::span<const PreparedFillLayer>{impl_->fill_layers};
}

std::span<const PreparedFillRegion>
PreparedScene::fill_regions() const noexcept {
  return impl_ == nullptr
             ? std::span<const PreparedFillRegion>{}
             : std::span<const PreparedFillRegion>{impl_->fill_regions};
}

std::span<const PreparedFillVertex>
PreparedScene::fill_vertices() const noexcept {
  return impl_ == nullptr
             ? std::span<const PreparedFillVertex>{}
             : std::span<const PreparedFillVertex>{impl_->fill_vertices};
}

std::span<const PreparedFillTriangle>
PreparedScene::fill_triangles() const noexcept {
  return impl_ == nullptr
             ? std::span<const PreparedFillTriangle>{}
             : std::span<const PreparedFillTriangle>{impl_->fill_triangles};
}

std::span<const PreparedImageLayer>
PreparedScene::image_layers() const noexcept {
  return impl_ == nullptr
             ? std::span<const PreparedImageLayer>{}
             : std::span<const PreparedImageLayer>{impl_->image_layers};
}

std::span<const PreparedImageTile>
PreparedScene::image_tiles() const noexcept {
  return impl_ == nullptr
             ? std::span<const PreparedImageTile>{}
             : std::span<const PreparedImageTile>{impl_->image_tiles};
}

std::span<const PreparedMarkerLayer>
PreparedScene::marker_layers() const noexcept {
  return impl_ == nullptr
             ? std::span<const PreparedMarkerLayer>{}
             : std::span<const PreparedMarkerLayer>{impl_->marker_layers};
}

std::span<const PreparedMarker> PreparedScene::markers() const noexcept {
  return impl_ == nullptr
             ? std::span<const PreparedMarker>{}
             : std::span<const PreparedMarker>{impl_->markers};
}

std::span<const PreparedSymbolLayer>
PreparedScene::symbol_layers() const noexcept {
  return impl_ == nullptr
             ? std::span<const PreparedSymbolLayer>{}
             : std::span<const PreparedSymbolLayer>{impl_->symbol_layers};
}

std::span<const PreparedSymbol> PreparedScene::symbols() const noexcept {
  return impl_ == nullptr
             ? std::span<const PreparedSymbol>{}
             : std::span<const PreparedSymbol>{impl_->symbols};
}

std::span<const PreparedTextLayer> PreparedScene::text_layers() const noexcept {
  return impl_ == nullptr
             ? std::span<const PreparedTextLayer>{}
             : std::span<const PreparedTextLayer>{impl_->text_layers};
}

std::span<const PreparedTextRun> PreparedScene::text_runs() const noexcept {
  return impl_ == nullptr
             ? std::span<const PreparedTextRun>{}
             : std::span<const PreparedTextRun>{impl_->text_runs};
}

std::span<const PreparedGlyph> PreparedScene::glyphs() const noexcept {
  return impl_ == nullptr
             ? std::span<const PreparedGlyph>{}
             : std::span<const PreparedGlyph>{impl_->glyphs};
}

std::span<const PreparedTextFont> PreparedScene::text_fonts() const noexcept {
  return impl_ == nullptr
             ? std::span<const PreparedTextFont>{}
             : std::span<const PreparedTextFont>{impl_->text_fonts};
}

std::span<const PreparedGlyphOutline>
PreparedScene::glyph_outlines() const noexcept {
  return impl_ == nullptr
             ? std::span<const PreparedGlyphOutline>{}
             : std::span<const PreparedGlyphOutline>{impl_->glyph_outlines};
}

std::span<const OutlineCommand>
PreparedScene::outline_commands() const noexcept {
  return impl_ == nullptr
             ? std::span<const OutlineCommand>{}
             : std::span<const OutlineCommand>{impl_->outline_commands};
}

std::span<const SceneTextIssue> PreparedScene::text_issues() const noexcept {
  return impl_ == nullptr
             ? std::span<const SceneTextIssue>{}
             : std::span<const SceneTextIssue>{impl_->text_issues};
}

std::span<const SceneValueIssue>
PreparedScene::value_issues() const noexcept {
  return impl_ == nullptr
             ? std::span<const SceneValueIssue>{}
             : std::span<const SceneValueIssue>{impl_->value_issues};
}

std::span<const PreparedTrackHeaderEntry>
PreparedScene::track_header_entries() const noexcept {
  return impl_ == nullptr
             ? std::span<const PreparedTrackHeaderEntry>{}
             : std::span<const PreparedTrackHeaderEntry>{
                   impl_->track_header_entries};
}

std::span<const PreparedCustomLayer>
PreparedScene::custom_layers() const noexcept {
  return impl_ == nullptr
             ? std::span<const PreparedCustomLayer>{}
             : std::span<const PreparedCustomLayer>{impl_->custom_layers};
}

std::span<const PreparedCustomPrimitive>
PreparedScene::custom_primitives() const noexcept {
  return impl_ == nullptr
             ? std::span<const PreparedCustomPrimitive>{}
             : std::span<const PreparedCustomPrimitive>{
                   impl_->custom_primitives};
}

std::span<const PhysicalPoint>
PreparedScene::custom_vertices() const noexcept {
  return impl_ == nullptr
             ? std::span<const PhysicalPoint>{}
             : std::span<const PhysicalPoint>{impl_->custom_vertices};
}

std::span<const PreparedCustomClipPath>
PreparedScene::custom_clip_paths() const noexcept {
  return impl_ == nullptr
             ? std::span<const PreparedCustomClipPath>{}
             : std::span<const PreparedCustomClipPath>{
                   impl_->custom_clip_paths};
}

std::optional<EntityId>
PreparedScene::track_id_for_layer(EntityId layer_id) const noexcept {
  if (impl_ == nullptr) {
    return std::nullopt;
  }
  for (const auto &layer : impl_->curve_layers) {
    if (layer.id == layer_id) {
      return layer.track_id;
    }
  }
  for (const auto &layer : impl_->interval_layers) {
    if (layer.id == layer_id) {
      return layer.track_id;
    }
  }
  for (const auto &layer : impl_->fill_layers) {
    if (layer.id == layer_id) {
      return layer.track_id;
    }
  }
  for (const auto &layer : impl_->image_layers) {
    if (layer.id == layer_id) {
      return layer.track_id;
    }
  }
  for (const auto &layer : impl_->marker_layers) {
    if (layer.id == layer_id) {
      return layer.track_id;
    }
  }
  for (const auto &layer : impl_->symbol_layers) {
    if (layer.id == layer_id) {
      return layer.track_id;
    }
  }
  for (const auto &layer : impl_->text_layers) {
    if (layer.id == layer_id) {
      return layer.track_id;
    }
  }
  for (const auto &layer : impl_->custom_layers) {
    if (layer.id == layer_id) {
      return layer.track_id;
    }
  }
  return std::nullopt;
}

std::optional<CurvePick>
PreparedScene::pick_curve(const CurvePickQuery &query) const noexcept {
  const auto &position = query.scene_position;
  if (impl_ == nullptr || !std::isfinite(position.left.value) ||
      !std::isfinite(position.top.value) ||
      !std::isfinite(query.tolerance.value) || query.tolerance.value < 0.0 ||
      !std::isfinite(
          query.horizontal_device_independent_pixels_per_millimetre) ||
      query.horizontal_device_independent_pixels_per_millimetre <= 0.0 ||
      !std::isfinite(query.vertical_device_independent_pixels_per_millimetre) ||
      query.vertical_device_independent_pixels_per_millimetre <= 0.0) {
    return std::nullopt;
  }
  const auto vertical_tolerance_millimetres =
      query.tolerance.value /
      query.vertical_device_independent_pixels_per_millimetre;
  if (!std::isfinite(vertical_tolerance_millimetres)) {
    return std::nullopt;
  }

  const auto point_distance = [&](const PreparedCurvePoint &point) {
    return std::hypot(
        (point.position.left.value - position.left.value) *
            query.horizontal_device_independent_pixels_per_millimetre,
        (point.position.top.value - position.top.value) *
            query.vertical_device_independent_pixels_per_millimetre);
  };
  const auto segment_distance =
      [&](const PreparedCurvePoint &first,
          const PreparedCurvePoint &second) -> std::pair<double, double> {
    const auto delta_x =
        (second.position.left.value - first.position.left.value) *
        query.horizontal_device_independent_pixels_per_millimetre;
    const auto delta_y =
        (second.position.top.value - first.position.top.value) *
        query.vertical_device_independent_pixels_per_millimetre;
    const auto length_squared = delta_x * delta_x + delta_y * delta_y;
    if (length_squared == 0.0) {
      return {point_distance(first), 0.0};
    }
    const auto projected =
        ((position.left.value - first.position.left.value) *
             query.horizontal_device_independent_pixels_per_millimetre *
             delta_x +
         (position.top.value - first.position.top.value) *
             query.vertical_device_independent_pixels_per_millimetre *
             delta_y) /
        length_squared;
    const auto parameter = std::clamp(projected, 0.0, 1.0);
    const auto closest_x =
        first.position.left.value *
            query.horizontal_device_independent_pixels_per_millimetre +
        parameter * delta_x;
    const auto closest_y =
        first.position.top.value *
            query.vertical_device_independent_pixels_per_millimetre +
        parameter * delta_y;
    return {
        std::hypot(
            closest_x -
                position.left.value *
                    query.horizontal_device_independent_pixels_per_millimetre,
            closest_y -
                position.top.value *
                    query.vertical_device_independent_pixels_per_millimetre),
        parameter};
  };

  for (auto index_iterator = impl_->curve_pick_indices.rbegin();
       index_iterator != impl_->curve_pick_indices.rend(); ++index_iterator) {
    const auto &pick_index = *index_iterator;
    const auto &layer =
        impl_->curve_layers[static_cast<std::size_t>(pick_index.layer_index)];
    const auto track = std::find_if(impl_->tracks.begin(), impl_->tracks.end(),
                                    [&](const PreparedTrack &candidate) {
                                      return candidate.id == layer.track_id;
                                    });
    if (track == impl_->tracks.end() ||
        position.left.value < track->clip.left.value ||
        position.top.value < track->clip.top.value ||
        position.left.value >
            track->clip.left.value + track->clip.width.value ||
        position.top.value > track->clip.top.value + track->clip.height.value) {
      continue;
    }

    const PreparedCurvePoint *best_point = nullptr;
    auto best_distance = std::numeric_limits<double>::infinity();
    const auto bin_for = [&](double top) {
      if (top <= 0.0) {
        return std::size_t{0};
      }
      if (top >= impl_->physical_height.value) {
        return pick_index.bins.size() - std::size_t{1};
      }
      return std::min(static_cast<std::size_t>(top / pick_index.bin_height),
                      pick_index.bins.size() - std::size_t{1});
    };
    const auto first_bin =
        bin_for(position.top.value - vertical_tolerance_millimetres);
    const auto last_bin =
        bin_for(position.top.value + vertical_tolerance_millimetres);
    for (auto bin = first_bin; bin <= last_bin; ++bin) {
      for (const auto primitive_index : pick_index.bins[bin]) {
        const auto &primitive =
            pick_index.primitives[static_cast<std::size_t>(primitive_index)];
        const auto &first =
            impl_
                ->curve_points[static_cast<std::size_t>(primitive.first_point)];
        const auto &second = impl_->curve_points[static_cast<std::size_t>(
            primitive.second_point)];
        if (primitive.first_point == primitive.second_point) {
          const auto distance = point_distance(first);
          if (distance < best_distance) {
            best_distance = distance;
            best_point = &first;
          }
          continue;
        }
        const auto [distance, parameter] = segment_distance(first, second);
        if (distance < best_distance) {
          best_distance = distance;
          best_point = parameter <= 0.5 ? &first : &second;
        }
      }
    }
    if (best_point != nullptr && best_distance <= query.tolerance.value) {
      return CurvePick{
          .document_id = impl_->document_id,
          .layer_id = layer.id,
          .track_id = layer.track_id,
          .curve_id = layer.curve_id,
          .sample_index = best_point->sample_index,
          .reference_depth = best_point->reference_depth,
          .display_depth = best_point->display_depth != 0.0 ||
                                   best_point->reference_depth == 0.0
                               ? best_point->display_depth
                               : best_point->reference_depth,
          .value = best_point->value,
          .distance = DeviceIndependentPixels{best_distance},
      };
    }
  }
  return std::nullopt;
}

std::optional<FillPick>
PreparedScene::pick_fill(const FillPickQuery &query) const noexcept {
  if (impl_ == nullptr) {
    return std::nullopt;
  }
  const auto position = query.scene_position;
  // Iterate fill layers in reverse z order so the topmost layer wins.
  for (auto layer_it = impl_->fill_layers.rbegin();
       layer_it != impl_->fill_layers.rend(); ++layer_it) {
    const auto track =
        std::find_if(impl_->tracks.begin(), impl_->tracks.end(),
                     [&](const PreparedTrack &candidate) {
                       return candidate.id == layer_it->track_id;
                     });
    if (track == impl_->tracks.end() ||
        position.left.value < track->clip.left.value ||
        position.top.value < track->clip.top.value ||
        position.left.value >
            track->clip.left.value + track->clip.width.value ||
        position.top.value >
            track->clip.top.value + track->clip.height.value) {
      continue;
    }
    for (auto region_index = layer_it->first_region + layer_it->region_count;
         region_index > layer_it->first_region; --region_index) {
      const auto &region =
          impl_->fill_regions[static_cast<std::size_t>(region_index - 1)];
      const auto vertex_begin =
          impl_->fill_vertices.begin() +
          static_cast<std::ptrdiff_t>(region.first_vertex);
      const auto vertex_end = vertex_begin +
                              static_cast<std::ptrdiff_t>(region.vertex_count);
      std::vector<detail::PolygonPoint> ring;
      ring.reserve(static_cast<std::size_t>(vertex_end - vertex_begin));
      for (auto v = vertex_begin; v != vertex_end; ++v) {
        ring.push_back(
            detail::PolygonPoint{.x = v->position.left.value,
                                 .y = v->position.top.value});
      }
      if (!detail::point_in_polygon(ring, position.left.value,
                                    position.top.value)) {
        continue;
      }
      // Resolve both dependent curve identities from their prepared layers.
      const auto resolve_curve_id = [&](EntityId layer_id) -> EntityId {
        for (const auto &cl : impl_->curve_layers) {
          if (cl.id == layer_id) {
            return cl.curve_id;
          }
        }
        return EntityId{};
      };
      return FillPick{
          .layer_id = layer_it->id,
          .upper_curve_layer_id = region.upper_curve_layer_id,
          .lower_curve_layer_id = region.lower_curve_layer_id,
          .upper_curve_id = resolve_curve_id(region.upper_curve_layer_id),
          .lower_curve_id = resolve_curve_id(region.lower_curve_layer_id),
          .reference_depth =
              0.5 * (region.top_reference_depth + region.bottom_reference_depth),
      };
    }
  }
  return std::nullopt;
}

std::optional<ImagePick>
PreparedScene::pick_image(const ImagePickQuery &query) const noexcept {
  if (impl_ == nullptr) {
    return std::nullopt;
  }
  const auto position = query.scene_position;
  const auto depth_top = impl_->reference_depth_top;
  const auto depth_span = impl_->reference_depth_bottom - depth_top;
  // Iterate image layers in reverse z order so the topmost layer wins.
  for (auto layer_it = impl_->image_layers.rbegin();
       layer_it != impl_->image_layers.rend(); ++layer_it) {
    for (auto tile_index = layer_it->first_tile + layer_it->tile_count;
         tile_index > layer_it->first_tile; --tile_index) {
      const auto &tile =
          impl_->image_tiles[static_cast<std::size_t>(tile_index - 1)];
      const auto left = tile.rect.left.value;
      const auto right = left + tile.rect.width.value;
      const auto top = tile.rect.top.value;
      const auto bottom = top + tile.rect.height.value;
      if (position.left.value < left || position.left.value > right ||
          position.top.value < top || position.top.value > bottom) {
        continue;
      }
      // Invert the depth mapping (depth = top + (top_mm / height) * span) to
      // report the reference depth at the hit point.
      auto reference_depth = depth_top;
      if (impl_->physical_height.value > 0.0 && depth_span > 0.0) {
        reference_depth =
            depth_top + (position.top.value / impl_->physical_height.value) *
                            depth_span;
      }
      return ImagePick{
          .layer_id = layer_it->id,
          .image_source_id = tile.image_source_id,
          .level = tile.level,
          .row = tile.row,
          .col = tile.col,
          .reference_depth = reference_depth,
      };
    }
  }
  return std::nullopt;
}

std::optional<CustomPick>
PreparedScene::pick_custom(const CustomPickQuery &query) const noexcept {
  if (impl_ == nullptr) {
    return std::nullopt;
  }
  const auto position = query.scene_position;
  const auto depth_top = impl_->reference_depth_top;
  const auto depth_span = impl_->reference_depth_bottom - depth_top;
  // Tolerance in scene millimetres (mirrors pick_curve's per-axis conversion).
  const auto h_ppmm =
      query.horizontal_device_independent_pixels_per_millimetre > 0.0
          ? query.horizontal_device_independent_pixels_per_millimetre
          : 1.0;
  const auto v_ppmm =
      query.vertical_device_independent_pixels_per_millimetre > 0.0
          ? query.vertical_device_independent_pixels_per_millimetre
          : 1.0;
  const auto tol_h = query.tolerance.value / h_ppmm;
  const auto tol_v = query.tolerance.value / v_ppmm;
  // Iterate custom layers in reverse z order so the topmost layer wins.
  for (auto layer_it = impl_->custom_layers.rbegin();
       layer_it != impl_->custom_layers.rend(); ++layer_it) {
    // Respect an optional layer-local clip path.
    if (layer_it->clip_path_index != no_clip) {
      const auto &clip =
          impl_->custom_clip_paths[static_cast<std::size_t>(
              layer_it->clip_path_index)];
      std::vector<detail::PolygonPoint> ring;
      ring.reserve(clip.points.size());
      for (const auto &point : clip.points) {
        ring.push_back(
            detail::PolygonPoint{.x = point.left.value, .y = point.top.value});
      }
      if (!detail::point_in_polygon(ring, position.left.value,
                                    position.top.value)) {
        continue;
      }
    }
    for (auto primitive_index = layer_it->first_primitive +
                                layer_it->primitive_count;
         primitive_index > layer_it->first_primitive; --primitive_index) {
      const auto &primitive = impl_->custom_primitives[static_cast<std::size_t>(
          primitive_index - 1)];
      const auto left = primitive.bounds.left.value;
      const auto right = left + primitive.bounds.width.value;
      const auto top = primitive.bounds.top.value;
      const auto bottom = top + primitive.bounds.height.value;
      if (position.left.value < left - tol_h ||
          position.left.value > right + tol_h ||
          position.top.value < top - tol_v ||
          position.top.value > bottom + tol_v) {
        continue;
      }
      auto reference_depth = depth_top;
      if (impl_->physical_height.value > 0.0 && depth_span > 0.0) {
        reference_depth =
            depth_top + (position.top.value / impl_->physical_height.value) *
                            depth_span;
      }
      return CustomPick{
          .layer_id = layer_it->id,
          .source_id = primitive.source_id,
          .source_primitive_index = primitive.source_primitive_index,
          .kind = primitive.kind,
          .reference_depth = reference_depth,
      };
    }
  }
  return std::nullopt;
}

std::optional<Error> detail::ScenePreparer::preflight(
    const WellLogDocument &document,
    const ScenePresentation &presentation) noexcept {
  try {
    if (const auto xform = validate_depth_transform(
            presentation.depth_transform_map());
        xform.has_value()) {
      return *xform;
    }
    const auto depth_range = presentation.reference_depth_range();
    // When a non-identity Depth Transform is active, sampling axes must already
    // be in the presentation Reference Depth domain (no silent unit convert).
    if (!presentation.depth_transform_map().control_points.empty()) {
      for (const auto &axis : document.sampling_axes()) {
        if (axis.domain != depth_range.domain) {
          return presentation_error(axis.id);
        }
      }
    }
    const auto layer_count =
        presentation.curve_layers().size() +
        presentation.interval_layers().size() +
        presentation.image_layers().size() +
        presentation.marker_layers().size() +
        presentation.symbol_layers().size() + presentation.text_layers().size() +
        presentation.custom_layers().size();
    if (presentation.document_id() != document.id() ||
        depth_range.domain == DepthDomain::source_index ||
        depth_range.unit.empty() || !std::isfinite(depth_range.top) ||
        !std::isfinite(depth_range.bottom) ||
        depth_range.top == depth_range.bottom ||
        !std::isfinite(depth_range.bottom - depth_range.top) ||
        !std::isfinite(presentation.physical_height().value) ||
        presentation.physical_height().value <= 0.0 ||
        presentation.tracks().empty() || layer_count == 0) {
      return presentation_error(presentation.document_id());
    }

    std::unordered_set<EntityId, EntityIdHash> ids;
    const auto add_id = [&ids](EntityId id) { return !id.is_nil() && ids.insert(id).second; };
    if (!add_id(document.id())) {
      return presentation_error(document.id());
    }
    for (const auto &axis : document.sampling_axes()) {
      if (!add_id(axis.id)) {
        return presentation_error(axis.id);
      }
    }
    for (const auto &curve : document.curves()) {
      if (!add_id(curve.id)) {
        return presentation_error(curve.id);
      }
    }
    for (const auto &image : document.image_sources()) {
      if (!add_id(image.id)) {
        return presentation_error(image.id);
      }
    }
    for (const auto &source : document.custom_sources()) {
      if (!add_id(source.id)) {
        return presentation_error(source.id);
      }
    }
    for (const auto &interval : document.intervals()) {
      if (!add_id(interval.id)) {
        return presentation_error(interval.id);
      }
    }
    for (const auto &marker : document.markers()) {
      if (!add_id(marker.id)) {
        return presentation_error(marker.id);
      }
    }
    for (const auto &symbol : document.symbols()) {
      if (!add_id(symbol.id)) {
        return presentation_error(symbol.id);
      }
    }
    for (const auto &annotation : document.annotations()) {
      if (!add_id(annotation.id)) {
        return presentation_error(annotation.id);
      }
    }

    std::unordered_map<EntityId, const TrackSpec *, EntityIdHash> tracks;
    double total_width{};
    for (const auto &track : presentation.tracks()) {
      if (!add_id(track.id) || !std::isfinite(track.width.value) ||
          track.width.value <= 0.0 || !std::isfinite(track.header.height.value) ||
          track.header.height.value < 0.0 ||
          (track.header.height.value > 0.0 &&
           (!std::isfinite(track.header.font_size.value) ||
            track.header.font_size.value <= 0.0))) {
        return presentation_error(track.id);
      }
      total_width += track.width.value;
      if (!std::isfinite(total_width)) {
        return presentation_error(track.id);
      }
      tracks.emplace(track.id, &track);
    }

    std::unordered_map<EntityId, const TrackScaleSpec *, EntityIdHash> scales;
    for (const auto &scale : presentation.scales()) {
      if (!add_id(scale.id) || !tracks.contains(scale.track_id) ||
          !std::isfinite(scale.minimum) || !std::isfinite(scale.maximum) ||
          scale.minimum >= scale.maximum ||
          !std::isfinite(scale.maximum - scale.minimum) || scale.unit.empty() ||
          (scale.mode == ScaleMode::logarithmic && scale.minimum <= 0.0)) {
        return presentation_error(scale.id);
      }
      scales.emplace(scale.id, &scale);
    }

    std::unordered_map<EntityId, const CurveLayerSpec *, EntityIdHash>
        curve_layers;
    for (const auto &layer : presentation.curve_layers()) {
      const auto scale = scales.find(layer.scale_id);
      const auto curve = std::find_if(
          document.curves().begin(), document.curves().end(),
          [&layer](const Curve &candidate) { return candidate.id == layer.curve_id; });
      if (!add_id(layer.id) || !tracks.contains(layer.track_id) ||
          scale == scales.end() || scale->second->track_id != layer.track_id ||
          curve == document.curves().end() ||
          !std::isfinite(layer.line_width.value) || layer.line_width.value <= 0.0) {
        return presentation_error(layer.id);
      }
      const auto axis = std::find_if(
          document.sampling_axes().begin(), document.sampling_axes().end(),
          [&curve](const SamplingAxis &candidate) {
            return candidate.id == curve->sampling_axis_id;
          });
      if (axis == document.sampling_axes().end() ||
          axis->domain != depth_range.domain ||
          axis->domain == DepthDomain::source_index ||
          axis->unit != depth_range.unit || curve->unit != scale->second->unit) {
        return presentation_error(layer.id);
      }
      curve_layers.emplace(layer.id, &layer);
    }

    constexpr std::size_t maximum_pattern_primitives = 256;
    constexpr std::size_t maximum_polyline_points = 1024;
    constexpr double maximum_tile_extent_millimetres = 500.0;
    std::unordered_set<EntityId, EntityIdHash> pattern_ids;
    const auto point_valid = [](const PhysicalPoint &point) {
      return std::isfinite(point.left.value) && std::isfinite(point.top.value);
    };
    for (const auto &pattern : presentation.patterns()) {
      if (!add_id(pattern.id) || !std::isfinite(pattern.tile_width.value) ||
          pattern.tile_width.value <= 0.0 ||
          pattern.tile_width.value > maximum_tile_extent_millimetres ||
          !std::isfinite(pattern.tile_height.value) ||
          pattern.tile_height.value <= 0.0 ||
          pattern.tile_height.value > maximum_tile_extent_millimetres ||
          !std::isfinite(pattern.rotation_degrees) ||
          !std::isfinite(pattern.stroke_width.value) ||
          pattern.stroke_width.value <= 0.0 ||
          pattern.stroke_width.value > maximum_tile_extent_millimetres ||
          !point_valid(pattern.scene_anchor) ||
          pattern.primitives.size() > maximum_pattern_primitives) {
        return presentation_error(pattern.id);
      }
      for (const auto &primitive : pattern.primitives) {
        const auto valid = std::visit(
            [&point_valid](const auto &value) {
              using T = std::decay_t<decltype(value)>;
              if constexpr (std::is_same_v<T, PatternLine>) {
                return point_valid(value.from) && point_valid(value.to);
              } else if constexpr (std::is_same_v<T, PatternPolyline>) {
                return value.points.size() >= 2 &&
                       value.points.size() <= maximum_polyline_points &&
                       std::all_of(value.points.begin(), value.points.end(),
                                   point_valid);
              } else {
                return point_valid(value.center) &&
                       std::isfinite(value.radius.value) &&
                       value.radius.value > 0.0 &&
                       value.radius.value <= maximum_tile_extent_millimetres;
              }
            },
            primitive);
        if (!valid) {
          return presentation_error(pattern.id);
        }
      }
      pattern_ids.insert(pattern.id);
    }

    for (const auto &layer : presentation.interval_layers()) {
      if (!add_id(layer.id) || !tracks.contains(layer.track_id) ||
          (layer.draw_labels &&
           (!std::isfinite(layer.label_font_size.value) ||
            layer.label_font_size.value <= 0.0))) {
        return presentation_error(layer.id);
      }
    }
    if (!presentation.interval_layers().empty()) {
      for (const auto &interval : document.intervals()) {
        if (!interval.pattern_id.is_nil() &&
            !pattern_ids.contains(interval.pattern_id)) {
          return presentation_error(interval.id);
        }
      }
    }

    for (const auto &layer : presentation.crossover_fill_layers()) {
      const auto upper = curve_layers.find(layer.upper_curve_layer_id);
      const auto lower = curve_layers.find(layer.lower_curve_layer_id);
      const auto color_set = layer.fill_color.has_value();
      const auto pattern_set = layer.pattern_id.has_value() && !layer.pattern_id->is_nil();
      if (!add_id(layer.id) || !tracks.contains(layer.track_id) ||
          upper == curve_layers.end() || lower == curve_layers.end() ||
          upper->second->track_id != layer.track_id ||
          lower->second->track_id != layer.track_id ||
          upper->second->id == lower->second->id || color_set == pattern_set ||
          (pattern_set && !pattern_ids.contains(*layer.pattern_id))) {
        return presentation_error(layer.id);
      }
    }

    constexpr std::uint64_t maximum_image_dimension_px = 65536ULL;
    constexpr std::uint64_t maximum_image_pixels = 512ULL * 1024ULL * 1024ULL;
    for (const auto &layer : presentation.image_layers()) {
      const auto source = std::find_if(
          document.image_sources().begin(), document.image_sources().end(),
          [&layer](const ImageSource &candidate) {
            return candidate.id == layer.image_source_id;
          });
      if (!add_id(layer.id) || !tracks.contains(layer.track_id) ||
          source == document.image_sources().end() || source->width_px == 0 ||
          source->height_px == 0 ||
          source->width_px > maximum_image_dimension_px ||
          source->height_px > maximum_image_dimension_px || source->dpi == 0 ||
          source->reference_depth_bottom <= source->reference_depth_top ||
          !std::isfinite(source->reference_depth_top) ||
          !std::isfinite(source->reference_depth_bottom)) {
        return presentation_error(layer.id);
      }
      if (source->width_px * source->height_px > maximum_image_pixels) {
        return Error{.code = ErrorCode::invalid_image,
                     .severity = Severity::error,
                     .entity_id = source->id,
                     .message = MessageKey::image_pixels_exceed_limit,
                     .arguments = {}};
      }
    }

    constexpr std::size_t maximum_custom_primitives = 4096;
    constexpr std::size_t maximum_custom_polyline_points = 8192;
    constexpr std::size_t maximum_custom_vertices = 1 << 20;
    for (const auto &layer : presentation.custom_layers()) {
      const auto source = std::find_if(
          document.custom_sources().begin(), document.custom_sources().end(),
          [&layer](const CustomLayerSource &candidate) {
            return candidate.id == layer.custom_source_id;
          });
      if (!add_id(layer.id) || !tracks.contains(layer.track_id) ||
          source == document.custom_sources().end()) {
        return presentation_error(layer.id);
      }
      if (source->primitives.empty()) {
        return Error{.code = ErrorCode::invalid_custom_source,
                     .severity = Severity::error,
                     .entity_id = source->id,
                     .message = MessageKey::custom_source_empty,
                     .arguments = {}};
      }
      if (source->primitives.size() > maximum_custom_primitives) {
        return Error{.code = ErrorCode::invalid_custom_source,
                     .severity = Severity::error,
                     .entity_id = source->id,
                     .message = MessageKey::custom_source_primitives_exceed_limit,
                     .arguments = {}};
      }
      std::size_t total_vertices{};
      for (const auto &primitive : source->primitives) {
        const auto valid = std::visit(
            [&point_valid, &total_vertices](const auto &value) {
              using T = std::decay_t<decltype(value)>;
              if constexpr (std::is_same_v<T, CustomPolyline>) {
                return value.points.size() >= 2 &&
                       value.points.size() <= maximum_custom_polyline_points &&
                       std::isfinite(value.width.value) &&
                       value.width.value >= 0.0 &&
                       std::all_of(value.points.begin(), value.points.end(),
                                   point_valid);
              } else if constexpr (std::is_same_v<T, CustomTriangle>) {
                total_vertices += 3;
                return point_valid(value.a) && point_valid(value.b) &&
                       point_valid(value.c);
              } else if constexpr (std::is_same_v<T, CustomQuad>) {
                total_vertices += 6;
                return std::isfinite(value.rect.left.value) &&
                       std::isfinite(value.rect.top.value) &&
                       std::isfinite(value.rect.width.value) &&
                       std::isfinite(value.rect.height.value);
              } else {
                total_vertices += 24;
                return point_valid(value.center) &&
                       std::isfinite(value.size.value) && value.size.value > 0.0;
              }
            },
            primitive);
        if (!valid) {
          return presentation_error(layer.id);
        }
      }
      if (total_vertices > maximum_custom_vertices) {
        return Error{.code = ErrorCode::invalid_custom_source,
                     .severity = Severity::error,
                     .entity_id = source->id,
                     .message = MessageKey::custom_source_points_exceed_limit,
                     .arguments = {}};
      }
      if (source->clip.has_value() &&
          (source->clip->points.size() < 3 ||
           source->clip->points.size() > maximum_custom_polyline_points ||
           !std::all_of(source->clip->points.begin(), source->clip->points.end(),
                        point_valid))) {
        return presentation_error(layer.id);
      }
    }

    for (const auto &layer : presentation.marker_layers()) {
      if (!add_id(layer.id) || !tracks.contains(layer.track_id) ||
          !std::isfinite(layer.line_width.value) || layer.line_width.value <= 0.0 ||
          (layer.draw_labels &&
           (!std::isfinite(layer.label_font_size.value) ||
            layer.label_font_size.value <= 0.0))) {
        return presentation_error(layer.id);
      }
    }
    for (const auto &layer : presentation.symbol_layers()) {
      if (!add_id(layer.id) || !tracks.contains(layer.track_id) ||
          !std::isfinite(layer.symbol_size.value) || layer.symbol_size.value <= 0.0) {
        return presentation_error(layer.id);
      }
    }
    for (const auto &layer : presentation.text_layers()) {
      if (!add_id(layer.id) || !tracks.contains(layer.track_id)) {
        return presentation_error(layer.id);
      }
    }
    if (!presentation.text_layers().empty()) {
      for (const auto &annotation : document.annotations()) {
        if (annotation.anchor == AnnotationAnchor::track &&
            !tracks.contains(annotation.track_id)) {
          return presentation_error(annotation.id);
        }
      }
    }
    return std::nullopt;
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

Result<PreparedScene>
detail::ScenePreparer::prepare(const WellLogDocument &document,
                               const ScenePresentation &presentation,
                               TextEngine *text_engine) noexcept {
  return prepare_impl(document, presentation, nullptr, nullptr, nullptr,
                      nullptr, {}, text_engine);
}

Result<PreparedScene> detail::ScenePreparer::prepare(
    const WellLogDocument &document, const ScenePresentation &presentation,
    const CurveLodMap &curve_lods, const CurveLodQuery &query,
    std::stop_token stop_token, TextEngine *text_engine) noexcept {
  return prepare_impl(document, presentation, &curve_lods, &query, nullptr,
                      nullptr, stop_token, text_engine);
}

Result<PreparedScene> detail::ScenePreparer::prepare(
    const WellLogDocument &document, const ScenePresentation &presentation,
    const CurveLodMap &curve_lods, const CurveLodQuery &query,
    const ImagePyramidMap &image_pyramids,
    const ImagePyramidQuery &image_query, std::stop_token stop_token,
    TextEngine *text_engine) noexcept {
  return prepare_impl(document, presentation, &curve_lods, &query,
                      &image_pyramids, &image_query, stop_token, text_engine);
}

Result<PreparedScene> detail::ScenePreparer::prepare_impl(
    const WellLogDocument &document, const ScenePresentation &presentation,
    const CurveLodMap *curve_lods, const CurveLodQuery *query,
    const ImagePyramidMap *image_pyramids, const ImagePyramidQuery *image_query,
    std::stop_token stop_token, TextEngine *text_engine) noexcept {
  try {
    if (stop_token.stop_requested()) {
      return cancellation_error();
    }
    if (const auto validation = preflight(document, presentation);
        validation.has_value()) {
      return *validation;
    }
    const auto depth_range = presentation.reference_depth_range();
    const auto layer_count =
        presentation.curve_layers().size() +
        presentation.interval_layers().size() +
        presentation.image_layers().size() +
        presentation.marker_layers().size() +
        presentation.symbol_layers().size() + presentation.text_layers().size() +
        presentation.custom_layers().size();
    if (presentation.document_id() != document.id() ||
        depth_range.domain == DepthDomain::source_index ||
        depth_range.unit.empty() || !std::isfinite(depth_range.top) ||
        !std::isfinite(depth_range.bottom) ||
        depth_range.top == depth_range.bottom ||
        !std::isfinite(depth_range.bottom - depth_range.top) ||
        !std::isfinite(presentation.physical_height().value) ||
        presentation.physical_height().value <= 0.0 ||
        presentation.tracks().empty() || layer_count == 0) {
      return presentation_error(presentation.document_id());
    }

    std::unordered_set<EntityId, EntityIdHash> ids;
    ids.insert(document.id());
    for (const auto &axis : document.sampling_axes()) {
      ids.insert(axis.id);
    }
    for (const auto &curve : document.curves()) {
      ids.insert(curve.id);
    }
    std::unordered_map<EntityId, PhysicalRect, EntityIdHash> track_bounds;
    auto scene = std::make_shared<PreparedScene::Impl>();
    scene->document_id = document.id();
    scene->document_revision = document.revision();
    scene->physical_height = presentation.physical_height();
    scene->reference_depth_domain = depth_range.domain;
    scene->reference_depth_unit = std::string{depth_range.unit};
    scene->reference_depth_top = depth_range.top;
    scene->reference_depth_bottom = depth_range.bottom;
    scene->font_asset_fingerprint =
        std::string{presentation.font_asset_fingerprint()};
    scene->presentation_version = presentation.presentation_version();
    scene->depth_transform_version = presentation.depth_transform().version;
    scene->tracks.reserve(presentation.tracks().size());
    scene->curve_layers.reserve(presentation.curve_layers().size());

    // Track z-order owns horizontal layout order as well as render order. This
    // makes a TrackSpec patch observable in PreparedScene geometry rather than
    // preserving the builder's historical insertion order.
    std::vector<const TrackSpec *> ordered_tracks;
    ordered_tracks.reserve(presentation.tracks().size());
    for (const auto &track : presentation.tracks()) {
      ordered_tracks.push_back(&track);
    }
    order_layers_by_z(ordered_tracks);

    double left{};
    for (const auto *track_pointer : ordered_tracks) {
      const auto &track = *track_pointer;
      if (stop_token.stop_requested()) {
        return cancellation_error();
      }
      if (track.id.is_nil() || !ids.insert(track.id).second ||
          !std::isfinite(track.width.value) || track.width.value <= 0.0 ||
          !std::isfinite(track.header.height.value) ||
          track.header.height.value < 0.0 ||
          (track.header.height.value > 0.0 &&
           (!std::isfinite(track.header.font_size.value) ||
            track.header.font_size.value <= 0.0))) {
        return presentation_error(track.id);
      }
      const auto right = left + track.width.value;
      if (!std::isfinite(right)) {
        return presentation_error(track.id);
      }
      const auto bounds = PhysicalRect{
          .left = Millimetres{left},
          .top = Millimetres{0.0},
          .width = track.width,
          .height = presentation.physical_height(),
      };
      scene->tracks.push_back(PreparedTrack{
          .id = track.id,
          .bounds = bounds,
          .clip = bounds,
          .z_order = track.z_order,
      });
      track_bounds.emplace(track.id, bounds);
      left = right;
    }
    scene->physical_width = Millimetres{left};

    std::unordered_map<EntityId, const TrackScaleSpec *, EntityIdHash> scales;
    for (const auto &scale : presentation.scales()) {
      if (stop_token.stop_requested()) {
        return cancellation_error();
      }
      if (scale.id.is_nil() || !ids.insert(scale.id).second ||
          !track_bounds.contains(scale.track_id) ||
          !std::isfinite(scale.minimum) || !std::isfinite(scale.maximum) ||
          scale.minimum >= scale.maximum ||
          !std::isfinite(scale.maximum - scale.minimum) ||
          scale.unit.empty() ||
          (scale.mode == ScaleMode::logarithmic && scale.minimum <= 0.0)) {
        return presentation_error(scale.id);
      }
      scales.emplace(scale.id, &scale);
    }

    std::vector<const CurveLayerSpec *> ordered_layers;
    ordered_layers.reserve(presentation.curve_layers().size());
    for (const auto &layer : presentation.curve_layers()) {
      ordered_layers.push_back(&layer);
    }
    order_layers_by_z(ordered_layers);

    for (const auto *layer_pointer : ordered_layers) {
      if (stop_token.stop_requested()) {
        return cancellation_error();
      }
      const auto &layer = *layer_pointer;
      const auto scale = scales.find(layer.scale_id);
      const auto curve =
          std::find_if(document.curves().begin(), document.curves().end(),
                       [&](const Curve &candidate) {
                         return candidate.id == layer.curve_id;
                       });
      if (layer.id.is_nil() || !ids.insert(layer.id).second ||
          !track_bounds.contains(layer.track_id) || scale == scales.end() ||
          scale->second->track_id != layer.track_id ||
          curve == document.curves().end() ||
          !std::isfinite(layer.line_width.value) ||
          layer.line_width.value <= 0.0) {
        return presentation_error(layer.id);
      }

      const auto axis = std::find_if(
          document.sampling_axes().begin(), document.sampling_axes().end(),
          [&](const SamplingAxis &candidate) {
            return candidate.id == curve->sampling_axis_id;
          });
      if (axis == document.sampling_axes().end()) {
        return presentation_error(layer.id);
      }
      if (axis->domain != depth_range.domain ||
          axis->domain == DepthDomain::source_index ||
          axis->unit != depth_range.unit ||
          curve->unit != scale->second->unit) {
        return presentation_error(layer.id);
      }

      const auto bounds = track_bounds.at(layer.track_id);
      const auto first_segment =
          static_cast<std::uint64_t>(scene->curve_segments.size());
      std::optional<std::uint64_t> segment_start;
      std::uint32_t nonpositive_log_values = 0;
      const auto logarithmic =
          scale->second->mode == ScaleMode::logarithmic;
      const auto scale_minimum = scale->second->minimum;
      const auto scale_maximum = scale->second->maximum;
      const auto log_minimum = std::log(scale_minimum);
      const auto log_maximum = std::log(scale_maximum);
      const auto close_segment = [&]() {
        if (!segment_start.has_value()) {
          return;
        }
        scene->curve_segments.push_back(PreparedCurveSegment{
            .layer_id = layer.id,
            .first_point = *segment_start,
            .point_count =
                static_cast<std::uint64_t>(scene->curve_points.size()) -
                *segment_start,
        });
        segment_start.reset();
      };

      const auto append_sample = [&](std::uint64_t sample_index) {
        const auto depth = axis->coordinates.value_as_double(sample_index);
        const auto value = curve->values.value_as_double(sample_index);
        const auto qc = qc_state_at(document, *curve, sample_index);
        const auto qc_hidden = qc_state_is_suppressed(
            qc, layer.qc_display.hide_suspect, layer.qc_display.hide_invalid,
            layer.qc_display.hide_user_excluded);
        const auto missing =
            qc_hidden ||
            (!curve->nulls.empty() && curve->nulls.is_null(sample_index)) ||
            !depth.has_value() || !value.has_value() ||
            !std::isfinite(*depth) || !std::isfinite(*value);
        if (missing) {
          close_segment();
          return true;
        }

        if (logarithmic && *value <= 0.0) {
          // Non-positive values cannot be drawn on a logarithmic scale:
          // they break the polyline and aggregate into a diagnostic.
          close_segment();
          ++nonpositive_log_values;
          return true;
        }

        if (!segment_start.has_value()) {
          segment_start =
              static_cast<std::uint64_t>(scene->curve_points.size());
        }
        auto normalized_value =
            logarithmic
                ? (std::log(*value) - log_minimum) / (log_maximum - log_minimum)
                : (*value - scale_minimum) / (scale_maximum - scale_minimum);
        if (scale->second->direction == ScaleDirection::right_to_left) {
          normalized_value = 1.0 - normalized_value;
        }
        // Reference Depth → Display Depth via the well Depth Transform (#161).
        // Presentation depth_range is expressed in Display Depth space when a
        // non-identity transform is set (shared multi-well viewport).
        const auto display_depth = map_reference_to_display(
            presentation.depth_transform_map(), *depth);
        const auto normalized_depth =
            (display_depth - depth_range.top) /
            (depth_range.bottom - depth_range.top);
        const auto horizontal_offset = normalized_value * bounds.width.value;
        const auto left_position = bounds.left.value + horizontal_offset;
        const auto top_position =
            normalized_depth * presentation.physical_height().value;
        if (!std::isfinite(normalized_value) ||
            !std::isfinite(normalized_depth) ||
            !std::isfinite(horizontal_offset) ||
            !std::isfinite(left_position) || !std::isfinite(top_position) ||
            !std::isfinite(display_depth)) {
          return false;
        }
        scene->curve_points.push_back(PreparedCurvePoint{
            .position =
                PhysicalPoint{
                    .left = Millimetres{left_position},
                    .top = Millimetres{top_position},
                },
            .sample_index = sample_index,
            .reference_depth = *depth,
            .display_depth = display_depth,
            .value = *value,
        });
        return true;
      };

      const auto lod = curve_lods == nullptr ? CurveLodMap::const_iterator{}
                                             : curve_lods->find(curve->id);
      if (!layer.visible) {
        // Hidden: identity and style are preserved without geometry.
      } else if (curve_lods != nullptr && query != nullptr &&
          lod != curve_lods->end()) {
        const auto selection = lod->second.query(*query, stop_token);
        if (!selection.has_value()) {
          return selection.error();
        }
        for (const auto &segment : selection.value().segments()) {
          close_segment();
          for (std::uint64_t offset = 0; offset < segment.point_count;
               ++offset) {
            if ((offset & std::uint64_t{4095}) == 0 &&
                stop_token.stop_requested()) {
              return cancellation_error();
            }
            const auto &point =
                selection.value().points()[static_cast<std::size_t>(
                    segment.first_point + offset)];
            if (!append_sample(point.sample_index)) {
              return presentation_error(layer.id);
            }
          }
          close_segment();
        }
      } else {
        const auto sample_count = curve->values.length();
        for (std::uint64_t sample_index = 0; sample_index < sample_count;
             ++sample_index) {
          if ((sample_index & std::uint64_t{4095}) == 0 &&
              stop_token.stop_requested()) {
            return cancellation_error();
          }
          if (!append_sample(sample_index)) {
            return presentation_error(layer.id);
          }
        }
      }
      close_segment();
      if (nonpositive_log_values > 0) {
        scene->value_issues.push_back(SceneValueIssue{
            .code = ValueIssueCode::nonpositive_log_values,
            .entity_id = layer.id,
            .occurrence_count = nonpositive_log_values,
        });
      }

      const auto layer_index =
          static_cast<std::uint64_t>(scene->curve_layers.size());
      scene->curve_layers.push_back(PreparedCurveLayer{
          .id = layer.id,
          .track_id = layer.track_id,
          .curve_id = layer.curve_id,
          .scale_id = layer.scale_id,
          .color = layer.color,
          .line_width = layer.line_width,
          .z_order = layer.z_order,
          .first_segment = first_segment,
          .segment_count =
              static_cast<std::uint64_t>(scene->curve_segments.size()) -
              first_segment,
          .visible = layer.visible,
      });

      PreparedScene::Impl::CurvePickIndex pick_index{
          .layer_index = layer_index,
          .bin_height = 0.0,
          .primitives = {},
          .bins = {},
      };
      constexpr std::size_t maximum_pick_bins = 2048;
      constexpr double target_pick_bin_height = 2.0;
      const auto requested_bin_count =
          std::ceil(scene->physical_height.value / target_pick_bin_height);
      const auto bin_count =
          requested_bin_count >= static_cast<double>(maximum_pick_bins)
              ? maximum_pick_bins
              : std::max(std::size_t{1},
                         static_cast<std::size_t>(requested_bin_count));
      pick_index.bin_height =
          scene->physical_height.value / static_cast<double>(bin_count);
      pick_index.bins.resize(bin_count);
      for (std::uint64_t segment_offset = 0;
           segment_offset < scene->curve_layers.back().segment_count;
           ++segment_offset) {
        if (stop_token.stop_requested()) {
          return cancellation_error();
        }
        const auto &segment = scene->curve_segments[static_cast<std::size_t>(
            first_segment + segment_offset)];
        if (segment.point_count == 0) {
          continue;
        }
        const auto primitive_count =
            segment.point_count == 1 ? std::uint64_t{1}
                                     : segment.point_count - std::uint64_t{1};
        for (std::uint64_t primitive_offset = 0;
             primitive_offset < primitive_count; ++primitive_offset) {
          if (stop_token.stop_requested()) {
            return cancellation_error();
          }
          const auto first_point =
              segment.first_point +
              (segment.point_count == 1 ? std::uint64_t{0} : primitive_offset);
          const auto second_point =
              segment.point_count == 1 ? first_point : first_point + 1;
          const auto &first =
              scene->curve_points[static_cast<std::size_t>(first_point)];
          const auto &second =
              scene->curve_points[static_cast<std::size_t>(second_point)];
          const auto minimum_top =
              std::min(first.position.top.value, second.position.top.value);
          const auto maximum_top =
              std::max(first.position.top.value, second.position.top.value);
          if (maximum_top < 0.0 || minimum_top > scene->physical_height.value) {
            continue;
          }
          const auto primitive_index =
              static_cast<std::uint64_t>(pick_index.primitives.size());
          pick_index.primitives.push_back(PreparedScene::Impl::PickPrimitive{
              .first_point = first_point,
              .second_point = second_point,
          });
          const auto bin_for = [&](double top) {
            if (top <= 0.0) {
              return std::size_t{0};
            }
            if (top >= scene->physical_height.value) {
              return bin_count - std::size_t{1};
            }
            return std::min(
                static_cast<std::size_t>(top / pick_index.bin_height),
                bin_count - std::size_t{1});
          };
          const auto first_bin = bin_for(minimum_top);
          const auto last_bin = bin_for(maximum_top);
          for (auto bin = first_bin; bin <= last_bin; ++bin) {
            if (stop_token.stop_requested()) {
              return cancellation_error();
            }
            pick_index.bins[bin].push_back(primitive_index);
          }
        }
      }
      scene->curve_pick_indices.push_back(std::move(pick_index));
    }

    {
      std::unordered_map<EntityId, std::unordered_set<EntityId, EntityIdHash>,
                         EntityIdHash>
          visible_scales_per_track;
      for (const auto *layer_pointer : ordered_layers) {
        if (layer_pointer->visible) {
          visible_scales_per_track[layer_pointer->track_id].insert(
              layer_pointer->scale_id);
        }
      }
      for (const auto &[track_id, scale_ids] : visible_scales_per_track) {
        if (scale_ids.size() > 4) {
          scene->value_issues.push_back(SceneValueIssue{
              .code = ValueIssueCode::scale_readability_hint,
              .entity_id = track_id,
              .occurrence_count =
                  static_cast<std::uint32_t>(scale_ids.size()),
          });
        }
      }
    }

    // Pattern definitions are the single vector source of truth shared by
    // the screen and vector backends (ADR 0020). They are validated against
    // the untrusted-asset limits of ADR 0042 before entering the scene.
    constexpr std::size_t maximum_pattern_primitives = 256;
    constexpr std::size_t maximum_polyline_points = 1024;
    constexpr double maximum_tile_extent_millimetres = 500.0;
    std::unordered_set<EntityId, EntityIdHash> pattern_ids;
    scene->patterns.reserve(presentation.patterns().size());
    for (const auto &pattern : presentation.patterns()) {
      if (stop_token.stop_requested()) {
        return cancellation_error();
      }
      if (pattern.id.is_nil() || !ids.insert(pattern.id).second ||
          !std::isfinite(pattern.tile_width.value) ||
          pattern.tile_width.value <= 0.0 ||
          pattern.tile_width.value > maximum_tile_extent_millimetres ||
          !std::isfinite(pattern.tile_height.value) ||
          pattern.tile_height.value <= 0.0 ||
          pattern.tile_height.value > maximum_tile_extent_millimetres ||
          !std::isfinite(pattern.rotation_degrees) ||
          !std::isfinite(pattern.stroke_width.value) ||
          pattern.stroke_width.value <= 0.0 ||
          pattern.stroke_width.value > maximum_tile_extent_millimetres ||
          !std::isfinite(pattern.scene_anchor.left.value) ||
          !std::isfinite(pattern.scene_anchor.top.value) ||
          pattern.primitives.size() > maximum_pattern_primitives) {
        return presentation_error(pattern.id);
      }
      bool primitives_valid = true;
      const auto point_valid = [](const PhysicalPoint &point) {
        return std::isfinite(point.left.value) &&
               std::isfinite(point.top.value);
      };
      for (const auto &primitive : pattern.primitives) {
        if (const auto *line = std::get_if<PatternLine>(&primitive)) {
          primitives_valid = point_valid(line->from) && point_valid(line->to);
        } else if (const auto *polyline =
                       std::get_if<PatternPolyline>(&primitive)) {
          primitives_valid =
              polyline->points.size() >= 2 &&
              polyline->points.size() <= maximum_polyline_points &&
              std::all_of(polyline->points.begin(), polyline->points.end(),
                          point_valid);
        } else {
          const auto &circle = std::get<PatternCircle>(primitive);
          primitives_valid = point_valid(circle.center) &&
                             std::isfinite(circle.radius.value) &&
                             circle.radius.value > 0.0 &&
                             circle.radius.value <=
                                 maximum_tile_extent_millimetres;
        }
        if (!primitives_valid) {
          break;
        }
      }
      if (!primitives_valid) {
        return presentation_error(pattern.id);
      }
      pattern_ids.insert(pattern.id);
      scene->patterns.push_back(pattern);
    }

    const auto &depth_xform = presentation.depth_transform_map();
    const auto depth_to_top = [&](double reference_depth) {
      const auto display =
          map_reference_to_display(depth_xform, reference_depth);
      return (display - depth_range.top) / (depth_range.bottom - depth_range.top) *
             presentation.physical_height().value;
    };

    std::vector<const IntervalLayerSpec *> ordered_interval_layers;
    ordered_interval_layers.reserve(presentation.interval_layers().size());
    for (const auto &layer : presentation.interval_layers()) {
      ordered_interval_layers.push_back(&layer);
    }
    order_layers_by_z(ordered_interval_layers);

    for (const auto *layer_pointer : ordered_interval_layers) {
      if (stop_token.stop_requested()) {
        return cancellation_error();
      }
      const auto &layer = *layer_pointer;
      const auto bounds = track_bounds.find(layer.track_id);
      if (layer.id.is_nil() || !ids.insert(layer.id).second ||
          bounds == track_bounds.end() ||
          (layer.draw_labels &&
           (!std::isfinite(layer.label_font_size.value) ||
            layer.label_font_size.value <= 0.0))) {
        return presentation_error(layer.id);
      }
      const auto first_interval =
          static_cast<std::uint64_t>(scene->intervals.size());
      for (const auto &interval : document.intervals()) {
        if (stop_token.stop_requested()) {
          return cancellation_error();
        }
        if (layer.semantic_filter.has_value() &&
            interval.semantic != *layer.semantic_filter) {
          continue;
        }
        if (!interval.pattern_id.is_nil() &&
            !pattern_ids.contains(interval.pattern_id)) {
          return presentation_error(interval.id);
        }
        // Cull in Display Depth space (depth_range is display when a transform
        // is active — #161). The window and the interval span are compared
        // direction-agnostically: a decreasing display (TVDSS) must not cull
        // interior intervals.
        const auto interval_display_top = map_reference_to_display(
            depth_xform, interval.top_reference_depth);
        const auto interval_display_bottom = map_reference_to_display(
            depth_xform, interval.bottom_reference_depth);
        const auto span_lo =
            std::min(interval_display_top, interval_display_bottom);
        const auto span_hi =
            std::max(interval_display_top, interval_display_bottom);
        const auto win_lo = std::min(depth_range.top, depth_range.bottom);
        const auto win_hi = std::max(depth_range.top, depth_range.bottom);
        if (span_lo >= win_hi || span_hi <= win_lo) {
          continue;
        }
        const auto unclipped_top = depth_to_top(interval.top_reference_depth);
        const auto unclipped_bottom =
            depth_to_top(interval.bottom_reference_depth);
        const auto top = std::clamp(unclipped_top, 0.0,
                                    presentation.physical_height().value);
        const auto bottom = std::clamp(unclipped_bottom, 0.0,
                                       presentation.physical_height().value);
        const auto height = bottom - top;
        if (!std::isfinite(top) || !std::isfinite(bottom) ||
            !std::isfinite(height) || height <= 0.0) {
          continue;
        }
        scene->intervals.push_back(PreparedInterval{
            .layer_id = layer.id,
            .interval_id = interval.id,
            .rect =
                PhysicalRect{
                    .left = bounds->second.left,
                    .top = Millimetres{top},
                    .width = bounds->second.width,
                    .height = Millimetres{height},
                },
            .fill_color = interval.fill_color,
            .pattern_id = interval.pattern_id,
            .top_reference_depth = interval.top_reference_depth,
            .bottom_reference_depth = interval.bottom_reference_depth,
            .label_run_index = no_text_run,
        });
      }
      scene->interval_layers.push_back(PreparedIntervalLayer{
          .id = layer.id,
          .track_id = layer.track_id,
          .z_order = layer.z_order,
          .first_interval = first_interval,
          .interval_count =
              static_cast<std::uint64_t>(scene->intervals.size()) -
              first_interval,
      });
    }

    // Crossover fill layers (ADR 0017; rendering.md section 6). Built after
    // curve + interval layers so they can read the prepared curve geometry
    // and the validated pattern ids. Each fill encloses the region between
    // two crossing curve layers; the boundary is computed from mapped
    // x-coordinates and triangulated once for both backends to consume.
    std::vector<const CrossoverFillLayerSpec *> ordered_fill_layers;
    ordered_fill_layers.reserve(presentation.crossover_fill_layers().size());
    for (const auto &layer : presentation.crossover_fill_layers()) {
      ordered_fill_layers.push_back(&layer);
    }
    order_layers_by_z(ordered_fill_layers);

    const auto find_prepared_curve_layer =
        [&](EntityId layer_id) -> const PreparedCurveLayer * {
      for (const auto &cl : scene->curve_layers) {
        if (cl.id == layer_id) {
          return &cl;
        }
      }
      return nullptr;
    };

    for (const auto *layer_pointer : ordered_fill_layers) {
      if (stop_token.stop_requested()) {
        return cancellation_error();
      }
      const auto &layer = *layer_pointer;
      const auto bounds = track_bounds.find(layer.track_id);
      const auto *upper_layer =
          find_prepared_curve_layer(layer.upper_curve_layer_id);
      const auto *lower_layer =
          find_prepared_curve_layer(layer.lower_curve_layer_id);
      const auto color_set = layer.fill_color.has_value();
      const auto pattern_set =
          layer.pattern_id.has_value() && !layer.pattern_id->is_nil();
      if (layer.id.is_nil() || !ids.insert(layer.id).second ||
          bounds == track_bounds.end() || upper_layer == nullptr ||
          lower_layer == nullptr ||
          upper_layer->track_id != layer.track_id ||
          lower_layer->track_id != layer.track_id ||
          upper_layer->id == lower_layer->id ||
          (color_set == pattern_set)) {
        // Exactly one of fill_color / pattern_id must be set; both layers
        // must exist, share this track, and differ.
        return presentation_error(layer.id);
      }
      if (pattern_set && !pattern_ids.contains(*layer.pattern_id)) {
        return presentation_error(*layer.pattern_id);
      }
      const auto fill_color =
          color_set ? *layer.fill_color : RgbaColor{};
      const auto pattern_id =
          pattern_set ? *layer.pattern_id : EntityId{};

      const auto first_region =
          static_cast<std::uint64_t>(scene->fill_regions.size());

      // Hidden fill, or a hidden dependent curve, contributes identity but
      // no geometry (mirrors hidden curve-layer behaviour).
      if (layer.visible && upper_layer->visible && lower_layer->visible) {
        const auto upper_samples =
            collect_layer_samples(scene->curve_segments, scene->curve_points,
                                  *upper_layer);
        const auto lower_samples =
            collect_layer_samples(scene->curve_segments, scene->curve_points,
                                  *lower_layer);
        const auto regions = build_crossover_regions(
            upper_samples, lower_samples, stop_token);
        if (!regions.has_value()) {
          return cancellation_error();
        }
        for (const auto &region : *regions) {
          if (stop_token.stop_requested()) {
            return cancellation_error();
          }
          if (region.ring.size() < 3) {
            continue;
          }
          const auto first_vertex =
              static_cast<std::uint32_t>(scene->fill_vertices.size());
          for (const auto &p : region.ring) {
            scene->fill_vertices.push_back(PreparedFillVertex{.position = p});
          }
          std::vector<detail::PolygonPoint> polygon;
          polygon.reserve(region.ring.size());
          double min_left = std::numeric_limits<double>::infinity();
          double min_top = std::numeric_limits<double>::infinity();
          double max_left = -std::numeric_limits<double>::infinity();
          double max_top = -std::numeric_limits<double>::infinity();
          for (const auto &p : region.ring) {
            polygon.push_back(detail::PolygonPoint{.x = p.left.value,
                                                   .y = p.top.value});
            min_left = std::min(min_left, p.left.value);
            min_top = std::min(min_top, p.top.value);
            max_left = std::max(max_left, p.left.value);
            max_top = std::max(max_top, p.top.value);
          }
          const auto triangles = detail::triangulate_polygon(polygon);
          const auto first_triangle =
              static_cast<std::uint64_t>(scene->fill_triangles.size());
          for (std::size_t t = 0; t + 2 < triangles.size(); t += 3) {
            scene->fill_triangles.push_back(PreparedFillTriangle{
                .a = triangles[t] + first_vertex,
                .b = triangles[t + 1] + first_vertex,
                .c = triangles[t + 2] + first_vertex,
            });
          }
          scene->fill_regions.push_back(PreparedFillRegion{
              .layer_id = layer.id,
              .first_vertex = first_vertex,
              .vertex_count = static_cast<std::uint64_t>(region.ring.size()),
              .first_triangle = first_triangle,
              .triangle_count = triangles.size() / 3,
              .fill_color = fill_color,
              .pattern_id = pattern_id,
              .upper_curve_layer_id = layer.upper_curve_layer_id,
              .lower_curve_layer_id = layer.lower_curve_layer_id,
              .top_reference_depth = region.top_reference_depth,
              .bottom_reference_depth = region.bottom_reference_depth,
              .bounds =
                  PhysicalRect{
                      .left = Millimetres{min_left},
                      .top = Millimetres{min_top},
                      .width = Millimetres{max_left - min_left},
                      .height = Millimetres{max_top - min_top},
                  },
          });
        }
      }
      scene->fill_layers.push_back(PreparedFillLayer{
          .id = layer.id,
          .track_id = layer.track_id,
          .z_order = layer.z_order,
          .first_region = first_region,
          .region_count =
              static_cast<std::uint64_t>(scene->fill_regions.size()) -
              first_region,
      });
    }

    // Image layers (rendering.md section 10). Validated against the untrusted
    // asset limits (ADR 0042), then a multi-resolution pyramid selects the
    // visible (+ prefetched) tiles at the viewport's resolution; each tile is
    // placed as a full-track-width rect over its depth slice. The engine does
    // NOT decode pixels — the host resolves tile bytes via the image_tile
    // resolver (ADR 0032).
    constexpr std::uint64_t maximum_image_dimension_px = 65536ULL;
    constexpr std::uint64_t maximum_image_pixels = 512ULL * 1024ULL * 1024ULL;
    constexpr std::uint32_t minimum_image_dpi = 1;
    std::vector<const ImageLayerSpec *> ordered_image_layers;
    ordered_image_layers.reserve(presentation.image_layers().size());
    for (const auto &layer : presentation.image_layers()) {
      ordered_image_layers.push_back(&layer);
    }
    order_layers_by_z(ordered_image_layers);

    const auto find_image_source =
        [&](EntityId source_id) -> const ImageSource * {
      for (const auto &candidate : document.image_sources()) {
        if (candidate.id == source_id) {
          return &candidate;
        }
      }
      return nullptr;
    };

    for (const auto *layer_pointer : ordered_image_layers) {
      if (stop_token.stop_requested()) {
        return cancellation_error();
      }
      const auto &layer = *layer_pointer;
      const auto bounds = track_bounds.find(layer.track_id);
      const auto *source = find_image_source(layer.image_source_id);
      if (layer.id.is_nil() || !ids.insert(layer.id).second ||
          bounds == track_bounds.end() || source == nullptr ||
          source->width_px == 0 || source->height_px == 0 ||
          source->width_px > maximum_image_dimension_px ||
          source->height_px > maximum_image_dimension_px ||
          source->dpi < minimum_image_dpi ||
          source->reference_depth_bottom <= source->reference_depth_top ||
          !std::isfinite(source->reference_depth_top) ||
          !std::isfinite(source->reference_depth_bottom)) {
        return presentation_error(layer.id);
      }
      const auto pixel_count = source->width_px * source->height_px;
      if (pixel_count > maximum_image_pixels) {
        return Error{
            .code = ErrorCode::invalid_image,
            .severity = Severity::error,
            .entity_id = source->id,
            .message = MessageKey::image_pixels_exceed_limit,
            .arguments = {},
        };
      }
      const auto first_tile =
          static_cast<std::uint64_t>(scene->image_tiles.size());
      if (layer.visible && image_pyramids != nullptr &&
          image_query != nullptr) {
        const auto pyramid = image_pyramids->find(layer.image_source_id);
        if (pyramid != image_pyramids->end()) {
          const auto selection = pyramid->second.query(*image_query, stop_token);
          if (!selection.has_value()) {
            return selection.error();
          }
          for (const auto &tile : selection.value().tiles) {
            if (stop_token.stop_requested()) {
              return cancellation_error();
            }
            // Clip the tile's depth span to the presentation range. Sorted
            // bounds keep std::clamp valid for decreasing windows (TVDSS).
            const auto win_lo = std::min(depth_range.top, depth_range.bottom);
            const auto win_hi = std::max(depth_range.top, depth_range.bottom);
            const auto top_depth = std::clamp(tile.top_reference_depth,
                                              win_lo, win_hi);
            const auto bottom_depth = std::clamp(tile.bottom_reference_depth,
                                                 win_lo, win_hi);
            if (bottom_depth <= top_depth) {
              continue;
            }
            const auto tile_top = std::clamp(depth_to_top(top_depth), 0.0,
                                             presentation.physical_height().value);
            const auto tile_bottom =
                std::clamp(depth_to_top(bottom_depth), 0.0,
                           presentation.physical_height().value);
            const auto height = tile_bottom - tile_top;
            if (height <= 0.0) {
              continue;
            }
            scene->image_tiles.push_back(PreparedImageTile{
                .layer_id = layer.id,
                .image_source_id = layer.image_source_id,
                .rect = PhysicalRect{
                    .left = bounds->second.left,
                    .top = Millimetres{tile_top},
                    .width = bounds->second.width,
                    .height = Millimetres{height},
                },
                .level = tile.level,
                .row = tile.row,
                .col = tile.col,
                .width_px = tile.width_px,
                .height_px = tile.height_px,
                .pixel_format = source->pixel_format,
                .dpi = source->dpi,
                .source = source->source,
            });
          }
        }
      }
      scene->image_layers.push_back(PreparedImageLayer{
          .id = layer.id,
          .track_id = layer.track_id,
          .image_source_id = layer.image_source_id,
          .z_order = layer.z_order,
          .first_tile = first_tile,
          .tile_count = static_cast<std::uint64_t>(scene->image_tiles.size()) -
                        first_tile,
      });
    }

    // Custom layers (ADR 0018/0046, rendering.md section 11). A host-authored
    // declarative source of polylines, triangles, quads and symbols, validated
    // against the untrusted-asset limits of ADR 0042. The primitives are plain
    // data — no shader/script/command field exists, so the security constraint
    // is enforced by the type system. Polylines decompose into the shared
    // curve segment/point stream (so GL draws them as edges and SVG emits them
    // as paths with no new backend code); triangles/quads/symbols flatten into
    // custom_primitives/custom_vertices for the solid-batch upload loop. Each
    // layer may declare a layer-local clip path that masks only its own
    // primitives.
    constexpr std::size_t maximum_custom_primitives = 4096;
    constexpr std::size_t maximum_custom_polyline_points = 8192;
    constexpr std::size_t maximum_custom_vertices = 1 << 20;
    const auto custom_point_valid = [](const PhysicalPoint &point) {
      return std::isfinite(point.left.value) && std::isfinite(point.top.value);
    };
    const auto find_custom_source =
        [&](EntityId source_id) -> const CustomLayerSource * {
      for (const auto &candidate : document.custom_sources()) {
        if (candidate.id == source_id) {
          return &candidate;
        }
      }
      return nullptr;
    };
    std::vector<const CustomLayerSpec *> ordered_custom_layers;
    ordered_custom_layers.reserve(presentation.custom_layers().size());
    for (const auto &layer : presentation.custom_layers()) {
      ordered_custom_layers.push_back(&layer);
    }
    order_layers_by_z(ordered_custom_layers);

    for (const auto *layer_pointer : ordered_custom_layers) {
      if (stop_token.stop_requested()) {
        return cancellation_error();
      }
      const auto &layer = *layer_pointer;
      const auto bounds = track_bounds.find(layer.track_id);
      const auto *source = find_custom_source(layer.custom_source_id);
      if (layer.id.is_nil() || !ids.insert(layer.id).second ||
          bounds == track_bounds.end() || source == nullptr) {
        return presentation_error(layer.id);
      }
      if (source->primitives.empty()) {
        return Error{
            .code = ErrorCode::invalid_custom_source,
            .severity = Severity::error,
            .entity_id = source->id,
            .message = MessageKey::custom_source_empty,
            .arguments = {},
        };
      }
      if (source->primitives.size() > maximum_custom_primitives) {
        return Error{
            .code = ErrorCode::invalid_custom_source,
            .severity = Severity::error,
            .entity_id = source->id,
            .message = MessageKey::custom_source_primitives_exceed_limit,
            .arguments = {},
        };
      }
      // Validate every primitive's geometry up front (ADR 0042): a single bad
      // point rejects the whole source rather than producing a partial scene.
      std::size_t total_vertices = 0;
      for (const auto &primitive : source->primitives) {
        if (const auto *polyline = std::get_if<CustomPolyline>(&primitive)) {
          // ADR 0050 dash validation (#840): non-positive / non-finite dash
          // segments and offsets are rejected here for ALL backends — the GL
          // subdivision used to NaN-loop on a zero-sum cycle like [0,0] and
          // silently drop the whole polyline, while SVG/PDF emitted invalid
          // values (`stroke-dasharray="0 0"` / `[0 0] d`). Odd-length arrays
          // stay legal (SVG/PDF duplicate them per spec; GL now does too).
          const auto dash_segment_valid = [](const Millimetres &segment) {
            return std::isfinite(segment.value) && segment.value > 0.0;
          };
          const auto dash_valid =
              polyline->dash_pattern.segments.empty() ||
              (std::isfinite(polyline->dash_pattern.offset) &&
               std::all_of(polyline->dash_pattern.segments.begin(),
                           polyline->dash_pattern.segments.end(),
                           dash_segment_valid));
          if (polyline->points.size() < 2 ||
              polyline->points.size() > maximum_custom_polyline_points ||
              !std::isfinite(polyline->width.value) ||
              polyline->width.value < 0.0 ||
              !std::all_of(polyline->points.begin(), polyline->points.end(),
                           custom_point_valid) ||
              !dash_valid) {
            return presentation_error(layer.id);
          }
          continue;
        }
        if (const auto *triangle = std::get_if<CustomTriangle>(&primitive)) {
          if (!custom_point_valid(triangle->a) ||
              !custom_point_valid(triangle->b) ||
              !custom_point_valid(triangle->c)) {
            return presentation_error(layer.id);
          }
          total_vertices += 3;
        } else if (const auto *quad = std::get_if<CustomQuad>(&primitive)) {
          if (!std::isfinite(quad->rect.left.value) ||
              !std::isfinite(quad->rect.top.value) ||
              !std::isfinite(quad->rect.width.value) ||
              !std::isfinite(quad->rect.height.value)) {
            return presentation_error(layer.id);
          }
          total_vertices += 6;
        } else {
          const auto &symbol = std::get<CustomSymbolOccurrence>(primitive);
          if (!custom_point_valid(symbol.center) ||
              !std::isfinite(symbol.size.value) ||
              symbol.size.value <= 0.0) {
            return presentation_error(layer.id);
          }
          total_vertices += 24;
        }
      }
      if (total_vertices > maximum_custom_vertices) {
        return Error{
            .code = ErrorCode::invalid_custom_source,
            .severity = Severity::error,
            .entity_id = source->id,
            .message = MessageKey::custom_source_points_exceed_limit,
            .arguments = {},
        };
      }
      // Resolve an optional layer-local clip path.
      std::uint64_t clip_path_index = no_clip;
      if (source->clip.has_value()) {
        if (source->clip->points.size() < 3 ||
            source->clip->points.size() > maximum_custom_polyline_points ||
            !std::all_of(source->clip->points.begin(),
                         source->clip->points.end(), custom_point_valid)) {
          return presentation_error(layer.id);
        }
        clip_path_index =
            static_cast<std::uint64_t>(scene->custom_clip_paths.size());
        scene->custom_clip_paths.push_back(
            PreparedCustomClipPath{.points = source->clip->points});
      }
      const auto first_primitive =
          static_cast<std::uint64_t>(scene->custom_primitives.size());
      if (layer.visible) {
        // When the source declares a layer-local clip path, build it once as a
        // PolygonPoint ring so filled primitives can be clipped to it in scene
        // millimetres before entering the primitive stream. This keeps GL, SVG
        // and pick all seeing the same clipped geometry (no per-backend clip).
        std::vector<detail::PolygonPoint> clip_ring;
        if (clip_path_index != no_clip) {
          const auto &clip_points = scene->custom_clip_paths[clip_path_index].points;
          clip_ring.reserve(clip_points.size());
          for (const auto &point : clip_points) {
            clip_ring.push_back(
                detail::PolygonPoint{.x = point.left.value, .y = point.top.value});
          }
        }
        const auto bounds_of =
            [](const std::vector<PhysicalPoint> &points) -> PhysicalRect {
          if (points.empty()) {
            return {};
          }
          auto min_left = points.front().left.value;
          auto max_left = min_left;
          auto min_top = points.front().top.value;
          auto max_top = min_top;
          for (const auto &point : points) {
            min_left = std::min(min_left, point.left.value);
            max_left = std::max(max_left, point.left.value);
            min_top = std::min(min_top, point.top.value);
            max_top = std::max(max_top, point.top.value);
          }
          return PhysicalRect{.left = Millimetres{min_left},
                              .top = Millimetres{min_top},
                              .width = Millimetres{max_left - min_left},
                              .height = Millimetres{max_top - min_top}};
        };
        // Emits a filled primitive (triangle/quad) from its polygon ring,
        // clipped to the layer-local clip path if present, then triangulated.
        // The prepared primitive stores every triangle's vertices (3 each) so
        // GL/SVG/pick all consume exact clipped geometry.
        const auto emit_filled =
            [&](const std::vector<PhysicalPoint> &ring, EntityId source_id,
                std::size_t primitive_index, CustomPrimitiveKind kind,
                RgbaColor color, EntityId pattern_id = {}) {
              std::vector<detail::PolygonPoint> polygon;
              polygon.reserve(ring.size());
              for (const auto &point : ring) {
                polygon.push_back(detail::PolygonPoint{.x = point.left.value,
                                                       .y = point.top.value});
              }
              std::vector<detail::PolygonPoint> clipped = polygon;
              if (!clip_ring.empty()) {
                clipped = detail::clip_polygon_to_polygon(polygon, clip_ring);
                if (clipped.size() < 3) {
                  return; // primitive lies entirely outside the clip path
                }
              }
              const auto indices = detail::triangulate_polygon(clipped);
              if (indices.size() < 3) {
                return;
              }
              const auto first_vertex =
                  static_cast<std::uint64_t>(scene->custom_vertices.size());
              std::vector<PhysicalPoint> emitted;
              emitted.reserve(indices.size());
              for (const auto index_32 : indices) {
                const auto &p = clipped[static_cast<std::size_t>(index_32)];
                const auto point =
                    PhysicalPoint{.left = Millimetres{p.x}, .top = Millimetres{p.y}};
                scene->custom_vertices.push_back(point);
                emitted.push_back(point);
              }
              scene->custom_primitives.push_back(PreparedCustomPrimitive{
                  .layer_id = layer.id,
                  .source_id = source_id,
                  .source_primitive_index = primitive_index,
                  .kind = kind,
                  .color = color,
                  .first_vertex = first_vertex,
                  .vertex_count = emitted.size(),
                  .pattern_id = pattern_id,
                  .bounds = bounds_of(emitted),
              });
            };
        for (std::size_t index = 0; index < source->primitives.size();
             ++index) {
          const auto &primitive = source->primitives[index];
          if (const auto *polyline = std::get_if<CustomPolyline>(&primitive)) {
            // Polylines store their points in custom_vertices and a single
            // PreparedCustomPrimitive record (vertex_count = point count). The
            // GL and SVG backends both treat vertex_count >= 2 with kind
            // polyline as a line strip (closed when the source was closed).
            // Under a layer-local clip, segments with both endpoints outside
            // the clip are dropped; partial-line clipping is deferred.
            const auto first_vertex =
                static_cast<std::uint64_t>(scene->custom_vertices.size());
            std::vector<PhysicalPoint> kept;
            kept.reserve(polyline->points.size());
            for (const auto &point : polyline->points) {
              if (!clip_ring.empty() &&
                  !detail::point_in_polygon(clip_ring, point.left.value,
                                            point.top.value)) {
                // Keep the boundary point only if a neighbour is inside, so a
                // polyline entering the clip still connects across the edge.
                if (!kept.empty()) {
                  kept.push_back(point);
                }
                continue;
              }
              kept.push_back(point);
            }
            if (kept.size() < 2) {
              continue;
            }
            for (const auto &point : kept) {
              scene->custom_vertices.push_back(point);
            }
            scene->custom_primitives.push_back(PreparedCustomPrimitive{
                .layer_id = layer.id,
                .source_id = source->id,
                .source_primitive_index = index,
                .kind = CustomPrimitiveKind::polyline,
                .color = polyline->color,
                .stroke_width = polyline->width,
                .first_vertex = first_vertex,
                .vertex_count = kept.size(),
                .closed = polyline->closed,
                .dash_pattern = polyline->dash_pattern,
                .bounds = bounds_of(kept),
            });
            continue;
          }
          if (const auto *triangle = std::get_if<CustomTriangle>(&primitive)) {
            emit_filled(std::vector<PhysicalPoint>{triangle->a, triangle->b,
                                                   triangle->c},
                        source->id, index, CustomPrimitiveKind::triangle,
                        triangle->fill_color);
          } else if (const auto *quad = std::get_if<CustomQuad>(&primitive)) {
            // Names avoid shadowing outer track-layout `left`/`top` (MSVC C4456).
            const auto quad_left = quad->rect.left.value;
            const auto quad_top = quad->rect.top.value;
            const auto quad_right = quad_left + quad->rect.width.value;
            const auto quad_bottom = quad_top + quad->rect.height.value;
            emit_filled(
                std::vector<PhysicalPoint>{
                    PhysicalPoint{.left = Millimetres{quad_left},
                                  .top = Millimetres{quad_top}},
                    PhysicalPoint{.left = Millimetres{quad_right},
                                  .top = Millimetres{quad_top}},
                    PhysicalPoint{.left = Millimetres{quad_right},
                                  .top = Millimetres{quad_bottom}},
                    PhysicalPoint{.left = Millimetres{quad_left},
                                  .top = Millimetres{quad_bottom}}},
                source->id, index, CustomPrimitiveKind::quad, quad->fill_color,
                quad->pattern_id);
          } else {
            const auto &symbol =
                std::get<CustomSymbolOccurrence>(primitive);
            // A symbol whose center lies outside the layer-local clip is
            // dropped; the GL renderer rebuilds its geometry from center/kind/
            // size at upload time, so only the center vertex is stored.
            if (!clip_ring.empty() &&
                !detail::point_in_polygon(clip_ring, symbol.center.left.value,
                                          symbol.center.top.value)) {
              continue;
            }
            const auto first_vertex =
                static_cast<std::uint64_t>(scene->custom_vertices.size());
            scene->custom_vertices.push_back(symbol.center);
            const auto half = symbol.size.value * 0.5;
            scene->custom_primitives.push_back(PreparedCustomPrimitive{
                .layer_id = layer.id,
                .source_id = source->id,
                .source_primitive_index = index,
                .kind = CustomPrimitiveKind::symbol,
                .color = symbol.color,
                .first_vertex = first_vertex,
                .vertex_count = 1,
                .symbol_kind = symbol.kind,
                .bounds = PhysicalRect{
                    .left = Millimetres{symbol.center.left.value - half},
                    .top = Millimetres{symbol.center.top.value - half},
                    .width = symbol.size,
                    .height = symbol.size,
                },
            });
          }
        }
      }
      scene->custom_layers.push_back(PreparedCustomLayer{
          .id = layer.id,
          .track_id = layer.track_id,
          .custom_source_id = layer.custom_source_id,
          .z_order = layer.z_order,
          .first_primitive = first_primitive,
          .primitive_count =
              static_cast<std::uint64_t>(scene->custom_primitives.size()) -
              first_primitive,
          .clip_path_index = clip_path_index,
          .visible = layer.visible,
      });
    }

    std::vector<const MarkerLayerSpec *> ordered_marker_layers;
    ordered_marker_layers.reserve(presentation.marker_layers().size());
    for (const auto &layer : presentation.marker_layers()) {
      ordered_marker_layers.push_back(&layer);
    }
    order_layers_by_z(ordered_marker_layers);

    for (const auto *layer_pointer : ordered_marker_layers) {
      if (stop_token.stop_requested()) {
        return cancellation_error();
      }
      const auto &layer = *layer_pointer;
      if (layer.id.is_nil() || !ids.insert(layer.id).second ||
          !track_bounds.contains(layer.track_id) ||
          !std::isfinite(layer.line_width.value) ||
          layer.line_width.value <= 0.0 ||
          (layer.draw_labels &&
           (!std::isfinite(layer.label_font_size.value) ||
            layer.label_font_size.value <= 0.0)) ||
          (layer.draw_symbols &&
           (!std::isfinite(layer.symbol_size.value) ||
            layer.symbol_size.value <= 0.0))) {
        return presentation_error(layer.id);
      }
      const auto first_marker =
          static_cast<std::uint64_t>(scene->markers.size());
      for (const auto &marker : document.markers()) {
        if (stop_token.stop_requested()) {
          return cancellation_error();
        }
        const auto marker_display =
            map_reference_to_display(depth_xform, marker.reference_depth);
        const auto win_lo = std::min(depth_range.top, depth_range.bottom);
        const auto win_hi = std::max(depth_range.top, depth_range.bottom);
        if (marker_display < win_lo || marker_display > win_hi) {
          continue;
        }
        const auto top = depth_to_top(marker.reference_depth);
        if (!std::isfinite(top)) {
          continue;
        }
        scene->markers.push_back(PreparedMarker{
            .layer_id = layer.id,
            .marker_id = marker.id,
            .display_top = Millimetres{top},
            .reference_depth = marker.reference_depth,
            .semantic = marker.semantic,
            .label_run_index = no_text_run,
        });
      }
      scene->marker_layers.push_back(PreparedMarkerLayer{
          .id = layer.id,
          .track_id = layer.track_id,
          .z_order = layer.z_order,
          .line_color = layer.line_color,
          .line_width = layer.line_width,
          .draw_symbols = layer.draw_symbols,
          .symbol_size = layer.symbol_size,
          .first_marker = first_marker,
          .marker_count =
              static_cast<std::uint64_t>(scene->markers.size()) - first_marker,
      });
    }

    std::vector<const SymbolLayerSpec *> ordered_symbol_layers;
    ordered_symbol_layers.reserve(presentation.symbol_layers().size());
    for (const auto &layer : presentation.symbol_layers()) {
      ordered_symbol_layers.push_back(&layer);
    }
    order_layers_by_z(ordered_symbol_layers);

    for (const auto *layer_pointer : ordered_symbol_layers) {
      if (stop_token.stop_requested()) {
        return cancellation_error();
      }
      const auto &layer = *layer_pointer;
      const auto bounds = track_bounds.find(layer.track_id);
      if (layer.id.is_nil() || !ids.insert(layer.id).second ||
          bounds == track_bounds.end() ||
          !std::isfinite(layer.symbol_size.value) ||
          layer.symbol_size.value <= 0.0) {
        return presentation_error(layer.id);
      }
      const auto first_symbol =
          static_cast<std::uint64_t>(scene->symbols.size());
      for (const auto &symbol : document.symbols()) {
        if (stop_token.stop_requested()) {
          return cancellation_error();
        }
        const auto symbol_display =
            map_reference_to_display(depth_xform, symbol.reference_depth);
        // Normalize the window: a decreasing display domain (TVDSS maps
        // 1000->2000, 1003->1997, so top > bottom) must not cull every
        // in-window symbol — same as the marker/interval paths (issue #462).
        const auto symbol_win_lo = std::min(depth_range.top, depth_range.bottom);
        const auto symbol_win_hi = std::max(depth_range.top, depth_range.bottom);
        if (symbol_display < symbol_win_lo || symbol_display > symbol_win_hi) {
          continue;
        }
        const auto symbol_top = depth_to_top(symbol.reference_depth);
        const auto symbol_left = bounds->second.left.value +
                                 symbol.track_fraction * bounds->second.width.value;
        if (!std::isfinite(symbol_top) || !std::isfinite(symbol_left)) {
          continue;
        }
        scene->symbols.push_back(PreparedSymbol{
            .layer_id = layer.id,
            .symbol_id = symbol.id,
            .center =
                PhysicalPoint{
                    .left = Millimetres{symbol_left},
                    .top = Millimetres{symbol_top},
                },
            .kind = symbol.kind,
            .reference_depth = symbol.reference_depth,
        });
      }
      scene->symbol_layers.push_back(PreparedSymbolLayer{
          .id = layer.id,
          .track_id = layer.track_id,
          .z_order = layer.z_order,
          .color = layer.color,
          .symbol_size = layer.symbol_size,
          .first_symbol = first_symbol,
          .symbol_count =
              static_cast<std::uint64_t>(scene->symbols.size()) - first_symbol,
      });
    }

    std::vector<const TextLayerSpec *> ordered_text_layers;
    ordered_text_layers.reserve(presentation.text_layers().size());
    for (const auto &layer : presentation.text_layers()) {
      if (layer.id.is_nil() || !ids.insert(layer.id).second ||
          !track_bounds.contains(layer.track_id)) {
        return presentation_error(layer.id);
      }
      ordered_text_layers.push_back(&layer);
    }
    order_layers_by_z(ordered_text_layers);

    // Text preparation. Shaping goes through the injected TextEngine so
    // the core stays free of text-rendering dependencies (ADR 0029); the
    // same glyph positions feed the screen and vector backends.
    std::uint64_t suppressed_runs = 0;
    std::set<std::pair<std::uint32_t, std::uint32_t>> used_glyph_keys;
    std::set<std::uint32_t> used_font_indices;
    const auto append_text_run = [&](EntityId layer_id, EntityId source_id,
                                     std::string_view text,
                                     std::string_view language,
                                     TextOrientation orientation,
                                     double rotation_degrees,
                                     Millimetres font_size, PhysicalPoint anchor,
                                     RgbaColor color) -> Result<std::uint64_t> {
      const auto direction = orientation == TextOrientation::vertical
                                 ? TextDirection::top_to_bottom
                                 : TextDirection::left_to_right;
      auto shaped = text_engine->shape(TextShapeRequest{
          .text = text,
          .language = language,
          .direction = direction,
      });
      if (!shaped.has_value()) {
        return shaped.error();
      }
      const auto scale = font_size.value;
      const auto theta = orientation == TextOrientation::rotated
                             ? rotation_degrees * 3.14159265358979323846 / 180.0
                             : 0.0;
      const auto cos_t = std::cos(theta);
      const auto sin_t = std::sin(theta);
      const auto first_glyph =
          static_cast<std::uint64_t>(scene->glyphs.size());
      auto pen_x = 0.0;
      auto pen_y = 0.0;
      auto minimum_x = std::numeric_limits<double>::infinity();
      auto minimum_y = std::numeric_limits<double>::infinity();
      auto maximum_x = -std::numeric_limits<double>::infinity();
      auto maximum_y = -std::numeric_limits<double>::infinity();
      for (const auto &glyph : shaped.value().glyphs) {
        if (stop_token.stop_requested()) {
          return cancellation_error();
        }
        // Run space is y-down millimetres relative to the anchor. Upright
        // glyphs in vertical runs are centered on the pen line; rotated
        // glyphs keep their shaped origin and turn 90 degrees.
        auto offset_x = glyph.offset_x * scale;
        auto offset_y = -glyph.offset_y * scale;
        auto glyph_rotation =
            orientation == TextOrientation::rotated ? rotation_degrees : 0.0;
        if (orientation == TextOrientation::vertical) {
          glyph_rotation = glyph.upright ? 0.0 : 90.0;
          if (glyph.upright) {
            offset_x -= 0.5 * scale;
            offset_y += 0.5 * scale;
          }
        }
        const auto run_x = pen_x + offset_x;
        const auto run_y = pen_y + offset_y;
        const auto scene_x =
            anchor.left.value + run_x * cos_t - run_y * sin_t;
        const auto scene_y =
            anchor.top.value + run_x * sin_t + run_y * cos_t;
        if (!std::isfinite(scene_x) || !std::isfinite(scene_y)) {
          return presentation_error(source_id);
        }
        used_glyph_keys.emplace(glyph.font_index, glyph.glyph_id);
        used_font_indices.insert(glyph.font_index);
        scene->glyphs.push_back(PreparedGlyph{
            .font_index = glyph.font_index,
            .glyph_id = glyph.glyph_id,
            .code_point = glyph.code_point,
            .origin =
                PhysicalPoint{
                    .left = Millimetres{scene_x},
                    .top = Millimetres{scene_y},
                },
            .rotation_degrees = glyph_rotation,
            .upright = glyph.upright,
        });
        minimum_x = std::min(minimum_x, scene_x);
        minimum_y = std::min(minimum_y, scene_y);
        maximum_x = std::max(maximum_x, scene_x);
        maximum_y = std::max(maximum_y, scene_y);
        pen_x += glyph.advance_x * scale;
        pen_y += -glyph.advance_y * scale;
      }
      if (scene->glyphs.size() == first_glyph) {
        // Nothing shaped: keep an empty run so indices stay stable.
        minimum_x = anchor.left.value;
        minimum_y = anchor.top.value;
        maximum_x = anchor.left.value;
        maximum_y = anchor.top.value;
      }
      if (!shaped.value().missing_code_points.empty()) {
        scene->text_issues.push_back(SceneTextIssue{
            .code = TextIssueCode::missing_glyphs,
            .entity_id = source_id,
            .occurrence_count = static_cast<std::uint32_t>(
                shaped.value().missing_code_points.size()),
        });
      }
      if (shaped.value().used_fallback_font) {
        scene->text_issues.push_back(SceneTextIssue{
            .code = TextIssueCode::fallback_font_used,
            .entity_id = source_id,
            .occurrence_count = 1,
        });
      }
      const auto run_index =
          static_cast<std::uint64_t>(scene->text_runs.size());
      scene->text_runs.push_back(PreparedTextRun{
          .layer_id = layer_id,
          .source_entity_id = source_id,
          .anchor = anchor,
          .orientation = orientation,
          .rotation_degrees = rotation_degrees,
          .color = color,
          .font_size = font_size,
          .bounds =
              PhysicalRect{
                  .left = Millimetres{minimum_x},
                  .top = Millimetres{minimum_y - scale},
                  .width = Millimetres{maximum_x - minimum_x + scale},
                  .height = Millimetres{maximum_y - minimum_y + 2.0 * scale},
              },
          .first_glyph = first_glyph,
          .glyph_count = static_cast<std::uint64_t>(scene->glyphs.size()) -
                         first_glyph,
          .text = std::string{text},
      });
      return run_index;
    };

    for (const auto *layer_pointer : ordered_text_layers) {
      if (stop_token.stop_requested()) {
        return cancellation_error();
      }
      const auto &layer = *layer_pointer;
      const auto first_run =
          static_cast<std::uint64_t>(scene->text_runs.size());
      for (const auto &annotation : document.annotations()) {
        if (stop_token.stop_requested()) {
          return cancellation_error();
        }
        PhysicalPoint anchor;
        switch (annotation.anchor) {
        case AnnotationAnchor::reference_depth: {
          const auto annotation_display = map_reference_to_display(
              depth_xform, annotation.reference_depth);
          // Decreasing display domain (TVDSS): compare against the
          // normalized window like markers/symbols (issue #462).
          const auto annotation_win_lo =
              std::min(depth_range.top, depth_range.bottom);
          const auto annotation_win_hi =
              std::max(depth_range.top, depth_range.bottom);
          if (annotation_display < annotation_win_lo ||
              annotation_display > annotation_win_hi) {
            continue;
          }
          const auto bounds = track_bounds.at(layer.track_id);
          anchor = PhysicalPoint{
              .left = Millimetres{bounds.left.value +
                                  annotation.track_fraction *
                                      bounds.width.value},
              .top = Millimetres{depth_to_top(annotation.reference_depth)},
          };
          break;
        }
        case AnnotationAnchor::track: {
          const auto bounds = track_bounds.find(annotation.track_id);
          if (bounds == track_bounds.end()) {
            return presentation_error(annotation.id);
          }
          anchor = PhysicalPoint{
              .left = Millimetres{bounds->second.left.value +
                                  annotation.horizontal_fraction *
                                      bounds->second.width.value},
              .top = Millimetres{annotation.depth_fraction *
                                 presentation.physical_height().value},
          };
          break;
        }
        case AnnotationAnchor::scene_point:
          anchor = annotation.scene_point;
          break;
        }
        if (!std::isfinite(anchor.left.value) ||
            !std::isfinite(anchor.top.value)) {
          return presentation_error(annotation.id);
        }
        if (text_engine == nullptr) {
          ++suppressed_runs;
          continue;
        }
        const auto run = append_text_run(
            layer.id, annotation.id, annotation.text, annotation.language,
            annotation.orientation, annotation.rotation_degrees,
            annotation.font_size, anchor, layer.color);
        if (!run.has_value()) {
          return run.error();
        }
      }
      scene->text_layers.push_back(PreparedTextLayer{
          .id = layer.id,
          .track_id = layer.track_id,
          .z_order = layer.z_order,
          .color = layer.color,
          .first_run = first_run,
          .run_count = static_cast<std::uint64_t>(scene->text_runs.size()) -
                       first_run,
      });
    }

    // Track headers: one entry per visible curve layer, rendered through
    // the same text pipeline (ADR 0023).
    const auto format_number = [](double value) {
      std::array<char, 32> buffer{};
      const auto result =
          std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                        std::chars_format::general, 6);
      return std::string{buffer.data(), result.ptr};
    };
    for (const auto &track : presentation.tracks()) {
      if (track.header.height.value <= 0.0) {
        continue;
      }
      const auto line_height =
          track.header.font_size.value * header_line_height_factor;
      std::uint64_t line = 0;
      for (const auto *layer_pointer : ordered_layers) {
        if (stop_token.stop_requested()) {
          return cancellation_error();
        }
        const auto &layer = *layer_pointer;
        if (layer.track_id != track.id || !layer.visible) {
          continue;
        }
        const auto &scale = *scales.at(layer.scale_id);
        const auto curve = std::find_if(
            document.curves().begin(), document.curves().end(),
            [&](const Curve &candidate) {
              return candidate.id == layer.curve_id;
            });
        auto name = curve->display_name.empty() ? curve->mnemonic
                                                : curve->display_name;
        auto text = name;
        text.push_back(' ');
        text += format_number(scale.minimum);
        text += "..";
        text += format_number(scale.maximum);
        text.push_back(' ');
        text += scale.unit;
        text.push_back(' ');
        text += scale.mode == ScaleMode::logarithmic ? "log" : "linear";
        if (scale.direction == ScaleDirection::right_to_left) {
          text += " rev";
        }
        auto label_run_index = no_text_run;
        if (text_engine == nullptr) {
          ++suppressed_runs;
        } else {
          const auto anchor = PhysicalPoint{
              .left = Millimetres{track_bounds.at(track.id).left.value +
                                  header_left_padding_millimetres},
              .top = Millimetres{static_cast<double>(line) * line_height +
                                 track.header.font_size.value *
                                     header_baseline_factor +
                                 header_padding_millimetres},
          };
          const auto run = append_text_run(
              layer.id, layer.id, text, "", TextOrientation::horizontal, 0.0,
              track.header.font_size, anchor, layer.color);
          if (!run.has_value()) {
            return run.error();
          }
          label_run_index = run.value();
        }
        scene->track_header_entries.push_back(PreparedTrackHeaderEntry{
            .track_id = track.id,
            .curve_layer_id = layer.id,
            .curve_name = std::move(name),
            .color = layer.color,
            .scale_minimum = scale.minimum,
            .scale_maximum = scale.maximum,
            .unit = scale.unit,
            .mode = scale.mode,
            .direction = scale.direction,
            .label_run_index = label_run_index,
        });
        ++line;
      }
    }

    // Interval and marker labels share the same text pipeline so glyphs
    // are shaped once for screen and export.
    const auto &interval_specs = presentation.interval_layers();
    for (std::size_t layer_index = 0;
         layer_index < scene->interval_layers.size(); ++layer_index) {
      if (stop_token.stop_requested()) {
        return cancellation_error();
      }
      auto &prepared_layer = scene->interval_layers[layer_index];
      const auto spec = std::find_if(
          interval_specs.begin(), interval_specs.end(),
          [&](const IntervalLayerSpec &candidate) {
            return candidate.id == prepared_layer.id;
          });
      if (spec == interval_specs.end() || !spec->draw_labels) {
        continue;
      }
      for (std::uint64_t offset = 0; offset < prepared_layer.interval_count;
           ++offset) {
        auto &interval =
            scene->intervals[static_cast<std::size_t>(
                prepared_layer.first_interval + offset)];
        const auto source = std::find_if(
            document.intervals().begin(), document.intervals().end(),
            [&](const Interval &candidate) {
              return candidate.id == interval.interval_id;
            });
        if (source == document.intervals().end() || source->label.empty()) {
          continue;
        }
        if (text_engine == nullptr) {
          ++suppressed_runs;
          continue;
        }
        const auto anchor = PhysicalPoint{
            .left = Millimetres{interval.rect.left.value + 1.0},
            .top = Millimetres{interval.rect.top.value + 0.5 +
                               spec->label_font_size.value * 0.85},
        };
        const auto run = append_text_run(
            prepared_layer.id, interval.interval_id, source->label, "",
            TextOrientation::horizontal, 0.0, spec->label_font_size, anchor,
            spec->label_color);
        if (!run.has_value()) {
          return run.error();
        }
        interval.label_run_index = run.value();
      }
    }
    const auto &marker_specs = presentation.marker_layers();
    for (std::size_t layer_index = 0;
         layer_index < scene->marker_layers.size(); ++layer_index) {
      if (stop_token.stop_requested()) {
        return cancellation_error();
      }
      auto &prepared_layer = scene->marker_layers[layer_index];
      const auto spec = std::find_if(
          marker_specs.begin(), marker_specs.end(),
          [&](const MarkerLayerSpec &candidate) {
            return candidate.id == prepared_layer.id;
          });
      if (spec == marker_specs.end() || !spec->draw_labels) {
        continue;
      }
      const auto bounds = track_bounds.at(prepared_layer.track_id);
      for (std::uint64_t offset = 0; offset < prepared_layer.marker_count;
           ++offset) {
        auto &marker = scene->markers[static_cast<std::size_t>(
            prepared_layer.first_marker + offset)];
        const auto source = std::find_if(
            document.markers().begin(), document.markers().end(),
            [&](const Marker &candidate) {
              return candidate.id == marker.marker_id;
            });
        if (source == document.markers().end() || source->label.empty()) {
          continue;
        }
        if (text_engine == nullptr) {
          ++suppressed_runs;
          continue;
        }
        const auto anchor = PhysicalPoint{
            .left = Millimetres{bounds.left.value + 1.0},
            .top = Millimetres{marker.display_top.value - 0.6},
        };
        const auto run = append_text_run(
            prepared_layer.id, marker.marker_id, source->label, "",
            TextOrientation::horizontal, 0.0, spec->label_font_size, anchor,
            spec->label_color);
        if (!run.has_value()) {
          return run.error();
        }
        marker.label_run_index = run.value();
      }
    }

    if (suppressed_runs > 0) {
      scene->text_issues.push_back(SceneTextIssue{
          .code = TextIssueCode::text_engine_unavailable,
          .entity_id = document.id(),
          .occurrence_count = static_cast<std::uint32_t>(suppressed_runs),
      });
    }

    // Resolve outlines and font metadata for every glyph the runs use so
    // the prepared scene is self-contained for both backends. The used
    // sets are empty when no engine shaped anything.
    if (text_engine == nullptr) {
      return PreparedScene{std::move(scene)};
    }
    for (const auto font_index : used_font_indices) {
      if (stop_token.stop_requested()) {
        return cancellation_error();
      }
      scene->text_fonts.push_back(PreparedTextFont{
          .index = font_index,
          .fingerprint = text_engine->font_fingerprint(font_index),
          .family_name = text_engine->font_family_name(font_index),
      });
    }
    for (const auto &[font_index, glyph_id] : used_glyph_keys) {
      if (stop_token.stop_requested()) {
        return cancellation_error();
      }
      auto outline = text_engine->glyph_outline(font_index, glyph_id);
      if (!outline.has_value()) {
        return outline.error();
      }
      const auto first_command =
          static_cast<std::uint64_t>(scene->outline_commands.size());
      for (const auto &command : outline.value().commands) {
        scene->outline_commands.push_back(command);
      }
      scene->glyph_outlines.push_back(PreparedGlyphOutline{
          .font_index = font_index,
          .glyph_id = glyph_id,
          .advance_x = outline.value().advance_x,
          .left = outline.value().left,
          .bottom = outline.value().bottom,
          .right = outline.value().right,
          .top = outline.value().top,
          .first_command = first_command,
          .command_count =
              static_cast<std::uint64_t>(scene->outline_commands.size()) -
              first_command,
      });
    }
    return PreparedScene{std::move(scene)};
  } catch (const std::bad_alloc &) {
    return Error{
        .code = ErrorCode::resource_exhausted,
        .severity = Severity::error,
        .entity_id = presentation.document_id(),
        .message = MessageKey::resource_exhausted,
        .arguments = {},
    };
  } catch (...) {
    return Error{
        .code = ErrorCode::internal_error,
        .severity = Severity::error,
        .entity_id = presentation.document_id(),
        .message = MessageKey::internal_error,
        .arguments = {},
    };
  }
}

Result<PreparedScene>
compose_multi_well_scene(std::span<const WellScenePlacement> wells,
                         Millimetres physical_height) noexcept {
  try {
    if (wells.empty() || !std::isfinite(physical_height.value) ||
        physical_height.value <= 0.0) {
      return Error{.code = ErrorCode::invalid_presentation,
                   .severity = Severity::error,
                   .entity_id = std::nullopt,
                   .message = MessageKey::presentation_invalid,
                   .arguments = {}};
    }
    auto shift_left_pt = [](PhysicalPoint &point, double dx) {
      point.left.value += dx;
    };
    auto shift_left_rect = [](PhysicalRect &rect, double dx) {
      rect.left.value += dx;
    };
    auto shift_scene_impl = [&](PreparedScene::Impl &impl, double dx,
                                EntityId document_id_override) {
      if (!document_id_override.is_nil()) {
        impl.document_id = document_id_override;
      }
      if (dx == 0.0) {
        return;
      }
      for (auto &track : impl.tracks) {
        shift_left_rect(track.bounds, dx);
        shift_left_rect(track.clip, dx);
      }
      for (auto &point : impl.curve_points) {
        shift_left_pt(point.position, dx);
      }
      for (auto &interval : impl.intervals) {
        shift_left_rect(interval.rect, dx);
      }
      for (auto &vertex : impl.fill_vertices) {
        shift_left_pt(vertex.position, dx);
      }
      for (auto &region : impl.fill_regions) {
        shift_left_rect(region.bounds, dx);
      }
      for (auto &tile : impl.image_tiles) {
        shift_left_rect(tile.rect, dx);
      }
      for (auto &symbol : impl.symbols) {
        shift_left_pt(symbol.center, dx);
      }
      for (auto &run : impl.text_runs) {
        shift_left_pt(run.anchor, dx);
        shift_left_rect(run.bounds, dx);
      }
      for (auto &vertex : impl.custom_vertices) {
        shift_left_pt(vertex, dx);
      }
    };

    auto out = std::make_shared<PreparedScene::Impl>();
    out->physical_height = physical_height;
    out->physical_width = Millimetres{0.0};
    // Use the first well as the surface document id (host reads picks'
    // document_id for true well identity).
    out->document_id = wells.front().document_id;
    out->document_revision = DocumentRevision{1};
    out->reference_depth_domain = DepthDomain::measured_depth;
    out->reference_depth_unit = "m";
    out->font_asset_fingerprint = "multi-well-surface";

    double max_right = 0.0;
    bool depth_range_set = false;

    for (const auto &placement : wells) {
      if (placement.scene == nullptr || placement.scene->impl_ == nullptr) {
        continue;
      }
      const auto &src = *placement.scene->impl_;
      const auto dx = placement.left.value;
      // Copy source then shift (avoids mutating the live per-well scene).
      PreparedScene::Impl local = src;
      shift_scene_impl(local, dx, placement.document_id);
      // Depth range: union of wells (shared display depth should match).
      if (!depth_range_set) {
        out->document_revision = local.document_revision.value == 0
                                     ? DocumentRevision{1}
                                     : local.document_revision;
        out->reference_depth_domain = local.reference_depth_domain;
        out->reference_depth_unit = local.reference_depth_unit;
        out->reference_depth_top = local.reference_depth_top;
        out->reference_depth_bottom = local.reference_depth_bottom;
        out->presentation_version = local.presentation_version;
        out->font_asset_fingerprint = local.font_asset_fingerprint;
        depth_range_set = true;
      }
      // Append collections with index remapping for segments/points.
      const auto point_base = out->curve_points.size();
      const auto segment_base = out->curve_segments.size();
      const auto layer_base = out->curve_layers.size();
      for (const auto &point : local.curve_points) {
        out->curve_points.push_back(point);
      }
      for (auto segment : local.curve_segments) {
        segment.first_point += static_cast<std::uint64_t>(point_base);
        out->curve_segments.push_back(segment);
      }
      for (auto layer : local.curve_layers) {
        layer.first_segment += static_cast<std::uint64_t>(segment_base);
        out->curve_layers.push_back(layer);
      }
      for (const auto &track : local.tracks) {
        out->tracks.push_back(track);
      }
      const auto interval_base = out->intervals.size();
      const auto run_base = out->text_runs.size();
      for (auto interval : local.intervals) {
        if (interval.label_run_index != no_text_run) {
          interval.label_run_index += static_cast<std::uint64_t>(run_base);
        }
        out->intervals.push_back(interval);
      }
      for (auto layer : local.interval_layers) {
        layer.first_interval += static_cast<std::uint64_t>(interval_base);
        out->interval_layers.push_back(layer);
      }
      const auto marker_base = out->markers.size();
      for (auto marker : local.markers) {
        if (marker.label_run_index != no_text_run) {
          marker.label_run_index += static_cast<std::uint64_t>(run_base);
        }
        out->markers.push_back(marker);
      }
      for (auto layer : local.marker_layers) {
        layer.first_marker += static_cast<std::uint64_t>(marker_base);
        out->marker_layers.push_back(layer);
      }
      // Crossover fill / symbol / image / text / custom layers (issue #472):
      // shift_scene_impl already translates their geometry; append them with
      // index remapping so multi-well composites stop losing fills, text,
      // images and custom layers (the shifts used to be dead code).
      const auto symbol_base = out->symbols.size();
      for (const auto &symbol : local.symbols) {
        out->symbols.push_back(symbol);
      }
      for (auto layer : local.symbol_layers) {
        layer.first_symbol += static_cast<std::uint64_t>(symbol_base);
        out->symbol_layers.push_back(layer);
      }
      const auto fill_vertex_base = out->fill_vertices.size();
      const auto fill_triangle_base = out->fill_triangles.size();
      for (const auto &vertex : local.fill_vertices) {
        out->fill_vertices.push_back(vertex);
      }
      for (auto triangle : local.fill_triangles) {
        triangle.a = static_cast<std::uint32_t>(
            triangle.a + static_cast<std::uint64_t>(fill_vertex_base));
        triangle.b = static_cast<std::uint32_t>(
            triangle.b + static_cast<std::uint64_t>(fill_vertex_base));
        triangle.c = static_cast<std::uint32_t>(
            triangle.c + static_cast<std::uint64_t>(fill_vertex_base));
        out->fill_triangles.push_back(triangle);
      }
      const auto fill_region_base = out->fill_regions.size();
      for (auto region : local.fill_regions) {
        region.first_vertex += static_cast<std::uint64_t>(fill_vertex_base);
        region.first_triangle += static_cast<std::uint64_t>(fill_triangle_base);
        out->fill_regions.push_back(region);
      }
      for (auto layer : local.fill_layers) {
        layer.first_region += static_cast<std::uint64_t>(fill_region_base);
        out->fill_layers.push_back(layer);
      }
      const auto tile_base = out->image_tiles.size();
      for (const auto &tile : local.image_tiles) {
        out->image_tiles.push_back(tile);
      }
      for (auto layer : local.image_layers) {
        layer.first_tile += static_cast<std::uint64_t>(tile_base);
        out->image_layers.push_back(layer);
      }
      const auto glyph_base = out->glyphs.size();
      for (const auto &glyph : local.glyphs) {
        out->glyphs.push_back(glyph);
      }
      for (auto run : local.text_runs) {
        run.first_glyph += static_cast<std::uint64_t>(glyph_base);
        out->text_runs.push_back(run);
      }
      for (auto layer : local.text_layers) {
        layer.first_run += static_cast<std::uint64_t>(run_base);
        out->text_layers.push_back(layer);
      }
      // Glyph outlines/commands are keyed (font_index, glyph_id); skip
      // outlines the composite already carries so SVG defs stay unique.
      for (const auto &outline : local.glyph_outlines) {
        const auto already = std::any_of(
            out->glyph_outlines.begin(), out->glyph_outlines.end(),
            [&](const PreparedGlyphOutline &existing) {
              return existing.font_index == outline.font_index &&
                     existing.glyph_id == outline.glyph_id;
            });
        if (already) {
          continue;
        }
        out->glyph_outlines.push_back(outline);
      }
      const auto custom_vertex_base = out->custom_vertices.size();
      for (const auto &vertex : local.custom_vertices) {
        out->custom_vertices.push_back(vertex);
      }
      const auto custom_primitive_base = out->custom_primitives.size();
      for (auto primitive : local.custom_primitives) {
        primitive.first_vertex += static_cast<std::uint64_t>(custom_vertex_base);
        out->custom_primitives.push_back(primitive);
      }
      for (auto layer : local.custom_layers) {
        layer.first_primitive +=
            static_cast<std::uint64_t>(custom_primitive_base);
        out->custom_layers.push_back(layer);
      }
      for (const auto &pattern : local.patterns) {
        const auto already = std::any_of(
            out->patterns.begin(), out->patterns.end(),
            [&](const PatternDefinition &existing) {
              return existing.id == pattern.id;
            });
        if (!already) {
          out->patterns.push_back(pattern);
        }
      }
      // Remap pick indices for this well's layers.
      for (auto pick : local.curve_pick_indices) {
        pick.layer_index += static_cast<std::uint64_t>(layer_base);
        for (auto &primitive : pick.primitives) {
          primitive.first_point += static_cast<std::uint64_t>(point_base);
          primitive.second_point += static_cast<std::uint64_t>(point_base);
        }
        out->curve_pick_indices.push_back(std::move(pick));
      }
      const auto right =
          placement.left.value + local.physical_width.value;
      max_right = std::max(max_right, right);
    }
    if (out->tracks.empty() && out->curve_layers.empty()) {
      return Error{.code = ErrorCode::invalid_presentation,
                   .severity = Severity::error,
                   .entity_id = std::nullopt,
                   .message = MessageKey::presentation_invalid,
                   .arguments = {}};
    }
    out->physical_width = Millimetres{max_right};
    return PreparedScene{std::move(out)};
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

std::optional<CurvePick>
pick_curve_multi_well(std::span<const WellScenePlacement> wells,
                      const CurvePickQuery &query) noexcept {
  // Right-to-left so a well drawn later (higher z / right) wins ties.
  std::optional<CurvePick> best;
  for (auto it = wells.rbegin(); it != wells.rend(); ++it) {
    if (it->scene == nullptr) {
      continue;
    }
    CurvePickQuery local = query;
    local.scene_position.left.value =
        query.scene_position.left.value - it->left.value;
    auto hit = it->scene->pick_curve(local);
    if (hit.has_value()) {
      hit->document_id = it->document_id;
      if (best.has_value() &&
          hit->distance.value >= best->distance.value) {
        continue;
      }
      best = hit;
    }
  }
  return best;
}

Result<PreparedScene> append_surface_overlay_geometry(
    PreparedScene surface,
    std::span<const SurfaceOverlayGeometry> overlays) noexcept {
  try {
    if (surface.impl_ == nullptr || overlays.empty()) {
      return surface;
    }
    // Copy so we never mutate a shared per-well or previously composed scene.
    auto out = std::make_shared<PreparedScene::Impl>(*surface.impl_);
    const auto width = out->physical_width.value;
    const auto height = out->physical_height.value;
    if (!std::isfinite(width) || width <= 0.0 || !std::isfinite(height) ||
        height <= 0.0) {
      return Error{.code = ErrorCode::invalid_presentation,
                   .severity = Severity::error,
                   .entity_id = std::nullopt,
                   .message = MessageKey::presentation_invalid,
                   .arguments = {}};
    }

    // Stable synthetic track id for the full-surface overlay lane.
    static const EntityId k_overlay_track =
        *EntityId::parse("16100000-0000-4000-8000-00000000ff01");
    static const EntityId k_overlay_layer =
        *EntityId::parse("16100000-0000-4000-8000-00000000ff02");

    const PhysicalRect surface_rect{
        .left = Millimetres{0.0},
        .top = Millimetres{0.0},
        .width = Millimetres{width},
        .height = Millimetres{height},
    };
    // Highest z so overlays sit above well tracks when SVG iterates by track.
    std::int32_t max_z = 0;
    for (const auto &track : out->tracks) {
      max_z = std::max(max_z, track.z_order);
    }
    for (const auto &overlay : overlays) {
      max_z = std::max(max_z, overlay.z_order);
    }
    out->tracks.push_back(PreparedTrack{
        .id = k_overlay_track,
        .bounds = surface_rect,
        .clip = surface_rect,
        .z_order = max_z + 1,
    });

    const auto first_primitive =
        static_cast<std::uint64_t>(out->custom_primitives.size());
    std::uint64_t primitive_count = 0;

    auto bounds_of_points =
        [](std::initializer_list<PhysicalPoint> points) -> PhysicalRect {
      double min_l = std::numeric_limits<double>::infinity();
      double min_t = std::numeric_limits<double>::infinity();
      double max_l = -std::numeric_limits<double>::infinity();
      double max_t = -std::numeric_limits<double>::infinity();
      for (const auto &p : points) {
        min_l = std::min(min_l, p.left.value);
        min_t = std::min(min_t, p.top.value);
        max_l = std::max(max_l, p.left.value);
        max_t = std::max(max_t, p.top.value);
      }
      return PhysicalRect{
          .left = Millimetres{min_l},
          .top = Millimetres{min_t},
          .width = Millimetres{max_l - min_l},
          .height = Millimetres{max_t - min_t},
      };
    };

    for (const auto &overlay : overlays) {
      if (overlay.id.is_nil()) {
        continue;
      }
      if (overlay.kind == SurfaceOverlayGeometry::Kind::horizon_line) {
        const auto first_vertex =
            static_cast<std::uint64_t>(out->custom_vertices.size());
        out->custom_vertices.push_back(overlay.left_top);
        out->custom_vertices.push_back(overlay.right_top);
        out->custom_primitives.push_back(PreparedCustomPrimitive{
            .layer_id = k_overlay_layer,
            .source_id = overlay.id,
            .source_primitive_index = primitive_count,
            .kind = CustomPrimitiveKind::polyline,
            .color = overlay.color,
            .stroke_width = overlay.line_width,
            .first_vertex = first_vertex,
            .vertex_count = 2,
            .closed = false,
            .bounds = bounds_of_points({overlay.left_top, overlay.right_top}),
        });
        ++primitive_count;
      } else {
        // Correlation band: two triangles (TL, TR, BR) + (TL, BR, BL).
        const auto first_vertex =
            static_cast<std::uint64_t>(out->custom_vertices.size());
        const auto &lt = overlay.left_top;
        const auto &rt = overlay.right_top;
        const auto &rb = overlay.right_bottom;
        const auto &lb = overlay.left_bottom;
        out->custom_vertices.push_back(lt);
        out->custom_vertices.push_back(rt);
        out->custom_vertices.push_back(rb);
        out->custom_vertices.push_back(lt);
        out->custom_vertices.push_back(rb);
        out->custom_vertices.push_back(lb);
        out->custom_primitives.push_back(PreparedCustomPrimitive{
            .layer_id = k_overlay_layer,
            .source_id = overlay.id,
            .source_primitive_index = primitive_count,
            .kind = CustomPrimitiveKind::quad,
            .color = overlay.color,
            .first_vertex = first_vertex,
            .vertex_count = 6,
            .bounds = bounds_of_points({lt, rt, rb, lb}),
        });
        ++primitive_count;
      }
    }

    if (primitive_count == 0) {
      // No overlay geometry survived: roll back the synthetic overlay track
      // pushed above so the scene carries no empty extra track (SVG emits a
      // stray group, picking walks one extra track — issue #480).
      if (!out->tracks.empty() && out->tracks.back().id == k_overlay_track) {
        out->tracks.pop_back();
      }
      return PreparedScene{std::move(out)};
    }
    out->custom_layers.push_back(PreparedCustomLayer{
        .id = k_overlay_layer,
        .track_id = k_overlay_track,
        .custom_source_id = k_overlay_layer,
        .z_order = max_z + 1,
        .first_primitive = first_primitive,
        .primitive_count = primitive_count,
        .clip_path_index = no_clip,
        .visible = true,
    });
    return PreparedScene{std::move(out)};
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

} // namespace welllog
