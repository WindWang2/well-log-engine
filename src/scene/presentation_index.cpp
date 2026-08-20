#include <welllog/scene/presentation_index.hpp>

#include <algorithm>

namespace welllog {
namespace {

// Stable ordering by z_order (ascending); ties keep presentation order, the
// same contract prepare_impl gives the horizontal layout.
template <typename Entity>
void order_by_z(std::vector<const Entity *> &entities) {
  std::stable_sort(entities.begin(), entities.end(),
                   [](const Entity *a, const Entity *b) {
                     return a->z_order < b->z_order;
                   });
}

} // namespace

PresentationBindingIndex::PresentationBindingIndex(
    const ScenePresentation &presentation) {
  for (const auto &track : presentation.tracks()) {
    tracks_.emplace(track.id, &track);
  }
  ordered_tracks_.reserve(presentation.tracks().size());
  for (const auto &track : presentation.tracks()) {
    ordered_tracks_.push_back(&track);
  }
  order_by_z(ordered_tracks_);

  // Scales resolve to their track like any layer (scale → track is a binding
  // question too), but are kept out of layers_by_track_ so all_layers_of_track
  // stays a pure layer list.
  for (const auto &scale : presentation.scales()) {
    scales_.emplace(scale.id, &scale);
    scales_by_track_[scale.track_id].push_back(&scale);
    layer_track_.emplace(scale.id, scale.track_id);
  }
  const auto index_layers = [this](const auto &layers) {
    for (const auto &layer : layers) {
      layer_track_.emplace(layer.id, layer.track_id);
      layers_by_track_[layer.track_id].push_back(layer.id);
    }
  };
  for (const auto &layer : presentation.curve_layers()) {
    curve_layers_.emplace(layer.id, &layer);
    curve_layers_by_track_[layer.track_id].push_back(&layer);
    curve_layers_by_curve_[layer.curve_id].push_back(&layer);
  }
  index_layers(presentation.curve_layers());
  index_layers(presentation.interval_layers());
  index_layers(presentation.crossover_fill_layers());
  index_layers(presentation.image_layers());
  index_layers(presentation.marker_layers());
  index_layers(presentation.symbol_layers());
  index_layers(presentation.text_layers());
  index_layers(presentation.custom_layers());
}

const TrackSpec *PresentationBindingIndex::track(EntityId id) const noexcept {
  const auto found = tracks_.find(id);
  return found == tracks_.end() ? nullptr : found->second;
}

const TrackScaleSpec *
PresentationBindingIndex::scale(EntityId id) const noexcept {
  const auto found = scales_.find(id);
  return found == scales_.end() ? nullptr : found->second;
}

const CurveLayerSpec *
PresentationBindingIndex::curve_layer(EntityId id) const noexcept {
  const auto found = curve_layers_.find(id);
  return found == curve_layers_.end() ? nullptr : found->second;
}

std::span<const TrackSpec *const>
PresentationBindingIndex::tracks_in_z_order() const noexcept {
  return std::span<const TrackSpec *const>{ordered_tracks_};
}

std::span<const TrackScaleSpec *const>
PresentationBindingIndex::scales_of_track(EntityId track_id) const noexcept {
  const auto found = scales_by_track_.find(track_id);
  return found == scales_by_track_.end()
             ? std::span<const TrackScaleSpec *const>{}
             : std::span<const TrackScaleSpec *const>{found->second};
}

std::span<const CurveLayerSpec *const>
PresentationBindingIndex::curve_layers_of_track(
    EntityId track_id) const noexcept {
  const auto found = curve_layers_by_track_.find(track_id);
  return found == curve_layers_by_track_.end()
             ? std::span<const CurveLayerSpec *const>{}
             : std::span<const CurveLayerSpec *const>{found->second};
}

std::vector<EntityId>
PresentationBindingIndex::all_layers_of_track(EntityId track_id) const
    noexcept {
  const auto found = layers_by_track_.find(track_id);
  return found == layers_by_track_.end() ? std::vector<EntityId>{}
                                         : found->second;
}

std::span<const CurveLayerSpec *const>
PresentationBindingIndex::curve_layers_of_curve(EntityId curve_id) const
    noexcept {
  const auto found = curve_layers_by_curve_.find(curve_id);
  return found == curve_layers_by_curve_.end()
             ? std::span<const CurveLayerSpec *const>{}
             : std::span<const CurveLayerSpec *const>{found->second};
}

const TrackSpec *
PresentationBindingIndex::track_of_scale(EntityId scale_id) const noexcept {
  const auto *resolved = scale(scale_id);
  return resolved == nullptr ? nullptr : track(resolved->track_id);
}

const TrackSpec *
PresentationBindingIndex::track_of_layer(EntityId layer_id) const noexcept {
  const auto found = layer_track_.find(layer_id);
  return found == layer_track_.end() ? nullptr : track(found->second);
}

const TrackScaleSpec *
PresentationBindingIndex::scale_of_curve_layer(
    const CurveLayerSpec &layer) const noexcept {
  return scale(layer.scale_id);
}

} // namespace welllog
