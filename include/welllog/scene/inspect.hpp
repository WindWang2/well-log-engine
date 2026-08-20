#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <welllog/core/document.hpp>
#include <welllog/core/entity_id.hpp>
#include <welllog/core/units.hpp>
#include <welllog/scene/export.hpp>
#include <welllog/scene/scene.hpp>

namespace welllog {

// Everything a hover/inspect tooltip shows about a curve pick (ADR 0030
// semantic picking, extended by the Track/Data workflow): the resolved
// well/track/curve/layer/scale identities, the curve's human labels, the raw
// sample value with its QC state, the scale context, and — for a derived
// curve — its provenance/freshness. Resolved in C++ (one hash lookup per
// field via the binding indexes) so Python/UI layers only receive the
// coalesced result, never per-field scans.
struct CurvePickInfo {
  EntityId document_id{};
  EntityId track_id{};
  EntityId layer_id{};
  EntityId curve_id{};
  EntityId scale_id{};
  EntityId sampling_axis_id{};

  std::string mnemonic;
  std::string display_name;
  std::string unit;
  std::string scale_unit;

  std::uint64_t sample_index{};
  double reference_depth{};
  double display_depth{};
  double raw_value{};
  QcState qc_state{QcState::valid};

  ScaleMode scale_mode{ScaleMode::linear};
  double scale_minimum{};
  double scale_maximum{};
  ScaleDirection scale_direction{ScaleDirection::left_to_right};

  bool derived{false};
  DerivedFreshness derived_freshness{DerivedFreshness::current};
  std::string algorithm_id;
  std::string algorithm_version;
};

// Resolves a CurvePick against the document + presentation it was made from.
// Returns nullopt when the pick refers to entities that no longer exist in
// the passed state (a stale pick after a revision change — the caller simply
// shows nothing rather than an error).
[[nodiscard]] WELLLOG_SCENE_API std::optional<CurvePickInfo>
resolve_curve_pick(const WellLogDocument &document,
                   const ScenePresentation &presentation,
                   const CurvePick &pick) noexcept;

} // namespace welllog
