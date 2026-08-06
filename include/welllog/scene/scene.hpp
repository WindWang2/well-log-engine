#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <welllog/core/document.hpp>
#include <welllog/core/result.hpp>
#include <welllog/core/units.hpp>
#include <welllog/scene/export.hpp>
#include <welllog/scene/text_engine.hpp>

namespace welllog {

struct ReferenceDepthRange {
  DepthDomain domain{DepthDomain::measured_depth};
  std::string_view unit;
  double top{};
  double bottom{};
};

// Monotonically increasing version of a presentation/profile, captured so an
// export is self-describing and reproducible (table-and-export.md section 8.1).
struct PresentationVersion {
  std::uint64_t value{};
  friend constexpr bool operator==(PresentationVersion,
                                   PresentationVersion) = default;
};

// A lightweight descriptor of the depth mapping that produced a scene's
// reference depth axis: the domain/unit of the source axis, the top/bottom
// reference-depth window, and a version tag. This is NOT the full reversible
// depth-transform chain of ADR 0013 (which composes measured/true
// vertical/sub-sea transforms) — only enough for an export snapshot to record
// which depth mapping it was produced against (#186). The full chain remains a
// later topic.
struct DepthTransformDescriptor {
  DepthDomain domain{DepthDomain::measured_depth};
  // Owning (not string_view): the descriptor is copied into ExportSnapshot,
  // which is a portable value type that may outlive the PreparedScene whose
  // storage a non-owning view would point into.
  std::string unit;
  double reference_top{};
  double reference_bottom{};
  std::uint64_t version{};
  friend constexpr bool operator==(const DepthTransformDescriptor &,
                                   const DepthTransformDescriptor &) = default;
};

// Piecewise-linear, strictly monotonic, reversible depth map (#161, ADR 0013).
// Empty control_points ⇒ identity (reference_depth == display_depth).
struct DepthControlPoint {
  double reference_depth{};
  double display_depth{};
};

enum class DepthExtrapolatePolicy : std::uint8_t {
  // Outside the control span, clamp to the nearest endpoint display/reference.
  clamp,
  // Continue the first/last segment slope (default).
  linear,
};

struct DepthTransform {
  std::vector<DepthControlPoint> control_points;
  DepthExtrapolatePolicy extrapolate{DepthExtrapolatePolicy::linear};
  std::uint64_t version{};
  friend bool operator==(const DepthTransform &,
                         const DepthTransform &) = default;
};

// Validates strict ordering of reference_depth and same-direction monotonic
// display_depth. Empty transforms are valid (identity). Returns nullopt on
// success, or an Error diagnostic on conflict/non-monotonic control points.
[[nodiscard]] WELLLOG_SCENE_API std::optional<Error>
validate_depth_transform(const DepthTransform &transform) noexcept;

// Maps Reference Depth → Display Depth. Identity when control_points empty.
[[nodiscard]] WELLLOG_SCENE_API double
map_reference_to_display(const DepthTransform &transform,
                         double reference_depth) noexcept;

// Inverse map Display Depth → Reference Depth (unique when valid).
[[nodiscard]] WELLLOG_SCENE_API double
map_display_to_reference(const DepthTransform &transform,
                         double display_depth) noexcept;

// Builds a transform that places each source marker reference depth at the
// corresponding target display depth (typically target's reference depth under
// identity). Pairs must be sorted by source reference and form a valid transform.
[[nodiscard]] WELLLOG_SCENE_API Result<DepthTransform>
depth_transform_aligning_markers(
    std::span<const double> source_reference_depths,
    std::span<const double> target_display_depths) noexcept;

struct DeviceIndependentPixels {
  double value{};
  friend constexpr bool operator==(DeviceIndependentPixels,
                                   DeviceIndependentPixels) = default;
};

enum class ScaleDirection : std::uint8_t {
  left_to_right,
  right_to_left,
};

enum class ScaleMode : std::uint8_t {
  linear,
  logarithmic,
};

// Optional per-track header configuration. When `height` is zero the
// track renders no header; otherwise each visible curve layer gets a
// header line with its name, color, range, unit and scale type (ADR 0023).
struct TrackHeaderSpec {
  Millimetres height{0.0};
  Millimetres font_size{2.5};
};

struct TrackSpec {
  EntityId id;
  Millimetres width;
  std::int32_t z_order{};
  TrackHeaderSpec header{};
};

struct TrackScaleSpec {
  EntityId id;
  EntityId track_id;
  ScaleMode mode{ScaleMode::linear};
  double minimum{};
  double maximum{};
  ScaleDirection direction{ScaleDirection::left_to_right};
  std::string unit;
};

// How a curve layer treats QC Mask states when preparing geometry (#159).
// Masks never rewrite values; this policy only affects drawability.
struct QcDisplayPolicy {
  bool hide_suspect{false};
  bool hide_invalid{true};
  bool hide_user_excluded{true};
};

struct CurveLayerSpec {
  EntityId id;
  EntityId track_id;
  EntityId curve_id;
  EntityId scale_id;
  RgbaColor color;
  Millimetres line_width;
  std::int32_t z_order{};
  // Hidden layers keep their identity, style and visibility in the
  // prepared scene but contribute no geometry.
  bool visible{true};
  QcDisplayPolicy qc_display{};
};

// A straight segment inside a pattern tile, in tile-local millimetres.
struct PatternLine {
  PhysicalPoint from;
  PhysicalPoint to;
};

struct PatternPolyline {
  std::vector<PhysicalPoint> points;
  bool closed{};
};

struct PatternCircle {
  PhysicalPoint center;
  Millimetres radius;
  bool filled{};
};

// The constrained vector vocabulary allowed inside a PatternDefinition.
// Scripts, shaders and external resources are never accepted (ADR 0020,
// ADR 0042).
using PatternPrimitive =
    std::variant<PatternLine, PatternPolyline, PatternCircle>;

// The single vector source of truth for a geological pattern. The tile is
// a repeating unit with physical size, anchored to `scene_anchor` in scene
// millimetres so that scrolling and adjacent intervals share phase.
// Screen backends rasterize the tile into an atlas; vector backends emit
// the primitives directly.
struct PatternDefinition {
  EntityId id;
  Millimetres tile_width;
  Millimetres tile_height;
  double rotation_degrees{};
  RgbaColor foreground{};
  RgbaColor background{};
  Millimetres stroke_width{0.2};
  PhysicalPoint scene_anchor{};
  std::vector<PatternPrimitive> primitives;
  // Version of the pattern's vector source, captured by the export snapshot so
  // an export is reproducible (table-and-export.md section 8.1).
  std::uint64_t version{};
};

// Displays document Intervals as clipped, filled (solid or patterned)
// rectangles spanning the track.
struct IntervalLayerSpec {
  EntityId id;
  EntityId track_id;
  std::int32_t z_order{};
  bool draw_labels{true};
  Millimetres label_font_size{3.0};
  RgbaColor label_color{0, 0, 0, 255};
};

// Which enclosed side of a crossover to fill (rendering.md section 6).
enum class CrossoverFillRule : std::uint8_t {
  // Fill where the upper curve's mapped x is to the right of the lower's
  // (upper-minus-lower). Extensible with further rules without reshaping
  // the spec.
  upper_minus_lower,
};

// Fills the enclosed region between two curve layers whose mapped track-x
// polylines cross (ADR 0017; rendering.md section 6). The fill boundary is
// computed from the mapped x-coordinates, never raw values; curves may use
// different scales, units and directions. Exactly one of fill_color /
// pattern_id must be set.
struct CrossoverFillLayerSpec {
  EntityId id;
  EntityId track_id;
  std::int32_t z_order{};
  EntityId upper_curve_layer_id;
  EntityId lower_curve_layer_id;
  CrossoverFillRule rule{CrossoverFillRule::upper_minus_lower};
  std::optional<RgbaColor> fill_color;
  std::optional<EntityId> pattern_id;
  bool visible{true};
};

// Displays a raster ImageSource registered by depth, decoding/uploading only
// the visible tiles (+ limited prefetch) of a multi-resolution pyramid
// (rendering.md section 10). The image spans the full track width over its
// depth range; pixels are supplied by the host via the image_tile resolver.
struct ImageLayerSpec {
  EntityId id;
  EntityId track_id;
  EntityId image_source_id;
  std::int32_t z_order{};
  bool visible{true};
};

// Displays document Markers as zero-thickness horizontal lines across the
// track with optional labels.
struct MarkerLayerSpec {
  EntityId id;
  EntityId track_id;
  std::int32_t z_order{};
  RgbaColor line_color{0, 0, 0, 255};
  Millimetres line_width{0.3};
  bool draw_labels{true};
  Millimetres label_font_size{3.0};
  RgbaColor label_color{0, 0, 0, 255};
};

// Displays document SymbolOccurrences at their depth and track fraction.
struct SymbolLayerSpec {
  EntityId id;
  EntityId track_id;
  std::int32_t z_order{};
  RgbaColor color{0, 0, 0, 255};
  Millimetres symbol_size{3.0};
};

// Displays document TextAnnotations, including rotated and true vertical
// typesetting.
struct TextLayerSpec {
  EntityId id;
  EntityId track_id;
  std::int32_t z_order{};
  RgbaColor color{0, 0, 0, 255};
};

// Displays a host-authored declarative CustomLayerSource (ADR 0018/0046). The
// layer is a thin reference to a document custom source (which owns the
// primitives); it participates in track layout, z-order, visibility and the
// upload budget like every other layer. The primitives are decomposed by the
// scene kernel into the shared primitive streams — no host GL/shader code.
struct CustomLayerSpec {
  EntityId id;
  EntityId track_id;
  EntityId custom_source_id;
  std::int32_t z_order{};
  bool visible{true};
};

class WELLLOG_SCENE_API ScenePresentation {
public:
  ScenePresentation();
  ~ScenePresentation();
  ScenePresentation(const ScenePresentation &);
  ScenePresentation &operator=(const ScenePresentation &);
  ScenePresentation(ScenePresentation &&) noexcept;
  ScenePresentation &operator=(ScenePresentation &&) noexcept;

  [[nodiscard]] EntityId document_id() const noexcept;
  [[nodiscard]] ReferenceDepthRange reference_depth_range() const noexcept;
  [[nodiscard]] Millimetres physical_height() const noexcept;
  [[nodiscard]] std::string_view font_asset_fingerprint() const noexcept;
  [[nodiscard]] PresentationVersion presentation_version() const noexcept;
  [[nodiscard]] DepthTransformDescriptor depth_transform() const noexcept;
  // Full piecewise Depth Transform (#161). Empty control points = identity.
  [[nodiscard]] const DepthTransform &depth_transform_map() const noexcept;
  [[nodiscard]] std::span<const TrackSpec> tracks() const noexcept;
  [[nodiscard]] std::span<const TrackScaleSpec> scales() const noexcept;
  [[nodiscard]] std::span<const CurveLayerSpec> curve_layers() const noexcept;
  [[nodiscard]] std::span<const PatternDefinition> patterns() const noexcept;
  [[nodiscard]] std::span<const IntervalLayerSpec>
  interval_layers() const noexcept;
  [[nodiscard]] std::span<const CrossoverFillLayerSpec>
  crossover_fill_layers() const noexcept;
  [[nodiscard]] std::span<const ImageLayerSpec>
  image_layers() const noexcept;
  [[nodiscard]] std::span<const MarkerLayerSpec>
  marker_layers() const noexcept;
  [[nodiscard]] std::span<const SymbolLayerSpec>
  symbol_layers() const noexcept;
  [[nodiscard]] std::span<const TextLayerSpec> text_layers() const noexcept;
  [[nodiscard]] std::span<const CustomLayerSpec>
  custom_layers() const noexcept;

private:
  struct Impl;
  explicit ScenePresentation(std::shared_ptr<const Impl> impl);
  std::shared_ptr<const Impl> impl_;
  friend class ScenePresentationBuilder;
};

class WELLLOG_SCENE_API ScenePresentationBuilder {
public:
  ScenePresentationBuilder(EntityId document_id,
                           ReferenceDepthRange reference_depth_range,
                           Millimetres physical_height,
                           std::string_view font_asset_fingerprint) noexcept;
  ~ScenePresentationBuilder();
  ScenePresentationBuilder(ScenePresentationBuilder &&) noexcept;
  ScenePresentationBuilder &operator=(ScenePresentationBuilder &&) noexcept;
  ScenePresentationBuilder(const ScenePresentationBuilder &) = delete;
  ScenePresentationBuilder &
  operator=(const ScenePresentationBuilder &) = delete;

  ScenePresentationBuilder &add_track(const TrackSpec &track) noexcept;
  // Export-metadata setters (ADR 0048): optional version tags recorded by the
  // export snapshot so an export is self-describing and reproducible. Defaults
  // to version 0 when unset, which keeps existing presentations byte-identical.
  ScenePresentationBuilder &
  set_presentation_version(PresentationVersion version) noexcept;
  ScenePresentationBuilder &
  set_depth_transform_version(std::uint64_t version) noexcept;
  // Sets the full reversible Depth Transform (#161). Rejects invalid maps at
  // prepare/preflight time. Also bumps the export version tag when non-zero.
  ScenePresentationBuilder &
  set_depth_transform(const DepthTransform &transform) noexcept;
  ScenePresentationBuilder &add_scale(const TrackScaleSpec &scale) noexcept;
  ScenePresentationBuilder &
  add_curve_layer(const CurveLayerSpec &layer) noexcept;
  ScenePresentationBuilder &add_pattern(const PatternDefinition &pattern) noexcept;
  ScenePresentationBuilder &
  add_interval_layer(const IntervalLayerSpec &layer) noexcept;
  ScenePresentationBuilder &
  add_crossover_fill_layer(const CrossoverFillLayerSpec &layer) noexcept;
  ScenePresentationBuilder &add_image_layer(const ImageLayerSpec &layer) noexcept;
  ScenePresentationBuilder &
  add_marker_layer(const MarkerLayerSpec &layer) noexcept;
  ScenePresentationBuilder &
  add_symbol_layer(const SymbolLayerSpec &layer) noexcept;
  ScenePresentationBuilder &
  add_text_layer(const TextLayerSpec &layer) noexcept;
  ScenePresentationBuilder &
  add_custom_layer(const CustomLayerSpec &layer) noexcept;
  [[nodiscard]] ScenePresentation build() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct PreparedTrack {
  EntityId id;
  PhysicalRect bounds;
  PhysicalRect clip;
  std::int32_t z_order{};
};

struct PreparedCurveLayer {
  EntityId id;
  EntityId track_id;
  EntityId curve_id;
  EntityId scale_id;
  RgbaColor color;
  Millimetres line_width;
  std::int32_t z_order{};
  std::uint64_t first_segment{};
  std::uint64_t segment_count{};
  bool visible{true};
};

struct PreparedCurveSegment {
  EntityId layer_id;
  std::uint64_t first_point{};
  std::uint64_t point_count{};
};

struct PreparedCurvePoint {
  PhysicalPoint position;
  std::uint64_t sample_index{};
  double reference_depth{};
  // Display Depth after the well's Depth Transform (#161). Equals
  // reference_depth when the transform is identity.
  double display_depth{};
  double value{};
};

// Sentinel for layer members that have no associated prepared text run.
inline constexpr std::uint64_t no_text_run =
    std::numeric_limits<std::uint64_t>::max();

// Sentinel for a custom layer that declares no clip path.
inline constexpr std::uint64_t no_clip =
    std::numeric_limits<std::uint64_t>::max();

// One track header line (ADR 0023): everything the header shows about a
// curve, plus the prepared text run rendering it (or `no_text_run`).
struct PreparedTrackHeaderEntry {
  EntityId track_id;
  EntityId curve_layer_id;
  std::string curve_name;
  RgbaColor color{};
  double scale_minimum{};
  double scale_maximum{};
  std::string unit;
  ScaleMode mode{ScaleMode::linear};
  ScaleDirection direction{ScaleDirection::left_to_right};
  std::uint64_t label_run_index{no_text_run};
};

struct PreparedIntervalLayer {
  EntityId id;
  EntityId track_id;
  std::int32_t z_order{};
  std::uint64_t first_interval{};
  std::uint64_t interval_count{};
};

// An interval clipped to its track bounds. `rect` is in scene millimetres;
// pattern tiling is anchored at the pattern's `scene_anchor`, so adjacent
// intervals and scrolling share phase. `label_run_index` indexes
// PreparedScene::text_runs or equals `no_text_run`.
struct PreparedInterval {
  EntityId layer_id;
  EntityId interval_id;
  PhysicalRect rect{};
  RgbaColor fill_color{};
  EntityId pattern_id;
  double top_reference_depth{};
  double bottom_reference_depth{};
  std::uint64_t label_run_index{no_text_run};
};

struct PreparedFillLayer {
  EntityId id;
  EntityId track_id;
  std::int32_t z_order{};
  std::uint64_t first_region{};
  std::uint64_t region_count{};
};

// A fill-region boundary vertex (scene millimetres).
struct PreparedFillVertex {
  PhysicalPoint position{};
};

// Triangle indices into PreparedScene::fill_vertices.
struct PreparedFillTriangle {
  std::uint32_t a{};
  std::uint32_t b{};
  std::uint32_t c{};
};

// One enclosed region between two crossing curve polylines (rendering.md
// section 6). `first_vertex/vertex_count` index the closed boundary ring
// (used by SVG path emission and point-in-polygon picking);
// `first_triangle/triangle_count` index its triangulation (consumed by GL).
// Both dependent curve layers are carried so a pick can return them.
struct PreparedFillRegion {
  EntityId layer_id;
  std::uint64_t first_vertex{};
  std::uint64_t vertex_count{};
  std::uint64_t first_triangle{};
  std::uint64_t triangle_count{};
  RgbaColor fill_color{};
  EntityId pattern_id;
  EntityId upper_curve_layer_id;
  EntityId lower_curve_layer_id;
  double top_reference_depth{};
  double bottom_reference_depth{};
  PhysicalRect bounds{};
};

struct PreparedImageLayer {
  EntityId id;
  EntityId track_id;
  EntityId image_source_id;
  std::int32_t z_order{};
  std::uint64_t first_tile{};
  std::uint64_t tile_count{};
};

// One visible tile of a raster image, placed in scene millimetres. The tile
// spans the full track width over its depth slice; `level/row/col` identify
// the pyramid tile the host resolves into decoded pixels. `pixel_format`
// records the bytes the resolver will supply (the engine never decodes).
struct PreparedImageTile {
  EntityId layer_id;
  EntityId image_source_id;
  PhysicalRect rect{};
  std::uint32_t level{};
  std::uint32_t row{};
  std::uint32_t col{};
  std::uint32_t width_px{};
  std::uint32_t height_px{};
  PixelFormat pixel_format{PixelFormat::rgba8};
  std::uint32_t dpi{};
  BufferSourceReference source;
};

struct PreparedMarkerLayer {
  EntityId id;
  EntityId track_id;
  std::int32_t z_order{};
  RgbaColor line_color{};
  Millimetres line_width{};
  std::uint64_t first_marker{};
  std::uint64_t marker_count{};
};

// A zero-thickness marker line spanning the full track width at
// `display_top` (scene millimetres).
struct PreparedMarker {
  EntityId layer_id;
  EntityId marker_id;
  Millimetres display_top{};
  double reference_depth{};
  std::uint64_t label_run_index{no_text_run};
};

struct PreparedSymbolLayer {
  EntityId id;
  EntityId track_id;
  std::int32_t z_order{};
  RgbaColor color{};
  Millimetres symbol_size{};
  std::uint64_t first_symbol{};
  std::uint64_t symbol_count{};
};

struct PreparedSymbol {
  EntityId layer_id;
  EntityId symbol_id;
  PhysicalPoint center{};
  SymbolKind kind{SymbolKind::circle};
  double reference_depth{};
};

// Metadata for one font face used by the prepared text runs. The index
// matches PreparedGlyph::font_index and PreparedGlyphOutline::font_index.
struct PreparedTextFont {
  std::uint32_t index{};
  std::string fingerprint;
  std::string family_name;
};

// One positioned glyph. `origin` is the baseline origin in scene
// millimetres with the run rotation already applied to the pen position;
// `rotation_degrees` rotates the glyph itself around `origin` (run
// rotation plus 90 degrees for rotated glyphs in vertical typesetting).
struct PreparedGlyph {
  std::uint32_t font_index{};
  std::uint32_t glyph_id{};
  char32_t code_point{};
  PhysicalPoint origin{};
  double rotation_degrees{};
  bool upright{true};
};

struct PreparedTextRun {
  EntityId layer_id;
  EntityId source_entity_id;
  PhysicalPoint anchor{};
  TextOrientation orientation{TextOrientation::horizontal};
  double rotation_degrees{};
  RgbaColor color{};
  Millimetres font_size{};
  PhysicalRect bounds{};
  std::uint64_t first_glyph{};
  std::uint64_t glyph_count{};
  std::string text;
};

struct PreparedTextLayer {
  EntityId id;
  EntityId track_id;
  std::int32_t z_order{};
  RgbaColor color{};
  std::uint64_t first_run{};
  std::uint64_t run_count{};
};

// A flattened triangle/quad/symbol primitive from a CustomLayerSource, in scene
// millimetres. `first_vertex/vertex_count` index PreparedScene::custom_vertices
// (triangles contribute 3, quads 6, symbols a variable fan — same layout the
// GL renderer already builds for the built-in symbol layer). Polylines are NOT
// stored here: they are decomposed into PreparedCurveSegment/PreparedCurvePoint
// (so GL draws them as edges and SVG emits them as paths with no new code) and
// their identity is recovered via the layer_id on the segment.
struct PreparedCustomPrimitive {
  EntityId layer_id;
  EntityId source_id;
  std::uint64_t source_primitive_index{};
  CustomPrimitiveKind kind{CustomPrimitiveKind::triangle};
  RgbaColor color{};
  // For polylines: the stroke width. Unused by filled kinds.
  Millimetres stroke_width{0.3};
  std::uint64_t first_vertex{};
  std::uint64_t vertex_count{};
  // For polylines: whether the ring is closed (a final segment joins the last
  // point back to the first). Unused by filled kinds.
  bool closed{};
  // For polylines: the dash pattern (empty = solid, ADR 0050).
  DashPattern dash_pattern{};
  // For quads: references a PatternDefinition on the ScenePresentation; nil =
  // solid fill (ADR 0050, same mechanism as Interval).
  EntityId pattern_id{};
  // For symbols: the symbol kind (circle/square/...). Unused by other kinds.
  SymbolKind symbol_kind{SymbolKind::circle};
  // Triangle/quad extent in scene millimetres (used by point-in-shape picking).
  PhysicalRect bounds{};
};

struct PreparedCustomLayer {
  EntityId id;
  EntityId track_id;
  EntityId custom_source_id;
  std::int32_t z_order{};
  std::uint64_t first_primitive{};
  std::uint64_t primitive_count{};
  // Index into PreparedScene::custom_clip_paths, or no_clip when the source
  // declares no clip path.
  std::uint64_t clip_path_index{no_clip};
  bool visible{true};
};

// A flattened clip path (scene millimetres) masking one custom layer's own
// primitives. `points` is the closed ring used by SVG clip-path emission and
// by pick containment testing.
struct PreparedCustomClipPath {
  std::vector<PhysicalPoint> points;
};

// Vector outline of one glyph in em fractions (y-up, glyph-local), shared
// by the vector exporters and by the screen backend's atlas rasterizer.
struct PreparedGlyphOutline {
  std::uint32_t font_index{};
  std::uint32_t glyph_id{};
  double advance_x{};
  double left{};
  double bottom{};
  double right{};
  double top{};
  std::uint64_t first_command{};
  std::uint64_t command_count{};
};

enum class TextIssueCode : std::uint8_t {
  missing_glyphs,
  fallback_font_used,
  text_engine_unavailable,
};

struct SceneTextIssue {
  TextIssueCode code{};
  EntityId entity_id;
  std::uint32_t occurrence_count{};
};

enum class ValueIssueCode : std::uint8_t {
  // Curve samples with value <= 0 on a logarithmic scale are not drawn
  // and aggregate here (data-model-and-api.md section 9).
  nonpositive_log_values,
  // More than four visible scales in one track hurts readability; the
  // kernel warns but never refuses (ADR 0023).
  scale_readability_hint,
};

struct SceneValueIssue {
  ValueIssueCode code{};
  EntityId entity_id;
  std::uint32_t occurrence_count{};
};

struct CurvePick {
  // Owning well document when the hit comes from a multi-well surface (#160).
  // Nil for single-document prepared scenes (legacy single-well path).
  EntityId document_id{};
  EntityId layer_id;
  EntityId track_id{};
  EntityId curve_id;
  std::uint64_t sample_index{};
  double reference_depth{};
  double display_depth{};
  double value{};
  DeviceIndependentPixels distance;
};

struct CurvePickQuery {
  PhysicalPoint scene_position;
  DeviceIndependentPixels tolerance;
  double horizontal_device_independent_pixels_per_millimetre{};
  double vertical_device_independent_pixels_per_millimetre{};
};

// A hit on a crossover fill region (ADR 0030 semantic picking). Carries
// BOTH dependent curve layers and the reference depth at the hit, so the
// host can report which curves produced the filled band.
struct FillPick {
  EntityId layer_id;
  EntityId upper_curve_layer_id;
  EntityId lower_curve_layer_id;
  EntityId upper_curve_id;
  EntityId lower_curve_id;
  double reference_depth{};
};

struct FillPickQuery {
  PhysicalPoint scene_position;
};

// A hit on a raster image tile (ADR 0030 semantic picking). Carries the
// image source identity, the pyramid tile, and the reference depth at the
// hit point (computed from the tile's depth span), so the host can report
// which image and depth the user picked.
struct ImagePick {
  EntityId layer_id;
  EntityId image_source_id;
  std::uint32_t level{};
  std::uint32_t row{};
  std::uint32_t col{};
  double reference_depth{};
};

struct ImagePickQuery {
  PhysicalPoint scene_position;
};

// A hit on a custom-layer primitive (ADR 0030 semantic picking). Carries the
// layer, the owning custom source, the primitive's index within that source,
// its kind, and the reference depth at the hit point (inverted from the scene
// millimetre position). Host-supplied pick metadata is recovered from the
// source's primitive list by index.
struct CustomPick {
  EntityId layer_id;
  EntityId source_id;
  std::uint64_t source_primitive_index{};
  CustomPrimitiveKind kind{CustomPrimitiveKind::triangle};
  double reference_depth{};
};

struct CustomPickQuery {
  PhysicalPoint scene_position;
  DeviceIndependentPixels tolerance;
  double horizontal_device_independent_pixels_per_millimetre{};
  double vertical_device_independent_pixels_per_millimetre{};
};

namespace detail {
class ScenePreparer;
}

struct WellScenePlacement;
struct SurfaceOverlayGeometry;

class WELLLOG_SCENE_API PreparedScene {
public:
  PreparedScene();
  ~PreparedScene();
  PreparedScene(const PreparedScene &);
  PreparedScene &operator=(const PreparedScene &);
  PreparedScene(PreparedScene &&) noexcept;
  PreparedScene &operator=(PreparedScene &&) noexcept;

  [[nodiscard]] EntityId document_id() const noexcept;
  [[nodiscard]] DocumentRevision document_revision() const noexcept;
  [[nodiscard]] Millimetres physical_width() const noexcept;
  [[nodiscard]] Millimetres physical_height() const noexcept;
  [[nodiscard]] ReferenceDepthRange reference_depth_range() const noexcept;
  [[nodiscard]] std::string_view font_asset_fingerprint() const noexcept;
  [[nodiscard]] PresentationVersion presentation_version() const noexcept;
  [[nodiscard]] DepthTransformDescriptor depth_transform() const noexcept;
  [[nodiscard]] std::span<const PreparedTrack> tracks() const noexcept;
  [[nodiscard]] std::span<const PreparedCurveLayer>
  curve_layers() const noexcept;
  [[nodiscard]] std::span<const PreparedCurveSegment>
  curve_segments() const noexcept;
  [[nodiscard]] std::span<const PreparedCurvePoint>
  curve_points() const noexcept;
  [[nodiscard]] std::span<const PatternDefinition> patterns() const noexcept;
  [[nodiscard]] std::span<const PreparedIntervalLayer>
  interval_layers() const noexcept;
  [[nodiscard]] std::span<const PreparedInterval> intervals() const noexcept;
  [[nodiscard]] std::span<const PreparedFillLayer>
  fill_layers() const noexcept;
  [[nodiscard]] std::span<const PreparedFillRegion>
  fill_regions() const noexcept;
  [[nodiscard]] std::span<const PreparedFillVertex>
  fill_vertices() const noexcept;
  [[nodiscard]] std::span<const PreparedFillTriangle>
  fill_triangles() const noexcept;
  [[nodiscard]] std::span<const PreparedImageLayer>
  image_layers() const noexcept;
  [[nodiscard]] std::span<const PreparedImageTile> image_tiles() const noexcept;
  [[nodiscard]] std::span<const PreparedMarkerLayer>
  marker_layers() const noexcept;
  [[nodiscard]] std::span<const PreparedMarker> markers() const noexcept;
  [[nodiscard]] std::span<const PreparedSymbolLayer>
  symbol_layers() const noexcept;
  [[nodiscard]] std::span<const PreparedSymbol> symbols() const noexcept;
  [[nodiscard]] std::span<const PreparedTextLayer>
  text_layers() const noexcept;
  [[nodiscard]] std::span<const PreparedTextRun> text_runs() const noexcept;
  [[nodiscard]] std::span<const PreparedGlyph> glyphs() const noexcept;
  [[nodiscard]] std::span<const PreparedTextFont> text_fonts() const noexcept;
  [[nodiscard]] std::span<const PreparedGlyphOutline>
  glyph_outlines() const noexcept;
  [[nodiscard]] std::span<const OutlineCommand>
  outline_commands() const noexcept;
  [[nodiscard]] std::span<const SceneTextIssue> text_issues() const noexcept;
  [[nodiscard]] std::span<const SceneValueIssue>
  value_issues() const noexcept;
  [[nodiscard]] std::span<const PreparedTrackHeaderEntry>
  track_header_entries() const noexcept;
  [[nodiscard]] std::span<const PreparedCustomLayer>
  custom_layers() const noexcept;
  [[nodiscard]] std::span<const PreparedCustomPrimitive>
  custom_primitives() const noexcept;
  [[nodiscard]] std::span<const PhysicalPoint> custom_vertices() const noexcept;
  [[nodiscard]] std::span<const PreparedCustomClipPath>
  custom_clip_paths() const noexcept;
  // Resolves the owning track of any prepared layer identity (curve,
  // interval, marker, symbol, text, crossover fill or custom), for backends
  // mapping layer-scoped content back to its track clip.
  [[nodiscard]] std::optional<EntityId>
  track_id_for_layer(EntityId layer_id) const noexcept;
  [[nodiscard]] std::optional<CurvePick>
  pick_curve(const CurvePickQuery &query) const noexcept;
  [[nodiscard]] std::optional<FillPick>
  pick_fill(const FillPickQuery &query) const noexcept;
  [[nodiscard]] std::optional<ImagePick>
  pick_image(const ImagePickQuery &query) const noexcept;
  [[nodiscard]] std::optional<CustomPick>
  pick_custom(const CustomPickQuery &query) const noexcept;

private:
  struct Impl;
  explicit PreparedScene(std::shared_ptr<const Impl> impl);
  std::shared_ptr<const Impl> impl_;
  friend class detail::ScenePreparer;
  // WELLLOG_SCENE_API must match free-function declarations below (MSVC C2375
  // "redefinition; different linkage" on shared builds — #234).
  friend WELLLOG_SCENE_API Result<PreparedScene>
  compose_multi_well_scene(std::span<const WellScenePlacement> wells,
                           Millimetres physical_height) noexcept;
  friend WELLLOG_SCENE_API Result<PreparedScene>
  append_surface_overlay_geometry(
      PreparedScene surface,
      std::span<const SurfaceOverlayGeometry> overlays) noexcept;
};

// One well's prepared scene and its horizontal placement on a multi-well
// surface (#160, ADR 0012). `left` is the scene-mm offset of this well's local
// origin within the shared surface.
struct WellScenePlacement {
  EntityId document_id{};
  Millimetres left{};
  std::shared_ptr<const PreparedScene> scene;
};

// Composes horizontally arranged single-well prepared scenes into one surface
// scene (same physical height, cumulative width). Used for multi-well SVG
// export and unified picking. Off-screen wells should be omitted by the caller
// (horizontal culling / virtualization — #160).
[[nodiscard]] WELLLOG_SCENE_API Result<PreparedScene>
compose_multi_well_scene(std::span<const WellScenePlacement> wells,
                         Millimetres physical_height) noexcept;

// Pre-resolved cross-well overlay geometry in surface scene millimetres
// (#161). Session resolves Marker EntityIds → Display Depth → top before
// calling append_surface_overlay_geometry.
struct SurfaceOverlayGeometry {
  EntityId id{};
  enum class Kind : std::uint8_t {
    horizon_line,
    correlation_band,
  };
  Kind kind{Kind::horizon_line};
  // Horizon: left_top ↔ right_top. Band: also left_bottom ↔ right_bottom.
  PhysicalPoint left_top{};
  PhysicalPoint right_top{};
  PhysicalPoint left_bottom{};
  PhysicalPoint right_bottom{};
  RgbaColor color{255, 165, 0, 90};
  Millimetres line_width{0.35};
  std::int32_t z_order{50};
};

// Appends a full-width overlay track + custom primitives (polylines for
// horizons, triangulated quads for correlation bands) so multi-well SVG/GL
// export and visual snapshots include the overlays without mutating wells.
[[nodiscard]] WELLLOG_SCENE_API Result<PreparedScene>
append_surface_overlay_geometry(
    PreparedScene surface,
    std::span<const SurfaceOverlayGeometry> overlays) noexcept;

// Picks a curve on a multi-well surface by testing each well in reverse order
// (rightmost/topmost wins) with a left-shifted query. Returns the hit with
// `document_id` and `track_id` filled.
[[nodiscard]] WELLLOG_SCENE_API std::optional<CurvePick>
pick_curve_multi_well(std::span<const WellScenePlacement> wells,
                      const CurvePickQuery &query) noexcept;

} // namespace welllog
