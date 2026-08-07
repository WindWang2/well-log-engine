// Authoritative depth-axis tick selection (Epic B) — mirror of the Desktop
// `depth_ruler.nice_depth_ticks` semantics; see axis_ticks.hpp.

#include <welllog/scene/axis_ticks.hpp>

#include <cmath>
#include <limits>

namespace welllog {

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

}  // namespace welllog
