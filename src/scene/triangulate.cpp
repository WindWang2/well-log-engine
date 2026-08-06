#include "triangulate.hpp"

#include <cmath>

namespace welllog::detail {
namespace {

// Signed area via the shoelace formula; sign reveals winding order
// (positive = counter-clockwise).
[[nodiscard]] double signed_area(const std::vector<PolygonPoint> &polygon) {
  double sum = 0.0;
  const auto count = polygon.size();
  for (std::size_t i = 0; i < count; ++i) {
    const auto &a = polygon[i];
    const auto &b = polygon[(i + 1) % count];
    sum += (b.x - a.x) * (b.y + a.y);
  }
  return sum * 0.5;
}

// 2D cross product of (b - a) and (c - a).
[[nodiscard]] double cross(const PolygonPoint &a, const PolygonPoint &b,
                           const PolygonPoint &c) {
  return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// True when point p lies inside or on triangle (a, b, c).
[[nodiscard]] bool point_in_triangle(const PolygonPoint &a,
                                     const PolygonPoint &b,
                                     const PolygonPoint &c,
                                     const PolygonPoint &p) {
  const auto d1 = cross(a, b, p);
  const auto d2 = cross(b, c, p);
  const auto d3 = cross(c, a, p);
  const auto has_neg = (d1 < 0.0) || (d2 < 0.0) || (d3 < 0.0);
  const auto has_pos = (d1 > 0.0) || (d2 > 0.0) || (d3 > 0.0);
  return !(has_neg && has_pos);
}

// True when (a, b, c) is convex given the polygon winding sign. A convex
// vertex is a candidate ear tip.
[[nodiscard]] bool is_convex(const PolygonPoint &a, const PolygonPoint &b,
                             const PolygonPoint &c, double winding) {
  // For CCW (winding > 0) a left turn (cross > 0) is convex; mirror for CW.
  const auto turn = cross(a, b, c);
  return winding > 0.0 ? turn > 0.0 : turn < 0.0;
}

} // namespace

std::vector<std::uint32_t>
triangulate_polygon(const std::vector<PolygonPoint> &polygon) noexcept {
  const auto count = polygon.size();
  if (count < 3) {
    return {};
  }
  const auto winding = signed_area(polygon);

  // Indices of the remaining polygon vertices, in order.
  std::vector<std::uint32_t> indices;
  indices.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    indices.push_back(i);
  }

  std::vector<std::uint32_t> triangles;
  triangles.reserve((count - 2) * 3);

  // Guard against degenerate spirals: cap iterations well above the number
  // of ears any simple polygon can have.
  const std::size_t max_steps = count * count + 4;
  auto guard = std::size_t{0};
  while (indices.size() > 2 && guard < max_steps) {
    ++guard;
    const auto n = indices.size();
    bool ear_found = false;
    for (std::size_t i = 0; i < n; ++i) {
      const auto prev = indices[(i + n - 1) % n];
      const auto curr = indices[i];
      const auto next = indices[(i + 1) % n];
      const auto &a = polygon[prev];
      const auto &b = polygon[curr];
      const auto &c = polygon[next];
      if (!is_convex(a, b, c, winding)) {
        continue;
      }
      // Reject the ear if any other remaining vertex lies inside it.
      bool contains_other = false;
      for (std::size_t j = 0; j < n; ++j) {
        const auto idx = indices[j];
        if (idx == prev || idx == curr || idx == next) {
          continue;
        }
        if (point_in_triangle(a, b, c, polygon[idx])) {
          contains_other = true;
          break;
        }
      }
      if (contains_other) {
        continue;
      }
      triangles.push_back(prev);
      triangles.push_back(curr);
      triangles.push_back(next);
      indices.erase(indices.begin() + static_cast<std::ptrdiff_t>(i));
      ear_found = true;
      break;
    }
    if (!ear_found) {
      // No strict ear (numerical edge / near-collinear): clip the most
      // convex remaining vertex to make progress rather than spinning.
      if (n >= 3) {
        const auto i0 = indices[0];
        const auto i1 = indices[1 % n];
        const auto i2 = indices[2 % n];
        triangles.push_back(i0);
        triangles.push_back(i1);
        triangles.push_back(i2);
        indices.erase(indices.begin() + 1);
      } else {
        break;
      }
    }
  }
  return triangles;
}

bool point_in_polygon(const std::vector<PolygonPoint> &polygon, double x,
                      double y) noexcept {
  const auto count = polygon.size();
  if (count < 3) {
    return false;
  }
  bool inside = false;
  for (std::size_t i = 0, j = count - 1; i < count; j = i++) {
    const auto &a = polygon[j];
    const auto &b = polygon[i];
    // Standard even-odd ray cast. Skip horizontal edges (a.y == b.y), which
    // never straddle the test depth and would otherwise divide by zero.
    if (a.y == b.y) {
      continue;
    }
    const auto intersects =
        ((a.y > y) != (b.y > y)) &&
        (x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x);
    if (intersects) {
      inside = !inside;
    }
  }
  return inside;
}

std::vector<PolygonPoint>
clip_polygon_to_polygon(const std::vector<PolygonPoint> &subject,
                        const std::vector<PolygonPoint> &clip) noexcept {
  if (subject.size() < 3 || clip.size() < 3) {
    return {};
  }
  // Sutherland–Hodgman treats `clip` as convex; for the layer-local custom
  // clip the host path is expected to be convex (a mask region). Compute the
  // clip winding once so the per-edge inside test honours either winding.
  const auto clip_winding = signed_area(clip);
  auto output = subject;
  const auto clip_count = clip.size();
  for (std::size_t edge = 0; edge < clip_count; ++edge) {
    const auto &edge_start = clip[edge];
    const auto &edge_end = clip[(edge + 1) % clip_count];
    // Inside test: on the same side as the clip's winding (left for CCW,
    // right for CW). The cross product sign is the signed turn at the edge.
    const auto inside = [&](const PolygonPoint &p) {
      const auto cross_val = (edge_end.x - edge_start.x) * (p.y - edge_start.y) -
                             (edge_end.y - edge_start.y) * (p.x - edge_start.x);
      return clip_winding >= 0.0 ? cross_val >= 0.0 : cross_val <= 0.0;
    };
    const auto intersect = [&](const PolygonPoint &a, const PolygonPoint &b) {
      const auto dx = b.x - a.x;
      const auto dy = b.y - a.y;
      const auto denom = (edge_end.x - edge_start.x) * dy -
                         (edge_end.y - edge_start.y) * dx;
      if (denom == 0.0) {
        return a; // parallel — keep an endpoint, harmless for our regions
      }
      const auto t = ((edge_start.x - a.x) * dy -
                      (edge_start.y - a.y) * dx) /
                     denom;
      return PolygonPoint{.x = a.x + t * dx, .y = a.y + t * dy};
    };
    std::vector<PolygonPoint> input = std::move(output);
    output.clear();
    output.reserve(input.size());
    const auto input_count = input.size();
    for (std::size_t i = 0; i < input_count; ++i) {
      const auto &current = input[i];
      const auto &previous = input[(i + input_count - 1) % input_count];
      const auto current_in = inside(current);
      const auto previous_in = inside(previous);
      if (current_in) {
        if (!previous_in) {
          output.push_back(intersect(previous, current));
        }
        output.push_back(current);
      } else if (previous_in) {
        output.push_back(intersect(previous, current));
      }
    }
    if (output.size() < 3) {
      return {};
    }
  }
  return output;
}

} // namespace welllog::detail
