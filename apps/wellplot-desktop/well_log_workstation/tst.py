"""True stratigraphic thickness (Epic D) — Desktop side.

The SDK (``scene/tst.hpp``, C++) is the single source of truth for the
piecewise-planar TST model; this module mirrors its exact semantics
(``tst_through_layers`` / ``tst_along_path`` / ``tst_along_surface_path``,
validation included) so the workstation runs without the Python binding, and
prefers the binding when it is importable (resolver pattern, same as
``depth_ruler``). The mirror is locked to the C++ behaviour by parity tests
that assert the *same* analytic fixture values as
``tests/integration/tst_layers_test.cpp`` and
``tests/integration/tst_surfaces_test.cpp``.

Conventions (documented in tst.hpp):
* coordinates: x = north, y = east, z = down / increasing depth (TVD);
* dip_deg measured from horizontal (0 = flat, 90 = vertical); dip_azimuth_deg
  is the compass azimuth of the dip direction (direction of maximum dip);
  normal = (sin δ·cos φ, sin δ·sin φ, cos δ) — with φ = 0 this degenerates to
  the (sin δ, 0, cos δ) convention of the C++ fixtures;
* bedding layers are (top_md, bottom_md) intervals along the well path, each
  with its own unit normal; layers must be ordered, non-overlapping and
  strictly positive in extent; intervals outside every layer contribute
  nothing (undeclared bedding is the caller's responsibility);
* surface bedding (Epic D high-order): a layer may be bounded by two
  surfaces sampled on regular (x, y) grids instead of declared normals —
  the path's crossings are computed geometrically and the local normal is
  interpolated from the surfaces (see ``tst_along_surface_path``);
* zero TST is a VALID result (well parallel to bedding); every input
  violation raises ValueError — never a silent default or merge.
"""

from __future__ import annotations

import json
import math
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable, Sequence

import numpy as np

from well_log_workstation.survey import SurveyTrajectory
from well_log_workstation.tops_model import well_data_dir
from well_log_workstation.workspace import Workspace

BEDDING_FILENAME = "bedding.json"
# v1: planar layers (top/bottom MD + dip/azimuth); v2: optional per-layer
# top_surface / bottom_surface grids (SurfaceGridSpec). Reading is tolerant
# of both.
BEDDING_SCHEMA_VERSION = 2

# Mirror of the C++ ThicknessKind — the API never returns a bare thickness.
KIND_TST = "true_stratigraphic_thickness"

_UNIT_TOLERANCE = 1e-6


# ---------------------------------------------------------------------------
# Mirrors of the SDK value structs (tst.hpp)
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class WellDirection3D:
    """Unit vector along the wellbore (Z = down / increasing depth)."""

    x: float = 0.0
    y: float = 0.0
    z: float = 1.0


@dataclass(frozen=True)
class BedNormal3D:
    """Unit normal of the local bedding plane."""

    x: float = 0.0
    y: float = 0.0
    z: float = 1.0


@dataclass(frozen=True)
class BeddingLayer:
    """One layer of a piecewise-planar bedding model (MD interval + normal)."""

    top_md: float
    bottom_md: float
    normal: BedNormal3D


@dataclass(frozen=True)
class PathPoint3D:
    """A point on the wellbore path: measured depth + 3D position."""

    md: float
    x: float
    y: float
    z: float


@dataclass(frozen=True)
class SurfaceGrid:
    """Mirror of the SDK SurfaceGrid (tst.hpp): z = f(x, y) TVD at regular
    grid nodes, piecewise bilinear between nodes.

    x = northing, y = easting; nodes are spaced ``x_step_m`` / ``y_step_m``
    apart starting at (``x_origin_m``, ``y_origin_m``). ``z_tvd`` holds
    ``y_nodes × x_nodes`` values, row-major (y outer, x inner). Contract:
    ≥ 2 nodes per dimension, finite positive steps, finite origin, finite
    heights; the surface is defined only inside its rectangular footprint.
    """

    x_origin_m: float
    y_origin_m: float
    x_step_m: float
    y_step_m: float
    x_nodes: int
    y_nodes: int
    z_tvd: Sequence[float] = field(default_factory=list)


@dataclass(frozen=True)
class TrueStratigraphicThickness:
    """Mirror of the SDK result struct."""

    value: float
    kind: str = KIND_TST
    measured_interval_m: float = 0.0
    normal_dot: float = 0.0


# ---------------------------------------------------------------------------
# Orientation helpers
# ---------------------------------------------------------------------------


def normal_from_dip_azimuth(dip_deg: float, dip_azimuth_deg: float) -> BedNormal3D:
    """Bedding unit normal from dip (from horizontal) and dip azimuth.

    ``dip_azimuth_deg`` is the compass azimuth of the dip direction; with
    azimuth 0 the normal is (sin δ, 0, cos δ) — the C++ fixture convention.
    """
    d = math.radians(float(dip_deg))
    a = math.radians(float(dip_azimuth_deg))
    return BedNormal3D(
        x=math.sin(d) * math.cos(a),
        y=math.sin(d) * math.sin(a),
        z=math.cos(d),
    )


# ---------------------------------------------------------------------------
# The mirror implementation (locked to tst_layers_test.cpp by parity tests)
# ---------------------------------------------------------------------------


def _unit_vector(x: float, y: float, z: float) -> float:
    """Norm of (x, y, z) when it is a finite unit vector, else ValueError."""
    if not all(math.isfinite(v) for v in (x, y, z)):
        raise ValueError("向量必须有限")
    norm = math.sqrt(x * x + y * y + z * z)
    if not (norm > 0.0) or abs(norm - 1.0) > _UNIT_TOLERANCE:
        raise ValueError("向量必须是单位向量")
    return norm


def _valid_layers(layers: Sequence[BeddingLayer]) -> None:
    """Validate the BeddingLayer contract; raises ValueError on violation."""
    prev_bottom_md = 0.0
    for i, layer in enumerate(layers):
        if not math.isfinite(layer.top_md) or not math.isfinite(layer.bottom_md):
            raise ValueError(f"层 {i}: 深度边界必须有限")
        if not (layer.bottom_md > layer.top_md):
            raise ValueError(f"层 {i}: 厚度必须为正（倒置/零厚度层无效）")
        _unit_vector(layer.normal.x, layer.normal.y, layer.normal.z)
        if i > 0 and layer.top_md < prev_bottom_md:
            raise ValueError(f"层 {i}: 层序必须有序且不重叠")
        prev_bottom_md = layer.bottom_md


def _accumulate_layers(
    start_md: float, end_md: float, w: WellDirection3D,
    layers: Sequence[BeddingLayer],
) -> float:
    """Σ overlap(interval, layer) · |w·n| over the declared layers."""
    total = 0.0
    for layer in layers:
        lo = max(start_md, layer.top_md)
        hi = min(end_md, layer.bottom_md)
        if not (hi > lo):
            continue
        dot = abs(
            w.x * layer.normal.x + w.y * layer.normal.y + w.z * layer.normal.z
        )
        total += (hi - lo) * dot
    return total


def _mirror_tst_through_layers(
    start_md: float,
    measured_length_m: float,
    well_direction: WellDirection3D,
    layers: Sequence[BeddingLayer],
) -> TrueStratigraphicThickness:
    """Exact mirror of ``tst_through_layers`` (tst.hpp)."""
    if not math.isfinite(start_md) or not math.isfinite(measured_length_m):
        raise ValueError("起始深度与长度必须有限")
    if measured_length_m < 0.0:
        raise ValueError("长度不能为负")
    _unit_vector(well_direction.x, well_direction.y, well_direction.z)
    _valid_layers(layers)
    total = _accumulate_layers(
        start_md, start_md + measured_length_m, well_direction, layers
    )
    return TrueStratigraphicThickness(
        value=total,
        measured_interval_m=measured_length_m,
        normal_dot=total / measured_length_m if measured_length_m > 0.0 else 0.0,
    )


def _mirror_tst_along_path(
    path: Sequence[PathPoint3D],
    layers: Sequence[BeddingLayer],
) -> TrueStratigraphicThickness:
    """Exact mirror of ``tst_along_path`` (tst.hpp)."""
    if len(path) < 2:
        raise ValueError("路径至少需要两个点")
    _valid_layers(layers)
    total = 0.0
    total_md = 0.0
    for i in range(1, len(path)):
        a, b = path[i - 1], path[i]
        if not all(
            math.isfinite(v)
            for v in (a.md, a.x, a.y, a.z, b.md, b.x, b.y, b.z)
        ):
            raise ValueError("路径点必须有限")
        if not (b.md > a.md):
            raise ValueError("路径 MD 必须严格递增")
        dx, dy, dz = b.x - a.x, b.y - a.y, b.z - a.z
        leg_length = math.sqrt(dx * dx + dy * dy + dz * dz)
        if not (leg_length > 0.0):
            raise ValueError("零长度腿（MD 递增但位置不变）路径不一致")
        leg = WellDirection3D(x=dx / leg_length, y=dy / leg_length, z=dz / leg_length)
        total += _accumulate_layers(a.md, b.md, leg, layers)
        total_md += b.md - a.md
    return TrueStratigraphicThickness(
        value=total,
        measured_interval_m=total_md,
        normal_dot=total / total_md,  # total_md > 0 by the md rule
    )


# ---------------------------------------------------------------------------
# Surface mirror (locked to tst_surfaces_test.cpp by parity tests)
# ---------------------------------------------------------------------------


def _valid_surface_grid(s: SurfaceGrid) -> None:
    """Validate the SurfaceGrid contract; raises ValueError on violation."""
    if not s.z_tvd or s.x_nodes < 2 or s.y_nodes < 2:
        raise ValueError("曲面网格至少需要两个节点且必须携带高度数组")
    if not (math.isfinite(s.x_origin_m) and math.isfinite(s.y_origin_m)):
        raise ValueError("曲面原点必须有限")
    if not (math.isfinite(s.x_step_m) and s.x_step_m > 0.0):
        raise ValueError("x 步长必须有限且为正")
    if not (math.isfinite(s.y_step_m) and s.y_step_m > 0.0):
        raise ValueError("y 步长必须有限且为正")
    if len(s.z_tvd) != s.x_nodes * s.y_nodes:
        raise ValueError("曲面高度数组长度与网格节点数不一致")
    if not all(math.isfinite(v) for v in s.z_tvd):
        raise ValueError("曲面高度必须有限")


def _in_footprint(s: SurfaceGrid, x: float, y: float) -> bool:
    return (
        x >= s.x_origin_m
        and x <= s.x_origin_m + (s.x_nodes - 1) * s.x_step_m
        and y >= s.y_origin_m
        and y <= s.y_origin_m + (s.y_nodes - 1) * s.y_step_m
    )


def _grid_cell(
    s: SurfaceGrid, x: float, y: float
) -> tuple[int, int, float, float]:
    """(i, j, u, v) of the in-footprint point; corner-left convention — a
    point exactly on a grid line belongs to the cell on the +x/+y side."""
    fx = max(0.0, (x - s.x_origin_m) / s.x_step_m)
    fy = max(0.0, (y - s.y_origin_m) / s.y_step_m)
    i = min(int(math.floor(fx)), s.x_nodes - 2)
    j = min(int(math.floor(fy)), s.y_nodes - 2)
    u = min(max(fx - math.floor(fx), 0.0), 1.0)
    v = min(max(fy - math.floor(fy), 0.0), 1.0)
    return i, j, u, v


def _surface_height(s: SurfaceGrid, x: float, y: float) -> float:
    """Bilinear surface height at an in-footprint point."""
    i, j, u, v = _grid_cell(s, x, y)
    xn = s.x_nodes
    z = s.z_tvd
    top = (1.0 - u) * z[j * xn + i] + u * z[j * xn + i + 1]
    bot = (1.0 - u) * z[(j + 1) * xn + i] + u * z[(j + 1) * xn + i + 1]
    return (1.0 - v) * top + v * bot


def _surface_height_in_cell(
    s: SurfaceGrid, cell: tuple[int, int, float, float], x: float, y: float
) -> float:
    """Height evaluated with an explicit cell (per-cell root solving; the
    caller guarantees the point lies within that cell)."""
    i, j, _, _ = cell
    u = (x - (s.x_origin_m + i * s.x_step_m)) / s.x_step_m
    v = (y - (s.y_origin_m + j * s.y_step_m)) / s.y_step_m
    xn = s.x_nodes
    z = s.z_tvd
    top = (1.0 - u) * z[j * xn + i] + u * z[j * xn + i + 1]
    bot = (1.0 - u) * z[(j + 1) * xn + i] + u * z[(j + 1) * xn + i + 1]
    return (1.0 - v) * top + v * bot


def _surface_normal(
    s: SurfaceGrid, x: float, y: float
) -> tuple[float, float, float]:
    """Interpolated unit normal (Z component positive, downward)."""
    i, j, u, v = _grid_cell(s, x, y)
    xn = s.x_nodes
    z = s.z_tvd
    dzx0 = (z[j * xn + i + 1] - z[j * xn + i]) / s.x_step_m
    dzx1 = (z[(j + 1) * xn + i + 1] - z[(j + 1) * xn + i]) / s.x_step_m
    dzy0 = (z[(j + 1) * xn + i] - z[j * xn + i]) / s.y_step_m
    dzy1 = (z[(j + 1) * xn + i + 1] - z[j * xn + i + 1]) / s.y_step_m
    fx = (1.0 - v) * dzx0 + v * dzx1
    fy = (1.0 - u) * dzy0 + u * dzy1
    norm = math.sqrt(fx * fx + fy * fy + 1.0)
    return (-fx / norm, -fy / norm, 1.0 / norm)


def _signed_distance(
    s: SurfaceGrid, a: PathPoint3D, b: PathPoint3D, t: float
) -> float:
    """d(t) = z(t) − f(x(t), y(t)) along the leg (negative = above)."""
    x = a.x + t * (b.x - a.x)
    y = a.y + t * (b.y - a.y)
    z = a.z + t * (b.z - a.z)
    return z - _surface_height(s, x, y)


def _signed_distance_in_cell(
    s: SurfaceGrid,
    cell: tuple[int, int, float, float],
    a: PathPoint3D,
    b: PathPoint3D,
    t: float,
) -> float:
    x = a.x + t * (b.x - a.x)
    y = a.y + t * (b.y - a.y)
    z = a.z + t * (b.z - a.z)
    return z - _surface_height_in_cell(s, cell, x, y)


def _quadratic_roots(
    c0: float, c1: float, c2: float, t_lo: float, t_hi: float
) -> list[float]:
    """Simple roots of c0 + c1·t + c2·t² in [t_lo, t_hi] (mirror of the C++
    quadratic_roots). A double root is a tangency — not a crossing."""
    def in_range(t: float) -> bool:
        return t >= t_lo - 1e-12 and t <= t_hi + 1e-12

    roots: list[float] = []
    scale = abs(c0) + abs(c1) + abs(c2) + 1e-300
    if abs(c2) <= 1e-15 * scale:  # linear in t
        if abs(c1) > 1e-15 * scale:
            t = -c0 / c1
            if in_range(t):
                roots.append(t)
        return roots
    disc = c1 * c1 - 4.0 * c2 * c0
    disc_scale = c1 * c1 + abs(4.0 * c2 * c0) + 1e-300
    if not (disc > 1e-12 * disc_scale):
        return roots
    sq = math.sqrt(disc)
    t1 = (-c1 - sq) / (2.0 * c2)
    t2 = (-c1 + sq) / (2.0 * c2)
    if in_range(t1):
        roots.append(t1)
    if in_range(t2):
        roots.append(t2)
    return roots


def _leg_surface_crossings(
    s: SurfaceGrid, a: PathPoint3D, b: PathPoint3D
) -> list[tuple[float, float]]:
    """Candidate crossings (t, md) of the leg with the surface: the leg's
    footprint is marched cell by cell and d(t) — a quadratic within one
    cell — is fitted from three samples and solved exactly (mirror of the
    C++ cell traversal)."""
    dx = b.x - a.x
    dy = b.y - a.y
    dmd = b.md - a.md
    ts = [0.0, 1.0]
    if abs(dx) > 0.0:
        for n in range(1, s.x_nodes - 1):
            line = s.x_origin_m + n * s.x_step_m
            t = (line - a.x) / dx
            if 0.0 < t < 1.0:
                ts.append(t)
    if abs(dy) > 0.0:
        for n in range(1, s.y_nodes - 1):
            line = s.y_origin_m + n * s.y_step_m
            t = (line - a.y) / dy
            if 0.0 < t < 1.0:
                ts.append(t)
    ts.sort()
    out: list[tuple[float, float]] = []
    for k in range(len(ts) - 1):
        t_lo, t_hi = ts[k], ts[k + 1]
        if not (t_hi > t_lo + 1e-15):
            continue  # degenerate interval (grid-node hit)
        m = 0.5 * (t_lo + t_hi)
        cell = _grid_cell(s, a.x + m * dx, a.y + m * dy)
        d_lo = _signed_distance_in_cell(s, cell, a, b, t_lo)
        d_mid = _signed_distance_in_cell(s, cell, a, b, m)
        d_hi = _signed_distance_in_cell(s, cell, a, b, t_hi)
        h = 0.5 * (t_hi - t_lo)
        A = d_mid
        B = 0.5 * (d_hi - d_lo)
        C = 0.5 * (d_lo + d_hi) - d_mid
        mh = m / h
        c0 = A - B * mh + C * mh * mh
        c1 = (B - 2.0 * C * mh) / h
        c2 = C / (h * h)
        for t in _quadratic_roots(c0, c1, c2, t_lo, t_hi):
            out.append((t, a.md + t * dmd))
    return out


def _unit_status(
    top: SurfaceGrid, bottom: SurfaceGrid, x: float, y: float, z: float
) -> bool:
    """Strictly below the top surface and above the bottom surface."""
    return (
        z - _surface_height(top, x, y) > 0.0
        and z - _surface_height(bottom, x, y) < 0.0
    )


def _position_at_md(
    path: Sequence[PathPoint3D], md: float
) -> tuple[float, float, float]:
    """(x, y, z) on the polyline path at a measured depth (coordinates are
    linear in md along each leg)."""
    k = 1
    while k + 1 < len(path) and path[k].md < md:
        k += 1
    a, b = path[k - 1], path[k]
    t = (md - a.md) / (b.md - a.md)
    return (
        a.x + t * (b.x - a.x),
        a.y + t * (b.y - a.y),
        a.z + t * (b.z - a.z),
    )


def _accumulate_unit_interval(
    path: Sequence[PathPoint3D],
    top: SurfaceGrid,
    bottom: SurfaceGrid,
    a_md: float,
    b_md: float,
) -> float:
    """TST of the in-unit sub-interval [a_md, b_md]: per leg, the covered
    part contributes ``length · |ŵ · n̂|`` with n̂ the normalized average of
    the two surfaces' interpolated normals at the covered part's midpoint
    (the documented local-parallel approximation)."""
    total = 0.0
    for k in range(1, len(path)):
        pa, pb = path[k - 1], path[k]
        lo = max(a_md, pa.md)
        hi = min(b_md, pb.md)
        if not (hi > lo):
            continue
        dx, dy, dz = pb.x - pa.x, pb.y - pa.y, pb.z - pa.z
        leg_length = math.sqrt(dx * dx + dy * dy + dz * dz)
        sub_length = leg_length * (hi - lo) / (pb.md - pa.md)
        t = ((lo + hi) * 0.5 - pa.md) / (pb.md - pa.md)
        mx = pa.x + t * dx
        my = pa.y + t * dy
        n0 = _surface_normal(top, mx, my)
        n1 = _surface_normal(bottom, mx, my)
        nx, ny, nz = n0[0] + n1[0], n0[1] + n1[1], n0[2] + n1[2]
        nnorm = math.sqrt(nx * nx + ny * ny + nz * nz)
        dot = abs(dx * nx + dy * ny + dz * nz) / (leg_length * nnorm)
        total += sub_length * dot
    return total


def _mirror_tst_along_surface_path(
    path: Sequence[PathPoint3D],
    surfaces: Sequence[SurfaceGrid],
) -> TrueStratigraphicThickness:
    """Exact mirror of ``tst_along_surface_path`` (tst.hpp): consecutive
    surfaces bound units; crossings are computed geometrically (per-cell
    quadratic roots), a candidate is a genuine crossing iff the in-unit
    status differs across it (tangencies skipped), every candidate is
    side-validated (surfaces must not cross along the well), and in-unit
    sub-segments contribute ``length · |ŵ · n̂|``."""
    if len(path) < 2:
        raise ValueError("路径至少需要两个点")
    for s in surfaces:
        _valid_surface_grid(s)
    total_md = 0.0
    for i in range(1, len(path)):
        a, b = path[i - 1], path[i]
        if not all(
            math.isfinite(v)
            for v in (a.md, a.x, a.y, a.z, b.md, b.x, b.y, b.z)
        ):
            raise ValueError("路径点必须有限")
        if not (b.md > a.md):
            raise ValueError("路径 MD 必须严格递增")
        dx, dy, dz = b.x - a.x, b.y - a.y, b.z - a.z
        leg_length = math.sqrt(dx * dx + dy * dy + dz * dz)
        if not (leg_length > 0.0):
            raise ValueError("零长度腿（MD 递增但位置不变）路径不一致")
        total_md += b.md - a.md
        for s in surfaces:
            if not (_in_footprint(s, a.x, a.y) and _in_footprint(s, b.x, b.y)):
                raise ValueError("路径点必须位于每个曲面的覆盖范围内")
    if len(surfaces) < 2:
        # No complete unit: a legal zero result (like an empty layer book).
        return TrueStratigraphicThickness(value=0.0, measured_interval_m=total_md)
    total = 0.0
    for u in range(len(surfaces) - 1):
        top, bottom = surfaces[u], surfaces[u + 1]
        # Unit ordering: bottom strictly below top at every path point.
        for p in path:
            if not (_surface_height(top, p.x, p.y) < _surface_height(bottom, p.x, p.y)):
                raise ValueError("曲面必须严格有序：底面须在顶面之下（沿井路径）")
        # Candidate crossings of both surfaces along every leg.
        events: list[tuple[float, int]] = []  # (md, surface_index)
        for i in range(1, len(path)):
            a, b = path[i - 1], path[i]
            dx, dy, dz = b.x - a.x, b.y - a.y, b.z - a.z
            leg_length = math.sqrt(dx * dx + dy * dy + dz * dz)
            wx, wy, wz = dx / leg_length, dy / leg_length, dz / leg_length
            for which, s in enumerate((top, bottom)):
                # A leg lying in a surface (both ends on it, direction in
                # the tangent plane) is coincident → input error.
                d_a = _signed_distance(s, a, b, 0.0)
                d_b = _signed_distance(s, a, b, 1.0)
                if abs(d_a) < 1e-9 and abs(d_b) < 1e-9:
                    n = _surface_normal(s, a.x, a.y)
                    if abs(wx * n[0] + wy * n[1] + wz * n[2]) < 1e-9:
                        raise ValueError("路径段与曲面重合（输入错误）")
                for t, md in _leg_surface_crossings(s, a, b):
                    events.append((md, which))
        events.sort(key=lambda e: (e[0], e[1]))
        # Deduplicate: the same (surface, md) may be reported by two
        # adjacent legs or cells at a shared boundary (path node / grid
        # line).
        candidates: list[tuple[float, int]] = []
        for e in events:
            if (
                candidates
                and candidates[-1][1] == e[1]
                and abs(candidates[-1][0] - e[0])
                <= 1e-9 * (1.0 + abs(e[0]))
            ):
                continue
            candidates.append(e)
        start_md = path[0].md
        end_md = path[-1].md
        # Side validation runs for every candidate (an inverted crossing is
        # exactly one that does not change the in-unit status — never a
        # silent zero-contribution merge); a candidate is a genuine crossing
        # iff the status differs across it (tangencies keep it unchanged).
        genuine: list[tuple[float, int]] = []
        prev_md = start_md
        for i, (md, which) in enumerate(candidates):
            cx, cy, cz = _position_at_md(path, md)
            if which == 0:
                if not (cz - _surface_height(bottom, cx, cy) < 0.0):
                    raise ValueError("曲面沿井路径交叉或重叠（输入错误）")
            elif not (cz - _surface_height(top, cx, cy) > 0.0):
                raise ValueError("曲面沿井路径交叉或重叠（输入错误）")
            next_md = candidates[i + 1][0] if i + 1 < len(candidates) else end_md
            bx, by, bz = _position_at_md(path, 0.5 * (prev_md + md))
            ax, ay, az = _position_at_md(path, 0.5 * (md + next_md))
            before = _unit_status(top, bottom, bx, by, bz)
            after = _unit_status(top, bottom, ax, ay, az)
            if before != after:
                genuine.append((md, which))
            prev_md = md
        # Accumulate the in-unit components (the status flips at every
        # genuine crossing).
        inside = _unit_status(top, bottom, path[0].x, path[0].y, path[0].z)
        boundary = start_md
        for md, _ in genuine:
            if inside:
                total += _accumulate_unit_interval(path, top, bottom, boundary, md)
            inside = not inside
            boundary = md
        if inside:
            total += _accumulate_unit_interval(path, top, bottom, boundary, end_md)
    return TrueStratigraphicThickness(
        value=total,
        measured_interval_m=total_md,
        normal_dot=total / total_md if total_md > 0.0 else 0.0,
    )


# ---------------------------------------------------------------------------
# Binding-first dispatch (SDK authoritative; mirror is the fallback)
# ---------------------------------------------------------------------------

_BINDING_CACHE: dict[str, object | None] = {}


def _binding_fn(name: str) -> object | None:
    """Resolve an SDK module-level binding function by name (cached)."""
    if name in _BINDING_CACHE:
        return _BINDING_CACHE[name]
    fn = None
    try:
        import welllog  # type: ignore

        candidate = getattr(welllog, name, None)
        if not callable(candidate):
            ext = getattr(welllog, "_QtWidgets", None)
            inner = getattr(ext, "welllog", None) if ext is not None else None
            if inner is not None:
                candidate = getattr(inner, name, None)
        if callable(candidate):
            fn = candidate
    except Exception:
        fn = None
    _BINDING_CACHE[name] = fn
    return fn


def _call_binding(name: str, *args: Any) -> Any | None:
    fn = _binding_fn(name)
    if fn is None:
        return None
    try:
        return fn(*args)
    except Exception:
        return None


def tst_through_layers(
    start_md: float,
    measured_length_m: float,
    well_direction: WellDirection3D,
    layers: Sequence[BeddingLayer],
) -> TrueStratigraphicThickness:
    """TST of a measured interval along a straight leg (binding-first)."""
    bound = _call_binding(
        "tst_through_layers", start_md, measured_length_m,
        well_direction, list(layers),
    )
    if bound is not None:
        return _result_from_bound(bound)
    return _mirror_tst_through_layers(
        start_md, measured_length_m, well_direction, layers
    )


def tst_along_path(
    path: Sequence[PathPoint3D],
    layers: Sequence[BeddingLayer],
) -> TrueStratigraphicThickness:
    """TST along a polyline path (binding-first)."""
    bound = _call_binding("tst_along_path", list(path), list(layers))
    if bound is not None:
        return _result_from_bound(bound)
    return _mirror_tst_along_path(path, layers)


def tst_along_surface_path(
    path: Sequence[PathPoint3D],
    surfaces: Sequence[SurfaceGrid],
) -> TrueStratigraphicThickness:
    """TST along a polyline path through a stack of bedding surfaces
    (binding-first): consecutive surfaces bound units; crossings are
    computed geometrically and the local normal is interpolated from the
    surfaces (see tst.hpp / the mirror docstring)."""
    bound = _call_binding("tst_along_surface_path", list(path), list(surfaces))
    if bound is not None:
        return _result_from_bound(bound)
    return _mirror_tst_along_surface_path(path, surfaces)


def _result_from_bound(bound: Any) -> TrueStratigraphicThickness:
    """Normalize a binding result (unknown shape) to the mirror result."""
    if isinstance(bound, TrueStratigraphicThickness):
        return bound
    try:
        value = float(bound.value if hasattr(bound, "value") else bound[0])
        measured = float(
            getattr(bound, "measured_interval_m", 0.0)
            if hasattr(bound, "measured_interval_m")
            else (bound[2] if len(bound) > 2 else 0.0)
        )
        dot = float(
            getattr(bound, "normal_dot", 0.0)
            if hasattr(bound, "normal_dot")
            else (bound[3] if len(bound) > 3 else 0.0)
        )
    except (TypeError, ValueError, IndexError):
        return _EMPTY_TST
    return TrueStratigraphicThickness(
        value=value, measured_interval_m=measured, normal_dot=dot
    )


_EMPTY_TST = TrueStratigraphicThickness(value=0.0)


# ---------------------------------------------------------------------------
# Well-path construction from the deviation survey
# ---------------------------------------------------------------------------


def path_from_trajectory(traj: SurveyTrajectory) -> list[PathPoint3D]:
    """PathPoint3D list from a computed trajectory (x=N, y=E, z=TVD down).

    Every survey station becomes a path point; consecutive stations form
    straight chord legs (the documented polyline approximation). Stations
    whose MD does not increase are skipped (carried forward by the survey
    computation) so the path stays monotonic.
    """
    out: list[PathPoint3D] = []
    md = np.asarray(traj.md, dtype=np.float64)
    for i in range(int(md.size)):
        pt = PathPoint3D(
            md=float(md[i]),
            x=float(traj.north[i]),
            y=float(traj.east[i]),
            z=float(traj.tvd[i]),
        )
        if out and not (pt.md > out[-1].md):
            continue
        out.append(pt)
    return out


# ---------------------------------------------------------------------------
# Bedding sidecar (wells/<id>/bedding.json) — mirrors the layer contract
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class SurfaceGridSpec:
    """Persisted surface grid (bedding.json v2): origin/steps/nodes + z.

    Mirrors the SDK SurfaceGrid contract; ``z_tvd`` is the flattened
    row-major (y outer, x inner) height array. Convert with ``to_grid()``
    before computing.
    """

    x_origin_m: float = 0.0
    y_origin_m: float = 0.0
    x_step_m: float = 1.0
    y_step_m: float = 1.0
    x_nodes: int = 2
    y_nodes: int = 2
    z_tvd: tuple[float, ...] = ()

    def to_grid(self) -> SurfaceGrid:
        return SurfaceGrid(
            x_origin_m=self.x_origin_m,
            y_origin_m=self.y_origin_m,
            x_step_m=self.x_step_m,
            y_step_m=self.y_step_m,
            x_nodes=self.x_nodes,
            y_nodes=self.y_nodes,
            z_tvd=list(self.z_tvd),
        )


@dataclass(frozen=True)
class BeddingLayerSpec:
    """Persisted bedding layer: MD interval + dip/azimuth (+ optional unit).

    Surface bedding (v2): the layer may be bounded by ``top_surface`` and
    ``bottom_surface`` grids instead of declared normals — its TST is then
    computed by ``tst_along_surface_path`` over the single unit they bound.
    A surface-typed layer must provide BOTH surfaces (one alone is an
    inconsistent spec — never a silent planar fallback). dip/azimuth and
    top/bottom MD remain as declared metadata.
    """

    top_md: float
    bottom_md: float
    dip_deg: float
    dip_azimuth_deg: float = 0.0
    unit_id: str = ""  # → stratigraphic dictionary (C1); empty = free
    source: str = ""
    top_surface: SurfaceGridSpec | None = None
    bottom_surface: SurfaceGridSpec | None = None

    def to_layer(self) -> BeddingLayer:
        return BeddingLayer(
            top_md=self.top_md,
            bottom_md=self.bottom_md,
            normal=normal_from_dip_azimuth(self.dip_deg, self.dip_azimuth_deg),
        )


def layer_surfaces(
    spec: BeddingLayerSpec,
) -> tuple[SurfaceGrid, SurfaceGrid] | None:
    """The unit-bounding surface pair of a surface-typed spec, or None for a
    planar spec. Exactly one surface present is an inconsistent spec and
    raises ValueError (never a silent planar fallback)."""
    has_top = spec.top_surface is not None
    has_bottom = spec.bottom_surface is not None
    if not has_top and not has_bottom:
        return None
    if not (has_top and has_bottom):
        raise ValueError("曲面层必须同时提供顶面与底面曲面（不允许单独一个）")
    assert spec.top_surface is not None and spec.bottom_surface is not None
    return spec.top_surface.to_grid(), spec.bottom_surface.to_grid()


def bedding_file_path(workspace: Workspace, well_id: str) -> Path:
    return well_data_dir(workspace, well_id) / BEDDING_FILENAME


def _surface_spec_from_json(item: Any) -> SurfaceGridSpec | None:
    """Parse a persisted surface grid; None when malformed (tolerant read)."""
    if not isinstance(item, dict):
        return None
    try:
        spec = SurfaceGridSpec(
            x_origin_m=float(item.get("x_origin_m", 0.0)),
            y_origin_m=float(item.get("y_origin_m", 0.0)),
            x_step_m=float(item.get("x_step_m", 1.0)),
            y_step_m=float(item.get("y_step_m", 1.0)),
            x_nodes=int(item.get("x_nodes", 2)),
            y_nodes=int(item.get("y_nodes", 2)),
            z_tvd=tuple(float(v) for v in item.get("z_tvd") or ()),
        )
    except (TypeError, ValueError):
        return None
    return spec


def _spec_from_json(item: dict[str, Any]) -> BeddingLayerSpec | None:
    try:
        top = float(item["top_md"])
        bottom = float(item["bottom_md"])
    except (KeyError, TypeError, ValueError):
        return None
    if not (math.isfinite(top) and math.isfinite(bottom)) or not (bottom > top):
        return None
    try:
        dip = float(item.get("dip_deg") or 0.0)
        az = float(item.get("dip_azimuth_deg") or 0.0)
    except (TypeError, ValueError):
        return None
    top_raw = item.get("top_surface")
    bottom_raw = item.get("bottom_surface")
    top_surface = _surface_spec_from_json(top_raw) if isinstance(top_raw, dict) else None
    bottom_surface = (
        _surface_spec_from_json(bottom_raw) if isinstance(bottom_raw, dict) else None
    )
    return BeddingLayerSpec(
        top_md=top,
        bottom_md=bottom,
        dip_deg=dip,
        dip_azimuth_deg=az,
        unit_id=str(item.get("unit_id") or ""),
        source=str(item.get("source") or ""),
        top_surface=top_surface,
        bottom_surface=bottom_surface,
    )


def load_bedding_for_well(
    workspace: Workspace, well_id: str
) -> tuple[list[BeddingLayerSpec], list[str]]:
    """Load the well bedding layers; missing/corrupt → ([], diagnostics)."""
    diagnostics: list[str] = []
    try:
        path = bedding_file_path(workspace, well_id)
    except Exception as exc:
        return [], [str(exc)]
    if not path.is_file():
        return [], []
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return [], [f"产状数据损坏: {exc}"]
    items = raw.get("layers") if isinstance(raw, dict) else raw
    if not isinstance(items, list):
        return [], ["产状数据格式无效"]
    specs: list[BeddingLayerSpec] = []
    for i, item in enumerate(items):
        if not isinstance(item, dict):
            diagnostics.append(f"跳过无效产状项 [{i}]")
            continue
        spec = _spec_from_json(item)
        if spec is None:
            diagnostics.append(f"跳过无效产状项 [{i}]（需 top_md/bottom_md 且 bottom>top）")
            continue
        specs.append(spec)
    specs.sort(key=lambda s: (s.top_md, s.bottom_md))
    return specs, diagnostics


def _surface_to_json(spec: SurfaceGridSpec) -> dict[str, Any]:
    return {
        "x_origin_m": spec.x_origin_m,
        "y_origin_m": spec.y_origin_m,
        "x_step_m": spec.x_step_m,
        "y_step_m": spec.y_step_m,
        "x_nodes": spec.x_nodes,
        "y_nodes": spec.y_nodes,
        "z_tvd": list(spec.z_tvd),
    }


def save_bedding_for_well(
    workspace: Workspace, well_id: str, specs: Iterable[BeddingLayerSpec]
) -> Path:
    """Persist the bedding layers beside the well data (atomic write)."""
    path = bedding_file_path(workspace, well_id)
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "schemaVersion": BEDDING_SCHEMA_VERSION,
        "well_id": well_id,
        "layers": [
            {
                "top_md": s.top_md,
                "bottom_md": s.bottom_md,
                "dip_deg": s.dip_deg,
                "dip_azimuth_deg": s.dip_azimuth_deg,
                **({"unit_id": s.unit_id} if s.unit_id else {}),
                **({"source": s.source} if s.source else {}),
                **(
                    {"top_surface": _surface_to_json(s.top_surface)}
                    if s.top_surface is not None
                    else {}
                ),
                **(
                    {"bottom_surface": _surface_to_json(s.bottom_surface)}
                    if s.bottom_surface is not None
                    else {}
                ),
            }
            for s in specs
        ],
    }
    tmp = path.with_suffix(".json.tmp")
    tmp.write_text(
        json.dumps(payload, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    tmp.replace(path)
    return path
