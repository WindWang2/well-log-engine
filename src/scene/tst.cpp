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

}  // namespace welllog
