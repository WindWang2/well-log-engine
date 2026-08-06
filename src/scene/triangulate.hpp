#pragma once

// Simple-polygon geometry used by the crossover fill kernel: ear-clipping
// triangulation and a point-in-polygon containment test. Pure CPU, no GL or
// export-boundary dependency. Both operate on a 2D point given as a struct
// with an accessible `.x` / `.y` (we template on the point type so the kernel
// can pass its own working coordinates without conversion).
//
// The polygons produced by crossover fill are x-monotone between consecutive
// crossing depths (the upper and lower polylines are functions of depth), so
// they are always simple; ear-clipping is sufficient and robust here.

#include <cstdint>
#include <vector>

namespace welllog::detail {

struct PolygonPoint {
  double x{};
  double y{};
};

// Triangulates a simple polygon (counter-clockwise or clockwise) via ear
// clipping. Returns a flat list of index triples into `polygon`. Polygons
// with fewer than 3 vertices return empty. `polygon` is read-only.
[[nodiscard]] std::vector<std::uint32_t>
triangulate_polygon(const std::vector<PolygonPoint> &polygon) noexcept;

// Ray-casting point-in-polygon test (even-odd rule). Robust for the convex
// and weakly-simple regions crossover fill produces. Returns true when
// `(x, y)` lies inside or on the boundary of `polygon`.
[[nodiscard]] bool point_in_polygon(const std::vector<PolygonPoint> &polygon,
                                    double x, double y) noexcept;

// Clips a convex or weakly-simple `subject` polygon against the convex clip
// polygon `clip` via Sutherland–Hodgman. Used by the custom-layer kernel to
// restrict a primitive's geometry to its layer-local clip path before it
// enters the prepared-scene primitive stream, so every backend (GL/SVG/pick)
// sees only the clipped geometry. Returns an empty vector when the result is
// empty (the subject lies entirely outside the clip).
[[nodiscard]] std::vector<PolygonPoint>
clip_polygon_to_polygon(const std::vector<PolygonPoint> &subject,
                        const std::vector<PolygonPoint> &clip) noexcept;

} // namespace welllog::detail
