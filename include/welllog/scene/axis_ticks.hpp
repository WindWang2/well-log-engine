// Authoritative depth-axis tick semantics (Epic B): nice-step tick selection
// for vertical axes. The Desktop `depth_ruler.py` hosts the same algorithm as
// its headless fallback; the SDK version here is the single source of truth
// and the parity test locks the two together (no drifting duplicate).
//
// Steps come from the depth-friendly nice ladder {1, 2, 2.5, 5} × 10^k (25 m,
// 250 m, …): the smallest step that keeps the tick count ≤ max_ticks is
// chosen, the first tick is the first multiple of the step ≥ d0.

#ifndef WELLLOG_SCENE_AXIS_TICKS_HPP
#define WELLLOG_SCENE_AXIS_TICKS_HPP

#include <welllog/scene/export.hpp>

#include <cstdint>
#include <vector>

namespace welllog {

struct AxisTicks {
  double step{};
  std::vector<double> values;  // ascending, within [d0, d1 + step*1e-9]
};

// Choose tick values for a depth window ``[d0, d1]`` (see module comment).
// Returns an empty value list for a degenerate/non-finite window.
[[nodiscard]] WELLLOG_SCENE_API AxisTicks
nice_axis_ticks(double d0, double d1, std::uint32_t max_ticks = 9) noexcept;

}  // namespace welllog

#endif  // WELLLOG_SCENE_AXIS_TICKS_HPP
