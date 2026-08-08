#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <welllog/core/entity_id.hpp>
#include <welllog/core/export.hpp>
#include <welllog/core/units.hpp>

namespace welllog {

struct DocumentRevision {
  std::uint64_t value{};
  friend constexpr bool operator==(DocumentRevision,
                                   DocumentRevision) = default;
};

enum class ScalarType : std::uint8_t {
  float32,
  float64,
  int16,
  int32,
  int64,
  uint8,
  uint16,
  uint32,
  uint64,
};

[[nodiscard]] WELLLOG_CORE_API std::uint64_t
scalar_size_bytes(ScalarType type) noexcept;
[[nodiscard]] WELLLOG_CORE_API std::string_view
scalar_type_name(ScalarType type) noexcept;
[[nodiscard]] WELLLOG_CORE_API std::optional<ScalarType>
parse_scalar_type(std::string_view name) noexcept;

enum class DepthDomain : std::uint8_t {
  measured_depth,
  true_vertical_depth,
  true_vertical_depth_subsea,
  source_index,
};

// Canonical lowercase Reference-Depth domain tokens shared by the manifest and
// the XML/table exporters (single source of truth). `parse_depth_domain` is the
// inverse; returns nullopt on an unknown token (no throw).
[[nodiscard]] WELLLOG_CORE_API std::string_view
depth_domain_name(DepthDomain domain) noexcept;
[[nodiscard]] WELLLOG_CORE_API std::optional<DepthDomain>
parse_depth_domain(std::string_view name) noexcept;

enum class AxisDirection : std::uint8_t {
  increasing,
  decreasing,
};

enum class BufferAccessMode : std::uint8_t {
  zero_copy,
  shared_copy,
  converted_copy,
};

struct BufferSourceReference {
  std::string uri;
  std::string checksum;
  std::uint64_t byte_offset{};
};

class WELLLOG_CORE_API SharedOwner {
public:
  SharedOwner();
  ~SharedOwner();
  SharedOwner(const SharedOwner &);
  SharedOwner &operator=(const SharedOwner &);
  SharedOwner(SharedOwner &&) noexcept;
  SharedOwner &operator=(SharedOwner &&) noexcept;

  template <typename T>
  explicit SharedOwner(std::shared_ptr<T> owner)
      : SharedOwner(std::shared_ptr<const void>{std::move(owner)}) {}

  [[nodiscard]] bool has_value() const noexcept;

private:
  struct Impl;
  explicit SharedOwner(std::shared_ptr<const void> owner) noexcept;
  std::shared_ptr<const Impl> impl_;
  friend class BufferView;
  friend class NullBitmapView;
};

template <typename T> inline constexpr bool dependent_false_v = false;

template <typename T> [[nodiscard]] consteval ScalarType scalar_type_for() {
  using Value = std::remove_cv_t<T>;
  if constexpr (std::is_same_v<Value, float>) {
    return ScalarType::float32;
  } else if constexpr (std::is_same_v<Value, double>) {
    return ScalarType::float64;
  } else if constexpr (std::is_same_v<Value, std::int16_t>) {
    return ScalarType::int16;
  } else if constexpr (std::is_same_v<Value, std::int32_t>) {
    return ScalarType::int32;
  } else if constexpr (std::is_same_v<Value, std::int64_t>) {
    return ScalarType::int64;
  } else if constexpr (std::is_same_v<Value, std::uint8_t>) {
    return ScalarType::uint8;
  } else if constexpr (std::is_same_v<Value, std::uint16_t>) {
    return ScalarType::uint16;
  } else if constexpr (std::is_same_v<Value, std::uint32_t>) {
    return ScalarType::uint32;
  } else if constexpr (std::is_same_v<Value, std::uint64_t>) {
    return ScalarType::uint64;
  } else {
    static_assert(dependent_false_v<T>, "unsupported WellLog scalar type");
  }
}

class WELLLOG_CORE_API BufferView {
public:
  BufferView();
  ~BufferView();
  BufferView(const BufferView &);
  BufferView &operator=(const BufferView &);
  BufferView(BufferView &&) noexcept;
  BufferView &operator=(BufferView &&) noexcept;

  [[nodiscard]] static BufferView
  from_raw(const void *data, std::uint64_t length, std::uint64_t stride_bytes,
           ScalarType scalar_type, std::uint64_t byte_capacity,
           SharedOwner owner, BufferSourceReference source = {},
           BufferAccessMode access_mode = BufferAccessMode::zero_copy) noexcept;

  template <typename T>
  [[nodiscard]] static BufferView
  from_vector(const std::shared_ptr<const std::vector<T>> &values,
              BufferSourceReference source = {}) {
    return from_raw(
        values ? values->data() : nullptr,
        values ? static_cast<std::uint64_t>(values->size()) : 0, sizeof(T),
        scalar_type_for<T>(),
        values ? static_cast<std::uint64_t>(values->size() * sizeof(T)) : 0,
        SharedOwner{values}, std::move(source));
  }

  [[nodiscard]] const std::byte *data() const noexcept;
  [[nodiscard]] std::uint64_t length() const noexcept;
  [[nodiscard]] std::uint64_t stride_bytes() const noexcept;
  [[nodiscard]] ScalarType scalar_type() const noexcept;
  [[nodiscard]] std::uint64_t byte_capacity() const noexcept;
  [[nodiscard]] bool has_owner() const noexcept;
  [[nodiscard]] const BufferSourceReference &source() const noexcept;
  [[nodiscard]] BufferAccessMode access_mode() const noexcept;
  [[nodiscard]] std::optional<double>
  value_as_double(std::uint64_t index) const noexcept;

private:
  struct Impl;
  explicit BufferView(std::shared_ptr<const Impl> impl);
  std::shared_ptr<const Impl> impl_;
};

class WELLLOG_CORE_API NullBitmapView {
public:
  NullBitmapView();
  ~NullBitmapView();
  NullBitmapView(const NullBitmapView &);
  NullBitmapView &operator=(const NullBitmapView &);
  NullBitmapView(NullBitmapView &&) noexcept;
  NullBitmapView &operator=(NullBitmapView &&) noexcept;

  [[nodiscard]] static NullBitmapView
  from_raw(const std::uint8_t *data, std::uint64_t bit_length,
           std::uint64_t byte_capacity, SharedOwner owner,
           BufferSourceReference source = {}) noexcept;

  [[nodiscard]] bool empty() const noexcept;
  [[nodiscard]] bool is_null(std::uint64_t index) const noexcept;
  [[nodiscard]] const std::uint8_t *data() const noexcept;
  [[nodiscard]] std::uint64_t bit_length() const noexcept;
  [[nodiscard]] std::uint64_t byte_capacity() const noexcept;
  [[nodiscard]] bool has_owner() const noexcept;
  [[nodiscard]] const BufferSourceReference &source() const noexcept;

private:
  struct Impl;
  explicit NullBitmapView(std::shared_ptr<const Impl> impl);
  std::shared_ptr<const Impl> impl_;
};

// A logical buffer spanning N immutable physical segments, each a `BufferView`
// carrying its own `SharedOwner` (#196, foundation for #162 "不复制旧数组": an
// appended tail block is a second segment; the old segment is retained
// untouched, with no contiguous copy). Added BESIDE the single-contiguous
// `BufferView`; no callers are migrated in this ticket. Element i maps across
// the concatenation of segments in order.
//
// All segments MUST share the same `scalar_type` (a heterogeneous composite is
// rejected at build); `stride_bytes` may differ per segment (segments are
// physical blocks that may pack differently), but element addressing is by
// logical index across the concatenation.
class WELLLOG_CORE_API CompositeBufferView {
public:
  CompositeBufferView();
  ~CompositeBufferView();
  CompositeBufferView(const CompositeBufferView &);
  CompositeBufferView &operator=(const CompositeBufferView &);
  CompositeBufferView(CompositeBufferView &&) noexcept;
  CompositeBufferView &operator=(CompositeBufferView &&) noexcept;

  // Builds a composite from an ordered list of segments. All segments must
  // share a scalar_type and have a non-null data pointer + length; returns an
  // empty composite on a mismatch or empty input.
  [[nodiscard]] static CompositeBufferView
  from_segments(std::vector<BufferView> segments) noexcept;

  // True when the composite holds no segments.
  [[nodiscard]] bool empty() const noexcept;

  // The scalar type shared by all segments (float64 on an empty composite).
  [[nodiscard]] ScalarType scalar_type() const noexcept;

  // The total element count across all segments (sum of each segment's length).
  [[nodiscard]] std::uint64_t length() const noexcept;

  // Reads element `index` across the concatenation, mapping it to the correct
  // segment + intra-segment offset. Out-of-range yields nullopt, consistent
  // with BufferView::value_as_double. No contiguous copy is made.
  [[nodiscard]] std::optional<double>
  value_as_double(std::uint64_t index) const noexcept;

  // The ordered segments, for consumers that walk block boundaries without
  // flattening (a span of const BufferView).
  [[nodiscard]] std::span<const BufferView> segments() const noexcept;

private:
  struct Impl;
  std::shared_ptr<const Impl> impl_;
  explicit CompositeBufferView(std::shared_ptr<const Impl> impl);
};

// A curve's value buffer — either a single contiguous `BufferView` (the common
// case) or a `CompositeBufferView` spanning N immutable segments (the append
// case, #162/#197). Consumers (LOD build, scene-prepare, table projection)
// read a curve through the three index-based accessors below; this adapter
// forwards to whichever underlying view the curve carries, so they need no
// branching. The implicit `BufferView` constructor keeps every existing
// `Curve{.values = someBufferView}` site compiling unchanged (#197 contract
// step: migrate consumers onto the composite-capable interface without
// touching call sites).
class WELLLOG_CORE_API CurveBuffer {
public:
  // Implicit so `Curve{.values = BufferView{...}}` keeps working (84+ sites).
  CurveBuffer(BufferView view) noexcept;
  // Explicit for the multi-segment append case.
  explicit CurveBuffer(CompositeBufferView composite) noexcept;

  CurveBuffer() = default;
  ~CurveBuffer() = default;
  CurveBuffer(const CurveBuffer &) = default;
  CurveBuffer &operator=(const CurveBuffer &) = default;
  CurveBuffer(CurveBuffer &&) noexcept = default;
  CurveBuffer &operator=(CurveBuffer &&) noexcept = default;

  // True when the curve carries no buffer (neither view set).
  [[nodiscard]] bool empty() const noexcept;
  // The total element count across the underlying view.
  [[nodiscard]] std::uint64_t length() const noexcept;
  // The scalar type (float64 when empty).
  [[nodiscard]] ScalarType scalar_type() const noexcept;
  // Reads element `index`; nullopt on out-of-range, consistent with
  // BufferView::value_as_double.
  [[nodiscard]] std::optional<double>
  value_as_double(std::uint64_t index) const noexcept;
  // True when the curve carries a composite (multi-segment) buffer.
  [[nodiscard]] bool is_composite() const noexcept;
  // The single-block view, when the curve carries one (empty otherwise). Lets
  // validators that need raw data()/stride/byte_capacity reach the single
  // contiguous block.
  [[nodiscard]] const BufferView &as_single() const noexcept;
  // The segments of a composite, when the curve carries one (empty otherwise).
  [[nodiscard]] std::span<const BufferView> segments() const noexcept;

private:
  BufferView single_;
  CompositeBufferView composite_;
  bool is_composite_{false};
};

struct SamplingAxis {
  EntityId id;
  // The depth coordinates, one per sample. Carried as a `CurveBuffer` so an
  // append (#162/#198) extends the axis by adding a tail segment, with no copy
  // of the existing coordinate block (ADR 0031). The common single-block case
  // is set implicitly from a `BufferView`; `is_composite()` is false there and
  // `as_single()` reaches the contiguous block.
  CurveBuffer coordinates;
  DepthDomain domain{DepthDomain::measured_depth};
  std::string unit;
  AxisDirection direction{AxisDirection::increasing};
  // Nominal sampling step (in the axis unit) of a regular axis, when the
  // coordinates follow a constant interval (e.g. 0.125 m). A description
  // only — the coordinates array stays the source of truth, and the value is
  // left unset for irregular sampling or unknown intervals. Consumed by
  // UI/table labels ("0.125 m") and explicit resampling targets. Must be
  // finite and > 0 when present.
  std::optional<double> nominal_interval{};
};

// Per-sample quality state for a QC Mask (#159, ADR 0025). Masks never rewrite
// the curve value buffer; graphics/table policy decides hide/colour/annotate.
enum class QcState : std::uint8_t {
  valid = 0,
  suspect = 1,
  invalid = 2,
  user_excluded = 3,
};

// Whether a derived curve still matches its input buffers (#159).
enum class DerivedFreshness : std::uint8_t {
  current = 0,
  stale = 1,
};

// Provenance for a Derived Curve. The curve's own `values` hold the derived
// samples; the original input Curve Buffer stays byte-identical (ADR 0025).
struct DerivedCurveProvenance {
  EntityId input_curve_id{};
  DocumentRevision input_revision{};
  std::string algorithm_id;
  std::string algorithm_version;
  // Opaque, host-defined parameter record (e.g. JSON) for audit/reproducibility.
  std::string parameters;
  EntityId output_sampling_axis_id{};
  // Identity of the input values buffer at derivation time. Used so unrelated
  // document edits (intervals, layout) do not mark the result stale, while
  // append/replace of the input does.
  const void *input_values_data{};
  std::uint64_t input_values_length{};
  DerivedFreshness freshness{DerivedFreshness::current};
};

// A QC Mask is sample-aligned with its target curve: `states` is a uint8 buffer
// whose length equals the curve sample count; each element is a QcState.
struct QcMask {
  EntityId id{};
  EntityId curve_id{};
  BufferView states;
};

struct Curve {
  EntityId id;
  std::string mnemonic;
  std::string display_name;
  std::string unit;
  EntityId sampling_axis_id;
  CurveBuffer values;
  NullBitmapView nulls;
  // Nil when the curve has no QC Mask. The mask lives in the document's
  // qc_masks collection and never mutates `values`.
  EntityId qc_mask_id{};
  // Present only for Derived Curves. Source (raw) curves leave this empty.
  std::optional<DerivedCurveProvenance> derived{};
};

// True when QC policy treats the sample as non-drawable / table-null.
[[nodiscard]] constexpr bool qc_state_is_suppressed(
    QcState state, bool hide_suspect, bool hide_invalid,
    bool hide_user_excluded) noexcept {
  switch (state) {
  case QcState::valid:
    return false;
  case QcState::suspect:
    return hide_suspect;
  case QcState::invalid:
    return hide_invalid;
  case QcState::user_excluded:
    return hide_user_excluded;
  }
  return false;
}

// Pixel layout of a raster image source. The engine never decodes image
// bytes itself (ADR 0042 governs decoding limits; the host performs it); this
// only describes the pre-decoded bytes the host supplies per tile.
enum class PixelFormat : std::uint8_t {
  rgba8,
  rgb8,
  r8,
};

// A large raster source (core photo, borehole image) registered by depth
// (rendering.md section 10). The bytes are NOT owned here — `source` is the
// data-source identity (uri/checksum/offset, ADR 0032) the host resolves into
// decoded tile bytes on demand. `dpi` is the explicit source resolution
// (ADR 0039: never derived from window/system DPI).
struct ImageSource {
  EntityId id;
  std::uint64_t width_px{};
  std::uint64_t height_px{};
  PixelFormat pixel_format{PixelFormat::rgba8};
  double reference_depth_top{};
  double reference_depth_bottom{};
  std::uint32_t dpi{};
  BufferSourceReference source;
};

enum class IntervalSemantic : std::uint8_t {
  lithology,
  stratigraphy,
  sequence,
  systems_tract,
  facies,
  custom,
};

// A closed depth interval. `top_reference_depth` must be less than
// `bottom_reference_depth`; a zero-thickness feature is a Marker, not an
// Interval. `pattern_id` references a PatternDefinition registered on the
// ScenePresentation; when nil the interval is solid-filled with
// `fill_color`. `label` is UTF-8.
struct Interval {
  EntityId id;
  double top_reference_depth{};
  double bottom_reference_depth{};
  IntervalSemantic semantic{IntervalSemantic::custom};
  EntityId pattern_id;
  RgbaColor fill_color{};
  std::string label;
};

enum class MarkerSemantic : std::uint8_t {
  formation_top,
  fault,
  fluid_contact,
  casing_shoe,
  custom,
};

// A zero-thickness depth feature drawn as a horizontal line with an
// optional UTF-8 `label`.
struct Marker {
  EntityId id;
  double reference_depth{};
  MarkerSemantic semantic{MarkerSemantic::custom};
  std::string label;
};

enum class SymbolKind : std::uint8_t {
  circle,
  square,
  triangle_up,
  diamond,
  cross,
  // Marker-semantic glyphs (casing_shoe etc.). Append-only: numeric values
  // are part of the manifest/table-export surface.
  triangle_down,
  shoe,
};

// A discrete symbol anchored at a depth; `track_fraction` is the horizontal
// anchor within the owning track in [0, 1].
struct SymbolOccurrence {
  EntityId id;
  double reference_depth{};
  double track_fraction{0.5};
  SymbolKind kind{SymbolKind::circle};
  std::string label;
};

enum class TextOrientation : std::uint8_t {
  horizontal,
  rotated,
  vertical,
};

enum class AnnotationAnchor : std::uint8_t {
  reference_depth,
  track,
  scene_point,
};

// A UTF-8 text annotation. The anchor type is explicit:
//  - reference_depth: `reference_depth` + `track_fraction` inside the layer's
//    track;
//  - track: `track_id` + `depth_fraction`/`horizontal_fraction` in [0, 1];
//  - scene_point: absolute `scene_point` in scene millimetres.
// `rotation_degrees` applies when `orientation` is `rotated`; `vertical`
// requests true vertical typesetting. `language` is a BCP 47 tag used for
// shaping and font fallback.
struct TextAnnotation {
  EntityId id;
  AnnotationAnchor anchor{AnnotationAnchor::reference_depth};
  double reference_depth{};
  double track_fraction{0.5};
  EntityId track_id;
  double depth_fraction{};
  double horizontal_fraction{};
  PhysicalPoint scene_point{};
  std::string text;
  std::string language;
  TextOrientation orientation{TextOrientation::horizontal};
  double rotation_degrees{};
  Millimetres font_size{3.0};
};

// --- Custom Layer (ADR 0018, rendering.md section 11) -----------------------
//
// The declarative extension vocabulary a host uses to build a professional
// Layer without touching the screen backend, shaders or the engine's private
// renderer.
// Primitives are plain data: points and colours. There is deliberately no
// field for shaders, scripts, commands or network resources (ADR 0042 — the
// struct itself cannot express them, so the constraint is enforced by the
// type system, not by parsing rules).

enum class CustomPrimitiveKind : std::uint8_t {
  // An open or closed polyline in scene millimetres.
  polyline,
  // A filled triangle in scene millimetres.
  triangle,
  // A filled axis-aligned quad (rect) in scene millimetres.
  quad,
  // A discrete symbol reusing the built-in symbol geometry (SymbolKind).
  symbol,
};

// An explicit on/off segment array defining a stroke's dash style, in scene
// millimetres (ADR 0050). `segments` alternates [on, off, on, off, ...]; the
// pattern repeats along the polyline. An empty array means solid. `offset`
// shifts the starting position along the dash cycle. Maps directly to SVG
// stroke-dasharray and PDF line dash arrays.
struct DashPattern {
  std::vector<Millimetres> segments;
  double offset{};
};

// A polyline stroke. `closed` closes the ring with a final segment (and, for
// SVG/GL, treats it as a closed path). `width` is the stroke width.
// `dash_pattern` is empty for a solid line (ADR 0050).
struct CustomPolyline {
  std::vector<PhysicalPoint> points;
  bool closed{};
  RgbaColor color{0, 0, 0, 255};
  Millimetres width{0.3};
  DashPattern dash_pattern{};
};

// A filled triangle defined by three scene-mm points.
struct CustomTriangle {
  PhysicalPoint a;
  PhysicalPoint b;
  PhysicalPoint c;
  RgbaColor fill_color{0, 0, 0, 255};
};

// A filled axis-aligned rectangle in scene millimetres. `pattern_id` references
// a PatternDefinition registered on the ScenePresentation; when nil the quad is
// solid-filled with `fill_color` (ADR 0050, same mechanism as Interval).
struct CustomQuad {
  PhysicalRect rect;
  RgbaColor fill_color{0, 0, 0, 255};
  EntityId pattern_id{};
};

// A discrete symbol, mirroring SymbolOccurrence but positioned directly in
// scene millimetres (it is not a document entity, so it carries its own kind,
// colour and size rather than referencing a SymbolOccurrence).
struct CustomSymbolOccurrence {
  PhysicalPoint center;
  SymbolKind kind{SymbolKind::circle};
  RgbaColor color{0, 0, 0, 255};
  Millimetres size{3.0};
};

// The constrained declarative primitive vocabulary (ADR 0018). A variant is
// used (as with PatternPrimitive) so the kind is structural and exhaustive.
using CustomPrimitive =
    std::variant<CustomPolyline, CustomTriangle, CustomQuad,
                 CustomSymbolOccurrence>;

// A closed clip path in scene millimetres. When present on a CustomLayerSource
// it masks only that custom layer's own primitives (layer-local clip).
struct CustomClipPath {
  std::vector<PhysicalPoint> points;
};

// A host-authored declarative layer registered on the document (ADR 0046). It
// is a document entity with an Entity ID and a content revision, so a host can
// version its primitives and the manifest can round-trip them. The primitives
// live here (with the data), not on the presentation layer; a CustomLayerSpec
// references the source by id, mirroring ImageSource/ImageLayerSpec (#152).
struct CustomLayerSource {
  EntityId id;
  // Content revision of this source's primitives, independent of the owning
  // document revision. Hosts bump it when the primitive set changes.
  DocumentRevision content_revision;
  std::vector<CustomPrimitive> primitives;
  std::optional<CustomClipPath> clip;
};

class WELLLOG_CORE_API WellLogDocument {
public:
  WellLogDocument();

  [[nodiscard]] EntityId id() const noexcept;
  [[nodiscard]] DocumentRevision revision() const noexcept;
  [[nodiscard]] std::span<const SamplingAxis> sampling_axes() const noexcept;
  [[nodiscard]] std::span<const Curve> curves() const noexcept;
  [[nodiscard]] std::span<const QcMask> qc_masks() const noexcept;
  [[nodiscard]] std::span<const Interval> intervals() const noexcept;
  [[nodiscard]] std::span<const Marker> markers() const noexcept;
  [[nodiscard]] std::span<const SymbolOccurrence> symbols() const noexcept;
  [[nodiscard]] std::span<const ImageSource> image_sources() const noexcept;
  [[nodiscard]] std::span<const TextAnnotation> annotations() const noexcept;
  [[nodiscard]] std::span<const CustomLayerSource>
  custom_sources() const noexcept;

private:
  struct Impl;
  explicit WellLogDocument(std::shared_ptr<const Impl> impl);
  std::shared_ptr<const Impl> impl_;
  friend class WellLogDocumentBuilder;
};

// Reads one sample's QC state; missing mask or out-of-range → valid.
[[nodiscard]] WELLLOG_CORE_API QcState
qc_state_at(const WellLogDocument &document, const Curve &curve,
            std::uint64_t sample_index) noexcept;

// Recomputes derived freshness against the document's current curves.
[[nodiscard]] WELLLOG_CORE_API DerivedFreshness
compute_derived_freshness(const WellLogDocument &document,
                          const Curve &curve) noexcept;

class WELLLOG_CORE_API WellLogDocumentBuilder {
public:
  WellLogDocumentBuilder(EntityId id, DocumentRevision revision) noexcept;
  ~WellLogDocumentBuilder();
  WellLogDocumentBuilder(WellLogDocumentBuilder &&) noexcept;
  WellLogDocumentBuilder &operator=(WellLogDocumentBuilder &&) noexcept;
  WellLogDocumentBuilder(const WellLogDocumentBuilder &) = delete;
  WellLogDocumentBuilder &operator=(const WellLogDocumentBuilder &) = delete;

  WellLogDocumentBuilder &add_sampling_axis(const SamplingAxis &axis) noexcept;
  WellLogDocumentBuilder &add_curve(const Curve &curve) noexcept;
  WellLogDocumentBuilder &add_qc_mask(const QcMask &mask) noexcept;
  WellLogDocumentBuilder &add_interval(const Interval &interval) noexcept;
  WellLogDocumentBuilder &add_marker(const Marker &marker) noexcept;
  WellLogDocumentBuilder &add_symbol(const SymbolOccurrence &symbol) noexcept;
  WellLogDocumentBuilder &add_image_source(const ImageSource &source) noexcept;
  WellLogDocumentBuilder &
  add_annotation(const TextAnnotation &annotation) noexcept;
  WellLogDocumentBuilder &
  add_custom_source(const CustomLayerSource &source) noexcept;
  [[nodiscard]] WellLogDocument build() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace welllog
