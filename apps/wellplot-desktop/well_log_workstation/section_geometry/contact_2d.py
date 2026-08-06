"""contact_2d — OWC/GOC fluid-contact polylines between wells (Phase-2, T4).

Pure numpy, headless. The reservoir section renders fluid contacts (OWC
blue, GOC orange) as polylines connecting per-well contact depths across
the section x-axis. Contacts use a dotted/dashed pattern by default
(ADR 0050); color + position also distinguish the contact type.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np


@dataclass(frozen=True)
class ContactSegment2D:
    """One fluid-contact polyline in section 2D space (x across, y depth)."""

    points: np.ndarray  # (N, 2) float64
    fluid_type: str  # "owc" | "goc"
    color: str  # resolved from fluid_type
    width_mm: float = 0.35
    # Dash pattern as [on, off, ...] in mm (ADR 0050). Default dotted line —
    # the standard convention for fluid contacts.
    dash_pattern: tuple[float, ...] = (1.0, 1.0)


_CONTACT_COLORS = {
    "owc": "#2563eb",  # OWC blue
    "goc": "#f59e0b",  # GOC orange
}


def contact_polyline(
    contacts_per_well: list[dict],
    well_positions: list[tuple[float, float]],
    fluid_type: str = "owc",
    datum_shifts: dict[str, float] | None = None,
) -> list[ContactSegment2D]:
    """Build inter-well contact polylines from per-well contact depths.

    Args:
        contacts_per_well: list of dicts, one per well, in section column
            order: ``{"depth": float}`` (contact depth in MD) - or
            ``{"depth": None}`` when the well has no contact for this fluid.
        well_positions: ``(x, y)`` per well in project CRS (section x-axis
            is cumulative distance along the well polyline).
        fluid_type: ``"owc"`` or ``"goc"``.
        datum_shifts: per-well-name shift applied to contact depths before
            section placement (md/tvdss/horizon alignment).

    Returns:
        One :class:`ContactSegment2D` connecting the wells that have a
        contact (gaps are skipped - a contact line only spans adjacent
        wells that both define it). Empty list when < 2 wells have depths.
    """
    color = _CONTACT_COLORS.get(fluid_type, "#64748b")
    wells = np.asarray(well_positions, dtype=np.float64)
    if wells.ndim != 2 or wells.shape[0] < 2:
        return []

    well_dists = np.zeros(wells.shape[0], dtype=np.float64)
    for i in range(1, wells.shape[0]):
        well_dists[i] = well_dists[i - 1] + float(
            np.hypot(*(wells[i] - wells[i - 1]))
        )

    shifts = datum_shifts or {}
    pts: list[list[float]] = []
    for i in range(wells.shape[0]):
        contact = (
            contacts_per_well[i] if i < len(contacts_per_well) else None
        )
        depth = contact.get("depth") if isinstance(contact, dict) else None
        if depth is None:
            # Gap: flush the current run and continue.
            if len(pts) >= 2:
                break  # only the first contiguous run is drawn per contact
            continue
        try:
            d = float(depth)
        except (TypeError, ValueError):
            continue
        y = d + shifts.get(str(i), 0.0)
        pts.append([float(well_dists[i]), y])

    if len(pts) < 2:
        return []
    arr = np.asarray(pts, dtype=np.float64)
    return [ContactSegment2D(points=arr, fluid_type=fluid_type, color=color)]
