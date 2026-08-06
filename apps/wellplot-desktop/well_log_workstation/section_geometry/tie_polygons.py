"""tie_polygons — inter-well tie quads between adjacent wells (Phase-2, T4).

Pure numpy, headless. The reservoir section fills the interval between two
formation tops on adjacent wells with a quad. Quads support an optional
``pattern_id`` referencing a registered PatternDefinition for lithology
hatch fill (ADR 0050); when None the quad is solid-filled with
``fill_color``. Output quads are 4-corner polygons in section 2D space
(x across, y depth).
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np


@dataclass(frozen=True)
class TieQuad2D:
    """One inter-well tie quad in section 2D space (x across, y depth)."""

    corners: np.ndarray  # (4, 2) float64: [left_top, right_top, right_bottom, left_bottom]
    fill_color: str
    label: str = ""
    # Optional PatternDefinition EntityId for lithology hatch fill (ADR 0050).
    # None = solid fill_color (backward-compatible default).
    pattern_id: str | None = None


def tie_quads(
    tops_per_well: list[list[dict]],
    well_positions: list[tuple[float, float]],
    fill_color: str = "#cbd5e1",
    datum_shifts: dict[str, float] | None = None,
    pattern_id: str | None = None,
) -> list[TieQuad2D]:
    """Build inter-well tie quads between adjacent wells sharing a top name.

    Args:
        tops_per_well: per well column, a list of ``{"name", "depth"}`` tops
            (aligned by name across wells).
        well_positions: ``(x, y)`` per well in project CRS (section x-axis
            is cumulative distance).
        fill_color: solid fill for the quads (used when ``pattern_id`` is None).
        datum_shifts: per-well-name shift applied to top depths before
            section placement.
        pattern_id: optional PatternDefinition EntityId for lithology hatch
            fill (ADR 0050). None = solid ``fill_color``.

    Returns:
        One quad per adjacent well pair that shares at least one top name.
        Empty list when < 2 wells or no shared tops.
    """
    wells = np.asarray(well_positions, dtype=np.float64)
    if wells.ndim != 2 or wells.shape[0] < 2:
        return []

    well_dists = np.zeros(wells.shape[0], dtype=np.float64)
    for i in range(1, wells.shape[0]):
        well_dists[i] = well_dists[i - 1] + float(
            np.hypot(*(wells[i] - wells[i - 1]))
        )

    shifts = datum_shifts or {}
    tops_by_well: list[dict[str, float]] = []
    for i in range(wells.shape[0]):
        shift = shifts.get(str(i), 0.0)
        tops = tops_per_well[i] if i < len(tops_per_well) else []
        tops_by_well.append(
            {
                str(t.get("name")): float(t.get("depth", 0.0)) + shift
                for t in tops or []
                if t.get("depth") is not None
            }
        )

    quads: list[TieQuad2D] = []
    for i in range(len(wells) - 1):
        left = tops_by_well[i]
        right = tops_by_well[i + 1]
        shared = sorted(set(left) & set(right))
        if not shared:
            continue
        x0 = float(well_dists[i])
        x1 = float(well_dists[i + 1])
        # Quad spans the top-most shared top (smallest depth) down to the
        # bottom-most shared top (largest depth) - the reservoir interval
        # both wells have in common.
        y_top = min(left[n] for n in shared)
        y_bot = max(left[n] for n in shared)
        corners = np.array(
            [
                [x0, y_top],
                [x1, y_top],
                [x1, y_bot],
                [x0, y_bot],
            ],
            dtype=np.float64,
        )
        quads.append(
            TieQuad2D(
                corners=corners,
                fill_color=fill_color,
                label=",".join(shared[:2]),
                pattern_id=pattern_id,
            )
        )
    return quads
