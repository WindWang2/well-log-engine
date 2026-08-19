#pragma once

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include <welllog/core/document.hpp>
#include <welllog/core/entity_id.hpp>
#include <welllog/core/export.hpp>

namespace welllog {

// O(1) entity resolution over one immutable WellLogDocument snapshot (the
// Track/Data Binding Resolver's document half). Built once per document
// revision and reused for every lookup; the pointers point into the document
// the index was built from, so the document (or its shared_ptr) must outlive
// the index. Documents are immutable pimpls, so the pointers stay valid for
// the document's lifetime.
//
// Sizes are driven by the presentation, not the sample count: building the
// index is O(number of entities) with no curve-buffer access, and lookups are
// hash lookups — safe for per-hover / per-refresh use.
class WELLLOG_CORE_API DocumentBindingIndex {
public:
  DocumentBindingIndex() = default;
  explicit DocumentBindingIndex(const WellLogDocument &document);

  [[nodiscard]] const SamplingAxis *axis(EntityId id) const noexcept;
  [[nodiscard]] const Curve *curve(EntityId id) const noexcept;
  [[nodiscard]] const QcMask *qc_mask(EntityId id) const noexcept;
  [[nodiscard]] const Interval *interval(EntityId id) const noexcept;
  [[nodiscard]] const Marker *marker(EntityId id) const noexcept;
  [[nodiscard]] const ImageSource *image_source(EntityId id) const noexcept;
  [[nodiscard]] const SymbolOccurrence *symbol(EntityId id) const noexcept;
  [[nodiscard]] const TextAnnotation *
  annotation(EntityId id) const noexcept;
  [[nodiscard]] const CustomLayerSource *
  custom_source(EntityId id) const noexcept;

  // Curves bound to one sampling axis, in document order (empty when the axis
  // is unknown). The span is owned by the index.
  [[nodiscard]] std::span<const Curve *const>
  curves_on_axis(EntityId axis_id) const noexcept;

  // The sampling axis a curve is bound to, or nullptr when the curve is
  // unknown or its axis is not registered on the document.
  [[nodiscard]] const SamplingAxis *
  axis_of_curve(const Curve &curve) const noexcept;

private:
  std::unordered_map<EntityId, const SamplingAxis *, EntityIdHash> axes_;
  std::unordered_map<EntityId, const Curve *, EntityIdHash> curves_;
  std::unordered_map<EntityId, const QcMask *, EntityIdHash> qc_masks_;
  std::unordered_map<EntityId, const Interval *, EntityIdHash> intervals_;
  std::unordered_map<EntityId, const Marker *, EntityIdHash> markers_;
  std::unordered_map<EntityId, const ImageSource *, EntityIdHash>
      image_sources_;
  std::unordered_map<EntityId, const SymbolOccurrence *, EntityIdHash>
      symbols_;
  std::unordered_map<EntityId, const TextAnnotation *, EntityIdHash>
      annotations_;
  std::unordered_map<EntityId, const CustomLayerSource *, EntityIdHash>
      custom_sources_;
  std::unordered_map<EntityId, std::vector<const Curve *>, EntityIdHash>
      curves_by_axis_;
};

} // namespace welllog
