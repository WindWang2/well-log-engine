"""trajectory_2d — thin wrapper around project_well_trajectory (Phase-2, T5).

T5 (#249): the Workstation trajectory display is a straight head->bottom
projection via ``geoviz_well_seismic_3d.well_geometry.project_well_trajectory``
(``_linspace_path``: head (x, y, kb) -> bottom (x, y, total_depth) straight
line; Z = MD). Phase-2 ignores real survey data (inclination/azimuth) even
if the user has it.

This module converts the Workstation's catalog well dict into the engine's
``WellHead`` dataclass and delegates - keeping the engine call behind a
thin seam so the Workstation never imports ``geoviz_well_seismic_3d``
directly elsewhere.
"""

from __future__ import annotations

import math

import numpy as np


def project_trajectory_2d(
    well: dict,
    n_samples: int = 32,
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Project a well dict to a straight head->bottom trajectory.

    Args:
        well: ``{"name", "x", "y", "bottom_x", "bottom_y", "total_depth_m",
        "kb_m"?}`` (catalog-derived wellhead).
        n_samples: number of points along the straight line.

    Returns:
        ``(xs, ys, md)`` three (N,) float64 arrays (x, y, MD depth).
    """
    from geoviz_well_seismic_3d.models import WellHead
    from geoviz_well_seismic_3d.well_geometry import project_well_trajectory
    from geoviz_well_seismic_3d.models import VerticalDomain

    head = WellHead(
        name=str(well.get("name", "")),
        x=float(well.get("x", 0.0)),
        y=float(well.get("y", 0.0)),
        bottom_x=float(well.get("bottom_x", well.get("x", 0.0))),
        bottom_y=float(well.get("bottom_y", well.get("y", 0.0))),
        total_depth_m=float(well.get("total_depth_m", 0.0)),
        kb_m=float(well.get("kb_m", 0.0)),
    )
    traj = project_well_trajectory(
        head,
        domain=VerticalDomain.MEASURED_DEPTH,
        td=None,
        n_samples=n_samples,
    )
    pts = np.asarray(traj.points, dtype=np.float64)  # (N, 3): (x, y, md)
    return pts[:, 0], pts[:, 1], pts[:, 2]


# ---------------------------------------------------------------------------
# Section-space trajectory projection (P1-C, FRS §3.1) — pure numpy, no geoviz.
#
# The section x-axis runs along the section line (azimuth). A well's closure
# (northing/easting displacement from the minimum-curvature survey) is
# projected onto that azimuth so the well's horizontal drift is visible in
# the section: its column shifts laterally and a curved trajectory polyline
# shows the deviated/horizontal segments.
# ---------------------------------------------------------------------------


def project_closure_to_section(
    traj: "SurveyTrajectory", azimuth_deg: float
) -> np.ndarray:
    """Project a trajectory's closure onto a section azimuth.

    Args:
        traj: computed survey trajectory (has ``north``/``east`` arrays).
        azimuth_deg: azimuth of the section line, degrees clockwise from
            north (0 = N–S section, 90 = E–W section).

    Returns:
        ``(N,)`` signed horizontal offset in metres per station (positive =
        toward the section azimuth direction).
    """
    from well_log_workstation.survey import SurveyTrajectory

    if not isinstance(traj, SurveyTrajectory) or traj.north.size == 0:
        return np.empty(0, dtype=np.float64)
    az = math.radians(float(azimuth_deg))
    north = np.asarray(traj.north, dtype=np.float64)
    east = np.asarray(traj.east, dtype=np.float64)
    # Dot (north, east) with the section direction unit vector.
    return north * math.cos(az) + east * math.sin(az)


def normalize_offsets(
    offsets_m: np.ndarray, well_spacing_m: float
) -> np.ndarray:
    """Convert metre offsets to well-index units (units of the well spacing).

    A positive offset of ``well_spacing_m`` maps to +1.0 (one column stride).
    Degenerate (non-positive) spacing returns zeros.
    """
    offsets = np.asarray(offsets_m, dtype=np.float64)
    if offsets.size == 0 or not well_spacing_m or well_spacing_m <= 0:
        return np.zeros_like(offsets) if offsets.size else np.empty(0)
    return offsets / float(well_spacing_m)


def section_trajectory_polyline(
    traj: "SurveyTrajectory",
    azimuth_deg: float,
    well_spacing_m: float,
    shift: float = 0.0,
) -> np.ndarray:
    """Build a section-space trajectory polyline for one well.

    Args:
        traj: computed survey trajectory.
        azimuth_deg: section-line azimuth (see :func:`project_closure_to_section`).
        well_spacing_m: typical inter-well spacing used to normalise offsets
            into well-index units.
        shift: display-depth shift (datum) added to each MD.

    Returns:
        ``(N, 2)`` polyline ``[x_offset_units, md + shift]``. Empty ``(0, 2)``
        for an empty trajectory.
    """
    if traj.md.size == 0:
        return np.empty((0, 2), dtype=np.float64)
    offsets = project_closure_to_section(traj, azimuth_deg)
    units = normalize_offsets(offsets, well_spacing_m)
    md = np.asarray(traj.md, dtype=np.float64)
    return np.column_stack([units, md + float(shift)])


def path_segment_frame(
    path: np.ndarray,
    sample_y: np.ndarray,
) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    """Per-sample position + containing-segment direction on an ``[x, y]`` path.

    Used by the Unfolded section mode (FRS §3.1): curve samples are placed on
    the wellbore polyline at their display depth, then offset laterally along
    the local normal — the direction returned here, mapped to pixels, gives
    that normal (perpendicular to the borehole as drawn).

    Args:
        path: ``(N, 2)`` ``[x_offset_units, display_depth]`` sorted ascending
            by display depth — e.g. the output of
            :func:`section_trajectory_polyline` (display depth = MD + shift,
            monotonic for a well-ordered survey).
        sample_y: ``(M,)`` display depths to evaluate; values outside the path
            range clamp to the nearest endpoint.

    Returns:
        ``(u, y, du, dy)`` each ``(M,)``: interpolated position and the
        direction of the segment containing the sample (endpoint delta in the
        path's own unit/depth space, un-normalised). Samples at the top/bottom
        clamp to the endpoint and carry the first/last segment direction; a
        single-row path yields a vertical direction ``(0, 1)`` (pure lateral
        normal); empty inputs return empty arrays.
    """
    path = np.asarray(path, dtype=np.float64)
    sample_y = np.asarray(sample_y, dtype=np.float64)
    n = path.shape[0]
    if n == 0 or sample_y.size == 0:
        return (
            np.empty(0, dtype=np.float64),
            np.empty(0, dtype=np.float64),
            np.empty(0, dtype=np.float64),
            np.empty(0, dtype=np.float64),
        )
    u = np.full_like(sample_y, np.nan)
    y = np.full_like(sample_y, np.nan)
    du = np.zeros_like(sample_y)
    dy = np.zeros_like(sample_y)
    if n == 1:
        u[:] = path[0, 0]
        y[:] = path[0, 1]
        dy[:] = 1.0
        return u, y, du, dy
    y = np.clip(sample_y, path[0, 1], path[-1, 1])
    u = np.interp(y, path[:, 1], path[:, 0])
    # Containing segment per sample (searchsorted on the display-depth column).
    idx = np.clip(np.searchsorted(path[:, 1], y, side="right") - 1, 0, n - 2)
    du = path[idx + 1, 0] - path[idx, 0]
    dy = path[idx + 1, 1] - path[idx, 1]
    return u, y, du, dy
