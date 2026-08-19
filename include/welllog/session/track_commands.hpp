#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <welllog/core/entity_id.hpp>
#include <welllog/core/units.hpp>
#include <welllog/scene/scene.hpp>
#include <welllog/session/export.hpp>

namespace welllog {

// --- Track/Data workflow commands (ADR 0055/0056) ---------------------------
//
// High-level, validated, undoable operations for the professional track
// workflow: adding/removing/reordering/resizing tracks, binding/unbinding/
// moving curve layers between tracks, and editing layer style and track
// scales. Every command is implemented ON TOP of ApplyPatchCommand: it reads
// the current document + presentation through the binding indexes, builds a
// DocumentPatch at the live revision, and delegates to the patch engine.
// There is deliberately no second mutation engine — the commands inherit the
// patch path's atomicity (all-or-nothing), base-revision safety, full
// preflight validation (dangling curve/scale/track references, unit
// compatibility, log-scale constraints), undo/redo history, event publishing
// and LOD-cache reuse.
//
// Nil `track_id`/`scale_id`/`layer_id` inputs on create are populated with a
// generated EntityId (EntityId::generate) reported back through the patch's
// effect; hosts that need deterministic ids supply their own.
//
// Raw curve and sampling-axis buffers are never copied or rewritten by any
// command here — a Move is a presentation-binding change only.

// Appends a new track. `z_order` nullopt places the track after the current
// last track in z-order; the presentation's z-orders are authoritative for
// horizontal layout.
struct AddTrackCommand {
  EntityId document_id{};
  EntityId track_id{};
  Millimetres width{40.0};
  std::optional<std::int32_t> z_order{};
  TrackHeaderSpec header{};
  bool visible{true};
};

// Removes a track TOGETHER with its scales and every layer placed in it, as
// one atomic patch (the explicit no-dangling-reference policy: a partial
// removal would be rejected by preflight, so the command cascades).
struct RemoveTrackCommand {
  EntityId document_id{};
  EntityId track_id{};
};

// Rewrites the tracks' z_orders to match `ordered_track_ids` (a complete
// list — every existing track exactly once). This is the track-drag reorder.
struct ReorderTracksCommand {
  EntityId document_id{};
  std::vector<EntityId> ordered_track_ids;
};

struct ResizeTrackCommand {
  EntityId document_id{};
  EntityId track_id{};
  Millimetres width{};
};

struct SetTrackHeaderCommand {
  EntityId document_id{};
  EntityId track_id{};
  TrackHeaderSpec header{};
};

struct SetTrackVisibilityCommand {
  EntityId document_id{};
  EntityId track_id{};
  bool visible{true};
};

// Binds a curve into a track as a new curve layer. `scale_id` nil reuses the
// first scale already in the track whose unit matches the curve's; when none
// exists a scale is created — with the curve's finite data extent when
// `auto_range` is set (an explicit host/user operation, never a per-frame
// adjustment), else 0..1. A curve MAY be bound several times (distinct
// layers, even in one track): duplicates are independent visual presentations
// of the same immutable buffer — that is the documented duplicate-binding
// policy.
struct BindCurveToTrackCommand {
  EntityId document_id{};
  EntityId curve_id{};
  EntityId track_id{};
  EntityId layer_id{};
  EntityId scale_id{};
  bool auto_range{true};
  RgbaColor color{.red = 0x1F, .green = 0x72, .blue = 0xB8, .alpha = 0xFF};
  Millimetres line_width{0.35};
  std::optional<std::int32_t> z_order{};
};

// Removes one curve layer (unbind). Scales are kept even when they become
// unused — a user-tuned range survives re-binding (explicit policy).
struct UnbindCurveFromTrackCommand {
  EntityId document_id{};
  EntityId layer_id{};
};

// Moves a curve layer to another track. The layer keeps its identity (so LOD
// caches and undo history stay continuous); only its track/scale binding and
// z-order change. `target_scale_id` nil picks the target track's first scale
// with a matching unit, creating one (auto-ranged from the curve) when the
// track has none.
struct MoveCurveLayerCommand {
  EntityId document_id{};
  EntityId layer_id{};
  EntityId target_track_id{};
  EntityId target_scale_id{};
};

// Adds a second visual presentation of the same curve (same track, one
// z-order slot above the source). Raw buffers are shared, never copied.
struct DuplicateCurveLayerCommand {
  EntityId document_id{};
  EntityId layer_id{};
  EntityId new_layer_id{};
};

// Rewrites the track's curve-layer z_orders to match the complete ordered
// list (the layer-drag reorder within a track).
struct ReorderCurveLayersCommand {
  EntityId document_id{};
  EntityId track_id{};
  std::vector<EntityId> ordered_layer_ids;
};

struct SetCurveLayerVisibilityCommand {
  EntityId document_id{};
  EntityId layer_id{};
  bool visible{true};
};

// Partial style edit: nullopt members keep the current value.
struct SetCurveLayerStyleCommand {
  EntityId document_id{};
  EntityId layer_id{};
  std::optional<RgbaColor> color{};
  std::optional<Millimetres> line_width{};
};

// Partial scale edit: nullopt members keep the current value. The resulting
// scale must satisfy the preflight invariants (finite, min < max, log ⇒
// min > 0); changing `unit` is allowed but rejected atomically when any bound
// curve's unit no longer matches.
struct SetTrackScaleCommand {
  EntityId document_id{};
  EntityId scale_id{};
  std::optional<ScaleMode> mode{};
  std::optional<double> minimum{};
  std::optional<double> maximum{};
  std::optional<ScaleDirection> direction{};
  std::optional<std::string> unit{};
};

// Recomputes the scale's min/max from the finite extent of every curve bound
// through layers using this scale — an explicit host/user "fit" operation.
// The degenerate fallbacks follow the documented auto-range policy (all
// non-finite ⇒ 0..1; constant curve ⇒ v..v+1).
struct AutoRangeTrackScaleCommand {
  EntityId document_id{};
  EntityId scale_id{};
};

} // namespace welllog
