"""Correlation well-distance helpers (FRS §3.x 实际井距 / 纵横比例尺解耦).

Pure-Python geodesy + layout helpers, no Qt, headless-testable. The
correlation canvas places well columns at equal pixel stride by default;
``wellhead_offsets`` turns wellhead lng/lat into per-well lateral offsets
(in well-index units) so columns can be spread to real ground distance.

This mirrors the section canvas' ``well_spacing=geographic`` mode but uses
wellhead surface distance (works for vertical wells without surveys),
whereas the section path uses survey closure displacement projected on the
section azimuth.
"""

from __future__ import annotations

import math
from typing import Sequence

# Mean Earth radius, WGS84-authalic sphere (metres). Spherical haversine is
# accurate to ~0.5% — plenty for relative well spacing within a field.
_EARTH_R_M = 6_371_000.0


def haversine_m(
    lng0: float, lat0: float, lng1: float, lat1: float
) -> float:
    """Great-circle distance between two WGS84 lng/lat points (metres)."""
    lat0r = math.radians(float(lat0))
    lat1r = math.radians(float(lat1))
    dlat = lat1r - lat0r
    dlng = math.radians(float(lng1) - float(lng0))
    a = (
        math.sin(dlat / 2.0) ** 2
        + math.cos(lat0r) * math.cos(lat1r) * math.sin(dlng / 2.0) ** 2
    )
    c = 2.0 * math.atan2(math.sqrt(a), math.sqrt(max(0.0, 1.0 - a)))
    return _EARTH_R_M * c


def bearing_deg(
    lng0: float, lat0: float, lng1: float, lat1: float
) -> float:
    """Initial bearing from point 0 → 1, degrees clockwise from north (0–360)."""
    lat0r = math.radians(float(lat0))
    lat1r = math.radians(float(lat1))
    dlng = math.radians(float(lng1) - float(lng0))
    x = math.sin(dlng) * math.cos(lat1r)
    y = math.cos(lat0r) * math.sin(lat1r) - math.sin(lat0r) * math.cos(
        lat1r
    ) * math.cos(dlng)
    return (math.degrees(math.atan2(x, y)) + 360.0) % 360.0


def wellhead_offsets(
    well_positions: Sequence[tuple[float, float]],
) -> tuple[list[float] | None, float]:
    """Per-well lateral offsets (well-index units) from wellhead lng/lat.

    Walks the well order accumulating haversine distance between adjacent
    wells, then normalises to well-index units so the *mean* inter-well gap
    still occupies one column stride (directly feedable into the canvas'
    offset interpolation, same convention as the section canvas). Wells with
    no coordinates (lng/lat both ``None``) trigger a graceful degradation:
    ``(None, 0.0)`` so the caller falls back to equal spacing.

    Returns:
        ``(offsets, spacing_m)`` where ``offsets[i]`` is well ``i``'s
        cumulative distance from well 0 in units of the mean gap, and
        ``spacing_m`` is the mean inter-well distance in metres.
    """
    pts: list[tuple[float, float]] = []
    for p in well_positions:
        lng = p[0]
        lat = p[1]
        # Missing coordinate (None / non-finite) → degrade to equal spacing.
        if lng is None or lat is None:
            return None, 0.0
        try:
            lng = float(lng)
            lat = float(lat)
        except (TypeError, ValueError):
            return None, 0.0
        if not (math.isfinite(lng) and math.isfinite(lat)):
            return None, 0.0
        pts.append((lng, lat))
    n = len(pts)
    if n == 0:
        return [], 0.0
    if n == 1:
        return [0.0], 0.0
    cum = [0.0] * n
    total = 0.0
    for i in range(1, n):
        lng0, lat0 = pts[i - 1]
        lng1, lat1 = pts[i]
        seg = haversine_m(lng0, lat0, lng1, lat1)
        total += seg
        cum[i] = total
    mean_gap = total / float(n - 1) if total > 0.0 else 0.0
    if mean_gap <= 0.0:
        return [0.0] * n, 0.0
    offsets = [c / mean_gap for c in cum]
    return offsets, mean_gap
