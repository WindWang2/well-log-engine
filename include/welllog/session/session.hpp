#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include <welllog/core/document.hpp>
#include <welllog/core/result.hpp>
#include <welllog/scene/image_pyramid.hpp>
#include <welllog/scene/scene.hpp>
#include <welllog/scene/text_engine.hpp>
#include <welllog/session/export.hpp>
#include <welllog/session/track_commands.hpp>

namespace welllog {

struct SetDocumentCommand {
  WellLogDocument document;
};

struct SetPresentationCommand {
  ScenePresentation presentation;
};

struct DepthViewport {
  double top{};
  double bottom{};
  friend constexpr bool operator==(DepthViewport, DepthViewport) = default;
};

// --- Multi-well surface layout (#160, ADR 0012) -----------------------------
//
// One WellLogDocument remains one well. The session may hold many documents;
// SetWellLayoutCommand arranges a subset left-to-right on a single surface
// with a shared Display Depth viewport. Single-well is the layout of one
// placement (or an empty layout + prepared_scene(doc) as today).

struct WellPlacement {
  EntityId document_id{};
  // Absolute left edge in surface millimetres. When packing is requested
  // (all left == 0 and width == 0), the session assigns lefts sequentially.
  Millimetres left{};
  // Well column width in millimetres. 0 → use the well's prepared scene /
  // presentation track sum when packing.
  Millimetres width{};
  bool visible{true};
};

struct SetWellLayoutCommand {
  std::vector<WellPlacement> wells;
  Millimetres gap{4.0};
  // When true, ignore caller left/width and pack left-to-right using each
  // well's prepared-scene width (or presentation track widths) + gap.
  bool pack_left_to_right{true};
};

struct ClearWellLayoutCommand {};

// Shared Display Depth window applied to every well in the active layout.
struct SetSharedDepthViewportCommand {
  DepthViewport viewport;
  std::uint32_t pixel_height{}; // 0 keeps each well's current pixel height
};

// Horizontal culling window in surface millimetres (nullopt = no cull).
struct SetSurfaceHorizontalViewCommand {
  double left_mm{};
  double right_mm{};
};

// Pans the horizontal window over the multi-well surface by a delta in
// surface millimetres (positive shifts the window right). Requires an active
// layout and an existing horizontal window; the shifted window is clamped to
// the surface extent [0, total layout width] so panning stops at the edges.
struct PanSurfaceHorizontalCommand {
  double delta_mm{};
};

// Sets the focused well of the unified surface (ADR 0011: the engine owns
// interaction state). Single-well is the surface with one placement: the
// focused well selects which document the implicit one-placement surface
// shows, which well selection gestures target, and the crosshair anchor.
struct SetFocusedWellCommand {
  EntityId document_id{};
};

// --- Depth Transform + Cross-Well Overlay (#161, ADR 0013) ------------------

// Applies a reversible piecewise Depth Transform to one well's presentation.
// Empty control_points clears to identity. Reference depth range on the
// presentation must be expressed in Display Depth space when a non-identity
// transform is active (shared multi-well viewport).
struct SetDepthTransformCommand {
  EntityId document_id{};
  DepthTransform transform{};
};

// Builds per-well transforms so each well's marker references map onto the
// target well's marker display depths (identity on the target). All marker
// id lists must have the same non-zero length and refer to markers that exist
// on the respective documents.
struct AlignWellsToMarkersCommand {
  EntityId target_document_id{};
  std::vector<EntityId> target_marker_ids;
  struct WellMarkers {
    EntityId document_id{};
    std::vector<EntityId> marker_ids;
  };
  std::vector<WellMarkers> wells;
  // Display-depth window after alignment (shared across the layout).
  DepthViewport shared_viewport{};
  std::uint32_t pixel_height{200};
};

// A cross-well horizon line or correlation band on the multi-well surface.
// References stable well + Marker entity ids (never raw screen coords).
struct CrossWellOverlay {
  EntityId id{};
  enum class Kind : std::uint8_t {
    horizon_line,
    correlation_band,
  };
  Kind kind{Kind::horizon_line};
  EntityId left_document_id{};
  EntityId right_document_id{};
  // Top markers (also used as the single marker pair for horizon_line).
  EntityId left_marker_id{};
  EntityId right_marker_id{};
  // Bottom markers for correlation_band only (nil for horizon_line).
  EntityId left_bottom_marker_id{};
  EntityId right_bottom_marker_id{};
  RgbaColor color{255, 165, 0, 90};
  Millimetres line_width{0.35};
  std::int32_t z_order{50};
};

struct SetCrossWellOverlaysCommand {
  std::vector<CrossWellOverlay> overlays;
};

struct CrosshairState {
  double track_fraction{};
  double display_depth{};
  friend constexpr bool operator==(CrosshairState, CrosshairState) = default;
};

struct SetViewportCommand {
  EntityId document_id;
  DepthViewport viewport;
};

struct SetViewportMetricsCommand {
  EntityId document_id;
  DepthViewport viewport;
  std::uint32_t pixel_height{};
};

struct PanDepthCommand {
  EntityId document_id;
  double display_depth_delta{};
};

struct ZoomDepthAtCommand {
  EntityId document_id;
  double anchor_display_depth{};
  double span_factor{};
};

struct ResetViewportCommand {
  EntityId document_id;
};

struct SetCrosshairCommand {
  EntityId document_id;
  std::optional<CrosshairState> crosshair;
};

// A closed Reference Depth Range on one Sampling Axis (ADR 0024). Selection is
// expressed in Reference Depth (the axis coordinate), never in Display Depth,
// screen pixels or LOD envelope points. `top <= bottom` and both finite.
struct SelectionDepthRange {
  double top{};
  double bottom{};
  friend constexpr bool operator==(SelectionDepthRange,
                                   SelectionDepthRange) = default;
};

// The shared semantic Selection Set state held by the session (ADR 0024). One
// selection per document, expressed over a single Sampling Axis: a Reference
// Depth Range plus the half-open `[first_row, last_row)` row span it maps to on
// that axis. `document_revision` is the revision the selection was made against
// (the invalidation key); `valid` becomes false when a document replacement
// could not safely remap the selection (the axis vanished or the range no
// longer fits), and a `selection_invalidated` event is published.
struct SelectionState {
  EntityId document_id;
  EntityId sampling_axis_id;
  SelectionDepthRange reference_depth_range;
  std::uint64_t first_row{};
  std::uint64_t last_row{}; // exclusive
  DocumentRevision document_revision;
  bool valid{true};
  friend constexpr bool operator==(const SelectionState &,
                                   const SelectionState &) = default;
};

// Selects a Reference Depth Range on a Sampling Axis of a document. The session
// maps the range to the half-open row span on that axis's coordinates.
struct SetSelectionCommand {
  EntityId document_id;
  EntityId sampling_axis_id;
  SelectionDepthRange reference_depth_range;
};

// Selects a half-open `[first_row, last_row)` row span on a Sampling Axis. The
// session maps the span back to a Reference Depth Range by reading the axis
// coordinate at the boundary rows. Drives graphic selection from a table.
struct SetRowSelectionCommand {
  EntityId document_id;
  EntityId sampling_axis_id;
  std::uint64_t first_row{};
  std::uint64_t last_row{}; // exclusive
};

// Clears any selection on a document.
struct ClearSelectionCommand {
  EntityId document_id;
};

// One curve tail-block in an AppendBatch (#162/#198, ADR 0031). The session
// appends `tail_values` to the existing curve identified by `curve_id` and
// `tail_coordinates` to that curve's sampling axis (`sampling_axis_id`), both as
// new immutable segments on the curve's/axis's composite buffer — the existing
// blocks are retained untouched with no contiguous copy. The two tail buffers
// must have equal length and a matching scalar type to the existing axis
// coordinates; the tail must continue the axis in its declared direction (no
// out-of-order or historical backfill — those require an explicit
// Replace/Patch).
struct CurveTailBlock {
  EntityId curve_id;
  EntityId sampling_axis_id;
  BufferView tail_coordinates;
  BufferView tail_values;
};

// Atomically appends a batch of curve tail-blocks to an existing document,
// producing one new Document Revision from the whole batch, or failing the
// whole batch (never a half-batch visible state). `target_revision` must be
// strictly greater than the document's current revision (monotonic revision
// gate — does not exist for SetDocumentCommand, which blindly replaces). Old
// data blocks are immutable and not re-copied. Out-of-order and historical
// backfill are rejected as Append.
struct AppendBatchCommand {
  EntityId document_id;
  DocumentRevision target_revision;
  std::vector<CurveTailBlock> blocks;
};

// How the session treats an existing viewport when an AppendBatchCommand
// produces a new revision (#200, ADR 0031 "Session 可固定视口或跟随最新深度").
// `fixed` (default) preserves the current depth window across the append;
// `follow_latest` advances the viewport's `bottom` to the appended tail's last
// reference depth, preserving the span (top = new_bottom - span). Applies only
// to AppendBatchCommand — a plain SetDocumentCommand always resets the viewport
// (it is a full document replacement).
enum class AppendViewportMode : std::uint8_t {
  fixed,
  follow_latest,
};

// --- Undoable Document Patch (#202/#158, ADR 0025) ---------------------------
//
// A declarative edit a host applies to a document + presentation. Raw curve
// value buffers are never edited in place (ADR 0025); a patch edits
// interpretation entities (Interval/Marker/Symbol/Annotation/QcMask), derived
// curves (Curve with provenance — never a raw source overwrite without
// derived), and layout entities (Track/Scale/CurveLayer) by EntityId. Each edit
// is Upsert (add-or-replace by id) or Remove (delete by id). A DocumentPatch
// declares the base revision it was built against; the session rejects a patch
// whose base != the current revision (PatchConflict — no guessing by
// name/position, ADR 0025).

// Every entity a patch may target. A variant makes the edit structural and
// exhaustive (the apply path switches on the alternative to route the entity to
// the right document/presentation collection).
using PatchableEntity =
    std::variant<Interval, Marker, SymbolOccurrence, TextAnnotation, QcMask,
                 Curve, TrackSpec, TrackScaleSpec, CurveLayerSpec>;

// Add-or-replace the entity carried in `entity` (matched by its EntityId on the
// target collection). The id must be non-nil.
struct UpsertEntity {
  PatchableEntity entity;
};

// Remove the entity with `id` from whichever collection holds it. The entity
// must currently exist.
struct RemoveEntity {
  EntityId id;
};

using EntityEdit = std::variant<UpsertEntity, RemoveEntity>;

// A batch of edits applied atomically (all-or-nothing) over a document +
// presentation at `base_revision`. Produces a single new Document Revision.
struct DocumentPatch {
  DocumentRevision base_revision;
  std::vector<EntityEdit> edits;
};

// Applies a DocumentPatch to a document, producing a new Document Revision
// (#202/#158). The patch is validated wholesale and either all-edits apply or
// none do (no half-applied state). A base-revision mismatch is rejected with
// patch_conflict. The Selection Set remaps or invalidates per ADR 0024 (the
// commit delegates through the existing document/presentation replace path).
struct ApplyPatchCommand {
  EntityId document_id;
  DocumentPatch patch;
};

// Restores the semantic state before the latest committed patch or visible
// append for this document (#203, ADR 0025). Widget pixels are transient and
// deliberately not part of the restored state.
struct UndoCommand {
  EntityId document_id;
};

// Re-applies the semantic state of the latest undone patch or visible append
// for this document (#203, ADR 0025).
struct RedoCommand {
  EntityId document_id;
};

struct CommandReceipt {
  std::uint64_t state_version{};
  EntityId document_id;
  DocumentRevision document_revision;
  bool asynchronous_preparation_started{};
  std::optional<std::uint64_t> diagnostic_id;
};

enum class ViewEventKind : std::uint8_t {
  documents_changed,
  diagnostic_published,
  presentation_changed,
  viewport_changed,
  crosshair_changed,
  frame_ready,
  selection_changed,
  selection_invalidated,
  // Read can_undo()/can_redo() after this event to observe the new stacks.
  history_changed,
  // The focused well of the unified surface changed (SetFocusedWellCommand).
  focused_well_changed,
};

struct ViewEvent {
  ViewEventKind kind{ViewEventKind::documents_changed};
  std::uint64_t state_version{};
  EntityId document_id;
  DocumentRevision document_revision;
};

using ViewEventObserverId = std::uint64_t;
using ViewEventObserver = std::function<void(const ViewEvent &)>;

enum class DiagnosticCode : std::uint16_t {
  missing_samples,
  asynchronous_preparation_failed,
  missing_glyphs,
  fallback_font_used,
  text_engine_unavailable,
  nonpositive_log_values,
  scale_readability_hint,
  // An ImageSource's pyramid build failed (non-cancelled); the image layer is
  // degraded (no tiles). Stable code for the observable degradation (#184).
  image_pyramid_unavailable,
};

struct PerformanceBudgets {
  std::uint64_t maximum_cpu_derived_bytes{};
  std::uint64_t maximum_gpu_cache_bytes{256ULL * 1024ULL * 1024ULL};
  std::uint64_t maximum_upload_bytes_per_frame{4ULL * 1024ULL * 1024ULL};
  // Carve-out of the GPU cache reserved for decoded image-tile textures,
  // LRU-evicted (ADR 0034). Defaults to a quarter of the GPU cache.
  std::uint64_t maximum_image_texture_bytes{64ULL * 1024ULL * 1024ULL};
  double prefetch_viewports{2.0};
  std::uint64_t asynchronous_sample_threshold{16'384};
  // Image-pyramid build options (#184): tile size + derived-metadata byte
  // budget for the ImagePyramidMap the session builds from ImageSource
  // entities. metadata-only (no pixel decode — ADR 0045); the host configures
  // LOD here, mirroring how curve-LOD budgets flow through this struct.
  ImagePyramidOptions image_pyramid_options{};
  // High-frequency append coalescing cap (#201, ADR 0031 "高频提交在 C++ 内合
  // 并并默认最多每秒触发十次可见刷新"): the maximum number of VISIBLE
  // revisions an AppendBatchCommand stream may produce per second. 0 (default)
  // disables coalescing — every AppendBatchCommand produces a revision
  // immediately (backward-compatible). A host streaming rapid appends (WITSML/
  // MQTT) sets this (e.g. 10) so back-to-back appends merge inside the engine
  // and emit at most N visible revisions per second; staged blocks flush on the
  // next due interval, on flush_append_coalesce(), or on poll_async().
  std::uint32_t append_refresh_rate_hz{0};
};

enum class PreparationState : std::uint8_t {
  unavailable,
  pending,
  ready,
};

struct PerformanceSnapshot {
  DocumentRevision document_revision;
  PreparationState preparation_state{PreparationState::unavailable};
  std::uint64_t cpu_derived_bytes{};
  std::uint64_t maximum_cpu_derived_bytes{};
  std::uint64_t maximum_gpu_cache_bytes{};
  std::uint64_t maximum_upload_bytes_per_frame{};
  std::uint64_t completed_tasks{};
  std::uint64_t cancelled_tasks{};
  std::uint64_t discarded_tasks{};
  bool frame_preparation_pending{};
  // Low-overhead worker / budget aggregates (#168). Per-frame prepare/upload/
  // draw timings live on WellLogView::frame_stats() (GUI paint path).
};

struct Diagnostic {
  std::uint64_t id{};
  DiagnosticCode code{DiagnosticCode::missing_samples};
  Severity severity{Severity::warning};
  EntityId document_id;
  EntityId entity_id;
  DocumentRevision document_revision;
  std::uint64_t occurrence_count{};
};

// Process-wide full-axis `axis_is_ordered` invocations. Defined in
// session.cpp so shared-library builds share one counter with tests (#755).
[[nodiscard]] WELLLOG_SESSION_API std::uint64_t
axis_is_ordered_full_scan_count() noexcept;
WELLLOG_SESSION_API void reset_axis_is_ordered_full_scan_count() noexcept;

class WELLLOG_SESSION_API WellLogSession {
public:
  WellLogSession();
  explicit WellLogSession(PerformanceBudgets budgets);
  ~WellLogSession();
  WellLogSession(WellLogSession &&) noexcept;
  WellLogSession &operator=(WellLogSession &&) noexcept;
  WellLogSession(const WellLogSession &) = delete;
  WellLogSession &operator=(const WellLogSession &) = delete;

  [[nodiscard]] Result<CommandReceipt> execute(SetDocumentCommand command);
  [[nodiscard]] Result<CommandReceipt>
  execute(const SetPresentationCommand &command);
  [[nodiscard]] Result<CommandReceipt>
  execute(const SetWellLayoutCommand &command);
  [[nodiscard]] Result<CommandReceipt>
  execute(const ClearWellLayoutCommand &command);
  [[nodiscard]] Result<CommandReceipt>
  execute(const SetSharedDepthViewportCommand &command);
  [[nodiscard]] Result<CommandReceipt>
  execute(const SetSurfaceHorizontalViewCommand &command);
  [[nodiscard]] Result<CommandReceipt>
  execute(const PanSurfaceHorizontalCommand &command);
  [[nodiscard]] Result<CommandReceipt>
  execute(const SetFocusedWellCommand &command);
  [[nodiscard]] Result<CommandReceipt>
  execute(const SetDepthTransformCommand &command);
  [[nodiscard]] Result<CommandReceipt>
  execute(const AlignWellsToMarkersCommand &command);
  [[nodiscard]] Result<CommandReceipt>
  execute(const SetCrossWellOverlaysCommand &command);
  [[nodiscard]] Result<CommandReceipt>
  execute(const SetViewportCommand &command);
  [[nodiscard]] Result<CommandReceipt>
  execute(const SetViewportMetricsCommand &command);
  [[nodiscard]] Result<CommandReceipt> execute(const PanDepthCommand &command);
  [[nodiscard]] Result<CommandReceipt>
  execute(const ZoomDepthAtCommand &command);
  [[nodiscard]] Result<CommandReceipt>
  execute(const ResetViewportCommand &command);
  [[nodiscard]] Result<CommandReceipt>
  execute(const SetCrosshairCommand &command);
  [[nodiscard]] Result<CommandReceipt>
  execute(const SetSelectionCommand &command);
  [[nodiscard]] Result<CommandReceipt>
  execute(const SetRowSelectionCommand &command);
  [[nodiscard]] Result<CommandReceipt>
  execute(const ClearSelectionCommand &command);
  [[nodiscard]] Result<CommandReceipt>
  execute(const AppendBatchCommand &command);
  [[nodiscard]] Result<CommandReceipt>
  execute(const ApplyPatchCommand &command);
  [[nodiscard]] Result<CommandReceipt> execute(const UndoCommand &command);
  [[nodiscard]] Result<CommandReceipt> execute(const RedoCommand &command);
  // --- Track/Data workflow commands (track_commands.hpp; ADR 0055/0056) ----
  // Each overload validates the binding against the live document +
  // presentation through the binding indexes, builds a DocumentPatch at the
  // current revision and delegates to execute(ApplyPatchCommand) — one
  // mutation engine, atomic, undoable, LOD-reusing.
  [[nodiscard]] Result<CommandReceipt>
  execute(const AddTrackCommand &command);
  [[nodiscard]] Result<CommandReceipt>
  execute(const RemoveTrackCommand &command);
  [[nodiscard]] Result<CommandReceipt>
  execute(const ReorderTracksCommand &command);
  [[nodiscard]] Result<CommandReceipt>
  execute(const ResizeTrackCommand &command);
  [[nodiscard]] Result<CommandReceipt>
  execute(const SetTrackHeaderCommand &command);
  [[nodiscard]] Result<CommandReceipt>
  execute(const SetTrackVisibilityCommand &command);
  [[nodiscard]] Result<CommandReceipt>
  execute(const BindCurveToTrackCommand &command);
  [[nodiscard]] Result<CommandReceipt>
  execute(const UnbindCurveFromTrackCommand &command);
  [[nodiscard]] Result<CommandReceipt>
  execute(const MoveCurveLayerCommand &command);
  [[nodiscard]] Result<CommandReceipt>
  execute(const DuplicateCurveLayerCommand &command);
  [[nodiscard]] Result<CommandReceipt>
  execute(const ReorderCurveLayersCommand &command);
  [[nodiscard]] Result<CommandReceipt>
  execute(const SetCurveLayerVisibilityCommand &command);
  [[nodiscard]] Result<CommandReceipt>
  execute(const SetCurveLayerStyleCommand &command);
  [[nodiscard]] Result<CommandReceipt>
  execute(const SetTrackScaleCommand &command);
  [[nodiscard]] Result<CommandReceipt>
  execute(const AutoRangeTrackScaleCommand &command);
  // Installs the text pipeline used to shape annotations and labels during
  // scene preparation (ADR 0029). Without an engine, text layers prepare
  // empty and a text_engine_unavailable diagnostic is published.
  void set_text_engine(std::shared_ptr<TextEngine> text_engine) noexcept;
  // Returns the installed text engine, or nullptr when none was set. The
  // returned shared_ptr keeps the engine alive for the caller's use window
  // (e.g. across a PdfSceneExporter::write), so a concurrent set_text_engine
  // cannot free it mid-use (#274 / T2; review D-001 lifetime fix).
  [[nodiscard]] std::shared_ptr<TextEngine> text_engine() const noexcept;
  [[nodiscard]] std::span<const ViewEvent> events() const noexcept;
  void clear_events() noexcept;
  [[nodiscard]] std::span<const Diagnostic> diagnostics() const noexcept;
  [[nodiscard]] std::optional<Error>
  diagnostic_error(std::uint64_t diagnostic_id) const noexcept;
  [[nodiscard]] std::shared_ptr<const WellLogDocument>
  document(EntityId id) const noexcept;
  // The live presentation bound to a document (nullptr before the first
  // SetPresentationCommand or after a document replacement that has not been
  // re-presented). The pointer is valid until the next session command; read
  // what you need, do not store it. Track managers, data trees and hover
  // inspectors resolve track/scale/layer state through this plus the
  // presentation binding index.
  [[nodiscard]] const ScenePresentation *presentation(EntityId id) const
      noexcept;
  [[nodiscard]] std::shared_ptr<const PreparedScene>
  prepared_scene(EntityId document_id) const noexcept;
  // Prepares (or re-prepares) the document's scene at export density so
  // fixed-page depth pagination resolves to the correct per-page curve
  // detail (T3 / #275). ``aggregate_pixel_height`` is the target — typically
  // ``PaginatedSvgExporter::required_aggregate_pixel_height(scene, page)``.
  // Returns a fresh scene WITHOUT disturbing the interactive prepared scene
  // (which stays at the viewport density). Uses the document's LOD pyramids
  // when ready; otherwise emits raw samples (density has no effect then).
  [[nodiscard]] Result<PreparedScene>
  prepare_for_export(EntityId document_id,
                     std::uint64_t aggregate_pixel_height) const noexcept;
  // Active multi-well layout placements (empty when single-well mode).
  [[nodiscard]] std::span<const WellPlacement> well_layout() const noexcept;
  // The focused well of the unified surface (ADR 0011). The implicit
  // one-placement surface (single well) shows this document; nullopt when
  // never set and no single-document fallback resolves.
  [[nodiscard]] std::optional<EntityId> focused_well() const noexcept;
  [[nodiscard]] std::span<const CrossWellOverlay>
  cross_well_overlays() const noexcept;
  // Per-document Depth Transform (identity when unset).
  [[nodiscard]] DepthTransform
  depth_transform(EntityId document_id) const noexcept;
  // Composes the multi-well surface from per-well prepared scenes with
  // horizontal culling. Returns nullptr when no well is prepared/visible.
  // Unified surface semantics: with an empty layout this resolves the
  // implicit one-placement surface (the focused well, else the session's
  // single prepared document) and returns that prepared scene unchanged —
  // single well IS a one-placement surface, never a separate path.
  // Single-document layouts of size 1 still compose.
  [[nodiscard]] std::shared_ptr<const PreparedScene>
  prepared_surface_scene() const noexcept;
  // Unified surface curve pick (document_id + track_id filled). Resolves the
  // same implicit one-placement surface when the layout is empty, so single
  // and multi-well picking share one path with per-well DepthTransform-correct
  // reference depths.
  [[nodiscard]] std::optional<CurvePick>
  pick_surface_curve(const CurvePickQuery &query) const noexcept;
  [[nodiscard]] std::optional<DepthViewport>
  shared_depth_viewport() const noexcept;
  // Unified surface viewport accessors: resolve the state a surface view
  // should render with, regardless of placement count. The depth viewport is
  // the shared Display Depth window when a layout is active, else the focused
  // (or single) document's viewport. The crosshair is the focused well's
  // (SetCrosshairCommand broadcasts it across layout wells, so all agree).
  [[nodiscard]] std::optional<DepthViewport>
  surface_depth_viewport() const noexcept;
  [[nodiscard]] std::optional<CrosshairState>
  surface_crosshair() const noexcept;
  // Full horizontal extent of the surface in millimetres (the right edge of
  // the last placement; NOT the culled compose width). Nullopt when no
  // placement resolves.
  [[nodiscard]] std::optional<double> surface_width_mm() const noexcept;
  // The active horizontal window in surface millimetres (left, right).
  [[nodiscard]] std::optional<std::pair<double, double>>
  surface_horizontal_view() const noexcept;
  // Virtualization counters for the last prepared_surface_scene() compose.
  struct SurfaceStatistics {
    std::uint64_t visible_wells{};
    std::uint64_t culled_wells{};
    std::uint64_t visible_tracks{};
    std::uint64_t culled_tracks{};
  };
  [[nodiscard]] SurfaceStatistics surface_statistics() const noexcept;
  // Per-document viewport. Unified-surface delegation: when the document is
  // a member of an active layout and a shared viewport is set, the shared
  // Display Depth window is returned (layout wells always pan/zoom together);
  // otherwise the document's own viewport.
  [[nodiscard]] std::optional<DepthViewport>
  viewport(EntityId document_id) const noexcept;
  [[nodiscard]] std::optional<std::uint32_t>
  viewport_pixel_height(EntityId document_id) const noexcept;
  [[nodiscard]] std::optional<CrosshairState>
  crosshair(EntityId document_id) const noexcept;
  // The shared semantic Selection Set entry for a document (ADR 0024). Empty
  // when the document has no selection.
  [[nodiscard]] std::optional<SelectionState>
  selection(EntityId document_id) const noexcept;
  // Per-document history observability (#203, ADR 0025). A successful new
  // patch or visible append clears redo entries before recording its undo.
  [[nodiscard]] bool can_undo(EntityId document_id) const noexcept;
  [[nodiscard]] bool can_redo(EntityId document_id) const noexcept;
  // The append viewport mode for a document (#200): whether an
  // AppendBatchCommand preserves the current viewport (`fixed`, the default) or
  // advances its bottom to the new tail depth (`follow_latest`). Returns `fixed`
  // when no mode has been set.
  [[nodiscard]] AppendViewportMode
  append_viewport_mode(EntityId document_id) const noexcept;
  // Sets the append viewport mode for a document (#200). The host/view uses
  // this to choose Fixed vs Follow-Latest behaviour; takes effect on the next
  // AppendBatchCommand for that document. Mirrors how other interaction state
  // (selection, crosshair) is exposed on the session.
  void set_append_viewport_mode(EntityId document_id,
                                AppendViewportMode mode) noexcept;
  [[nodiscard]] ViewEventObserverId
  subscribe_view_events(ViewEventObserver observer) noexcept;
  void unsubscribe_view_events(ViewEventObserverId observer_id) noexcept;
  void poll_async() noexcept;
  // Forces any coalesced (staged) AppendBatchCommand blocks for a document to
  // flush as a single visible revision now (#201). Returns the resulting
  // CommandReceipt on success, or an Error (e.g. invalid_document) when the
  // merged batch fails validation — a host flushing on unload can detect a
  // rejected batch rather than losing it silently. Returns a CommandReceipt at
  // the current revision (no new revision, no state change) when nothing was
  // staged. A no-op when coalescing is disabled. Use on unload or when the host
  // needs the staged tail visible immediately.
  [[nodiscard]] Result<CommandReceipt>
  flush_append_coalesce(EntityId document_id) noexcept;
  [[nodiscard]] PerformanceBudgets performance_budgets() const noexcept;
  // Replaces the performance budgets (#184: the host updates image-pyramid
  // build options via the view). Takes effect on the next document LOD build.
  void set_performance_budgets(PerformanceBudgets budgets) noexcept;
  [[nodiscard]] std::optional<PerformanceSnapshot>
  performance_snapshot(EntityId document_id) const noexcept;

private:
  enum class HistoryDirection : std::uint8_t {
    undo,
    redo,
  };

  // Track-data command support (track_commands.cpp): Impl is pimpl-private
  // to session.cpp, so the command implementations reach the live document
  // and presentation maps through these read-only views.
  [[nodiscard]] const std::unordered_map<
      EntityId, std::shared_ptr<const WellLogDocument>, EntityIdHash> &
  documents_view() const noexcept;
  [[nodiscard]] const std::unordered_map<EntityId, ScenePresentation,
                                         EntityIdHash> &
  presentations_view() const noexcept;

  [[nodiscard]] Result<CommandReceipt>
  execute_history(EntityId document_id, HistoryDirection direction);
  // Shared apply path for the selection commands (depth-range or row-span
  // source). Resolves, stores, versions, and publishes. Returns the receipt or
  // an error. Defined in the .cpp; `from_rows` selects the row→range path.
  [[nodiscard]] Result<CommandReceipt>
  apply_selection(EntityId document_id, EntityId axis_id,
                  SelectionDepthRange range, std::uint64_t first_row,
                  std::uint64_t last_row, bool from_rows);
  // Immediate commit of an AppendBatchCommand: validates + composites + commits
  // a single visible revision (#198). The coalescing gate in execute() may
  // merge several AppendBatchCommands' blocks and call this with the merged
  // batch (#201).
  [[nodiscard]] Result<CommandReceipt>
  commit_append_batch(const AppendBatchCommand &command);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace welllog
