// True Stratigraphic Thickness — planar/local model (Epic D); see tst.hpp.

#include <welllog/scene/tst.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

namespace welllog {

namespace {

// Validates a unit vector; returns the norm on success (for dot products).
std::optional<double> unit_vector(double x, double y, double z) noexcept {
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
    return std::nullopt;
  }
  const double norm = std::sqrt(x * x + y * y + z * z);
  if (!(norm > 0.0) || std::abs(norm - 1.0) > 1e-6) {
    return std::nullopt;
  }
  return norm;
}

Error invalid_geometry() noexcept {
  return Error{
      .code = ErrorCode::invalid_geometry,
      .severity = Severity::error,
      .entity_id = std::nullopt,
      .message = MessageKey::invalid_geometry,
      .arguments = {},
  };
}

// Sum of the per-layer projections of [start_md, end_md] along a straight
// leg with (unit) direction w. Intervals outside every layer contribute
// nothing (undeclared bedding is the caller's responsibility).
double accumulate_layers(double start_md, double end_md,
                         const WellDirection3D& w, const BeddingLayer* layers,
                         std::size_t layer_count) noexcept {
  double total = 0.0;
  for (std::size_t i = 0; i < layer_count; ++i) {
    const BeddingLayer& layer = layers[i];
    const double lo = std::max(start_md, layer.top_md);
    const double hi = std::min(end_md, layer.bottom_md);
    if (!(hi > lo)) {
      continue;
    }
    const double dot = std::abs(w.x * layer.normal.x + w.y * layer.normal.y +
                                w.z * layer.normal.z);
    total += (hi - lo) * dot;
  }
  return total;
}

// The BeddingLayer contract: ordered by ascending top_md, non-overlapping,
// strictly positive extent, finite bounds, unit normals.
bool valid_layers(const BeddingLayer* layers, std::size_t layer_count) noexcept {
  if (layer_count == 0) {
    return true;
  }
  if (layers == nullptr) {
    return false;
  }
  double prev_bottom_md = 0.0;
  for (std::size_t i = 0; i < layer_count; ++i) {
    const BeddingLayer& layer = layers[i];
    if (!std::isfinite(layer.top_md) || !std::isfinite(layer.bottom_md)) {
      return false;
    }
    if (!(layer.bottom_md > layer.top_md)) {
      return false;  // zero-extent / inverted layer
    }
    if (!unit_vector(layer.normal.x, layer.normal.y, layer.normal.z)) {
      return false;  // missing orientation is an input error
    }
    if (i > 0 && layer.top_md < prev_bottom_md) {
      return false;  // overlap or out-of-order
    }
    prev_bottom_md = layer.bottom_md;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Surface-based model (Epic D high-order extension) — see tst.hpp.
// ---------------------------------------------------------------------------

// The SurfaceGrid contract (tst.hpp): ≥ 2 nodes per dimension, finite
// positive steps, finite origin, finite heights, non-null height array.
bool valid_surface_grid(const SurfaceGrid& s) noexcept {
  if (s.z_tvd == nullptr || s.x_nodes < 2 || s.y_nodes < 2) {
    return false;
  }
  if (!std::isfinite(s.x_origin_m) || !std::isfinite(s.y_origin_m) ||
      !std::isfinite(s.x_step_m) || !(s.x_step_m > 0.0) ||
      !std::isfinite(s.y_step_m) || !(s.y_step_m > 0.0)) {
    return false;
  }
  const std::size_t total = s.x_nodes * s.y_nodes;
  for (std::size_t i = 0; i < total; ++i) {
    if (!std::isfinite(s.z_tvd[i])) {
      return false;
    }
  }
  return true;
}

// True when (x, y) is inside the surface's rectangular footprint
// (boundaries inclusive).
bool in_footprint(const SurfaceGrid& s, double x, double y) noexcept {
  return x >= s.x_origin_m &&
         x <= s.x_origin_m +
                  static_cast<double>(s.x_nodes - 1) * s.x_step_m &&
         y >= s.y_origin_m &&
         y <= s.y_origin_m + static_cast<double>(s.y_nodes - 1) * s.y_step_m;
}

// The grid cell containing an in-footprint point plus its fractional
// coordinates. The convention is corner-left: the cell index is
// floor((coord − origin)/step) clamped to [0, nodes − 2], so a point exactly
// on a grid line belongs to the cell on the +x/+y side (fraction 0).
struct GridCell {
  std::size_t i;
  std::size_t j;
  double u;
  double v;
};

GridCell grid_cell(const SurfaceGrid& s, double x, double y) noexcept {
  double fx = (x - s.x_origin_m) / s.x_step_m;
  double fy = (y - s.y_origin_m) / s.y_step_m;
  if (fx < 0.0) {
    fx = 0.0;  // defensive: callers guarantee the footprint
  }
  if (fy < 0.0) {
    fy = 0.0;
  }
  const std::size_t i =
      std::min(static_cast<std::size_t>(std::floor(fx)), s.x_nodes - 2);
  const std::size_t j =
      std::min(static_cast<std::size_t>(std::floor(fy)), s.y_nodes - 2);
  double u = fx - std::floor(fx);
  double v = fy - std::floor(fy);
  if (u < 0.0) {
    u = 0.0;
  }
  if (u > 1.0) {
    u = 1.0;
  }
  if (v < 0.0) {
    v = 0.0;
  }
  if (v > 1.0) {
    v = 1.0;
  }
  return GridCell{i, j, u, v};
}

// Surface height at an in-footprint point (bilinear on its cell).
double surface_height(const SurfaceGrid& s, double x, double y) noexcept {
  const GridCell c = grid_cell(s, x, y);
  const double* z = s.z_tvd + c.j * s.x_nodes + c.i;
  const double top = (1.0 - c.u) * z[0] + c.u * z[1];
  const double bot = (1.0 - c.u) * z[s.x_nodes] + c.u * z[s.x_nodes + 1];
  return (1.0 - c.v) * top + c.v * bot;
}

// Surface height at (x, y) evaluated with an explicit cell — used by the
// per-cell root solving, where the caller guarantees the evaluation point
// lies within that cell (u/v fall out naturally on the cell's bounds).
double surface_height_in_cell(const SurfaceGrid& s, const GridCell& c, double x,
                              double y) noexcept {
  const double u = (x - (s.x_origin_m + static_cast<double>(c.i) * s.x_step_m)) /
                   s.x_step_m;
  const double v = (y - (s.y_origin_m + static_cast<double>(c.j) * s.y_step_m)) /
                   s.y_step_m;
  const double* z = s.z_tvd + c.j * s.x_nodes + c.i;
  const double top = (1.0 - u) * z[0] + u * z[1];
  const double bot = (1.0 - u) * z[s.x_nodes] + u * z[s.x_nodes + 1];
  return (1.0 - v) * top + v * bot;
}

// Interpolated partial derivatives of the surface at an in-footprint point
// (bilinear interpolation of the cell's node differences).
void surface_gradient(const SurfaceGrid& s, double x, double y, double& fx,
                      double& fy) noexcept {
  const GridCell c = grid_cell(s, x, y);
  const double* z = s.z_tvd + c.j * s.x_nodes + c.i;
  const double dzx0 = (z[1] - z[0]) / s.x_step_m;
  const double dzx1 = (z[s.x_nodes + 1] - z[s.x_nodes]) / s.x_step_m;
  const double dzy0 = (z[s.x_nodes] - z[0]) / s.y_step_m;
  const double dzy1 = (z[s.x_nodes + 1] - z[1]) / s.y_step_m;
  fx = (1.0 - c.v) * dzx0 + c.v * dzx1;
  fy = (1.0 - c.u) * dzy0 + c.u * dzy1;
}

// Unit normal to the surface at an in-footprint point; Z component positive
// (downward), consistent with BedNormal3D.
struct SurfaceNormal3D {
  double x;
  double y;
  double z;
};

SurfaceNormal3D surface_normal(const SurfaceGrid& s, double x, double y) noexcept {
  double fx = 0.0;
  double fy = 0.0;
  surface_gradient(s, x, y, fx, fy);
  const double norm = std::sqrt(fx * fx + fy * fy + 1.0);
  return SurfaceNormal3D{-fx / norm, -fy / norm, 1.0 / norm};
}

// Signed distance of the leg [a, b] to the surface at parameter t:
// d(t) = z(t) − f(x(t), y(t)); negative = above the surface (shallower).
double signed_distance(const SurfaceGrid& s, const PathPoint3D& a,
                       const PathPoint3D& b, double t) noexcept {
  const double x = a.x + t * (b.x - a.x);
  const double y = a.y + t * (b.y - a.y);
  const double z = a.z + t * (b.z - a.z);
  return z - surface_height(s, x, y);
}

// Same, but evaluating the surface with an explicit cell (see
// surface_height_in_cell).
double signed_distance_in_cell(const SurfaceGrid& s, const GridCell& c,
                               const PathPoint3D& a, const PathPoint3D& b,
                               double t) noexcept {
  const double x = a.x + t * (b.x - a.x);
  const double y = a.y + t * (b.y - a.y);
  const double z = a.z + t * (b.z - a.z);
  return z - surface_height_in_cell(s, c, x, y);
}

// Roots of d(t) = c0 + c1·t + c2·t² restricted to [t_lo, t_hi]. A double
// root (tangency) is NOT a crossing — the path touches the surface without
// changing side — and is skipped; simple roots are appended to `roots`.
void quadratic_roots(double c0, double c1, double c2, double t_lo, double t_hi,
                     std::vector<double>& roots) noexcept {
  auto in_range = [&](double t) {
    return t >= t_lo - 1e-12 && t <= t_hi + 1e-12;
  };
  const double scale = std::abs(c0) + std::abs(c1) + std::abs(c2) + 1e-300;
  if (std::abs(c2) <= 1e-15 * scale) {  // linear in t
    if (std::abs(c1) > 1e-15 * scale) {
      const double t = -c0 / c1;
      if (in_range(t)) {
        roots.push_back(t);
      }
    }
    return;
  }
  const double disc = c1 * c1 - 4.0 * c2 * c0;
  const double disc_scale = c1 * c1 + std::abs(4.0 * c2 * c0) + 1e-300;
  if (!(disc > 1e-12 * disc_scale)) {
    return;  // no roots, or a double root = tangency, not a crossing
  }
  const double sq = std::sqrt(disc);
  const double t1 = (-c1 - sq) / (2.0 * c2);
  const double t2 = (-c1 + sq) / (2.0 * c2);
  if (in_range(t1)) {
    roots.push_back(t1);
  }
  if (in_range(t2)) {
    roots.push_back(t2);
  }
}

// One candidate crossing of a leg with a surface: parameter t, measured
// depth, and the surface's interpolated normal at the crossing point.
struct SurfaceCrossing {
  double t;
  double md;
  SurfaceNormal3D normal;
};

// All candidate crossings of the leg [a, b] with one surface. The leg's
// footprint is marched cell by cell (every interior grid line the leg
// crosses splits the leg into single-cell intervals); within one cell d(t)
// is a quadratic in t — the bilinear height of a linearly moving point — and
// its roots are fitted exactly from three samples and solved exactly. A root
// exactly on a shared grid line is reported by both adjacent cells and
// deduplicated by the caller.
void leg_surface_crossings(const SurfaceGrid& s, const PathPoint3D& a,
                           const PathPoint3D& b,
                           std::vector<SurfaceCrossing>& out) noexcept {
  const double dx = b.x - a.x;
  const double dy = b.y - a.y;
  const double dmd = b.md - a.md;
  // Cell boundaries along the leg: every interior grid line it crosses.
  std::vector<double> ts{0.0, 1.0};
  if (std::abs(dx) > 0.0) {
    for (std::size_t n = 1; n + 1 < s.x_nodes; ++n) {
      const double line = s.x_origin_m + static_cast<double>(n) * s.x_step_m;
      const double t = (line - a.x) / dx;
      if (t > 0.0 && t < 1.0) {
        ts.push_back(t);
      }
    }
  }
  if (std::abs(dy) > 0.0) {
    for (std::size_t n = 1; n + 1 < s.y_nodes; ++n) {
      const double line = s.y_origin_m + static_cast<double>(n) * s.y_step_m;
      const double t = (line - a.y) / dy;
      if (t > 0.0 && t < 1.0) {
        ts.push_back(t);
      }
    }
  }
  std::sort(ts.begin(), ts.end());
  std::vector<double> roots;
  for (std::size_t k = 0; k + 1 < ts.size(); ++k) {
    const double t_lo = ts[k];
    const double t_hi = ts[k + 1];
    if (!(t_hi > t_lo + 1e-15)) {
      continue;  // degenerate interval (grid-node hit)
    }
    // The interval lies in a single cell; the midpoint is interior, so the
    // cell lookup is unambiguous, and every sample uses that cell's nodes.
    const double m = 0.5 * (t_lo + t_hi);
    const GridCell cell = grid_cell(s, a.x + m * dx, a.y + m * dy);
    const double d_lo = signed_distance_in_cell(s, cell, a, b, t_lo);
    const double d_mid = signed_distance_in_cell(s, cell, a, b, m);
    const double d_hi = signed_distance_in_cell(s, cell, a, b, t_hi);
    // Work in s = (t − m)/h ∈ [−1, 1]: d = A + B·s + C·s², then map the
    // roots back to t. h > 0 here (t_hi > t_lo + 1e-15).
    const double h = 0.5 * (t_hi - t_lo);
    const double A = d_mid;
    const double B = 0.5 * (d_hi - d_lo);
    const double C = 0.5 * (d_lo + d_hi) - d_mid;
    const double mh = m / h;
    const double c0 = A - B * mh + C * mh * mh;
    const double c1 = (B - 2.0 * C * mh) / h;
    const double c2 = C / (h * h);
    roots.clear();
    quadratic_roots(c0, c1, c2, t_lo, t_hi, roots);
    for (const double t : roots) {
      const double x = a.x + t * dx;
      const double y = a.y + t * dy;
      out.push_back(SurfaceCrossing{
          .t = t,
          .md = a.md + t * dmd,
          .normal = surface_normal(s, x, y),
      });
    }
    if (t_hi >= 1.0) {
      break;
    }
  }
}

}  // namespace

Result<TrueStratigraphicThickness>
tst_from_measured_interval(double measured_length_m,
                           WellDirection3D well_direction,
                           BedNormal3D bed) noexcept {
  if (!std::isfinite(measured_length_m) || measured_length_m < 0.0) {
    return invalid_geometry();
  }
  const auto w_norm =
      unit_vector(well_direction.x, well_direction.y, well_direction.z);
  const auto n_norm = unit_vector(bed.x, bed.y, bed.z);
  if (!w_norm || !n_norm) {
    return invalid_geometry();
  }
  // Dot product of the (unit) direction and (unit) normal.
  const double dot = std::abs(
      well_direction.x * bed.x + well_direction.y * bed.y +
      well_direction.z * bed.z);
  return TrueStratigraphicThickness{
      .value = measured_length_m * dot,
      .kind = ThicknessKind::true_stratigraphic_thickness,
      .measured_interval_m = measured_length_m,
      .normal_dot = dot,
  };
}

Result<TrueStratigraphicThickness>
tst_from_interval_points(double top_x, double top_y, double top_z,
                         double bottom_x, double bottom_y, double bottom_z,
                         BedNormal3D bed) noexcept {
  if (!std::isfinite(top_x) || !std::isfinite(top_y) ||
      !std::isfinite(top_z) || !std::isfinite(bottom_x) ||
      !std::isfinite(bottom_y) || !std::isfinite(bottom_z)) {
    return invalid_geometry();
  }
  const auto n_norm = unit_vector(bed.x, bed.y, bed.z);
  if (!n_norm) {
    return invalid_geometry();
  }
  const double dx = bottom_x - top_x;
  const double dy = bottom_y - top_y;
  const double dz = bottom_z - top_z;
  const double length = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (!(length > 0.0)) {
    return invalid_geometry();  // zero-length interval
  }
  const double dot = std::abs(dx * bed.x + dy * bed.y + dz * bed.z);
  return TrueStratigraphicThickness{
      .value = dot,
      .kind = ThicknessKind::true_stratigraphic_thickness,
      .measured_interval_m = length,
      .normal_dot = dot / length,
  };
}

Result<TrueStratigraphicThickness>
tst_through_layers(double start_md, double measured_length_m,
                   WellDirection3D well_direction,
                   const BeddingLayer* layers,
                   std::size_t layer_count) noexcept {
  if (!std::isfinite(start_md) || !std::isfinite(measured_length_m) ||
      measured_length_m < 0.0) {
    return invalid_geometry();
  }
  if (!unit_vector(well_direction.x, well_direction.y, well_direction.z)) {
    return invalid_geometry();
  }
  if (!valid_layers(layers, layer_count)) {
    return invalid_geometry();
  }
  const double total = accumulate_layers(
      start_md, start_md + measured_length_m, well_direction, layers,
      layer_count);
  return TrueStratigraphicThickness{
      .value = total,
      .kind = ThicknessKind::true_stratigraphic_thickness,
      .measured_interval_m = measured_length_m,
      // Effective projection factor over the whole interval (0 for a
      // zero-length interval).
      .normal_dot =
          measured_length_m > 0.0 ? total / measured_length_m : 0.0,
  };
}

Result<TrueStratigraphicThickness>
tst_along_path(const PathPoint3D* path, std::size_t path_count,
               const BeddingLayer* layers, std::size_t layer_count) noexcept {
  if (path_count < 2 || path == nullptr) {
    return invalid_geometry();
  }
  if (!valid_layers(layers, layer_count)) {
    return invalid_geometry();
  }
  double total = 0.0;
  double total_md = 0.0;
  for (std::size_t i = 1; i < path_count; ++i) {
    const PathPoint3D& a = path[i - 1];
    const PathPoint3D& b = path[i];
    if (!std::isfinite(a.md) || !std::isfinite(a.x) || !std::isfinite(a.y) ||
        !std::isfinite(a.z) || !std::isfinite(b.md) || !std::isfinite(b.x) ||
        !std::isfinite(b.y) || !std::isfinite(b.z)) {
      return invalid_geometry();
    }
    if (!(b.md > a.md)) {
      return invalid_geometry();  // md must increase strictly
    }
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double dz = b.z - a.z;
    const double leg_length = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!(leg_length > 0.0)) {
      return invalid_geometry();  // zero-length leg: inconsistent path
    }
    const WellDirection3D leg{
        .x = dx / leg_length, .y = dy / leg_length, .z = dz / leg_length,
    };
    total += accumulate_layers(a.md, b.md, leg, layers, layer_count);
    total_md += b.md - a.md;
  }
  return TrueStratigraphicThickness{
      .value = total,
      .kind = ThicknessKind::true_stratigraphic_thickness,
      .measured_interval_m = total_md,
      // total_md > 0 is guaranteed by the strictly increasing md rule.
      .normal_dot = total / total_md,
  };
}

namespace {

// In-unit test at a path position: strictly below the top surface and
// strictly above the bottom surface of the unit.
bool unit_status(const SurfaceGrid& top, const SurfaceGrid& bottom, double x,
                 double y, double z) noexcept {
  return z - surface_height(top, x, y) > 0.0 &&
         z - surface_height(bottom, x, y) < 0.0;
}

// Position on the polyline path at a measured depth. Legs have strictly
// increasing md; md lies within [path[0].md, path[count−1].md] and every
// coordinate is linear in md along a leg.
void position_at_md(const PathPoint3D* path, std::size_t path_count,
                    double md, double& x, double& y, double& z) noexcept {
  std::size_t k = 1;
  while (k + 1 < path_count && path[k].md < md) {
    ++k;
  }
  const PathPoint3D& a = path[k - 1];
  const PathPoint3D& b = path[k];
  const double t = (md - a.md) / (b.md - a.md);
  x = a.x + t * (b.x - a.x);
  y = a.y + t * (b.y - a.y);
  z = a.z + t * (b.z - a.z);
}

// TST contribution of the path sub-interval [a_md, b_md] inside a unit: per
// leg, the covered part contributes ``length · |ŵ · n̂|`` with n̂ the
// normalized average of the two surfaces' interpolated unit normals at the
// covered part's midpoint (the documented local-parallel approximation).
double accumulate_unit_interval(const PathPoint3D* path,
                                std::size_t path_count, const SurfaceGrid& top,
                                const SurfaceGrid& bottom, double a_md,
                                double b_md) noexcept {
  double total = 0.0;
  for (std::size_t k = 1; k < path_count; ++k) {
    const PathPoint3D& pa = path[k - 1];
    const PathPoint3D& pb = path[k];
    const double lo = std::max(a_md, pa.md);
    const double hi = std::min(b_md, pb.md);
    if (!(hi > lo)) {
      continue;
    }
    const double dx = pb.x - pa.x;
    const double dy = pb.y - pa.y;
    const double dz = pb.z - pa.z;
    const double leg_length = std::sqrt(dx * dx + dy * dy + dz * dz);
    // Position and 3D length are both linear in md along the leg.
    const double sub_length = leg_length * (hi - lo) / (pb.md - pa.md);
    const double t = ((lo + hi) * 0.5 - pa.md) / (pb.md - pa.md);
    const double mx = pa.x + t * dx;
    const double my = pa.y + t * dy;
    const SurfaceNormal3D n0 = surface_normal(top, mx, my);
    const SurfaceNormal3D n1 = surface_normal(bottom, mx, my);
    const double nx = n0.x + n1.x;
    const double ny = n0.y + n1.y;
    const double nz = n0.z + n1.z;  // both z components > 0 → norm > 0
    const double nnorm = std::sqrt(nx * nx + ny * ny + nz * nz);
    const double dot =
        std::abs(dx * nx + dy * ny + dz * nz) / (leg_length * nnorm);
    total += sub_length * dot;
  }
  return total;
}

// One candidate crossing event along the path, per unit.
struct UnitEvent {
  double md;
  std::size_t surface_index;  // 0 = top (unit), 1 = bottom (unit + 1)
  SurfaceNormal3D normal;     // crossed surface's normal at the crossing
};

}  // namespace

Result<TrueStratigraphicThickness>
tst_along_surface_path(const PathPoint3D* path, std::size_t path_count,
                       const SurfaceGrid* surfaces,
                       std::size_t surface_count) noexcept {
  if (path_count < 2 || path == nullptr) {
    return invalid_geometry();
  }
  if (surface_count > 0 && surfaces == nullptr) {
    return invalid_geometry();
  }
  for (std::size_t u = 0; u < surface_count; ++u) {
    if (!valid_surface_grid(surfaces[u])) {
      return invalid_geometry();
    }
  }
  double total_md = 0.0;
  for (std::size_t k = 1; k < path_count; ++k) {
    const PathPoint3D& a = path[k - 1];
    const PathPoint3D& b = path[k];
    if (!std::isfinite(a.md) || !std::isfinite(a.x) || !std::isfinite(a.y) ||
        !std::isfinite(a.z) || !std::isfinite(b.md) || !std::isfinite(b.x) ||
        !std::isfinite(b.y) || !std::isfinite(b.z)) {
      return invalid_geometry();
    }
    if (!(b.md > a.md)) {
      return invalid_geometry();  // md must increase strictly
    }
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double dz = b.z - a.z;
    const double leg_length = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!(leg_length > 0.0)) {
      return invalid_geometry();  // zero-length leg: inconsistent path
    }
    total_md += b.md - a.md;
    // Footprint coverage: every path point must lie inside every surface
    // (an excursion is an input error, never a silent out-of-range default).
    for (std::size_t u = 0; u < surface_count; ++u) {
      if (!in_footprint(surfaces[u], a.x, a.y) ||
          !in_footprint(surfaces[u], b.x, b.y)) {
        return invalid_geometry();
      }
    }
  }
  if (surface_count < 2) {
    // No complete unit: a legal zero result (like an empty layer book).
    return TrueStratigraphicThickness{
        .value = 0.0,
        .kind = ThicknessKind::true_stratigraphic_thickness,
        .measured_interval_m = total_md,
        .normal_dot = 0.0,
    };
  }
  double total = 0.0;
  for (std::size_t u = 0; u + 1 < surface_count; ++u) {
    const SurfaceGrid& top = surfaces[u];
    const SurfaceGrid& bottom = surfaces[u + 1];
    // Unit ordering: bottom strictly below top at every path point (an
    // overlap/crossing along the well is an input error, never a silent
    // merge).
    for (std::size_t k = 0; k < path_count; ++k) {
      if (!(surface_height(top, path[k].x, path[k].y) <
            surface_height(bottom, path[k].x, path[k].y))) {
        return invalid_geometry();
      }
    }
    // Candidate crossings of both surfaces along every leg.
    std::vector<UnitEvent> events;
    for (std::size_t k = 1; k < path_count; ++k) {
      const PathPoint3D& a = path[k - 1];
      const PathPoint3D& b = path[k];
      const double dx = b.x - a.x;
      const double dy = b.y - a.y;
      const double dz = b.z - a.z;
      const double leg_length = std::sqrt(dx * dx + dy * dy + dz * dz);
      const double wx = dx / leg_length;
      const double wy = dy / leg_length;
      const double wz = dz / leg_length;
      const SurfaceGrid* pair[2] = {&top, &bottom};
      for (std::size_t which = 0; which < 2; ++which) {
        // A leg lying in a surface — both ends on it and the direction in
        // the tangent plane — is coincident: an input error.
        const double d_a = signed_distance(*pair[which], a, b, 0.0);
        const double d_b = signed_distance(*pair[which], a, b, 1.0);
        if (std::abs(d_a) < 1e-9 && std::abs(d_b) < 1e-9) {
          const SurfaceNormal3D n =
              surface_normal(*pair[which], a.x, a.y);
          const double dot = std::abs(wx * n.x + wy * n.y + wz * n.z);
          if (dot < 1e-9) {
            return invalid_geometry();  // coincident path segment
          }
        }
        std::vector<SurfaceCrossing> crossings;
        leg_surface_crossings(*pair[which], a, b, crossings);
        for (const SurfaceCrossing& c : crossings) {
          events.push_back(UnitEvent{
              .md = c.md, .surface_index = which, .normal = c.normal,
          });
        }
      }
    }
    std::sort(events.begin(), events.end(),
              [](const UnitEvent& x, const UnitEvent& y) {
                if (x.md != y.md) {
                  return x.md < y.md;
                }
                return x.surface_index < y.surface_index;
              });
    // Deduplicate: the same (surface, md) may be reported by two adjacent
    // legs or cells at a shared boundary (path node / grid line).
    std::vector<UnitEvent> candidates;
    for (const UnitEvent& e : events) {
      if (!candidates.empty()) {
        const UnitEvent& last = candidates.back();
        if (last.surface_index == e.surface_index &&
            std::abs(last.md - e.md) <= 1e-9 * (1.0 + std::abs(e.md))) {
          continue;
        }
      }
      candidates.push_back(e);
    }
    // A candidate is a genuine crossing iff the in-unit status differs
    // across it; tangencies (double roots, grazing a grid node) keep the
    // status unchanged and are skipped.
    const double start_md = path[0].md;
    const double end_md = path[path_count - 1].md;
    std::vector<UnitEvent> genuine;
    double prev_md = start_md;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
      const UnitEvent& e = candidates[i];
      // Side validation runs for every candidate: the other surface must be
      // on the correct side of the crossing point. A crossing with the
      // surfaces inverted there is exactly one that does NOT change the
      // in-unit status (the unit region is empty on one side) — never a
      // silent zero-contribution merge.
      double cx, cy, cz;
      position_at_md(path, path_count, e.md, cx, cy, cz);
      if (e.surface_index == 0) {
        if (!(cz - surface_height(bottom, cx, cy) < 0.0)) {
          return invalid_geometry();
        }
      } else if (!(cz - surface_height(top, cx, cy) > 0.0)) {
        return invalid_geometry();
      }
      const double next_md =
          i + 1 < candidates.size() ? candidates[i + 1].md : end_md;
      double bx, by, bz, ax, ay, az;
      position_at_md(path, path_count, 0.5 * (prev_md + e.md), bx, by, bz);
      position_at_md(path, path_count, 0.5 * (e.md + next_md), ax, ay, az);
      const bool before = unit_status(top, bottom, bx, by, bz);
      const bool after = unit_status(top, bottom, ax, ay, az);
      if (before != after) {
        genuine.push_back(e);
      }
      prev_md = e.md;
    }
    // Accumulate the in-unit components: the status flips at every genuine
    // crossing.
    bool inside =
        unit_status(top, bottom, path[0].x, path[0].y, path[0].z);
    double boundary = start_md;
    for (const UnitEvent& e : genuine) {
      if (inside) {
        total += accumulate_unit_interval(path, path_count, top, bottom,
                                          boundary, e.md);
      }
      inside = !inside;
      boundary = e.md;
    }
    if (inside) {
      total += accumulate_unit_interval(path, path_count, top, bottom,
                                        boundary, end_md);
    }
  }
  return TrueStratigraphicThickness{
      .value = total,
      .kind = ThicknessKind::true_stratigraphic_thickness,
      .measured_interval_m = total_md,
      .normal_dot = total_md > 0.0 ? total / total_md : 0.0,
  };
}

}  // namespace welllog
