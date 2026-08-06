#pragma once

// Export degradation report (criterion 7, table-and-export.md section 8.4).
// Backend-neutral, header-only (inline) so the SVG (welllog_export_vector) and
// PDF (welllog_export_pdf) exporters each link their own copy without one
// library depending on the other — mirroring export_layout.hpp. The report is
// the out-of-band channel through which a mixed-mode export records the layers
// it chose to rasterize, instead of rasterizing silently. Pure-vector mode
// (the default) leaves it empty and never rasterizes.
//
// Determinism: exporters fill `degraded_layers` in evaluate_complexity's
// iteration order (the scene's curve_layers() order, which is z-sorted at
// prepare time), so an identical scene + snapshot yields a byte-identical
// report.

#include <cstdint>
#include <vector>

#include <welllog/core/entity_id.hpp>
#include <welllog/scene/scene.hpp>

namespace welllog {

// The two export modes (table-and-export.md section 8.4). Stored on
// ExportPageSpec so the whole export is reproducible from the snapshot.
enum class ExportMode : std::uint8_t {
  // Every non-source-raster layer stays vector; complexity never forces a
  // silent fallback. If a complexity budget is set and a layer would exceed
  // it, the export fails with invalid_presentation rather than rasterizing.
  pure_vector,
  // The host explicitly allows complex layers to be rasterized for fidelity;
  // each such layer is recorded in the ExportReport (id + DPI + reason).
  mixed,
};

// Why a layer was rasterized in mixed mode. Extensible: future vector features
// not yet supported (e.g. gradient fills) would surface here.
enum class ExportDegradationReason : std::uint8_t {
  // The layer's vector complexity (e.g. curve point count) exceeded the
  // vector_complexity_budget set on ExportPageSpec.
  complexity_threshold,
  // A vector feature the backend does not yet emit natively (reserved).
  unsupported_vector_feature,
};

// One layer the export chose to rasterize. `target_dpi` is the export DPI the
// rasterized representation was (or would be) rendered at, taken from the
// ExportPageSpec so the host can reproduce it.
struct DegradedLayer {
  EntityId layer_id{};
  ExportDegradationReason reason{ExportDegradationReason::complexity_threshold};
  std::uint32_t target_dpi{};
};

// The out-of-band degradation report an exporter fills. Empty when no layer was
// degraded (always the case for pure-vector mode with no budget, or when no
// layer exceeded the budget).
struct ExportReport {
  std::vector<DegradedLayer> degraded_layers;
  [[nodiscard]] bool empty() const noexcept { return degraded_layers.empty(); }
};

// Result of evaluating the complexity heuristic against a snapshot's mode +
// budget. Returned so each backend can act on the decision identically:
//   - `over_budget` is empty → emit the layer as a vector (no degradation).
//   - `over_budget` non-empty AND mode == mixed → record each entry in the
//     ExportReport (the layer is rasterized by the future raster path).
//   - `over_budget` non-empty AND mode == pure_vector → the export must FAIL
//     with invalid_presentation (pure-vector mode never silently rasterizes).
// Determinism: layers are evaluated in curve_layers() order (the scene's stored
// order, stable across backends), so identical input yields an identical list.
struct ComplexityDecision {
  std::vector<DegradedLayer> over_budget;
  [[nodiscard]] bool would_degrade() const noexcept {
    return !over_budget.empty();
  }
};

// The shared complexity heuristic (criterion 7). Sums each visible curve
// layer's prepared point count across its segments and flags any layer whose
// total exceeds `complexity_budget`. A budget of 0 flags nothing. Takes the
// budget + target DPI directly (not the full ExportPageSpec) so this header
// does not depend on pagination.hpp — both backends call it with the same
// page.vector_complexity_budget / page.dpi, so they make the EXACT same
// degradation decision for the same scene + page: the basis of cross-backend
// parity. Future kinds (intervals, fills) extend this one function.
//
// Callers apply the result identically:
//   - empty AND mode == pure_vector or mixed → emit the layer as a vector.
//   - non-empty AND mode == mixed → record each entry in the ExportReport.
//   - non-empty AND mode == pure_vector → the export FAILS with
//     invalid_presentation (pure-vector mode never silently rasterizes).
[[nodiscard]] inline ComplexityDecision
evaluate_complexity(const PreparedScene &scene,
                    std::uint64_t complexity_budget,
                    std::uint32_t target_dpi) noexcept {
  ComplexityDecision decision;
  if (complexity_budget == 0) {
    return decision;
  }
  const auto segments = scene.curve_segments();
  for (const auto &layer : scene.curve_layers()) {
    if (!layer.visible) {
      continue;
    }
    std::uint64_t point_total = 0;
    const auto seg_end = layer.first_segment + layer.segment_count;
    for (std::uint64_t s = layer.first_segment; s < seg_end; ++s) {
      point_total += segments[s].point_count;
    }
    if (point_total > complexity_budget) {
      decision.over_budget.push_back(DegradedLayer{
          .layer_id = layer.id,
          .reason = ExportDegradationReason::complexity_threshold,
          .target_dpi = target_dpi,
      });
    }
  }
  return decision;
}

} // namespace welllog
