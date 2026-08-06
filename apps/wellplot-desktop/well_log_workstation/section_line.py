"""Section-line well picking (FRS §4.2 / P2-B, workflow 1).

Given a section line (two endpoints in lng/lat) and a buffer distance, pick
the wells whose heads fall inside the buffer and order them along the line.
Pure numpy, headless — the dialog and shell wiring sit on top of this.

Distances use a local lng/lat planar approximation (fine for field-scale
workspaces); spherical accuracy is out of scope.
"""

from __future__ import annotations

import math
from typing import Sequence

from well_log_workstation.workspace import WellCatalogEntry


def point_to_segment_distance(
    lng: float, lat: float,
    p0: tuple[float, float], p1: tuple[float, float],
) -> float:
    """Distance from a point to the segment p0→p1 (planar lng/lat)."""
    x0, y0 = float(lng), float(lat)
    ax, ay = float(p0[0]), float(p0[1])
    bx, by = float(p1[0]), float(p1[1])
    dx, dy = bx - ax, by - ay
    seg_len_sq = dx * dx + dy * dy
    if seg_len_sq <= 0.0:
        return math.hypot(x0 - ax, y0 - ay)
    t = ((x0 - ax) * dx + (y0 - ay) * dy) / seg_len_sq
    t = max(0.0, min(1.0, t))
    cx, cy = ax + t * dx, ay + t * dy
    return math.hypot(x0 - cx, y0 - cy)


def project_onto_segment(
    lng: float, lat: float,
    p0: tuple[float, float], p1: tuple[float, float],
) -> float:
    """Projection parameter t ∈ [0, 1] of the point's foot onto p0→p1."""
    x0, y0 = float(lng), float(lat)
    ax, ay = float(p0[0]), float(p0[1])
    dx = float(p1[0]) - ax
    dy = float(p1[1]) - ay
    seg_len_sq = dx * dx + dy * dy
    if seg_len_sq <= 0.0:
        return 0.0
    t = ((x0 - ax) * dx + (y0 - ay) * dy) / seg_len_sq
    return max(0.0, min(1.0, t))


def _well_lng_lat(well: WellCatalogEntry) -> tuple[float, float] | None:
    if well.lng is None or well.lat is None:
        return None
    try:
        return float(well.lng), float(well.lat)
    except (TypeError, ValueError):
        return None


def pick_wells_along_line(
    wells: Sequence[WellCatalogEntry],
    p0: tuple[float, float],
    p1: tuple[float, float],
    *,
    buffer_deg: float,
    max_wells: int | None = None,
) -> list[WellCatalogEntry]:
    """Pick + order wells along a section line.

    Args:
        wells: workspace catalog entries.
        p0, p1: section-line endpoints (lng, lat).
        buffer_deg: pick radius around the line (planar degrees).
        max_wells: optional cap on the returned count (line direction order).

    Returns:
        Wells with coordinates inside the buffer, ordered by their projection
        along the line (p0 → p1). Wells without coordinates are skipped.
    """
    if buffer_deg <= 0.0:
        return []
    picked: list[tuple[float, WellCatalogEntry]] = []
    for well in wells:
        pos = _well_lng_lat(well)
        if pos is None:
            continue
        if point_to_segment_distance(pos[0], pos[1], p0, p1) <= float(buffer_deg):
            t = project_onto_segment(pos[0], pos[1], p0, p1)
            picked.append((t, well))
    picked.sort(key=lambda item: item[0])
    out = [w for _, w in picked]
    if max_wells is not None and max_wells > 0:
        out = out[: max_wells]
    return out
