// True Stratigraphic Thickness — planar/local model (Epic D); see tst.hpp.

#include <welllog/scene/tst.hpp>

#include <cmath>
#include <limits>
#include <optional>

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

}  // namespace welllog
