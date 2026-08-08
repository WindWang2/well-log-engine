// Surface-based bedding TST (Epic D high-order extension): analytical
// fixtures.
//
// The piecewise-planar model (tst_layers_test.cpp) is extended from declared
// MD intervals with constant normals to bedding surfaces sampled on regular
// (x, y) grids: ``tst_along_surface_path`` computes the path's crossings
// with each surface geometrically (per-cell quadratic roots) and uses the
// surfaces' interpolated normals.
//
// Cases: horizontal planes × vertical well (TST = Δz); dipping planes ×
// vertical well at non-node positions (TST = Δz·cos δ); dipping plane with
// both x and y dips; horizontal planes × deviated 2-leg well (TST = TVT);
// folded (sine-sampled mesh) stack × vertical well at a grid node; a
// 3-surface stack exercising bays (enter/exit through either surface),
// start-inside / end-inside units and a strike-parallel leg (zero
// contribution); empty / single-surface stacks (legal zero); unit entirely
// below the path (legal zero); strike-parallel path (zero TST, legal);
// tangency at a mesh crest (not a crossing); crossing exactly at a path
// node; grid-contract errors; footprint excursions; inverted / overlapping
// surfaces (path-point and crossing-side checks); coincident path segment;
// path validation; duplicate (touching) surfaces.

#include <welllog/scene/tst.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>
#include <vector>

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

// ---------------------------------------------------------------------------
// Fixture helpers.
// ---------------------------------------------------------------------------

struct GridSpec {
  double x_origin;
  double y_origin;
  double x_step;
  double y_step;
  std::size_t x_nodes;
  std::size_t y_nodes;
};

// Sample f(x, y) on the grid, row-major (y outer, x inner).
template <typename F>
std::vector<double> grid_from(const GridSpec& g, F f) {
  std::vector<double> z(g.x_nodes * g.y_nodes);
  for (std::size_t j = 0; j < g.y_nodes; ++j) {
    for (std::size_t i = 0; i < g.x_nodes; ++i) {
      const double x = g.x_origin + static_cast<double>(i) * g.x_step;
      const double y = g.y_origin + static_cast<double>(j) * g.y_step;
      z[j * g.x_nodes + i] = f(x, y);
    }
  }
  return z;
}

std::vector<double> grid_const(const GridSpec& g, double height) {
  return grid_from(g, [height](double, double) { return height; });
}

// The SurfaceGrid is borrowed: the z vectors must outlive the call.
struct SurfaceBook {
  std::vector<std::vector<double>> storage;
  std::vector<SurfaceGrid> grids;
};

SurfaceBook make_book(const GridSpec& g, const std::vector<double>& z0) {
  SurfaceBook book;
  book.storage.push_back(z0);
  book.grids.push_back(SurfaceGrid{
      .x_origin_m = g.x_origin,
      .y_origin_m = g.y_origin,
      .x_step_m = g.x_step,
      .y_step_m = g.y_step,
      .x_nodes = g.x_nodes,
      .y_nodes = g.y_nodes,
      .z_tvd = book.storage.back().data(),
  });
  return book;
}

void push_surface(SurfaceBook& book, const GridSpec& g,
                  const std::vector<double>& z) {
  book.storage.push_back(z);
  book.grids.push_back(SurfaceGrid{
      .x_origin_m = g.x_origin,
      .y_origin_m = g.y_origin,
      .x_step_m = g.x_step,
      .y_step_m = g.y_step,
      .x_nodes = g.x_nodes,
      .y_nodes = g.y_nodes,
      .z_tvd = book.storage.back().data(),
  });
}

// The 100 km²-style base grid used by the plane fixtures: x, y ∈ [0, 400] m.
constexpr GridSpec kPlaneGrid{0.0, 0.0, 100.0, 100.0, 5, 2};

double plane_dip_x(double x, double) { return 0.25 * x + 100.0; }
double plane_dip_x_bottom(double x, double) { return 0.25 * x + 300.0; }
double plane_dip_xy(double x, double y) { return 0.25 * x + 0.125 * y + 100.0; }
double plane_dip_xy_bottom(double x, double y) {
  return 0.25 * x + 0.125 * y + 300.0;
}

// Folded surface sampled on a coarse sine: z = 100 + 10·sin(π·x/200) at
// x = 0, 100, 200, 300, 400 → 100, 110, 100, 90, 100.
std::vector<double> sine_mesh(const GridSpec& g) {
  std::vector<double> z(g.x_nodes * g.y_nodes);
  const double heights[5] = {100.0, 110.0, 100.0, 90.0, 100.0};
  for (std::size_t j = 0; j < g.y_nodes; ++j) {
    for (std::size_t i = 0; i < g.x_nodes; ++i) {
      z[j * g.x_nodes + i] = heights[i];
    }
  }
  return z;
}

// ---------------------------------------------------------------------------
// Horizontal planes × vertical well: TST = Δz, normal_dot = 1.
// ---------------------------------------------------------------------------
void horizontal_planes_vertical_well() {
  const GridSpec g{0.0, 0.0, 100.0, 100.0, 3, 3};
  SurfaceBook book = make_book(g, grid_const(g, 100.0));
  push_surface(book, g, grid_const(g, 300.0));
  const PathPoint3D path[] = {
      PathPoint3D{.md = 0.0, .x = 10.0, .y = 10.0, .z = 0.0},
      PathPoint3D{.md = 1000.0, .x = 10.0, .y = 10.0, .z = 1000.0},
  };
  const auto tst =
      tst_along_surface_path(path, 2, book.grids.data(), book.grids.size());
  require(tst.has_value(), "horizontal planes must compute");
  require_near(tst.value().value, 200.0, 1e-9,
               "TST = z2 − z1 through horizontal bedding");
  require_near(tst.value().measured_interval_m, 1000.0, 1e-9,
               "measured interval = whole path md");
  require_near(tst.value().normal_dot, 0.2, 1e-9,
               "normal_dot = TST / path md");
  require(tst.value().kind == ThicknessKind::true_stratigraphic_thickness,
          "kind must be TST");
}

// ---------------------------------------------------------------------------
// Dipping plane (x-dip) × vertical well at a non-node x:
// TST = Δz·cos δ, cos δ = 1/√(1 + 0.25²).
// ---------------------------------------------------------------------------
void dipping_plane_vertical_well() {
  SurfaceBook book = make_book(kPlaneGrid, grid_from(kPlaneGrid, plane_dip_x));
  push_surface(book, kPlaneGrid, grid_from(kPlaneGrid, plane_dip_x_bottom));
  // Well at x = 175 (between grid nodes 100/200), y = 50 (inside cell 0-100).
  const PathPoint3D path[] = {
      PathPoint3D{.md = 0.0, .x = 175.0, .y = 50.0, .z = 0.0},
      PathPoint3D{.md = 1000.0, .x = 175.0, .y = 50.0, .z = 1000.0},
  };
  const auto tst =
      tst_along_surface_path(path, 2, book.grids.data(), book.grids.size());
  require(tst.has_value(), "dipping plane must compute");
  const double cos_dip = 1.0 / std::sqrt(1.0 + 0.25 * 0.25);
  require_near(tst.value().value, 200.0 * cos_dip, 1e-9,
               "TST = Δz·cos δ through a dipping plane");
  require_near(tst.value().normal_dot, 200.0 * cos_dip / 1000.0, 1e-9,
               "normal_dot audit");
}

// ---------------------------------------------------------------------------
// Dipping plane with x and y dip × vertical well:
// TST = Δz/√(1 + m_x² + m_y²).
// ---------------------------------------------------------------------------
void dipping_plane_xy_vertical_well() {
  SurfaceBook book = make_book(kPlaneGrid, grid_from(kPlaneGrid, plane_dip_xy));
  push_surface(book, kPlaneGrid, grid_from(kPlaneGrid, plane_dip_xy_bottom));
  const PathPoint3D path[] = {
      PathPoint3D{.md = 0.0, .x = 175.0, .y = 37.5, .z = 0.0},
      PathPoint3D{.md = 1000.0, .x = 175.0, .y = 37.5, .z = 1000.0},
  };
  const auto tst =
      tst_along_surface_path(path, 2, book.grids.data(), book.grids.size());
  require(tst.has_value(), "xy-dipping plane must compute");
  const double cos_dip =
      1.0 / std::sqrt(1.0 + 0.25 * 0.25 + 0.125 * 0.125);
  require_near(tst.value().value, 200.0 * cos_dip, 1e-9,
               "TST = Δz·cos δ for a plane dipping in x and y");
}

// ---------------------------------------------------------------------------
// Horizontal planes × deviated 2-leg well: TST = TVT = 400 regardless of
// the well angle (the classic horizontal-bedding result).
// ---------------------------------------------------------------------------
void horizontal_planes_deviated_well() {
  const GridSpec g{0.0, 0.0, 100.0, 100.0, 11, 11};
  SurfaceBook book = make_book(g, grid_const(g, 200.0));
  push_surface(book, g, grid_const(g, 400.0));
  const double leg2_md = 500.0 * std::sqrt(2.0);
  const PathPoint3D path[] = {
      PathPoint3D{.md = 0.0, .x = 0.0, .y = 0.0, .z = 0.0},
      PathPoint3D{.md = 500.0, .x = 0.0, .y = 0.0, .z = 500.0},
      // Leg 2 runs up-dip: 45° from (0, 0, 500) to (500, 0, 0).
      PathPoint3D{.md = 500.0 + leg2_md, .x = 500.0, .y = 0.0, .z = 0.0},
  };
  const auto tst =
      tst_along_surface_path(path, 3, book.grids.data(), book.grids.size());
  require(tst.has_value(), "deviated well must compute");
  require_near(tst.value().value, 400.0, 1e-9,
               "TST = TVT for horizontal bedding, any well angle");
  require_near(tst.value().measured_interval_m, 500.0 + leg2_md, 1e-9,
               "measured interval = whole path md");
}

// ---------------------------------------------------------------------------
// Folded (sine mesh) stack × vertical well at a grid node (x = 200):
// the interpolated normal at the node is (−0.1, 0, 1)/√1.01.
// ---------------------------------------------------------------------------
void folded_stack_vertical_well_at_node() {
  SurfaceBook book = make_book(kPlaneGrid, sine_mesh(kPlaneGrid));
  // Same fold shifted down 200 m: 300, 310, 300, 290, 300.
  std::vector<double> z1b = sine_mesh(kPlaneGrid);
  for (double& v : z1b) {
    v += 200.0;
  }
  push_surface(book, kPlaneGrid, z1b);
  const PathPoint3D path[] = {
      PathPoint3D{.md = 0.0, .x = 200.0, .y = 50.0, .z = 0.0},
      PathPoint3D{.md = 1000.0, .x = 200.0, .y = 50.0, .z = 1000.0},
  };
  const auto tst =
      tst_along_surface_path(path, 2, book.grids.data(), book.grids.size());
  require(tst.has_value(), "folded stack must compute");
  // Normal at the node: gradient (−0.1, 0) → n̂ = (0.1, 0, 1)/√1.01.
  const double expected = 200.0 / std::sqrt(1.01);
  require_near(tst.value().value, expected, 1e-9,
               "TST through a folded unit at a node");
}

// ---------------------------------------------------------------------------
// 3-surface stack with bays, start-inside / end-inside units and a
// strike-parallel leg: three parallel dipping planes
// z = 0.25x + 100/300/500 and a path that enters unit 0 before the well
// begins, exits and re-enters it upward through the bottom surface (bay),
// ends inside it, and runs a leg parallel to the bedding (zero TST).
// Unit 0: [0, 100] + [700, 1200] in md; unit 1: [100, 300] + [500, 700].
// Every in-unit sub-segment is vertical except the parallel leg (dot = 0).
// ---------------------------------------------------------------------------
void three_surface_stack_bays() {
  const GridSpec g{-400.0, 0.0, 100.0, 100.0, 9, 2};
  SurfaceBook book = make_book(
      g, grid_from(g, [](double x, double) { return 0.25 * x + 100.0; }));
  push_surface(book, g,
               grid_from(g, [](double x, double) { return 0.25 * x + 300.0; }));
  push_surface(book, g,
               grid_from(g, [](double x, double) { return 0.25 * x + 500.0; }));
  const PathPoint3D path[] = {
      // Starts inside unit 0 (z = 100 at x = −400: between 0 and 200).
      PathPoint3D{.md = 0.0, .x = -400.0, .y = 0.0, .z = 100.0},
      // Leg 1 down: exits unit 0 through s1 at md 100, unit 1 at 300.
      PathPoint3D{.md = 400.0, .x = -400.0, .y = 0.0, .z = 500.0},
      // Leg 2 up: re-enters unit 1 at md 500 (through s2), exits unit 1 at
      // md 700 (through s1), re-enters unit 0 at md 700 and stays inside.
      PathPoint3D{.md = 800.0, .x = -400.0, .y = 0.0, .z = 100.0},
      // Leg 3 runs parallel to the bedding planes (d ≡ const, |ŵ·n̂| = 0):
      // no crossings, zero TST contribution.
      PathPoint3D{.md = 1200.0, .x = 400.0, .y = 0.0, .z = 300.0},
  };
  const auto tst =
      tst_along_surface_path(path, 4, book.grids.data(), book.grids.size());
  require(tst.has_value(), "stack with bays must compute");
  const double cos_dip = 1.0 / std::sqrt(1.0 + 0.25 * 0.25);
  // Unit 0: 100 m (start-inside) + 100 m (re-enter through s1) + 400 m
  // parallel leg (dot = 0). Unit 1: 200 m + 200 m (bay up through the
  // stack). All vertical legs contribute Δmd·cos δ.
  const double expected = 600.0 * cos_dip;
  require_near(tst.value().value, expected, 1e-9,
               "stack TST = Σ vertical in-unit segments · cos δ");
  require_near(tst.value().measured_interval_m, 1200.0, 1e-9,
               "measured interval = whole path md");
  require_near(tst.value().normal_dot, expected / 1200.0, 1e-9,
               "normal_dot audit");
}

// ---------------------------------------------------------------------------
// Empty and single-surface stacks are legal zero results.
// ---------------------------------------------------------------------------
void empty_and_single_surface_stacks() {
  const GridSpec g{0.0, 0.0, 100.0, 100.0, 3, 3};
  const PathPoint3D path[] = {
      PathPoint3D{.md = 0.0, .x = 10.0, .y = 10.0, .z = 0.0},
      PathPoint3D{.md = 500.0, .x = 10.0, .y = 10.0, .z = 500.0},
  };
  const auto empty =
      tst_along_surface_path(path, 2, nullptr, 0);
  require(empty.has_value(), "empty stack is legal");
  require_near(empty.value().value, 0.0, 0.0, "empty stack → zero TST");
  require_near(empty.value().measured_interval_m, 500.0, 1e-9,
               "empty stack reports the path md");
  SurfaceBook book = make_book(g, grid_const(g, 100.0));
  const auto single =
      tst_along_surface_path(path, 2, book.grids.data(), 1);
  require(single.has_value(), "single-surface stack is legal");
  require_near(single.value().value, 0.0, 0.0,
               "single surface → zero TST (no complete unit)");
}

// ---------------------------------------------------------------------------
// A unit entirely below the path contributes nothing (legal).
// ---------------------------------------------------------------------------
void unit_below_path() {
  const GridSpec g{0.0, 0.0, 100.0, 100.0, 3, 3};
  SurfaceBook book = make_book(g, grid_const(g, 500.0));
  push_surface(book, g, grid_const(g, 700.0));
  const PathPoint3D path[] = {
      PathPoint3D{.md = 0.0, .x = 10.0, .y = 10.0, .z = 0.0},
      PathPoint3D{.md = 100.0, .x = 10.0, .y = 10.0, .z = 100.0},
  };
  const auto tst =
      tst_along_surface_path(path, 2, book.grids.data(), book.grids.size());
  require(tst.has_value(), "unit below path must compute");
  require_near(tst.value().value, 0.0, 0.0, "unit below path → zero");
  require_near(tst.value().normal_dot, 0.0, 0.0, "normal_dot = 0");
}

// ---------------------------------------------------------------------------
// A strike-parallel path through a dipping unit: zero TST is a VALID result.
// ---------------------------------------------------------------------------
void strike_parallel_zero_tst() {
  const GridSpec g{0.0, 0.0, 100.0, 100.0, 5, 6};  // y ∈ [0, 500]
  SurfaceBook book = make_book(g, grid_from(g, plane_dip_x));
  push_surface(book, g, grid_from(g, plane_dip_x_bottom));
  // Along y at x = 50, z = 162.5 (midway between the two planes there).
  const PathPoint3D path[] = {
      PathPoint3D{.md = 0.0, .x = 50.0, .y = 0.0, .z = 162.5},
      PathPoint3D{.md = 500.0, .x = 50.0, .y = 500.0, .z = 162.5},
  };
  const auto tst =
      tst_along_surface_path(path, 2, book.grids.data(), book.grids.size());
  require(tst.has_value(), "strike-parallel path must compute");
  require_near(tst.value().value, 0.0, 1e-9,
               "strike-parallel path → zero TST (legal)");
  require_near(tst.value().normal_dot, 0.0, 1e-9, "normal_dot = 0");
}

// ---------------------------------------------------------------------------
// Tangency at a mesh crest is not a crossing: a horizontal path at the
// crest level z = 110 grazes the fold (d ≥ 0, d = 0 at x = 100) and stays
// inside the unit; TST comes from the whole leg with the midpoint normal.
// ---------------------------------------------------------------------------
void tangency_at_crest() {
  SurfaceBook book = make_book(kPlaneGrid, sine_mesh(kPlaneGrid));
  push_surface(book, kPlaneGrid, grid_const(kPlaneGrid, 120.0));
  const PathPoint3D path[] = {
      PathPoint3D{.md = 0.0, .x = 0.0, .y = 50.0, .z = 110.0},
      PathPoint3D{.md = 200.0, .x = 200.0, .y = 50.0, .z = 110.0},
  };
  const auto tst =
      tst_along_surface_path(path, 2, book.grids.data(), book.grids.size());
  require(tst.has_value(), "tangent path must compute");
  // Normal at the midpoint (x = 100, crest): n̂0 = (0.1, 0, 1)/√1.01,
  // n̂1 = (0, 0, 1); the average projected onto ŵ = (1, 0, 0).
  const double nx = 0.1 / std::sqrt(1.01);
  const double nz = 1.0 / std::sqrt(1.01) + 1.0;
  const double dot = nx / std::sqrt(nx * nx + nz * nz);
  const double expected = 200.0 * dot;
  require_near(tst.value().value, expected, 1e-9,
               "tangency is not a crossing; whole leg contributes");
}

// ---------------------------------------------------------------------------
// Crossing exactly at a path node (the path point lies on the surface): the
// duplicate candidates from the two adjacent legs are merged into one
// genuine crossing.
// ---------------------------------------------------------------------------
void crossing_at_path_node() {
  const GridSpec g{0.0, 0.0, 100.0, 100.0, 3, 3};
  SurfaceBook book = make_book(g, grid_const(g, 200.0));
  push_surface(book, g, grid_const(g, 400.0));
  const PathPoint3D path[] = {
      PathPoint3D{.md = 0.0, .x = 50.0, .y = 50.0, .z = 0.0},
      PathPoint3D{.md = 200.0, .x = 50.0, .y = 50.0, .z = 200.0},
      PathPoint3D{.md = 1000.0, .x = 50.0, .y = 50.0, .z = 1000.0},
  };
  const auto tst =
      tst_along_surface_path(path, 3, book.grids.data(), book.grids.size());
  require(tst.has_value(), "node crossing must compute");
  require_near(tst.value().value, 200.0, 1e-9,
               "TST = Δz with a crossing exactly at a path node");
}

// ---------------------------------------------------------------------------
// Grid-contract errors are input errors.
// ---------------------------------------------------------------------------
void invalid_grids() {
  const GridSpec g{0.0, 0.0, 100.0, 100.0, 3, 3};
  const std::vector<double> z = grid_const(g, 100.0);
  const std::vector<double> z1 = grid_const(g, 300.0);
  const PathPoint3D path[] = {
      PathPoint3D{.md = 0.0, .x = 10.0, .y = 10.0, .z = 0.0},
      PathPoint3D{.md = 100.0, .x = 10.0, .y = 10.0, .z = 100.0},
  };
  const auto run = [&](SurfaceGrid s0) {
    SurfaceGrid s1{};
    SurfaceGrid grids[2] = {s0, s1};
    grids[1] = SurfaceGrid{
        .x_origin_m = g.x_origin, .y_origin_m = g.y_origin,
        .x_step_m = g.x_step, .y_step_m = g.y_step,
        .x_nodes = g.x_nodes, .y_nodes = g.y_nodes,
        .z_tvd = z1.data(),
    };
    return tst_along_surface_path(path, 2, grids, 2);
  };
  const SurfaceGrid base{
      .x_origin_m = g.x_origin, .y_origin_m = g.y_origin,
      .x_step_m = g.x_step, .y_step_m = g.y_step,
      .x_nodes = g.x_nodes, .y_nodes = g.y_nodes,
      .z_tvd = z.data(),
  };
  {
    SurfaceGrid s = base;
    s.x_nodes = 1;
    require(!run(s).has_value(), "x_nodes < 2 is an error");
  }
  {
    SurfaceGrid s = base;
    s.y_nodes = 1;
    require(!run(s).has_value(), "y_nodes < 2 is an error");
  }
  {
    SurfaceGrid s = base;
    s.x_step_m = 0.0;
    require(!run(s).has_value(), "zero x_step is an error");
  }
  {
    SurfaceGrid s = base;
    s.y_step_m = -50.0;
    require(!run(s).has_value(), "negative y_step is an error");
  }
  {
    SurfaceGrid s = base;
    s.x_origin_m = std::numeric_limits<double>::quiet_NaN();
    require(!run(s).has_value(), "non-finite origin is an error");
  }
  {
    SurfaceGrid s = base;
    std::vector<double> bad = z;
    bad[0] = std::numeric_limits<double>::quiet_NaN();
    s.z_tvd = bad.data();
    require(!run(s).has_value(), "non-finite height is an error");
  }
  {
    SurfaceGrid s = base;
    s.z_tvd = nullptr;
    require(!run(s).has_value(), "null height array is an error");
  }
}

// ---------------------------------------------------------------------------
// A path point outside a surface's footprint is an input error.
// ---------------------------------------------------------------------------
void footprint_excursions() {
  const GridSpec g{0.0, 0.0, 100.0, 100.0, 3, 3};  // x, y ∈ [0, 200]
  SurfaceBook book = make_book(g, grid_const(g, 100.0));
  push_surface(book, g, grid_const(g, 300.0));
  const PathPoint3D out_x[] = {
      PathPoint3D{.md = 0.0, .x = 10.0, .y = 10.0, .z = 0.0},
      PathPoint3D{.md = 100.0, .x = 250.0, .y = 10.0, .z = 100.0},
  };
  const auto tst_x =
      tst_along_surface_path(out_x, 2, book.grids.data(), book.grids.size());
  require(!tst_x.has_value(), "x excursion is an error");
  const PathPoint3D out_y[] = {
      PathPoint3D{.md = 0.0, .x = 10.0, .y = -50.0, .z = 0.0},
      PathPoint3D{.md = 100.0, .x = 10.0, .y = 10.0, .z = 100.0},
  };
  const auto tst_y =
      tst_along_surface_path(out_y, 2, book.grids.data(), book.grids.size());
  require(!tst_y.has_value(), "y excursion is an error");
}

// ---------------------------------------------------------------------------
// Inverted surfaces (top below bottom at a path point) are input errors.
// ---------------------------------------------------------------------------
void inverted_surfaces() {
  SurfaceBook book = make_book(kPlaneGrid,
                               grid_from(kPlaneGrid, plane_dip_x_bottom));
  push_surface(book, kPlaneGrid, grid_from(kPlaneGrid, plane_dip_x));
  const PathPoint3D path[] = {
      PathPoint3D{.md = 0.0, .x = 175.0, .y = 50.0, .z = 0.0},
      PathPoint3D{.md = 1000.0, .x = 175.0, .y = 50.0, .z = 1000.0},
  };
  const auto tst =
      tst_along_surface_path(path, 2, book.grids.data(), book.grids.size());
  require(!tst.has_value(), "inverted surfaces are an error");
}

// ---------------------------------------------------------------------------
// Duplicate (touching) surfaces are an input error (ordering is strict).
// ---------------------------------------------------------------------------
void touching_surfaces() {
  const GridSpec g{0.0, 0.0, 100.0, 100.0, 3, 3};
  SurfaceBook book = make_book(g, grid_const(g, 100.0));
  push_surface(book, g, grid_const(g, 100.0));
  const PathPoint3D path[] = {
      PathPoint3D{.md = 0.0, .x = 10.0, .y = 10.0, .z = 0.0},
      PathPoint3D{.md = 100.0, .x = 10.0, .y = 10.0, .z = 100.0},
  };
  const auto tst =
      tst_along_surface_path(path, 2, book.grids.data(), book.grids.size());
  require(!tst.has_value(), "touching surfaces are an error");
}

// ---------------------------------------------------------------------------
// Surfaces crossing between path points are caught by the crossing-side
// check: s0: z = 0.1x + 100; s1: z = 0.001(x − 300)² + 0.1x + 95. The
// surfaces are ordered at the path points (x = 0 and x = 1000) but cross
// near x = 300; the path crosses s1 at x ≈ 245 where s1 is already above
// s0 — an input error, never a silent merge.
// ---------------------------------------------------------------------------
void surfaces_crossing_between_path_points() {
  const GridSpec g{0.0, 0.0, 100.0, 100.0, 11, 2};  // x ∈ [0, 1000]
  SurfaceBook book = make_book(
      g, grid_from(g, [](double x, double) { return 0.1 * x + 100.0; }));
  push_surface(book, g, grid_from(g, [](double x, double) {
                 const double d = x - 300.0;
                 return 0.001 * d * d + 0.1 * x + 95.0;
               }));
  // Zig-zag path: down-dip on leg 1, back up-dip on leg 2.
  const PathPoint3D path[] = {
      PathPoint3D{.md = 0.0, .x = 0.0, .y = 0.0, .z = 0.0},
      PathPoint3D{.md = 500.0, .x = 1000.0, .y = 0.0, .z = 500.0},
      PathPoint3D{.md = 1000.0, .x = 0.0, .y = 0.0, .z = 1000.0},
  };
  const auto tst =
      tst_along_surface_path(path, 3, book.grids.data(), book.grids.size());
  require(!tst.has_value(), "surfaces crossing along the well are an error");
}

// ---------------------------------------------------------------------------
// A path segment lying in a surface (coincident) is an input error.
// ---------------------------------------------------------------------------
void coincident_segment() {
  const GridSpec g{0.0, 0.0, 100.0, 100.0, 11, 6};  // x ∈ [0, 1000], y ∈ [0, 500]
  SurfaceBook book = make_book(g, grid_const(g, 100.0));
  push_surface(book, g, grid_const(g, 300.0));
  // Horizontal leg exactly in the top surface at z = 100.
  const PathPoint3D path[] = {
      PathPoint3D{.md = 0.0, .x = 0.0, .y = 0.0, .z = 100.0},
      PathPoint3D{.md = 500.0, .x = 500.0, .y = 0.0, .z = 100.0},
  };
  const auto tst =
      tst_along_surface_path(path, 2, book.grids.data(), book.grids.size());
  require(!tst.has_value(), "coincident path segment is an error");
}

// ---------------------------------------------------------------------------
// Path validation mirrors the polyline contract.
// ---------------------------------------------------------------------------
void invalid_paths() {
  const GridSpec g{0.0, 0.0, 100.0, 100.0, 3, 3};
  SurfaceBook book = make_book(g, grid_const(g, 100.0));
  push_surface(book, g, grid_const(g, 300.0));
  const auto run = [&](const PathPoint3D* path, std::size_t count) {
    return tst_along_surface_path(path, count, book.grids.data(),
                                  book.grids.size());
  };
  const PathPoint3D flat_md[] = {
      PathPoint3D{.md = 0.0, .x = 10.0, .y = 10.0, .z = 0.0},
      PathPoint3D{.md = 0.0, .x = 10.0, .y = 10.0, .z = 500.0},
  };
  require(!run(flat_md, 2).has_value(), "non-increasing md is an error");
  const PathPoint3D zero_leg[] = {
      PathPoint3D{.md = 0.0, .x = 10.0, .y = 10.0, .z = 0.0},
      PathPoint3D{.md = 100.0, .x = 10.0, .y = 10.0, .z = 0.0},
  };
  require(!run(zero_leg, 2).has_value(), "zero-length leg is an error");
  const PathPoint3D nan_point[] = {
      PathPoint3D{.md = 0.0, .x = 10.0, .y = 10.0, .z = 0.0},
      PathPoint3D{.md = 100.0, .x = std::numeric_limits<double>::quiet_NaN(),
                  .y = 10.0, .z = 100.0},
  };
  require(!run(nan_point, 2).has_value(), "non-finite point is an error");
  const PathPoint3D single[] = {
      PathPoint3D{.md = 0.0, .x = 10.0, .y = 10.0, .z = 0.0},
  };
  require(!run(single, 1).has_value(), "path_count < 2 is an error");
  require(!run(nullptr, 2).has_value(), "null path is an error");
}

}  // namespace

int main() {
  horizontal_planes_vertical_well();
  dipping_plane_vertical_well();
  dipping_plane_xy_vertical_well();
  horizontal_planes_deviated_well();
  folded_stack_vertical_well_at_node();
  three_surface_stack_bays();
  empty_and_single_surface_stacks();
  unit_below_path();
  strike_parallel_zero_tst();
  tangency_at_crest();
  crossing_at_path_node();
  invalid_grids();
  footprint_excursions();
  inverted_surfaces();
  touching_surfaces();
  surfaces_crossing_between_path_points();
  coincident_segment();
  invalid_paths();
  std::cout << "tst-surfaces: all analytical fixtures passed\n";
  return EXIT_SUCCESS;
}
