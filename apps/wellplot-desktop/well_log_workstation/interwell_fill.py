"""Inter-well fill bands for correlation canvas MVP (#297 / T9).

Pure helpers (no Qt). Given tops per column, build fill segments between
adjacent wells for consecutive shared horizon names (sorted by mean depth).

Does not touch single-well rendering — only produces geometry for the
correlation host canvas to paint.
"""

from __future__ import annotations

from dataclasses import dataclass

from well_log_workstation.tops_model import FormationTop


@dataclass(frozen=True)
class InterwellFillBand:
    """One fill polygon between adjacent columns (screen-depth domain).

    Depths are **measured** (pre-shift). Callers add display shifts for paint.
    """

    left_col: int
    right_col: int
    top_name: str
    bottom_name: str
    left_top_depth: float
    right_top_depth: float
    left_bottom_depth: float
    right_bottom_depth: float
    label: str = ""


def build_interwell_fill_bands(
    tops_per_column: list[list[FormationTop]],
    *,
    min_thickness: float = 1e-6,
) -> list[InterwellFillBand]:
    """Build fill bands for consecutive shared tops on adjacent columns.

    For each pair of adjacent columns, take the intersection of top names,
    order by average MD, and emit a band between each consecutive pair.
    """
    if len(tops_per_column) < 2:
        return []

    bands: list[InterwellFillBand] = []
    for i in range(len(tops_per_column) - 1):
        left_map = {
            t.name.strip(): float(t.depth)
            for t in tops_per_column[i]
            if t.name.strip() and t.depth == t.depth
        }
        right_map = {
            t.name.strip(): float(t.depth)
            for t in tops_per_column[i + 1]
            if t.name.strip() and t.depth == t.depth
        }
        shared = sorted(
            set(left_map) & set(right_map),
            key=lambda n: 0.5 * (left_map[n] + right_map[n]),
        )
        if len(shared) < 2:
            continue
        for a, b in zip(shared, shared[1:], strict=False):
            lt, rt = left_map[a], right_map[a]
            lb, rb = left_map[b], right_map[b]
            # Ensure top is shallower (smaller MD) on average
            if 0.5 * (lt + rt) > 0.5 * (lb + rb):
                a, b = b, a
                lt, rt, lb, rb = lb, rb, lt, rt
            if min(lb - lt, rb - rt) < min_thickness:
                continue
            bands.append(
                InterwellFillBand(
                    left_col=i,
                    right_col=i + 1,
                    top_name=a,
                    bottom_name=b,
                    left_top_depth=lt,
                    right_top_depth=rt,
                    left_bottom_depth=lb,
                    right_bottom_depth=rb,
                    label=f"{a}/{b}",
                )
            )
    return bands
