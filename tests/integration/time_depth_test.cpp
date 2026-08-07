// TWT time-depth relationship (Epic B): explicit velocity/time-depth map.
//
// Covers: forward/inverse round-trip, mid-segment interpolation, both
// monotonic directions, clamp vs linear extrapolation, validation rejects
// (non-monotonic, single point, non-finite, empty unit), unavailable state
// (empty relationship → NaN, never TVD×constant).

#include <welllog/scene/time_depth.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

using namespace welllog;

[[noreturn]] void fail(std::string_view message) {
  std::cerr << "FAIL: " << message << '\n';
  std::_Exit(EXIT_FAILURE);
}

void require(bool condition, std::string_view message) {
  if (!condition) {
    fail(message);
  }
}

void require_near(double actual, double expected, double tolerance,
                  std::string_view message) {
  if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
    fail(message);
  }
}

TimeDepthRelationship checkshot() {
  // Simple two-layer velocity model: 1000 m @ 500 ms, 2000 m @ 1500 ms
  // (1500 m/s below 1000 m → 1000 ms for the next 1000 m).
  return TimeDepthRelationship{
      .points = {
          TimeDepthControlPoint{.depth = 0.0, .time_ms = 0.0},
          TimeDepthControlPoint{.depth = 1000.0, .time_ms = 500.0},
          TimeDepthControlPoint{.depth = 2000.0, .time_ms = 1500.0},
      },
      .depth_domain = DepthDomain::true_vertical_depth,
      .depth_unit = "m",
      .time_unit = TimeUnit::milliseconds,
      .extrapolate = DepthExtrapolatePolicy::linear,
      .source = "checkshot-A",
      .version = 1,
  };
}

void availability_and_validation() {
  require(!time_depth_available(TimeDepthRelationship{}),
          "an empty relationship must be unavailable");
  require(validate_time_depth_relationship(TimeDepthRelationship{}) ==
              std::nullopt,
          "an empty (unavailable) relationship must validate");

  auto rel = checkshot();
  require(validate_time_depth_relationship(rel) == std::nullopt,
          "a valid checkshot map must validate");
  require(time_depth_available(rel), "a checkshot map must be available");

  auto single = rel;
  single.points.resize(1);
  require(validate_time_depth_relationship(single) != std::nullopt,
          "a single-point map must be rejected");

  auto non_monotonic = rel;
  non_monotonic.points[2].depth = 900.0;  // depth decreases
  require(validate_time_depth_relationship(non_monotonic) != std::nullopt,
          "non-monotonic depth must be rejected");

  auto time_flip = rel;
  time_flip.points[2].time_ms = 400.0;  // time decreases while depth increases
  require(validate_time_depth_relationship(time_flip) != std::nullopt,
          "same-direction monotonicity must be enforced");

  auto nan_point = rel;
  nan_point.points[1].time_ms = std::numeric_limits<double>::quiet_NaN();
  require(validate_time_depth_relationship(nan_point) != std::nullopt,
          "non-finite anchors must be rejected");

  auto no_unit = rel;
  no_unit.depth_unit.clear();
  require(validate_time_depth_relationship(no_unit) != std::nullopt,
          "a missing depth unit must be rejected");
}

void forward_and_inverse_round_trip() {
  const auto rel = checkshot();
  // Mid-segment: 1500 m lies between (1000, 500) and (2000, 1500): 1000 ms.
  require_near(depth_to_time(rel, 1500.0), 1000.0, 1e-9,
               "mid-segment interpolation");
  // Inverse returns the anchor depth.
  require_near(time_to_depth(rel, depth_to_time(rel, 1500.0)), 1500.0, 1e-6,
               "forward/inverse round-trip");
  // First segment slope 0.5 ms/m (0→1000 m ↔ 0→500 ms).
  require_near(depth_to_time(rel, 501.0), 250.5, 1e-9,
               "first-segment slope 0.5 ms/m");
  // Second segment slope 1 ms/m (1000→2000 m ↔ 500→1500 ms).
  require_near(depth_to_time(rel, 1501.0), 1001.0, 1e-9,
               "second-segment slope 1 ms/m");
}

void extrapolation_policies() {
  auto linear = checkshot();
  require_near(depth_to_time(linear, 2500.0), 2000.0, 1e-9,
               "linear extrapolation continues the last segment slope");
  require_near(time_to_depth(linear, -500.0), -1000.0, 1e-9,
               "linear extrapolation also applies to the inverse");

  auto clamped = checkshot();
  clamped.extrapolate = DepthExtrapolatePolicy::clamp;
  require_near(depth_to_time(clamped, 2500.0), 1500.0, 1e-9,
               "clamp extrapolation pins to the last anchor");
  require_near(time_to_depth(clamped, -100.0), 0.0, 1e-9,
               "clamp extrapolation pins the inverse to the first anchor");
}

void decreasing_map_is_reversible() {
  // Decreasing direction (reverse-drilled / sorted depth): both coordinates
  // decrease together — still a valid reversible map.
  TimeDepthRelationship rel{
      .points = {
          TimeDepthControlPoint{.depth = 2000.0, .time_ms = 1500.0},
          TimeDepthControlPoint{.depth = 1000.0, .time_ms = 500.0},
          TimeDepthControlPoint{.depth = 0.0, .time_ms = 0.0},
      },
      .depth_domain = DepthDomain::true_vertical_depth,
      .depth_unit = "m",
      .time_unit = TimeUnit::milliseconds,
      .extrapolate = DepthExtrapolatePolicy::clamp,
      .source = "desc",
  };
  require(validate_time_depth_relationship(rel) == std::nullopt,
          "a decreasing monotonic map must validate");
  require_near(depth_to_time(rel, 1500.0), 1000.0, 1e-9,
               "decreasing map interpolates the same values");
  require_near(time_to_depth(rel, 1000.0), 1500.0, 1e-9,
               "decreasing map inverse round-trips");
}

void unavailable_returns_nan() {
  const TimeDepthRelationship empty;
  require(std::isnan(depth_to_time(empty, 1000.0)),
          "an unavailable relationship must yield NaN (explicit degradation)");
  require(std::isnan(time_to_depth(empty, 500.0)),
          "an unavailable inverse must yield NaN");
  const auto rel = checkshot();
  require(std::isnan(depth_to_time(rel, std::numeric_limits<double>::infinity())),
          "non-finite input must yield NaN");
}

void time_units_are_explicit() {
  const auto rel = checkshot();
  require(rel.time_unit == TimeUnit::milliseconds,
          "the time unit must be explicit on the relationship");
  require(rel.depth_domain == DepthDomain::true_vertical_depth,
          "the depth domain must be explicit on the relationship");
  require(rel.source == "checkshot-A", "provenance must round-trip");
  require(rel.version == 1, "the version tag must be preserved");
}

}  // namespace

int main() {
  availability_and_validation();
  forward_and_inverse_round_trip();
  extrapolation_policies();
  decreasing_map_is_reversible();
  unavailable_returns_nan();
  time_units_are_explicit();
  std::cout << "PASS: time-depth relationship\n";
  return EXIT_SUCCESS;
}
