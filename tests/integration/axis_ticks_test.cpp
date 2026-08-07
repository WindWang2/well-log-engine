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

int main() {
  window_200_uses_25m();
  window_1000_1100_uses_20m();
  start_not_aligned_starts_at_first_multiple();
  fractional_steps_and_fallback();
  degenerate_and_non_finite();
  std::cout << "PASS: axis ticks\n";
  return EXIT_SUCCESS;
}
