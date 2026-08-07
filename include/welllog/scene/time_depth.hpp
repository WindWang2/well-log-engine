// TWT (two-way time) domain via an explicit time-depth relationship (Epic B).
//
// Extends the reversible depth-transform chain of ADR 0013: depth ↔ TWT is
// the same piecewise-linear, strictly monotonic, reversible map as
// `DepthTransform`, expressed over a time target unit. TWT is NEVER derived
// by a constant factor or an axis-label change — it only exists when an
// explicit velocity / time-depth table is supplied.
//
// An EMPTY relationship means "no velocity / time-depth data": consumers must
// surface that as unavailable / degradation (map functions return NaN), never
// silently substitute TVD×constant.
//
// Note: the depth-transform math currently lives in the scene module
// (DepthTransform, scene.hpp); this module follows that layout. Core remains
// the documented owner (architecture.md §2) — a move is tracked separately.

#ifndef WELLLOG_SCENE_TIME_DEPTH_HPP
#define WELLLOG_SCENE_TIME_DEPTH_HPP

#include <welllog/core/document.hpp>  // DepthDomain
#include <welllog/core/result.hpp>
#include <welllog/scene/export.hpp>
#include <welllog/scene/scene.hpp>  // DepthExtrapolatePolicy

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace welllog {

// One depth ↔ two-way-time anchor (e.g. a checkshot point).
struct TimeDepthControlPoint {
  double depth{};    // in the relationship's depth unit (e.g. m TVDSS)
  double time_ms{};  // two-way time in milliseconds
};

enum class TimeUnit : std::uint8_t {
  milliseconds,
  seconds,
};

// Piecewise-linear, strictly monotonic, reversible depth ↔ TWT map (#, Epic B).
struct TimeDepthRelationship {
  // Strictly monotonic in both coordinates; empty ⇒ unavailable.
  std::vector<TimeDepthControlPoint> points;
  // Which depth domain the control points are expressed in (the map is
  // depth-domain-scoped: MD vs TVD vs TVDSS anchor different depths).
  DepthDomain depth_domain{DepthDomain::true_vertical_depth};
  std::string depth_unit;  // "m" / "ft"
  TimeUnit time_unit{TimeUnit::milliseconds};
  // Same extrapolation policy as DepthTransform (linear slope vs clamp).
  DepthExtrapolatePolicy extrapolate{DepthExtrapolatePolicy::linear};
  // Provenance of the relationship (checkshot file, velocity survey id…).
  std::string source;
  std::uint64_t version{};
  friend bool operator==(const TimeDepthRelationship &,
                         const TimeDepthRelationship &) = default;
};

// True when the relationship carries enough points to map (≥ 2). Empty means
// TWT is unavailable — not identity.
[[nodiscard]] WELLLOG_SCENE_API bool
time_depth_available(const TimeDepthRelationship &relationship) noexcept;

// Validates a non-empty relationship: ≥ 2 points, all finite, strictly
// monotonic in both coordinates with the same direction, non-empty units.
// Empty relationships are valid (unavailable). Returns nullopt on success.
[[nodiscard]] WELLLOG_SCENE_API std::optional<Error>
validate_time_depth_relationship(
    const TimeDepthRelationship &relationship) noexcept;

// Depth → two-way time (in the relationship's time unit). NaN when the
// relationship is unavailable or the input is non-finite; otherwise the
// piecewise-linear map with the declared extrapolation policy.
[[nodiscard]] WELLLOG_SCENE_API double
depth_to_time(const TimeDepthRelationship &relationship, double depth) noexcept;

// TWT → depth. Unique inverse of :func:`depth_to_time` (valid map).
[[nodiscard]] WELLLOG_SCENE_API double
time_to_depth(const TimeDepthRelationship &relationship,
              double time_value) noexcept;

}  // namespace welllog

#endif  // WELLLOG_SCENE_TIME_DEPTH_HPP
