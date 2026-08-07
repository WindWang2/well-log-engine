// TWT (two-way time) domain via an explicit time-depth relationship (Epic B).
//
// The map reuses the ADR 0013 reversible depth-transform semantics: a
// piecewise-linear, strictly monotonic, reversible depth ↔ TWT map. An empty
// relationship is a distinct "unavailable" state — map functions return NaN,
// consumers surface degradation instead of substituting TVD×constant.

#include <welllog/scene/time_depth.hpp>

#include <cmath>
#include <limits>

namespace welllog {

namespace {

// Same-direction strict monotonicity over a coordinate pair.
bool strictly_monotonic(const std::vector<TimeDepthControlPoint> &points,
                        bool forward) noexcept {
  for (std::size_t i = 1; i < points.size(); ++i) {
    const double d0 = points[i - 1].depth;
    const double d1 = points[i].depth;
    const double t0 = points[i - 1].time_ms;
    const double t1 = points[i].time_ms;
    if (!std::isfinite(d0) || !std::isfinite(d1) || !std::isfinite(t0) ||
        !std::isfinite(t1)) {
      return false;
    }
    if (forward) {
      if (d1 <= d0 || t1 <= t0) {
        return false;
      }
    } else {
      if (d1 >= d0 || t1 >= t0) {
        return false;
      }
    }
  }
  return true;
}

double interpolate(double x, double x0, double x1, double y0,
                   double y1) noexcept {
  const double t = (x - x0) / (x1 - x0);
  return y0 + t * (y1 - y0);
}

// Piecewise-linear map: x is in the "from" coordinate (depth for depth→time,
// time for time→depth), points carry (depth, time_ms). Handles both monotonic
// directions (validation guarantees depth and time share direction).
double map_coordinate(const TimeDepthRelationship &relationship, double x,
                      bool depth_to_time) noexcept {
  const auto &pts = relationship.points;
  const std::size_t n = pts.size();
  if (n < 2) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const bool clamp = relationship.extrapolate == DepthExtrapolatePolicy::clamp;
  // Endpoints in ascending "from" order (a decreasing map's low end is the
  // LAST stored anchor).
  const bool increasing = pts.back().depth > pts.front().depth;
  const std::size_t lo = increasing ? 0 : n - 1;
  const std::size_t hi = increasing ? n - 1 : 0;
  const auto from_of = [&](std::size_t i) {
    return depth_to_time ? pts[i].depth : pts[i].time_ms;
  };
  const auto to_of = [&](std::size_t i) {
    return depth_to_time ? pts[i].time_ms : pts[i].depth;
  };
  const double lo_from = from_of(lo);
  const double hi_from = from_of(hi);

  if (x <= lo_from) {
    if (clamp) {
      return to_of(lo);
    }
    // Extrapolate along the segment adjacent to the low end.
    const std::size_t a = lo;
    const std::size_t b = increasing ? 1 : n - 2;
    return interpolate(x, from_of(a), from_of(b), to_of(a), to_of(b));
  }
  if (x >= hi_from) {
    if (clamp) {
      return to_of(hi);
    }
    const std::size_t a = increasing ? n - 2 : 1;
    const std::size_t b = hi;
    return interpolate(x, from_of(a), from_of(b), to_of(a), to_of(b));
  }
  // Interior: stored-order scan; the pair bracketing x works in either
  // direction (product of signed distances ≤ 0 ⇔ x between the two).
  for (std::size_t i = 1; i < n; ++i) {
    const double f0 = from_of(i - 1);
    const double f1 = from_of(i);
    if ((x - f0) * (x - f1) <= 0.0) {
      return interpolate(x, f0, f1, to_of(i - 1), to_of(i));
    }
  }
  return to_of(hi);
}

}  // namespace

bool time_depth_available(const TimeDepthRelationship &relationship) noexcept {
  return relationship.points.size() >= 2;
}

std::optional<Error> validate_time_depth_relationship(
    const TimeDepthRelationship &relationship) noexcept {
  if (relationship.points.empty()) {
    return std::nullopt;  // empty = unavailable, a valid state
  }
  if (relationship.points.size() < 2) {
    return Error{
        .code = ErrorCode::invalid_time_depth,
        .severity = Severity::error,
        .entity_id = std::nullopt,
        .message = MessageKey::time_depth_relationship_invalid,
        .arguments = {},
    };
  }
  if (relationship.depth_unit.empty()) {
    return Error{
        .code = ErrorCode::invalid_time_depth,
        .severity = Severity::error,
        .entity_id = std::nullopt,
        .message = MessageKey::time_depth_relationship_invalid,
        .arguments = {},
    };
  }
  // Both coordinates must be strictly monotonic in the same direction.
  if (!strictly_monotonic(relationship.points, /*forward=*/true) &&
      !strictly_monotonic(relationship.points, /*forward=*/false)) {
    return Error{
        .code = ErrorCode::invalid_time_depth,
        .severity = Severity::error,
        .entity_id = std::nullopt,
        .message = MessageKey::time_depth_relationship_invalid,
        .arguments = {},
    };
  }
  return std::nullopt;
}

double depth_to_time(const TimeDepthRelationship &relationship,
                     double depth) noexcept {
  if (!time_depth_available(relationship) || !std::isfinite(depth)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return map_coordinate(relationship, depth, /*depth_to_time=*/true);
}

double time_to_depth(const TimeDepthRelationship &relationship,
                     double time_value) noexcept {
  if (!time_depth_available(relationship) || !std::isfinite(time_value)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return map_coordinate(relationship, time_value, /*depth_to_time=*/false);
}

}  // namespace welllog
