"""Inter-well fill bands + pinchout wedges for the correlation canvas.

Pure helpers (no Qt). Given tops per column, build fill geometry between
adjacent wells:

* **Bands** — consecutive shared horizon names on two columns become a
  full-span fill quad (unchanged MVP behaviour).
* **Pinchout wedges** (FRS §3.3) — when a horizon interval exists on one
  column but not the neighbour, the interval wedges out (zero-thickness
  extrapolation) toward the missing side instead of being dropped.

Depths are measured (pre-shift); callers add display shifts for paint.
Does not touch single-well rendering.
"""

from __future__ import annotations

from dataclasses import dataclass

from well_log_workstation.tops_model import FormationTop

# Which side collapses to zero thickness. ``"off"`` ⇒ a normal quad band.
PINCH_OFF = "off"
PINCH_LEFT = "left"  # zero thickness on the left column (wedge points left)
PINCH_RIGHT = "right"  # zero thickness on the right column (wedge points right)
PINCH_SIDES = (PINCH_OFF, PINCH_LEFT, PINCH_RIGHT)

PINCHOUT_MODE_OFF = "off"
PINCHOUT_MODE_LINEAR = "linear"  # straight-edge wedge to apex
PINCHOUT_MODES = (PINCHOUT_MODE_OFF, PINCHOUT_MODE_LINEAR)


def _clamp_factor(value: float, lo: float = 0.05, hi: float = 1.0) -> float:
    """Clamp the apex fraction into a sane range (not flush against a column)."""
    return max(lo, min(hi, float(value)))


@dataclass(frozen=True)
class InterwellFillBand:
    """One fill polygon between adjacent columns (measured-depth domain).

    ``pinch`` selects the wedge behaviour; default ``"off"`` keeps the
    classic 4-corner quad (apex fields unused).

    For a wedge, ``apex_frac`` is the apex x position as a fraction of the
    gap between the two column inner edges (0 = left inner edge, 1 = right
    inner edge). ``apex_depth`` is the measured depth at which the
    interval pinches to zero thickness (mid-interval of the source column).
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
    pinch: str = PINCH_OFF
    apex_frac: float = 0.5
    apex_depth: float = 0.0
    smooth: bool = False


def _column_index_by_name(
    tops: list[FormationTop],
) -> dict[str, float]:
    """Strip whitespace, drop NaN depths / empty names → name→depth map."""
    return {
        t.name.strip(): float(t.depth)
        for t in tops
        if t.name.strip() and t.depth == t.depth
    }


def _ordered_intervals(
    depth_by_name: dict[str, float],
    *,
    skip_names: set[str] = (),
    min_thickness: float = 1e-6,
) -> list[tuple[str, str, float, float]]:
    """Consecutive intervals on one column, sorted by depth, skipping shared.

    Returns ``(top_name, bottom_name, top_depth, bottom_depth)`` tuples with
    top shallower than bottom; thin or inverted intervals are dropped.
    """
    names = [n for n in depth_by_name if n not in skip_names]
    names.sort(key=lambda n: depth_by_name[n])
    out: list[tuple[str, str, float, float]] = []
    for a, b in zip(names, names[1:], strict=False):
        ta, tb = depth_by_name[a], depth_by_name[b]
        if ta > tb:
            ta, tb = tb, ta
        if tb - ta < min_thickness:
            continue
        out.append((a, b, ta, tb))
    return out


def build_interwell_fill_bands(
    tops_per_column: list[list[FormationTop]],
    *,
    min_thickness: float = 1e-6,
    pinchout_mode: str = PINCHOUT_MODE_OFF,
    pinchout_factor: float = 0.5,
    pinchout_smooth: bool = False,
) -> list[InterwellFillBand]:
    """Build fill bands for adjacent columns.

    Shared-horizon consecutive pairs always become quad bands. When
    ``pinchout_mode == "linear"``, intervals present on only one column
    additionally become wedge bands pointing at the missing side.

    Args:
        tops_per_column: one list of :class:`FormationTop` per column.
        min_thickness: drop intervals thinner than this (measured depth).
        pinchout_mode: ``"off"`` (default) or ``"linear"``.
        pinchout_factor: apex x fraction across the inter-column gap
            (clamped to ``[0.05, 1.0]``).
        pinchout_smooth: mark wedge bands for Bézier smoothing at paint time.

    Returns:
        Ordered bands (quads first per pair, then that pair's wedges).
    """
    if len(tops_per_column) < 2:
        return []
    if pinchout_mode not in PINCHOUT_MODES:
        pinchout_mode = PINCHOUT_MODE_OFF
    factor = _clamp_factor(pinchout_factor)
    want_pinch = pinchout_mode == PINCHOUT_MODE_LINEAR

    bands: list[InterwellFillBand] = []
    for i in range(len(tops_per_column) - 1):
        left_map = _column_index_by_name(tops_per_column[i])
        right_map = _column_index_by_name(tops_per_column[i + 1])
        shared = sorted(
            set(left_map) & set(right_map),
            key=lambda n: 0.5 * (left_map[n] + right_map[n]),
        )

        # 1) Shared consecutive pairs → full quads (legacy behaviour).
        if len(shared) >= 2:
            for a, b in zip(shared, shared[1:], strict=False):
                lt, rt = left_map[a], right_map[a]
                lb, rb = left_map[b], right_map[b]
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

        # 2) Unilateral intervals → pinchout wedges.
        if not want_pinch:
            continue
        shared_set = set(shared)
        # Left-column-only intervals wedge toward the right column.
        for a, b, ta, tb in _ordered_intervals(
            left_map, skip_names=shared_set, min_thickness=min_thickness
        ):
            mid = 0.5 * (ta + tb)
            bands.append(
                InterwellFillBand(
                    left_col=i,
                    right_col=i + 1,
                    top_name=a,
                    bottom_name=b,
                    left_top_depth=ta,
                    left_bottom_depth=tb,
                    right_top_depth=mid,
                    right_bottom_depth=mid,
                    label=f"{a}/{b}→尖灭",
                    pinch=PINCH_RIGHT,
                    apex_frac=factor,
                    apex_depth=mid,
                    smooth=pinchout_smooth,
                )
            )
        # Right-column-only intervals wedge toward the left column.
        for a, b, ta, tb in _ordered_intervals(
            right_map, skip_names=shared_set, min_thickness=min_thickness
        ):
            mid = 0.5 * (ta + tb)
            bands.append(
                InterwellFillBand(
                    left_col=i,
                    right_col=i + 1,
                    top_name=a,
                    bottom_name=b,
                    left_top_depth=mid,
                    left_bottom_depth=mid,
                    right_top_depth=ta,
                    right_bottom_depth=tb,
                    label=f"{a}/{b}←尖灭",
                    pinch=PINCH_LEFT,
                    apex_frac=factor,
                    apex_depth=mid,
                    smooth=pinchout_smooth,
                )
            )
    return bands
