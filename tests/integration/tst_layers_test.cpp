// Piecewise-planar bedding TST (Epic D, slice 2 / D3): analytical fixtures.
//
// The planar/local model (tst_test.cpp) is extended with piecewise bedding:
//   * tst_through_layers — a measured interval along a straight well leg
//     through layers with individually constant bedding normals;
//   * tst_along_path — a polyline path whose legs cross the layers
//     (surface intersection), each leg with its own chord direction.
//
// Cases: varying orientation across a boundary; interval spanning a layer
// boundary (surface intersection); near-parallel well (tiny but non-zero
// TST); zero-TST layer contribution (legal); extreme dip inside one layer;
// missing orientation (input error); invalid layer books (overlap / inverted
// / zero-extent / non-finite / out-of-order); polyline exact geometry; path
// validation; empty layer books (legal, zero contribution); degenerate
// reduction to the single-plane form.

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

constexpr double kPi = 3.141592653589793;
constexpr double kSqrt2Over2 = 0.7071067811865476;

// Dip δ measured from horizontal: normal = (sin δ, 0, cos δ).
BedNormal3D dipping_bed(double dip_deg) {
  const double d = dip_deg * kPi / 180.0;
  return BedNormal3D{.x = std::sin(d), .y = 0.0, .z = std::cos(d)};
}

const BedNormal3D horizontal_bed{0.0, 0.0, 1.0};
const WellDirection3D vertical{0.0, 0.0, 1.0};

// Vertical well through two layers: 0-50 m horizontal, 50-100 m dipping 45°.
const BeddingLayer two_layers[] = {
    BeddingLayer{.top_md = 0.0, .bottom_md = 50.0, .normal = horizontal_bed},
    BeddingLayer{.top_md = 50.0, .bottom_md = 100.0, .normal = dipping_bed(45.0)},
};

void varying_orientation_across_boundary() {
  // TST = 50·|w·n1| + 50·|w·n2| = 50 + 50·cos(45°).
  const auto tst = tst_through_layers(0.0, 100.0, vertical, two_layers, 2);
  require(tst.has_value(), "two-layer bedding must compute");
  const double expected = 50.0 + 50.0 * kSqrt2Over2;
  require_near(tst.value().value, expected, 1e-9,
               "TST sums the per-layer projections");
  require_near(tst.value().normal_dot, expected / 100.0, 1e-9,
               "effective projection factor = TST / measured interval");
  require_near(tst.value().measured_interval_m, 100.0, 1e-9,
               "the measured interval must be reported");
}

void interval_spanning_boundary_surface_intersection() {
  // Interval 25-75 m crosses the layer boundary at 50 m: layer 1 contributes
  // 25·1, layer 2 contributes 25·cos(45°).
  const auto tst = tst_through_layers(25.0, 50.0, vertical, two_layers, 2);
  require(tst.has_value(), "boundary-spanning interval must compute");
  const double expected = 25.0 + 25.0 * kSqrt2Over2;
  require_near(tst.value().value, expected, 1e-9,
               "the interval is split at the surface intersection");
}

void near_parallel_well() {
  // Well nearly parallel to a 45°-dipping bed: w = (cos 45.1°, 0, -sin 45.1°)
  // is 0.1° off the bedding plane → |w·n| = sin(0.1°): tiny but non-zero.
  const BeddingLayer layer[] = {
      BeddingLayer{.top_md = 0.0, .bottom_md = 100.0, .normal = dipping_bed(45.0)},
  };
  const double a = (45.0 + 0.1) * kPi / 180.0;
  const WellDirection3D w{.x = std::cos(a), .y = 0.0, .z = -std::sin(a)};
  const auto tst = tst_through_layers(0.0, 100.0, w, layer, 1);
  require(tst.has_value(), "near-parallel must compute");
  const double expected = 100.0 * std::sin(0.1 * kPi / 180.0);
  require_near(tst.value().value, expected, 1e-9,
               "near-parallel yields a tiny but exact projection");
  require(tst.value().value > 0.0, "near-parallel TST stays positive");
}

void zero_tst_layer_contribution_is_legal() {
  // Layer 2 (40-100 m, dip 45°) is crossed by a well exactly parallel to its
  // bedding (w = (cos45, 0, -sin45)): its contribution is zero — a VALID
  // result, not an error. Layer 1 (horizontal) contributes 40·|w·n1|.
  const BeddingLayer layers[] = {
      BeddingLayer{.top_md = 0.0, .bottom_md = 40.0, .normal = horizontal_bed},
      BeddingLayer{.top_md = 40.0, .bottom_md = 100.0, .normal = dipping_bed(45.0)},
  };
  const WellDirection3D w{
      .x = kSqrt2Over2, .y = 0.0, .z = -kSqrt2Over2,
  };
  const auto tst = tst_through_layers(0.0, 100.0, w, layers, 2);
  require(tst.has_value(), "parallel layer must not be an error");
  require_near(tst.value().value, 40.0 * kSqrt2Over2, 1e-9,
               "only the horizontal layer contributes");
}

void extreme_dip_inside_one_layer() {
  // Layer 1 dips 89°, layer 2 is horizontal; vertical well.
  const BeddingLayer layers[] = {
      BeddingLayer{.top_md = 0.0, .bottom_md = 50.0, .normal = dipping_bed(89.0)},
      BeddingLayer{.top_md = 50.0, .bottom_md = 100.0, .normal = horizontal_bed},
  };
  const auto tst = tst_through_layers(0.0, 100.0, vertical, layers, 2);
  require(tst.has_value(), "extreme dip must compute");
  const double expected = 50.0 * std::cos(89.0 * kPi / 180.0) + 50.0;
  require_near(tst.value().value, expected, 1e-9,
               "per-layer extreme dip stays exact");
}

void missing_orientation_is_an_input_error() {
  // A layer with a zero bedding normal (no orientation declared) is an input
  // error — never a silent default, matching the planar model.
  const BeddingLayer bad[] = {
      BeddingLayer{.top_md = 0.0, .bottom_md = 50.0,
                   .normal = BedNormal3D{.x = 0.0, .y = 0.0, .z = 0.0}},
  };
  require(!tst_through_layers(0.0, 50.0, vertical, bad, 1).has_value(),
          "a zero bedding normal in a layer must be rejected");
  const BeddingLayer non_unit[] = {
      BeddingLayer{.top_md = 0.0, .bottom_md = 50.0,
                   .normal = BedNormal3D{.x = 0.0, .y = 0.0, .z = 2.0}},
  };
  require(!tst_through_layers(0.0, 50.0, vertical, non_unit, 1).has_value(),
          "a non-unit bedding normal in a layer must be rejected");
}

void invalid_layer_books_rejected() {
  // Overlapping layers.
  const BeddingLayer overlap[] = {
      BeddingLayer{.top_md = 0.0, .bottom_md = 60.0, .normal = horizontal_bed},
      BeddingLayer{.top_md = 50.0, .bottom_md = 100.0, .normal = horizontal_bed},
  };
  require(!tst_through_layers(0.0, 100.0, vertical, overlap, 2).has_value(),
          "overlapping layers must be rejected");
  // Inverted layer (bottom < top).
  const BeddingLayer inverted[] = {
      BeddingLayer{.top_md = 60.0, .bottom_md = 10.0, .normal = horizontal_bed},
  };
  require(!tst_through_layers(0.0, 100.0, vertical, inverted, 1).has_value(),
          "an inverted layer must be rejected");
  // Zero-extent layer (bottom == top).
  const BeddingLayer degenerate[] = {
      BeddingLayer{.top_md = 50.0, .bottom_md = 50.0, .normal = horizontal_bed},
  };
  require(!tst_through_layers(0.0, 100.0, vertical, degenerate, 1).has_value(),
          "a zero-extent layer must be rejected");
  // Out-of-order book.
  const BeddingLayer unordered[] = {
      BeddingLayer{.top_md = 60.0, .bottom_md = 100.0, .normal = horizontal_bed},
      BeddingLayer{.top_md = 0.0, .bottom_md = 50.0, .normal = horizontal_bed},
  };
  require(!tst_through_layers(0.0, 100.0, vertical, unordered, 2).has_value(),
          "an out-of-order layer book must be rejected");
  // Non-finite bounds.
  const BeddingLayer nan_layer[] = {
      BeddingLayer{.top_md = 0.0,
                   .bottom_md = std::numeric_limits<double>::quiet_NaN(),
                   .normal = horizontal_bed},
  };
  require(!tst_through_layers(0.0, 100.0, vertical, nan_layer, 1).has_value(),
          "a non-finite layer bound must be rejected");
  // Null pointer with a non-zero count.
  require(!tst_through_layers(0.0, 100.0, vertical, nullptr, 1).has_value(),
          "null layers with a non-zero count must be rejected");
  // Non-unit well direction still rejected in the layered form.
  require(!tst_through_layers(
              0.0, 100.0, WellDirection3D{.x = 0.0, .y = 0.0, .z = 2.0},
              two_layers, 2)
              .has_value(),
          "a non-unit well direction must be rejected");
}

void empty_layer_book_is_legal_zero() {
  // No declared bedding → no contribution; explicit, not an error.
  const auto tst = tst_through_layers(0.0, 100.0, vertical, nullptr, 0);
  require(tst.has_value(), "an empty layer book must compute");
  require_near(tst.value().value, 0.0, 0.0,
               "no declared bedding contributes zero TST");
  require_near(tst.value().measured_interval_m, 100.0, 0.0,
               "the measured interval is still reported");
  // Zero-length interval through layers: zero TST, legal.
  const auto zero_len = tst_through_layers(10.0, 0.0, vertical, two_layers, 2);
  require(zero_len.has_value(), "a zero-length interval must compute to zero");
  require_near(zero_len.value().value, 0.0, 0.0,
               "zero-length interval yields zero TST");
}

void degenerate_reduces_to_planar_form() {
  // A single horizontal layer must reproduce tst_from_measured_interval.
  const BeddingLayer single[] = {
      BeddingLayer{.top_md = -1000.0, .bottom_md = 1000.0, .normal = horizontal_bed},
  };
  const auto layered =
      tst_through_layers(0.0, 100.0, vertical, single, 1);
  const auto planar = tst_from_measured_interval(100.0, vertical, horizontal_bed);
  require(layered.has_value() && planar.has_value(),
          "both forms must compute");
  require_near(layered.value().value, planar.value().value, 1e-12,
               "single-layer form degenerates to the planar form");
}

// ---------------------------------------------------------------------------
// Polyline path (surface intersection along a deviated path)
// ---------------------------------------------------------------------------

void polyline_path_exact_geometry() {
  // Leg 1: vertical, md 0->50, p (0,0,0) -> (0,0,50). Leg 2: 45° along X,
  // md 50->100, p (0,0,50) -> (25√2, 0, 50+25√2).
  // Layers: 0-60 horizontal, 60-100 dipping 45°.
  const BeddingLayer layers[] = {
      BeddingLayer{.top_md = 0.0, .bottom_md = 60.0, .normal = horizontal_bed},
      BeddingLayer{.top_md = 60.0, .bottom_md = 100.0, .normal = dipping_bed(45.0)},
  };
  const double s = 50.0 * kSqrt2Over2;
  const PathPoint3D path[] = {
      PathPoint3D{.md = 0.0, .x = 0.0, .y = 0.0, .z = 0.0},
      PathPoint3D{.md = 50.0, .x = 0.0, .y = 0.0, .z = 50.0},
      PathPoint3D{.md = 100.0, .x = s, .y = 0.0, .z = 50.0 + s},
  };
  const auto tst = tst_along_path(path, 3, layers, 2);
  require(tst.has_value(), "polyline path must compute");
  // Leg 1: 50 m vertical through the horizontal layer → 50·1.
  // Leg 2: 10 m (50-60) horizontal-layer with |w·n1| = cos45 → 10·√2/2;
  //        40 m (60-100) in the 45° bed with |w·n2| = sin45·sin45 +
  //        cos45·cos45 = 1 → 40·1.
  const double expected = 50.0 + 10.0 * kSqrt2Over2 + 40.0;
  require_near(tst.value().value, expected, 1e-9,
               "legs cross the surfaces exactly");
  require_near(tst.value().measured_interval_m, 100.0, 1e-9,
               "total measured depth is the md span");
}

void polyline_path_bedding_gap_contributes_nothing() {
  // Layers cover 60-100 m only; the whole path sits above → zero TST.
  const BeddingLayer layers[] = {
      BeddingLayer{.top_md = 60.0, .bottom_md = 100.0, .normal = horizontal_bed},
  };
  const PathPoint3D path[] = {
      PathPoint3D{.md = 0.0, .x = 0.0, .y = 0.0, .z = 0.0},
      PathPoint3D{.md = 50.0, .x = 0.0, .y = 0.0, .z = 50.0},
  };
  const auto tst = tst_along_path(path, 2, layers, 1);
  require(tst.has_value(), "path without declared bedding must compute");
  require_near(tst.value().value, 0.0, 0.0,
               "undeclared bedding contributes zero");
}

void polyline_path_validation() {
  const BeddingLayer layers[] = {
      BeddingLayer{.top_md = 0.0, .bottom_md = 100.0, .normal = horizontal_bed},
  };
  const PathPoint3D ok[] = {
      PathPoint3D{.md = 0.0, .x = 0.0, .y = 0.0, .z = 0.0},
      PathPoint3D{.md = 50.0, .x = 0.0, .y = 0.0, .z = 50.0},
  };
  // Fewer than two points.
  require(!tst_along_path(ok, 1, layers, 1).has_value(),
          "a single-point path must be rejected");
  require(!tst_along_path(ok, 0, layers, 1).has_value(),
          "an empty path must be rejected");
  // Null path.
  require(!tst_along_path(nullptr, 2, layers, 1).has_value(),
          "a null path must be rejected");
  // Non-increasing md.
  const PathPoint3D flat_md[] = {
      PathPoint3D{.md = 50.0, .x = 0.0, .y = 0.0, .z = 0.0},
      PathPoint3D{.md = 50.0, .x = 0.0, .y = 0.0, .z = 50.0},
  };
  require(!tst_along_path(flat_md, 2, layers, 1).has_value(),
          "non-increasing md must be rejected");
  // Zero-length leg with increasing md (inconsistent path).
  const PathPoint3D stuck[] = {
      PathPoint3D{.md = 0.0, .x = 1.0, .y = 2.0, .z = 3.0},
      PathPoint3D{.md = 50.0, .x = 1.0, .y = 2.0, .z = 3.0},
  };
  require(!tst_along_path(stuck, 2, layers, 1).has_value(),
          "a zero-length leg must be rejected");
  // Non-finite point.
  const PathPoint3D nan_pt[] = {
      PathPoint3D{.md = 0.0, .x = 0.0, .y = 0.0, .z = 0.0},
      PathPoint3D{.md = 50.0, .x = 0.0, .y = 0.0,
                  .z = std::numeric_limits<double>::quiet_NaN()},
  };
  require(!tst_along_path(nan_pt, 2, layers, 1).has_value(),
          "non-finite path points must be rejected");
  // Invalid layer book propagates to the path form.
  const BeddingLayer overlap[] = {
      BeddingLayer{.top_md = 0.0, .bottom_md = 60.0, .normal = horizontal_bed},
      BeddingLayer{.top_md = 50.0, .bottom_md = 100.0, .normal = horizontal_bed},
  };
  require(!tst_along_path(ok, 2, overlap, 2).has_value(),
          "invalid layers must be rejected in the path form");
}

}  // namespace

int main() {
  varying_orientation_across_boundary();
  interval_spanning_boundary_surface_intersection();
  near_parallel_well();
  zero_tst_layer_contribution_is_legal();
  extreme_dip_inside_one_layer();
  missing_orientation_is_an_input_error();
  invalid_layer_books_rejected();
  empty_layer_book_is_legal_zero();
  degenerate_reduces_to_planar_form();
  polyline_path_exact_geometry();
  polyline_path_bedding_gap_contributes_nothing();
  polyline_path_validation();
  std::cout << "PASS: piecewise-planar true stratigraphic thickness\n";
  return EXIT_SUCCESS;
}
