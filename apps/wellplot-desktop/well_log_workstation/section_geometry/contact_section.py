"""contact_section — 2D fluid-contact (OWC/GOC) model for the reservoir section.

Replaces the 3D-CRS ``contact_polyline`` path which had no real CRS context in
the host. A fluid contact here is defined directly in section 2D space: a
fluid type (oil-water / gas-oil contact) plus a per-well MD depth (a well may
omit a depth when the contact is absent there).

Pure numpy + dataclasses, headless (no Qt). Outputs feed SectionCanvas:
``contact_segment_2d`` for the contact line, ``split_quad_by_contact`` for the
dual-fill (above = oil/gas colour, below = water colour) of a tie-quad the
contact passes through.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Iterable, Mapping

import numpy as np

from well_log_workstation.section_geometry.tie_polygons import TieQuad2D

# Contact-line colours (FRS convention: OWC blue, GOC orange).
_OWC_COLOR = "#2563eb"
_GOC_COLOR = "#f59e0b"
_DEFAULT_COLOR = "#64748b"
_FLUID_COLORS = {"owc": _OWC_COLOR, "goc": _GOC_COLOR}

# Dual-fill colours for a quad split by a contact.
# Above contact (oil column for OWC, gas column for GOC).
_FILL_ABOVE = {"owc": "#dc2626", "goc": "#f59e0b"}  # oil red / gas yellow
# Below contact (water column).
_FILL_BELOW = {"owc": "#2563eb", "goc": "#dc2626"}  # water blue / oil red (under gas)


def _fluid_color(fluid_type: str) -> str:
    return _FLUID_COLORS.get(str(fluid_type).lower(), _DEFAULT_COLOR)


@dataclass(frozen=True)
class FluidContact2D:
    """One fluid contact (OWC or GOC) with per-well MD depths.

    ``depths`` maps well column index → contact MD; a well absent from the map
    has no contact for this fluid (the contact line breaks there).
    """

    fluid_type: str  # "owc" | "goc"
    depths: dict[int, float] = field(default_factory=dict)
    color: str = ""

    def resolved_color(self) -> str:
        return self.color or _fluid_color(self.fluid_type)


def contact_depth_at(contact: FluidContact2D, well_idx: int) -> float | None:
    d = contact.depths.get(int(well_idx))
    if d is None:
        return None
    try:
        return float(d)
    except (TypeError, ValueError):
        return None


def contact_segment_2d(
    contact: FluidContact2D, well_count: int
) -> list[np.ndarray]:
    """Return contact line segments in section 2D space (x = well index, y = MD).

    Only contiguous runs of adjacent wells that both define the contact are
    connected; each run becomes one ``(N, 2)`` array. Empty list when fewer
    than two adjacent wells define the contact.
    """
    segments: list[np.ndarray] = []
    run: list[tuple[float, float]] = []
    for i in range(well_count):
        d = contact_depth_at(contact, i)
        if d is None:
            if len(run) >= 2:
                segments.append(np.asarray(run, dtype=np.float64))
            run = []
            continue
        run.append((float(i), float(d)))
    if len(run) >= 2:
        segments.append(np.asarray(run, dtype=np.float64))
    return segments


def _interpolate_depth(
    contact: FluidContact2D, x: float
) -> float | None:
    """Linear-interpolate the contact depth at section-x (well-index space).

    ``x`` is a fractional well index; returns None if either bounding well
    lacks a contact depth.
    """
    lo = int(np.floor(x))
    hi = int(np.ceil(x))
    if lo == hi:
        return contact_depth_at(contact, lo)
    d_lo = contact_depth_at(contact, lo)
    d_hi = contact_depth_at(contact, hi)
    if d_lo is None or d_hi is None:
        return None
    frac = x - lo
    return d_lo + frac * (d_hi - d_lo)


def split_quad_by_contact(
    quad: TieQuad2D, contact: FluidContact2D, well_count: int
) -> tuple[TieQuad2D, TieQuad2D] | None:
    """Split a tie-quad into above/below sub-polygons along a fluid contact.

    The quad is split when the contact passes through it: the contact's
    interpolated depth at the quad's left and right x must both lie strictly
    inside the quad's depth extent on that side, and both bounding wells must
    define the contact. Returns ``(above, below)`` as two new TieQuad2D with
    fluid-appropriate fill colours (above = oil/gas, below = water/oil);
    otherwise None (quad left untouched by the caller).

    Quad corner order is ``[left_top, right_top, right_bottom, left_bottom]``.
    """
    corners = np.asarray(quad.corners, dtype=np.float64)
    if corners.shape != (4, 2):
        return None
    left_top, right_top, right_bottom, left_bottom = corners
    x_l = float(left_top[0])
    x_r = float(right_top[0])
    if x_l == x_r:
        return None  # zero-width quad; nothing to split

    # Contact depths at the quad's left/right edges (well-index space).
    d_l = _interpolate_depth(contact, x_l)
    d_r = _interpolate_depth(contact, x_r)
    if d_l is None or d_r is None:
        return None

    # Contact must cross BOTH vertical edges strictly inside the quad extent.
    top_l, bot_l = float(left_top[1]), float(left_bottom[1])
    top_r, bot_r = float(right_top[1]), float(right_bottom[1])
    lo_l, hi_l = min(top_l, bot_l), max(top_l, bot_l)
    lo_r, hi_r = min(top_r, bot_r), max(top_r, bot_r)
    if not (lo_l < d_l < hi_l) or not (lo_r < d_r < hi_r):
        return None

    fluid = str(contact.fluid_type).lower()
    above_color = _FILL_ABOVE.get(fluid, _DEFAULT_COLOR)
    below_color = _FILL_BELOW.get(fluid, _DEFAULT_COLOR)

    # Above polygon: left_top → right_top → (x_r, d_r) → (x_l, d_l)
    above_corners = np.array(
        [
            [x_l, top_l],
            [x_r, top_r],
            [x_r, d_r],
            [x_l, d_l],
        ],
        dtype=np.float64,
    )
    # Below polygon: (x_l, d_l) → (x_r, d_r) → right_bottom → left_bottom
    below_corners = np.array(
        [
            [x_l, d_l],
            [x_r, d_r],
            [x_r, bot_r],
            [x_l, bot_l],
        ],
        dtype=np.float64,
    )
    above = TieQuad2D(corners=above_corners, fill_color=above_color, label=quad.label)
    below = TieQuad2D(corners=below_corners, fill_color=below_color, label=quad.label)
    return above, below


# ---------------------------------------------------------------------------
# Serialization
# ---------------------------------------------------------------------------


def contact_to_json(contact: FluidContact2D) -> dict[str, Any]:
    return {
        "fluid_type": str(contact.fluid_type),
        "depths": [
            [int(idx), float(d)]
            for idx, d in sorted(contact.depths.items())
            if d is not None
        ],
        "color": contact.color,
    }


def contact_from_json(raw: Any) -> FluidContact2D | None:
    if not isinstance(raw, dict):
        return None
    fluid = str(raw.get("fluid_type") or "").strip().lower()
    if fluid not in ("owc", "goc"):
        return None
    depths: dict[int, float] = {}
    raw_depths = raw.get("depths")
    if isinstance(raw_depths, dict):
        iterable = raw_depths.items()  # type: ignore[union-attr]
    elif isinstance(raw_depths, list):
        iterable = raw_depths
    else:
        iterable = []
    for item in iterable:
        try:
            if isinstance(item, (list, tuple)) and len(item) >= 2:
                idx = int(item[0])
                d = float(item[1])
            elif isinstance(item, str):
                # "idx:depth"
                k, v = item.split(":", 1)
                idx, d = int(k), float(v)
            else:
                continue
        except (TypeError, ValueError):
            continue
        depths[idx] = d
    color = str(raw.get("color") or "")
    return FluidContact2D(fluid_type=fluid, depths=depths, color=color)


def contacts_to_json(contacts: Iterable[FluidContact2D]) -> list[dict[str, Any]]:
    return [contact_to_json(c) for c in contacts]


def contacts_from_json(raw: Any) -> list[FluidContact2D]:
    if not isinstance(raw, list):
        return []
    out: list[FluidContact2D] = []
    for item in raw:
        c = contact_from_json(item)
        if c is not None:
            out.append(c)
    return out
