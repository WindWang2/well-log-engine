#include <welllog/core/document_index.hpp>

namespace welllog {
namespace {

template <typename Entity, typename Map>
void index_entities(const std::span<const Entity> entities, Map &map) {
  for (const auto &entity : entities) {
    map.emplace(entity.id, &entity);
  }
}

} // namespace

DocumentBindingIndex::DocumentBindingIndex(const WellLogDocument &document) {
  index_entities(document.sampling_axes(), axes_);
  index_entities(document.curves(), curves_);
  index_entities(document.qc_masks(), qc_masks_);
  index_entities(document.intervals(), intervals_);
  index_entities(document.markers(), markers_);
  index_entities(document.image_sources(), image_sources_);
  index_entities(document.symbols(), symbols_);
  index_entities(document.annotations(), annotations_);
  index_entities(document.custom_sources(), custom_sources_);
  // Document order (not hash order) so curves_on_axis is stable for UI lists.
  for (const auto &curve : document.curves()) {
    curves_by_axis_[curve.sampling_axis_id].push_back(&curve);
  }
}

namespace {

template <typename Map>
const auto *lookup(const Map &map, EntityId id) noexcept {
  const auto found = map.find(id);
  return found == map.end() ? nullptr : found->second;
}

} // namespace

const SamplingAxis *DocumentBindingIndex::axis(EntityId id) const noexcept {
  return lookup(axes_, id);
}

const Curve *DocumentBindingIndex::curve(EntityId id) const noexcept {
  return lookup(curves_, id);
}

const QcMask *DocumentBindingIndex::qc_mask(EntityId id) const noexcept {
  return lookup(qc_masks_, id);
}

const Interval *DocumentBindingIndex::interval(EntityId id) const noexcept {
  return lookup(intervals_, id);
}

const Marker *DocumentBindingIndex::marker(EntityId id) const noexcept {
  return lookup(markers_, id);
}

const ImageSource *
DocumentBindingIndex::image_source(EntityId id) const noexcept {
  return lookup(image_sources_, id);
}

const SymbolOccurrence *
DocumentBindingIndex::symbol(EntityId id) const noexcept {
  return lookup(symbols_, id);
}

const TextAnnotation *
DocumentBindingIndex::annotation(EntityId id) const noexcept {
  return lookup(annotations_, id);
}

const CustomLayerSource *
DocumentBindingIndex::custom_source(EntityId id) const noexcept {
  return lookup(custom_sources_, id);
}

std::span<const Curve *const>
DocumentBindingIndex::curves_on_axis(EntityId axis_id) const noexcept {
  const auto found = curves_by_axis_.find(axis_id);
  return found == curves_by_axis_.end()
             ? std::span<const Curve *const>{}
             : std::span<const Curve *const>{found->second};
}

const SamplingAxis *
DocumentBindingIndex::axis_of_curve(const Curve &curve) const noexcept {
  return axis(curve.sampling_axis_id);
}

} // namespace welllog
