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
#include <welllog/scene/scene.hpp>        // DepthTransform
#include <welllog/scene/time_depth.hpp>   // TimeDepthRelationship

#include <cstdint>
#include <string>
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

// Secondary-domain ticks over a DISPLAY window (multi-axis, Epic B): the
// window endpoints are inverse-mapped through the transform into the
// reference domain, then the authoritative ladder runs over that range. The
// returned values are in the REFERENCE domain; consumers position them via
// the forward mapping. Identity transforms behave like nice_axis_ticks.
[[nodiscard]] WELLLOG_SCENE_API AxisTicks ticks_for_reference_window(
    const DepthTransform &transform, double display_top, double display_bottom,
    std::uint32_t max_ticks = 9) noexcept;

// TWT ticks over a DISPLAY window: reference range = transform⁻¹(window),
// then the TWT relationship maps it into time. The relationship's
// ``depth_domain`` must match the transform's reference domain — the caller
// declares the chain explicitly (no implicit domain guessing). Values are in
// the relationship's time unit; empty when TWT is unavailable.
[[nodiscard]] WELLLOG_SCENE_API AxisTicks ticks_for_twt_window(
    const DepthTransform &transform, const TimeDepthRelationship &twt,
    double display_top, double display_bottom,
    std::uint32_t max_ticks = 9) noexcept;

// Secondary-axis ticks over a DISPLAY window for an EITHER-direction
// monotonic (reference, display) mapping (Epic B, multi-axis): unlike
// DepthTransform (which requires both coordinates strictly increasing), this
// accepts decreasing reference values — e.g. TVDSS (reference) vs MD
// (display) — which is the common single-well secondary axis. The window
// endpoints are mapped to reference values by linear interpolation over the
// display-sorted points (clamped outside the range), then the authoritative
// ladder runs over that range. Values are in the REFERENCE domain.
[[nodiscard]] WELLLOG_SCENE_API AxisTicks ticks_for_secondary_window(
    std::span<const std::pair<double, double>> reference_display_points,
    double display_top, double display_bottom,
    std::uint32_t max_ticks = 9) noexcept;

// Tick label with precision trimmed to the step — mirror of the Desktop
// ``depth_ruler.format_depth_label`` semantics (parity-tested): 1050/25 →
// "1050", 1050.5/0.5 → "1050.5", 1050.25/0.25 → "1050.25". Float drift is
// rounded away.
[[nodiscard]] WELLLOG_SCENE_API std::string
format_axis_tick_label(double value, double step) noexcept;

}  // namespace welllog

#endif  // WELLLOG_SCENE_AXIS_TICKS_HPP
