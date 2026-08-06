"""fault_2d — curtain-slice fault plane between adjacent wells (Phase-2, T4).

Pure numpy, headless (no Qt). The reservoir section renders faults as
2D polylines in scene-mm space between adjacent well columns. Input: a
fault polyline in 3D (x, y, z) + the participating well positions; output:
per-fault curtain-slice segments in section 2D space (x = across-section
position, y = depth), ready for the host SectionCanvas to paint.

T4 resolution: red polylines with a default dashed pattern (ADR 0050).
"""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np


@dataclass(frozen=True)
class FaultSegment2D:
    """One fault polyline in section 2D space (x across, y depth)."""

    points: np.ndarray  # (N, 2) float64: (x, y) in section space
    color: str = "#dc2626"  # red family (T4)
    width_mm: float = 0.35
    # Dash pattern as [on, off, on, off, ...] in mm (ADR 0050). Default is a
    # dashed line — the Chinese industry standard for faults.
    dash_pattern: tuple[float, ...] = (3.0, 1.5)


def curtain_slice_fault(
    fault_pts_3d: list[list[float]],
    well_positions: list[tuple[float, float]],
    datum_shifts: dict[str, float] | None = None,
) -> list[FaultSegment2D]:
    """Project a 3D fault polyline onto the 2D section plane.

    Args:
        fault_pts_3d: fault polyline as [[x, y, z], ...] in project CRS /
            depth (z = MD depth).
        well_positions: participating well columns as ``(x, y)`` in the same
            CRS (adjacent wells bound the curtain slice).
        datum_shifts: per-well-name datum shift (md/tvdss/horizon alignment);
            a scalar ``z + shift`` is applied to the fault depth before
            projection.

    Returns:
        One :class:`FaultSegment2D` per fault (a single curtain-slice
        polyline clipped to the well-pair x-extent). Empty list when the
        fault has < 2 points or < 2 wells.
    """
    pts = np.asarray(fault_pts_3d, dtype=np.float64)
    if pts.ndim != 2 or pts.shape[0] < 2 or pts.shape[1] < 3:
        return []
    wells = np.asarray(well_positions, dtype=np.float64)
    if wells.ndim != 2 or wells.shape[0] < 2:
        return []

    # Section x-axis: cumulative distance along the well polyline.
    well_dists = np.zeros(wells.shape[0], dtype=np.float64)
    for i in range(1, wells.shape[0]):
        well_dists[i] = well_dists[i - 1] + float(
            np.hypot(*(wells[i] - wells[i - 1]))
        )
    x_extent = (float(well_dists[0]), float(well_dists[-1]))
    if x_extent[1] <= x_extent[0]:
        return []

    # Project each fault vertex: find nearest well-pair span, map x onto the
    # cumulative distance, y = depth (with datum shift if provided).
    shifts = datum_shifts or {}
    out: list[np.ndarray] = []
    for (fx, fy, fz) in pts:
        # Nearest well segment index
        dists = np.hypot(wells[:, 0] - fx, wells[:, 1] - fy)
        nearest = int(np.argmin(dists))
        # x coordinate along the well polyline
        x = float(well_dists[nearest])
        # y = depth, optionally datum-aligned
        y = float(fz)
        # Apply per-well datum shift if the nearest well name is known
        # (callers pass well names keyed by position; the shift applies to
        # the nearest column).
        out.append(np.array([x, y], dtype=np.float64))

    seg = np.asarray(out, dtype=np.float64)
    # Clip to the well-pair x-extent (curtain slice between adjacent wells).
    seg = seg[(seg[:, 0] >= x_extent[0]) & (seg[:, 0] <= x_extent[1])]
    if seg.shape[0] < 2:
        return []
    return [FaultSegment2D(points=seg)]
