"""fault_section — 2D position+throw fault model for the reservoir section.

Replaces the 3D-CRS ``curtain_slice_fault`` path which had no real 3D
context in the host. A fault here is defined directly in section 2D space:
which two adjacent wells it falls between, where across that gap (x_frac),
its depth extent, and its throw (normal fault > 0 drops the down-dip side;
reverse < 0 lifts it).

Pure numpy + dataclasses, headless (no Qt). Outputs feed SectionCanvas:
``fault_polyline`` for the fault line, ``apply_fault_throw_to_quad`` for the
visual offset of tie-quad corners on the down-dip side.
"""

from __future__ import annotations

from dataclasses import dataclass, field, replace
from typing import Any, Iterable, Sequence

import numpy as np

from well_log_workstation.section_geometry.tie_polygons import TieQuad2D

_FAULT_COLOR = "#dc2626"  # red family (FRS / SY/T standard for faults)
_FAULT_DASH: tuple[float, ...] = (3.0, 1.5)


def _clamp_x_frac(value: float) -> float:
    return max(0.0, min(1.0, float(value)))


@dataclass(frozen=True)
class SectionFault2D:
    """One fault in section 2D space (x across wells, y = MD depth).

    ``between`` holds the adjacent well column indices the fault falls
    between; ``x_frac`` is the position across that gap (0 = at the left
    well, 1 = at the right well). ``throw`` is the vertical displacement of
    the **down-dip side** (the side with greater x, i.e. the right side of
    the fault): positive for normal faults (down-dip drops), negative for
    reverse faults (down-dip lifts).
    """

    name: str
    between: tuple[int, int]
    x_frac: float = 0.5
    top_depth: float = 0.0
    bottom_depth: float = 0.0
    throw: float = 0.0
    color: str = _FAULT_COLOR
    dash_pattern: tuple[float, ...] = _FAULT_DASH


def _well_cumulative_distances(well_count: int) -> np.ndarray:
    """Evenly spaced section-x positions: well i sits at x = i (unit gap).

    The host canvas maps these to pixels via the well-index fraction; we keep
    a unit-gap convention here so geometry is CRS-independent.
    """
    return np.arange(well_count, dtype=np.float64)


def fault_x(fault: SectionFault2D, well_count: int) -> float:
    """Section-x coordinate of the fault plane (unit-gap convention)."""
    left, right = fault.between
    if well_count <= 0:
        return 0.0
    left = max(0, min(well_count - 1, int(left)))
    right = max(0, min(well_count - 1, int(right)))
    if right == left:
        return float(left)
    frac = _clamp_x_frac(fault.x_frac)
    return float(left) + frac * (right - left)


def fault_polyline(fault: SectionFault2D, well_count: int) -> np.ndarray:
    """Return the fault plane as a 2-point polyline (top, bottom) in section space.

    Output shape ``(2, 2)``: rows are [x, depth] for the top and bottom of the
    fault plane. Empty array ``(0, 2)`` when the fault's well pair is invalid
    for the given well count.
    """
    left, right = fault.between
    if (
        well_count < 2
        or left < 0
        or right >= well_count
        or left == right
    ):
        return np.empty((0, 2), dtype=np.float64)
    x = fault_x(fault, well_count)
    top = min(fault.top_depth, fault.bottom_depth)
    bottom = max(fault.top_depth, fault.bottom_depth)
    return np.array([[x, top], [x, bottom]], dtype=np.float64)


def _quad_x_extent(corners: np.ndarray) -> tuple[float, float]:
    xs = corners[:, 0]
    return float(np.min(xs)), float(np.max(xs))


def _quad_y_extent(corners: np.ndarray) -> tuple[float, float]:
    ys = corners[:, 1]
    return float(np.min(ys)), float(np.max(ys))


def apply_fault_throw_to_quad(
    quad: TieQuad2D, fault: SectionFault2D, well_count: int
) -> TieQuad2D:
    """Displace the down-dip side of a quad according to the fault throw.

    A quad is affected when it spans the fault plane horizontally (its x
    extent straddles the fault x) and overlaps the fault's depth range. For
    each corner on the down-dip side (x > fault_x), ``throw`` is added to
    its depth (y). The quad is returned unchanged otherwise.

    Returns a new :class:`TieQuad2D` (corners copied); other fields preserved.
    """
    fx = fault_x(fault, well_count)
    corners = np.asarray(quad.corners, dtype=np.float64)
    if corners.shape != (4, 2):
        return quad
    x_min, x_max = _quad_x_extent(corners)
    if not (x_min < fx < x_max):
        # Quad must straddle the fault (strictly, so a quad flush with the
        # fault is not split — it sits on one side).
        return quad
    ftop = min(fault.top_depth, fault.bottom_depth)
    fbot = max(fault.top_depth, fault.bottom_depth)
    if fbot <= ftop:
        return quad
    y_min, y_max = _quad_y_extent(corners)
    if y_max < ftop or y_min > fbot:
        return quad
    if fault.throw == 0.0:
        return quad

    new_corners = corners.copy()
    for i in range(4):
        if new_corners[i, 0] > fx:
            new_corners[i, 1] += float(fault.throw)
    return replace(quad, corners=new_corners)


def apply_faults_to_quad(
    quad: TieQuad2D, faults: Iterable[SectionFault2D], well_count: int
) -> TieQuad2D:
    """Apply every fault's throw to a quad in sequence."""
    out = quad
    for fault in faults:
        out = apply_fault_throw_to_quad(out, fault, well_count)
    return out


# ---------------------------------------------------------------------------
# Serialization
# ---------------------------------------------------------------------------


def fault_to_json(fault: SectionFault2D) -> dict[str, Any]:
    return {
        "name": fault.name,
        "between": [int(fault.between[0]), int(fault.between[1])],
        "x_frac": _clamp_x_frac(fault.x_frac),
        "top_depth": float(fault.top_depth),
        "bottom_depth": float(fault.bottom_depth),
        "throw": float(fault.throw),
        "color": fault.color,
        "dash_pattern": list(fault.dash_pattern),
    }


def fault_from_json(raw: Any) -> SectionFault2D | None:
    if not isinstance(raw, dict):
        return None
    try:
        name = str(raw.get("name") or "").strip()
        between_raw = raw.get("between") or [0, 1]
        if not isinstance(between_raw, (list, tuple)) or len(between_raw) < 2:
            return None
        left = int(between_raw[0])
        right = int(between_raw[1])
        if left == right:
            right = left + 1
        dash_raw = raw.get("dash_pattern")
        dash = (
            tuple(float(x) for x in dash_raw)
            if isinstance(dash_raw, (list, tuple)) and dash_raw
            else _FAULT_DASH
        )
        return SectionFault2D(
            name=name or f"断层 {left}-{right}",
            between=(left, right),
            x_frac=_clamp_x_frac(float(raw.get("x_frac", 0.5))),
            top_depth=float(raw.get("top_depth", 0.0)),
            bottom_depth=float(raw.get("bottom_depth", 0.0)),
            throw=float(raw.get("throw", 0.0)),
            color=str(raw.get("color") or _FAULT_COLOR),
            dash_pattern=dash,
        )
    except (TypeError, ValueError):
        return None


def faults_to_json(faults: Sequence[SectionFault2D]) -> list[dict[str, Any]]:
    return [fault_to_json(f) for f in faults]


def faults_from_json(raw: Any) -> list[SectionFault2D]:
    if not isinstance(raw, list):
        return []
    out: list[SectionFault2D] = []
    for item in raw:
        fault = fault_from_json(item)
        if fault is not None:
            out.append(fault)
    return out
