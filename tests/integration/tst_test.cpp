// True Stratigraphic Thickness — analytical geometry fixtures (Epic D, D3).
//
// Cases: horizontal bed + vertical well; dipping bed + vertical well;
// horizontal bed + deviated well; dipping bed + deviated well; well parallel
// to bedding (zero TST — valid); interval points form; reversed interval;
// non-finite; extreme dip; missing/insufficient orientation (non-unit).

#include <welllog/scene/tst.hpp>

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

constexpr double kSqrt2Over2 = 0.7071067811865476;

// Vertical well (down = +Z), horizontal bedding.
const WellDirection3D vertical{0.0, 0.0, 1.0};
const BedNormal3D horizontal_bed{0.0, 0.0, 1.0};

// Dip δ measured from horizontal: normal = (sin δ, 0, cos δ).
BedNormal3D dipping_bed(double dip_deg) {
  const double d = dip_deg * 3.141592653589793 / 180.0;
  return BedNormal3D{.x = std::sin(d), .y = 0.0, .z = std::cos(d)};
}

// Deviated well: inclination from vertical along X.
WellDirection3D deviated_well(double inc_deg) {
  const double i = inc_deg * 3.141592653589793 / 180.0;
  return WellDirection3D{.x = std::sin(i), .y = 0.0, .z = std::cos(i)};
}

void horizontal_bed_vertical_well() {
  const auto tst = tst_from_measured_interval(100.0, vertical, horizontal_bed);
  require(tst.has_value(), "vertical well + horizontal bed must compute");
  require_near(tst.value().value, 100.0, 1e-9,
               "TST equals the measured interval (degenerate case)");
  require(tst.value().kind == ThicknessKind::true_stratigraphic_thickness,
          "the result kind must be explicit");
  require_near(tst.value().normal_dot, 1.0, 1e-9,
               "projection factor must be 1");
}

void dipping_bed_vertical_well() {
  // Classic apparent-thickness correction: TST = L·cos(δ).
  const auto tst = tst_from_measured_interval(100.0, vertical, dipping_bed(45.0));
  require(tst.has_value(), "dipping bed + vertical well must compute");
  require_near(tst.value().value, 100.0 * kSqrt2Over2, 1e-9,
               "TST = L·cos(45°) for a vertical well");
}

void horizontal_bed_deviated_well() {
  // A 45° deviated well through horizontal bedding: the measured interval is
  // longer than the true thickness by 1/cos(45°).
  const auto tst =
      tst_from_measured_interval(100.0, deviated_well(45.0), horizontal_bed);
  require(tst.has_value(), "deviated well + horizontal bed must compute");
  require_near(tst.value().value, 100.0 * kSqrt2Over2, 1e-9,
               "TST = L·cos(45°) for a deviated well");
}

void dipping_bed_deviated_well() {
  // Well deviated 30° from vertical; bed dips 20°. TST = L·cos(30−20) when
  // both tilt in the same plane and the well up-dips... the dot product
  // handles it exactly: n=(sin20,0,cos20), w=(sin30,0,cos30),
  // w·n = sin30·sin20 + cos30·cos20 = cos(30−20) = cos(10°).
  const auto tst = tst_from_measured_interval(
      100.0, deviated_well(30.0), dipping_bed(20.0));
  require(tst.has_value(), "dipping bed + deviated well must compute");
  const double expected = 100.0 * std::cos(10.0 * 3.141592653589793 / 180.0);
  require_near(tst.value().value, expected, 1e-9,
               "TST = L·cos(θ) with θ the angle between well and normal");
}

void well_parallel_to_bedding_zero_tst() {
  // The well runs ALONG the bedding plane (down-dip at 45°): its direction
  // is perpendicular to the bedding normal → zero stratigraphic thickness
  // is a VALID result (near-parallel intersection), not an error.
  const auto tst = tst_from_measured_interval(
      100.0, WellDirection3D{.x = kSqrt2Over2, .y = 0.0, .z = -kSqrt2Over2},
      dipping_bed(45.0));
  require(tst.has_value(), "parallel intersection must compute");
  require_near(tst.value().value, 0.0, 1e-9,
               "TST must be zero when the well is parallel to bedding");
}

void interval_points_form() {
  // Two points 50 m apart down a 45° deviated well (Δp = (35.36, 0, 35.36))
  // through horizontal bedding: TST = |Δp·n| = 35.36.
  const double s = 50.0 * kSqrt2Over2;
  const auto tst = tst_from_interval_points(0.0, 0.0, 0.0, s, 0.0, s,
                                            horizontal_bed);
  require(tst.has_value(), "interval-points form must compute");
  require_near(tst.value().value, s, 1e-9,
               "TST = projection of the segment onto the bedding normal");
  require_near(tst.value().measured_interval_m, 50.0, 1e-9,
               "the measured segment length must be reported");
}

void extreme_dip() {
  const auto tst = tst_from_measured_interval(100.0, vertical, dipping_bed(89.0));
  require(tst.has_value(), "extreme dip must compute");
  require_near(tst.value().value, 100.0 * std::cos(89.0 * 3.141592653589793 / 180.0),
               1e-9, "TST = L·cos(89°) stays exact at extreme dip");
}

void invalid_inputs_rejected() {
  // Reversed (negative) interval.
  require(!tst_from_measured_interval(-5.0, vertical, horizontal_bed).has_value(),
          "a reversed (negative) interval must be rejected");
  // Non-finite length.
  require(!tst_from_measured_interval(
              std::numeric_limits<double>::infinity(), vertical, horizontal_bed)
              .has_value(),
          "non-finite length must be rejected");
  // Non-unit well direction (missing/insufficient orientation).
  require(!tst_from_measured_interval(
              100.0, WellDirection3D{.x = 0.0, .y = 0.0, .z = 2.0},
              horizontal_bed)
              .has_value(),
          "a non-unit well direction must be rejected");
  // Zero bedding normal (missing orientation) — never a silent default.
  require(!tst_from_measured_interval(
              100.0, vertical, BedNormal3D{.x = 0.0, .y = 0.0, .z = 0.0})
              .has_value(),
          "a zero bedding normal must be rejected");
  // Non-finite point.
  require(!tst_from_interval_points(0.0, 0.0, 0.0,
                                    std::numeric_limits<double>::quiet_NaN(),
                                    0.0, 1.0, horizontal_bed)
              .has_value(),
          "non-finite points must be rejected");
  // Zero-length segment.
  require(!tst_from_interval_points(1.0, 2.0, 3.0, 1.0, 2.0, 3.0,
                                    horizontal_bed)
              .has_value(),
          "a zero-length interval must be rejected");
}

void kind_names_are_explicit() {
  // D1: the four kinds are distinct values; the API never returns a bare
  // "thickness".
  require(static_cast<int>(ThicknessKind::measured_depth_interval) !=
              static_cast<int>(ThicknessKind::true_stratigraphic_thickness),
          "measured-depth interval must be distinct from TST");
  require(static_cast<int>(ThicknessKind::tvd_interval) !=
              static_cast<int>(ThicknessKind::true_stratigraphic_thickness),
          "TVD interval must be distinct from TST");
  require(static_cast<int>(ThicknessKind::apparent_thickness) !=
              static_cast<int>(ThicknessKind::true_stratigraphic_thickness),
          "apparent thickness must be distinct from TST");
}

}  // namespace

int main() {
  horizontal_bed_vertical_well();
  dipping_bed_vertical_well();
  horizontal_bed_deviated_well();
  dipping_bed_deviated_well();
  well_parallel_to_bedding_zero_tst();
  interval_points_form();
  extreme_dip();
  invalid_inputs_rejected();
  kind_names_are_explicit();
  std::cout << "PASS: true stratigraphic thickness\n";
  return EXIT_SUCCESS;
}
