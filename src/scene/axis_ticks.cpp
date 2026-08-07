// Authoritative depth-axis tick selection (Epic B) — mirror of the Desktop
// `depth_ruler.nice_depth_ticks` semantics; see axis_ticks.hpp.

#include <welllog/scene/axis_ticks.hpp>

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

namespace welllog {

namespace {

AxisTicks ticks_over_range(double v0, double v1,
                           std::uint32_t max_ticks) noexcept {
  if (v0 > v1) {
    std::swap(v0, v1);
  }
  return nice_axis_ticks(v0, v1, max_ticks);
}

}  // namespace

AxisTicks nice_axis_ticks(double d0, double d1,
                          std::uint32_t max_ticks) noexcept {
  if (!std::isfinite(d0) || !std::isfinite(d1) || d1 <= d0 || max_ticks < 1) {
    return {};
  }
  const double span = d1 - d0;
  const double raw = span / static_cast<double>(max_ticks);
  const double magnitude = std::pow(10.0, std::floor(std::log10(raw)));
  double step = 10.0 * magnitude;
  for (const double factor : {1.0, 2.0, 2.5, 5.0}) {
    const double candidate = factor * magnitude;
    if (candidate >= raw) {
      step = candidate;
      break;
    }
  }
  AxisTicks out;
  out.step = step;
  const double first = std::ceil(d0 / step) * step;
  const double limit = d1 + step * 1e-9;
  for (double value = first; value <= limit; value += step) {
    out.values.push_back(value);
    // Guard against a non-progressing step (extreme magnitudes).
    if (out.values.size() > 4096) {
      out.values.clear();
      out.step = 0.0;
      return out;
    }
  }
  return out;
}

AxisTicks ticks_for_reference_window(const DepthTransform &transform,
                                     double display_top,
                                     double display_bottom,
                                     std::uint32_t max_ticks) noexcept {
  if (!std::isfinite(display_top) || !std::isfinite(display_bottom) ||
      display_bottom <= display_top || max_ticks < 1) {
    return {};
  }
  const double ref_top = map_display_to_reference(transform, display_top);
  const double ref_bottom = map_display_to_reference(transform, display_bottom);
  return ticks_over_range(ref_top, ref_bottom, max_ticks);
}

AxisTicks ticks_for_twt_window(const DepthTransform &transform,
                               const TimeDepthRelationship &twt,
                               double display_top, double display_bottom,
                               std::uint32_t max_ticks) noexcept {
  if (!time_depth_available(twt) || !std::isfinite(display_top) ||
      !std::isfinite(display_bottom) || display_bottom <= display_top ||
      max_ticks < 1) {
    return {};
  }
  const double ref_top = map_display_to_reference(transform, display_top);
  const double ref_bottom = map_display_to_reference(transform, display_bottom);
  const double time_top = depth_to_time(twt, ref_top);
  const double time_bottom = depth_to_time(twt, ref_bottom);
  if (!std::isfinite(time_top) || !std::isfinite(time_bottom)) {
    return {};
  }
  return ticks_over_range(time_top, time_bottom, max_ticks);
}

std::string format_axis_tick_label(double value, double step) noexcept {
  if (!std::isfinite(value) || !std::isfinite(step) || step <= 0.0) {
    return {};
  }
  int decimals = 0;
  while (decimals < 8 && std::abs(std::round(step * std::pow(10.0, decimals)) -
                                  step * std::pow(10.0, decimals)) > 1e-9) {
    ++decimals;
  }
  if (decimals == 0) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.0f", value);
    return std::string{buffer};
  }
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
  std::string text{buffer};
  while (!text.empty() && text.back() == '0') {
    text.pop_back();
  }
  if (!text.empty() && text.back() == '.') {
    text.pop_back();
  }
  return text;
}

}  // namespace welllog
