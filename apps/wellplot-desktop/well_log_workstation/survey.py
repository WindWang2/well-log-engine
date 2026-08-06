"""Deviation survey → trajectory (FRS §1.1 / P1-C).

Computes true vertical depth (TVD), TVD subsea (TVDSS), northing/easting
displacement and closure distance from MD / inclination / azimuth survey
stations using the **minimum-curvature** method (industry standard).

Pure numpy, headless (no Qt). Single-point (vertical) wells degenerate to
TVD = MD with zero displacement. This is the math layer; the host stores
survey data per well (``wells/<id>/survey.json``) and feeds it to
``WellSectionDatum(mode='tvd')``.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from typing import Any, Iterable, Sequence

import numpy as np


@dataclass(frozen=True)
class SurveyStation:
    """One survey station: measured depth, inclination and azimuth (degrees)."""

    md: float
    inc_deg: float
    az_deg: float


@dataclass(frozen=True)
class SurveyTrajectory:
    """Computed trajectory arrays, all length N (one per station)."""

    md: np.ndarray
    tvd: np.ndarray
    tvdss: np.ndarray
    north: np.ndarray
    east: np.ndarray
    closure_dist: np.ndarray
    kb_m: float = 0.0

    def __len__(self) -> int:
        return int(self.md.size)


_EMPTY = SurveyTrajectory(
    md=np.empty(0),
    tvd=np.empty(0),
    tvdss=np.empty(0),
    north=np.empty(0),
    east=np.empty(0),
    closure_dist=np.empty(0),
    kb_m=0.0,
)


def _ratio_factor(dogleg_deg: float) -> float:
    """Minimum-curvature ratio factor (RF).

    RF = (2 / dogleg) * tan(dogleg / 2). For very small doglegs this tends to
    1; the Taylor expansion avoids 0/0 and the tangent of a tiny angle.
    """
    if dogleg_deg < 1e-9:
        return 1.0
    dl = math.radians(dogleg_deg)
    return (2.0 / dl) * math.tan(dl / 2.0)


def _dogleg(inc1: float, az1: float, inc2: float, az2: float) -> float:
    """Total angle change (dogleg, degrees) between two stations.

    Uses the spherical-cosine form ``cos(DL) = cos(Δinc) + sin(inc1)sin(inc2)
    (cos(Δaz) - 1)``; clamps the argument to [-1, 1] for numerical safety.
    """
    di = inc1 - inc2
    da = az1 - az2
    cos_dl = math.cos(math.radians(di)) + (
        math.sin(math.radians(inc1))
        * math.sin(math.radians(inc2))
        * (math.cos(math.radians(da)) - 1.0)
    )
    cos_dl = max(-1.0, min(1.0, cos_dl))
    return math.degrees(math.acos(cos_dl))


def compute_trajectory(
    stations: Sequence[SurveyStation], *, kb_m: float = 0.0
) -> SurveyTrajectory:
    """Compute TVD / TVDSS / N / E / closure from survey stations.

    Args:
        stations: ordered survey stations (MD increasing). An empty sequence
            returns an empty trajectory; a single station yields TVD = MD with
            zero displacement (treated as vertical at that point).
        kb_m: kelly-bushing / rotary-table elevation (metres above MSL, positive
            upward). TVDSS = kb_m - TVD. Defaults to 0 (TVDSS == TVD).
    """
    pts = [(float(s.md), float(s.inc_deg), float(s.az_deg)) for s in stations]
    n = len(pts)
    if n == 0:
        return _EMPTY
    md = np.empty(n, dtype=np.float64)
    tvd = np.empty(n, dtype=np.float64)
    north = np.empty(n, dtype=np.float64)
    east = np.empty(n, dtype=np.float64)

    md[0] = pts[0][0]
    tvd[0] = pts[0][0] if pts[0][1] == 0.0 else 0.0
    # First station: if vertical (inc 0), TVD accumulates from 0 to md;
    # otherwise the well starts deviating immediately — place TVD at 0 and
    # let the first segment add to it.
    tvd[0] = 0.0 if n > 1 else pts[0][0]
    north[0] = 0.0
    east[0] = 0.0

    for i in range(1, n):
        md0, inc0, az0 = pts[i - 1]
        md1, inc1, az1 = pts[i]
        dmd = md1 - md0
        if dmd <= 0:
            # Non-monotonic MD: carry previous values forward.
            tvd[i] = tvd[i - 1]
            north[i] = north[i - 1]
            east[i] = east[i - 1]
            md[i] = md1
            continue
        dl = _dogleg(inc0, az0, inc1, az1)
        rf = _ratio_factor(dl)
        i0, a0 = math.radians(inc0), math.radians(az0)
        i1, a1 = math.radians(inc1), math.radians(az1)
        # Minimum-curvature incremental displacements.
        half = 0.5 * dmd * rf
        tvd[i] = tvd[i - 1] + half * (
            math.cos(i0) + math.cos(i1)
        )
        north[i] = north[i - 1] + half * (
            math.sin(i0) * math.cos(a0) + math.sin(i1) * math.cos(a1)
        )
        east[i] = east[i - 1] + half * (
            math.sin(i0) * math.sin(a0) + math.sin(i1) * math.sin(a1)
        )
        md[i] = md1

    # For a single vertical station TVD must equal MD.
    if n == 1 and pts[0][1] == 0.0:
        tvd[0] = pts[0][0]

    closure = np.hypot(north, east)
    tvdss = float(kb_m) - tvd
    return SurveyTrajectory(
        md=md,
        tvd=tvd,
        tvdss=tvdss,
        north=north,
        east=east,
        closure_dist=closure,
        kb_m=float(kb_m),
    )


def interpolate_tvd(traj: SurveyTrajectory, md_value: float) -> float:
    """Linear-interpolate TVD at a given MD along the trajectory.

    Clamps to the first/last TVD when ``md_value`` is outside the surveyed
    range. Returns 0.0 for an empty trajectory.
    """
    if traj.md.size == 0:
        return 0.0
    if traj.md.size == 1:
        return float(traj.tvd[0])
    return float(np.interp(md_value, traj.md, traj.tvd))


# ---------------------------------------------------------------------------
# Serialization
# ---------------------------------------------------------------------------


def survey_to_json(stations: Iterable[SurveyStation]) -> list[dict[str, Any]]:
    return [
        {"md": float(s.md), "inc": float(s.inc_deg), "az": float(s.az_deg)}
        for s in stations
    ]


def survey_from_json(raw: Any) -> list[SurveyStation]:
    if not isinstance(raw, list):
        return []
    out: list[SurveyStation] = []
    for item in raw:
        if not isinstance(item, dict):
            continue
        try:
            out.append(
                SurveyStation(
                    md=float(item.get("md", item.get("depth", 0.0))),
                    inc_deg=float(item.get("inc", item.get("inclination", 0.0))),
                    az_deg=float(item.get("az", item.get("azimuth", 0.0))),
                )
            )
        except (TypeError, ValueError):
            continue
    return out
