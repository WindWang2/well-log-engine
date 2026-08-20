#pragma once

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include <welllog/core/entity_id.hpp>
#include <welllog/core/units.hpp>
#include <welllog/scene/export.hpp>
#include <welllog/scene/scene.hpp>

namespace welllog {

// O(1) binding resolution over one immutable ScenePresentation snapshot (the
// Track/Data Binding Resolver's presentation half). Answers the professional
// workflow questions — track → scales/layers, curve → layers, layer → track,
// scale → track — without scanning the entity vectors on every hover, drag
// or property-panel refresh.
//
// Built once per presentation version; the pointers point into the
// presentation the index was built from, so it must outlive the index.
// Tracks are exposed in z-order (the authoritative horizontal layout order);
// per-track member lists preserve presentation order.
class WELLLOG_SCENE_API PresentationBindingIndex {
public:
  PresentationBindingIndex() = default;
  explicit PresentationBindingIndex(const ScenePresentation &presentation);

  [[nodiscard]] const TrackSpec *track(EntityId id) const noexcept;
  [[nodiscard]] const TrackScaleSpec *scale(EntityId id) const noexcept;
  [[nodiscard]] const CurveLayerSpec *curve_layer(EntityId id) const noexcept;

  // All tracks in z-order (ties keep presentation order). The authoritative
  // answer to "which tracks exist, left to right".
  [[nodiscard]] std::span<const TrackSpec *const>
  tracks_in_z_order() const noexcept;

  // Track membership (empty spans for unknown tracks).
  [[nodiscard]] std::span<const TrackScaleSpec *const>
  scales_of_track(EntityId track_id) const noexcept;
  [[nodiscard]] std::span<const CurveLayerSpec *const>
  curve_layers_of_track(EntityId track_id) const noexcept;
  // Every layer (any kind) placed in the track. Order: curve layers, then
  // interval, crossover fill, image, marker, symbol, text and custom layers,
  // each in presentation order — the exact set RemoveTrack must cascade over.
  [[nodiscard]] std::vector<EntityId>
  all_layers_of_track(EntityId track_id) const noexcept;

  // Curve → layers that present it, in presentation order (a curve may be
  // bound into several tracks or several times into one track; each layer is
  // an independent visual presentation of the same immutable buffer).
  [[nodiscard]] std::span<const CurveLayerSpec *const>
  curve_layers_of_curve(EntityId curve_id) const noexcept;

  // The owning track of a scale/layer, or nullptr when unknown. Covers every
  // layer kind, mirroring PreparedScene::track_id_for_layer without a scan.
  [[nodiscard]] const TrackSpec *track_of_scale(EntityId scale_id) const
      noexcept;
  [[nodiscard]] const TrackSpec *track_of_layer(EntityId layer_id) const
      noexcept;

  // The scale a curve layer binds to (nullptr when the layer or scale is
  // unknown); the common resolution step under cursor/hover inspection.
  [[nodiscard]] const TrackScaleSpec *
  scale_of_curve_layer(const CurveLayerSpec &layer) const noexcept;

private:
  std::unordered_map<EntityId, const TrackSpec *, EntityIdHash> tracks_;
  std::unordered_map<EntityId, const TrackScaleSpec *, EntityIdHash> scales_;
  std::unordered_map<EntityId, const CurveLayerSpec *, EntityIdHash>
      curve_layers_;
  std::unordered_map<EntityId, std::vector<const TrackScaleSpec *>, EntityIdHash>
      scales_by_track_;
  std::unordered_map<EntityId, std::vector<const CurveLayerSpec *>, EntityIdHash>
      curve_layers_by_track_;
  std::unordered_map<EntityId, std::vector<const CurveLayerSpec *>, EntityIdHash>
      curve_layers_by_curve_;
  // Every layer entity (all kinds) keyed by id → owning track id; also drives
  // all_layers_of_track via layers_by_track_ (deterministic kind order).
  std::unordered_map<EntityId, EntityId, EntityIdHash> layer_track_;
  std::unordered_map<EntityId, std::vector<EntityId>, EntityIdHash>
      layers_by_track_;
  std::vector<const TrackSpec *> ordered_tracks_;
};

} // namespace welllog
