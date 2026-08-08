// True Stratigraphic Thickness (Epic D): a provable planar/local model.
//
// The API is explicit about every input (D1): a measured-depth interval is
// NOT a thickness, TVD is NOT TST, and "planar bedding" is never silently
// assumed — the caller supplies the bedding normal and the well direction as
// unit vectors, and the model documents that it is valid only where the
// local planar approximation holds (varying orientation / surface
// intersection are future extensions, not silent fallbacks).
//
// TST = projection of the interval segment onto the bedding normal:
//   * measured interval L along the wellbore:  TST = L · |w · n|
//   * two 3D points on the wellbore:            TST = |Δp · n|
// For a vertical well (w = (0,0,1)) and horizontal bedding this degenerates
// to TST = L; for a vertical well and dip δ, TST = L·cos(δ).
//
// Piecewise-planar extension (D2): bedding orientation may vary along the
// well. ``tst_through_layers`` sums the per-layer projections of a measured
// interval along a straight well leg; ``tst_along_path`` does the same for a
// polyline path (each consecutive pair of path points forms one
// constant-direction leg — the wellbore between the points is the chord).
// The piecewise model is as explicit as the planar one: layers must be
// ordered by ascending top_md, non-overlapping, strictly positive in extent,
// and every layer carries its own unit normal. Intervals not covered by any
// declared layer contribute nothing (undeclared bedding is the caller's
// responsibility — e.g. net-sand TST over selected layers only); a
// missing/inverted/overlapping layer is an input error, never a silent
// merge or default.
//
// Zero TST is a VALID result (the well runs parallel to bedding); reversed
// or non-finite inputs are errors. Unit-vector inputs are validated — a
// missing/insufficient orientation is an input error, never a default.

#ifndef WELLLOG_SCENE_TST_HPP
#define WELLLOG_SCENE_TST_HPP

#include <cstddef>
#include <welllog/core/result.hpp>
#include <welllog/scene/export.hpp>

namespace welllog {

// Explicit thickness kinds (D1) — the API never returns a bare "thickness".
enum class ThicknessKind : std::uint8_t {
  measured_depth_interval,      // MD difference along the wellbore
  tvd_interval,                 // true-vertical-depth difference
  apparent_thickness,           // thickness along a non-normal measurement
  true_stratigraphic_thickness, // perpendicular to bedding
};

// Downhole direction of the wellbore at the interval (unit vector, Z = down
// / increasing depth).
struct WellDirection3D {
  double x{};
  double y{};
  double z{1.0};
};

// Bedding orientation: the unit normal to the local bedding plane. The
// caller derives it (e.g. from dip + dip azimuth); a zero/non-unit vector is
// an input error — "planar" is never assumed silently.
struct BedNormal3D {
  double x{};
  double y{};
  double z{1.0};
};

struct TrueStratigraphicThickness {
  double value{};  // metres (same unit as the input interval)
  ThicknessKind kind{ThicknessKind::true_stratigraphic_thickness};
  // Audit trail of the computation.
  double measured_interval_m{};  // input L (0 when computed from points)
  double normal_dot{};           // |w·n| or |Δp̂·n| (projection factor)
};

// One layer of a piecewise-planar bedding model (D2): the layer's extent
// along the well path in measured depth, and its locally constant bedding
// normal. Layers must be ordered by ascending top_md, non-overlapping, and
// strictly positive in extent; the normal must be a unit vector.
struct BeddingLayer {
  double top_md{};
  double bottom_md{};
  BedNormal3D normal{};
};

// A point on the wellbore path (D2 polyline form): measured depth along the
// wellbore plus the 3D position. Consecutive points form straight legs; the
// wellbore tangent between the points is approximated by the chord.
struct PathPoint3D {
  double md{};
  double x{};
  double y{};
  double z{};
};

// TST from a measured interval L along a deviated well through planar
// bedding: ``TST = L · |w·n|``. L must be finite and ≥ 0; both vectors must
// be finite unit vectors (norm within 1e-6).
[[nodiscard]] WELLLOG_SCENE_API Result<TrueStratigraphicThickness>
tst_from_measured_interval(double measured_length_m,
                           WellDirection3D well_direction,
                           BedNormal3D bed) noexcept;

// TST from two 3D points on the wellbore (top/bottom) intersecting bedding:
// ``TST = |(p_bottom - p_top) · n|``. Both points finite; the segment must
// have non-zero length (a zero-length interval is an input error).
[[nodiscard]] WELLLOG_SCENE_API Result<TrueStratigraphicThickness>
tst_from_interval_points(double top_x, double top_y, double top_z,
                         double bottom_x, double bottom_y, double bottom_z,
                         BedNormal3D bed) noexcept;

// TST of a measured interval (start_md, length) along a straight well leg
// through piecewise-planar bedding:
//   ``TST = Σ_i  overlap(interval, layer_i) · |w·n_i|``
// The interval may start anywhere; layers partially covered by it contribute
// their covered part; intervals (or parts) outside every layer contribute
// nothing. ``start_md``/length finite, length ≥ 0; layers must satisfy the
// BeddingLayer contract above (a violation is an input error).
[[nodiscard]] WELLLOG_SCENE_API Result<TrueStratigraphicThickness>
tst_through_layers(double start_md, double measured_length_m,
                   WellDirection3D well_direction,
                   const BeddingLayer* layers, std::size_t layer_count) noexcept;

// TST along a polyline well path through piecewise-planar bedding: each leg
// [path[i-1], path[i]] is a straight segment of direction Δp̂ and measured
// length Δmd; TST = Σ_legs Σ_layers overlap·|Δp̂·n_i|. Requires ≥ 2 points
// with strictly increasing md and non-zero-length legs (a zero-length leg
// with increasing md is an inconsistent path → input error).
[[nodiscard]] WELLLOG_SCENE_API Result<TrueStratigraphicThickness>
tst_along_path(const PathPoint3D* path, std::size_t path_count,
               const BeddingLayer* layers, std::size_t layer_count) noexcept;

}  // namespace welllog

#endif  // WELLLOG_SCENE_TST_HPP
