// Authoritative axis tick semantics (Epic B): nice ladder + parity anchors.
//
// The exact values here are the contract the Desktop `depth_ruler.py`
// fallback must reproduce (test_axis_tick_parity enforces it against the
// Python binding).

#include <welllog/scene/axis_ticks.hpp>

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
  if (std::abs(actual - expected) > tolerance) {
    fail(message);
  }
}

void window_200_uses_25m() {
  const auto ticks = nice_axis_ticks(0.0, 200.0);
  require_near(ticks.step, 25.0, 1e-9, "200 m window must use 25 m steps");
  require(ticks.values.size() == 9, "25 m steps over 0–200 m yield 9 ticks");
  require_near(ticks.values.front(), 0.0, 1e-9, "first tick at 0");
  require_near(ticks.values.back(), 200.0, 1e-9, "last tick at 200");
}

void window_1000_1100_uses_20m() {
  const auto ticks = nice_axis_ticks(1000.0, 1100.0);
  require_near(ticks.step, 20.0, 1e-9, "100 m window must use 20 m steps");
  require(ticks.values.size() <= 9, "tick count must respect max_ticks");
  require_near(ticks.values.front(), 1000.0, 1e-9, "first tick aligned");
  require_near(ticks.values.back(), 1100.0, 1e-9, "last tick at the window end");
}

void start_not_aligned_starts_at_first_multiple() {
  const auto ticks = nice_axis_ticks(1003.0, 1103.0);
  require_near(ticks.values.front(), 1020.0, 1e-9,
               "first tick is the first multiple of the step ≥ d0");
  require_near(ticks.values.back(), 1100.0, 1e-9, "ticks stop inside the window");
}

void fractional_steps_and_fallback() {
  const auto frac = nice_axis_ticks(0.0, 4.0);
  require_near(frac.step, 0.5, 1e-12, "0–4 m window uses 0.5 m steps");
  require(frac.values.size() == 9, "0.5 m steps over 0–4 m yield 9 ticks");
  // raw = 5/9 ≈ 0.56 → ladder all below → step 1.0 (10× magnitude fallback).
  const auto fallback = nice_axis_ticks(0.0, 5.0);
  require_near(fallback.step, 1.0, 1e-12, "ladder shortfall falls back to 10×");
  require(fallback.values.size() == 6, "1 m steps over 0–5 m yield 6 ticks");
}

void degenerate_and_non_finite() {
  const auto degenerate = nice_axis_ticks(10.0, 10.0);
  require(degenerate.values.empty(), "a zero-span window must yield no ticks");
  const auto reversed = nice_axis_ticks(10.0, 5.0);
  require(reversed.values.empty(), "a reversed window must yield no ticks");
  const auto nan = nice_axis_ticks(std::numeric_limits<double>::quiet_NaN(),
                                   10.0);
  require(nan.values.empty(), "non-finite windows must yield no ticks");
}

}  // namespace

void reference_window_ticks_inverse_map() {
  // Display = reference + 1000 (a datum shift): ticks over the display
  // window 2000–2200 must come out in the REFERENCE domain (1000–1200).
  DepthTransform shift;
  shift.control_points = {
      DepthControlPoint{.reference_depth = 0.0, .display_depth = 1000.0},
      DepthControlPoint{.reference_depth = 2000.0, .display_depth = 3000.0},
  };
  const auto ticks = ticks_for_reference_window(shift, 2000.0, 2200.0);
  require_near(ticks.step, 25.0, 1e-9, "reference ticks keep the nice ladder");
  require_near(ticks.values.front(), 1000.0, 1e-9,
               "reference-domain ticks are the inverse-mapped window");
  require_near(ticks.values.back(), 1200.0, 1e-9,
               "reference-domain ticks end at the mapped window end");
}

void twt_window_ticks_in_time_domain() {
  const TimeDepthRelationship twt{
      .points = {
          TimeDepthControlPoint{.depth = 0.0, .time_ms = 0.0},
          TimeDepthControlPoint{.depth = 1000.0, .time_ms = 500.0},
          TimeDepthControlPoint{.depth = 2000.0, .time_ms = 1500.0},
      },
      .depth_domain = DepthDomain::true_vertical_depth,
      .depth_unit = "m",
      .time_unit = TimeUnit::milliseconds,
      .source = "checkshot",
  };
  // Identity display transform: window 0–2000 m ↔ 0–1500 ms.
  const auto ticks = ticks_for_twt_window(DepthTransform{}, twt, 0.0, 2000.0);
  require_near(ticks.step, 250.0, 1e-9, "TWT ticks use the nice ladder in ms");
  require(ticks.values.front() >= 0.0, "TWT ticks start inside the window");
  require_near(ticks.values.back(), 1500.0, 1e-9,
               "TWT ticks end at the window end in time");
}

void twt_unavailable_yields_empty() {
  const auto ticks = ticks_for_twt_window(
      DepthTransform{}, TimeDepthRelationship{}, 0.0, 2000.0);
  require(ticks.values.empty(),
          "unavailable TWT must yield no ticks (explicit degradation)");
}

void label_precision_and_drift() {
  require(format_axis_tick_label(1050.0, 25.0) == "1050", "integer label");
  require(format_axis_tick_label(1000.0, 250.0) == "1000", "large step label");
  require(format_axis_tick_label(1050.5, 0.5) == "1050.5", "half label");
  require(format_axis_tick_label(1050.25, 0.25) == "1050.25",
          "quarter label");
  require(format_axis_tick_label(1050.0, 0.5) == "1050",
          "trailing zeros trimmed");
  require(format_axis_tick_label(1.1999999999999997, 0.25) == "1.2",
          "float drift rounded away");
  require(format_axis_tick_label(2.0000000000000004, 0.5) == "2",
          "float drift in whole values");
}

}  // namespace

int main() {
  window_200_uses_25m();
  window_1000_1100_uses_20m();
  start_not_aligned_starts_at_first_multiple();
  fractional_steps_and_fallback();
  degenerate_and_non_finite();
  reference_window_ticks_inverse_map();
  twt_window_ticks_in_time_domain();
  twt_unavailable_yields_empty();
  label_precision_and_drift();
  std::cout << "PASS: axis ticks\n";
  return EXIT_SUCCESS;
}
