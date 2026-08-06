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
