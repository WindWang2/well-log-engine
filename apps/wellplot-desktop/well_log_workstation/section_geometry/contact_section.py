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

    ``transition_m`` is the full thickness (m) of an optional transition zone
    centred on the contact: when > 0, ``split_quad_by_contact`` emits a middle
    band with a blended fill (oil↔water or gas↔oil) instead of a hard colour
    jump. Default 0 preserves the sharp dual-fill.
    """

    fluid_type: str  # "owc" | "goc"
    depths: dict[int, float] = field(default_factory=dict)
    color: str = ""
    transition_m: float = 0.0

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


def segment_by_depths(
    depths: Mapping[int, float], well_count: int
) -> list[np.ndarray]:
    """Depth-line segments in section 2D space (x = well index, y = MD).

    Only contiguous runs of adjacent wells that both define a depth are
    connected; each run becomes one ``(N, 2)`` array. Empty list when fewer
    than two adjacent wells define the depth. Shared by fluid contacts and
    erosion surfaces.
    """
    segments: list[np.ndarray] = []
    run: list[tuple[float, float]] = []
    for i in range(well_count):
        raw = depths.get(int(i))
        d: float | None = None
        if raw is not None:
            try:
                d = float(raw)
            except (TypeError, ValueError):
                d = None
        if d is None:
            if len(run) >= 2:
                segments.append(np.asarray(run, dtype=np.float64))
            run = []
            continue
        run.append((float(i), float(d)))
    if len(run) >= 2:
        segments.append(np.asarray(run, dtype=np.float64))
    return segments


def contact_segment_2d(
    contact: FluidContact2D, well_count: int
) -> list[np.ndarray]:
    """Return contact line segments in section 2D space (x = well index, y = MD).

    Only contiguous runs of adjacent wells that both define the contact are
    connected; each run becomes one ``(N, 2)`` array. Empty list when fewer
    than two adjacent wells define the contact.
    """
    return segment_by_depths(contact.depths, well_count)


def interpolate_depth_at(
    depths: Mapping[int, float], x: float
) -> float | None:
    """Linear-interpolate a per-well depth at section-x (well-index space).

    ``x`` is a fractional well index; returns None if either bounding well
    lacks a depth. Shared by fluid contacts and erosion surfaces.
    """
    lo = int(np.floor(x))
    hi = int(np.ceil(x))
    raw = depths.get(lo)
    d_lo = float(raw) if raw is not None else None
    if lo == hi:
        return d_lo
    raw_hi = depths.get(hi)
    d_hi = float(raw_hi) if raw_hi is not None else None
    if d_lo is None or d_hi is None:
        return None
    frac = x - lo
    return d_lo + frac * (d_hi - d_lo)


def _interpolate_depth(
    contact: FluidContact2D, x: float
) -> float | None:
    """Linear-interpolate the contact depth at section-x (well-index space)."""
    return interpolate_depth_at(contact.depths, x)


def _blend_hex(a: str, b: str) -> str:
    """Average two #RRGGBB colours for a transition-band fill."""
    def _rgb(h: str) -> tuple[int, int, int]:
        s = h.lstrip("#")
        if len(s) != 6:
            return (100, 116, 139)
        return int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16)

    ra, ga, ba = _rgb(a)
    rb, gb, bb = _rgb(b)
    return f"#{(ra + rb) // 2:02x}{(ga + gb) // 2:02x}{(ba + bb) // 2:02x}"


def split_quad_by_contact(
    quad: TieQuad2D, contact: FluidContact2D, well_count: int
) -> tuple[TieQuad2D, ...] | None:
    """Split a tie-quad into above/below (optional transition) along a contact.

    The quad is split when the contact passes through it: the contact's
    interpolated depth at the quad's left and right x must both lie strictly
    inside the quad's depth extent on that side, and both bounding wells must
    define the contact.

    Returns:
      * ``(above, below)`` when ``transition_m <= 0`` (sharp dual fill);
      * ``(above, transition, below)`` when ``transition_m > 0`` and the band
        fits inside the quad on both edges;
      * ``None`` if the contact does not cross the quad.

    Colours: above = oil/gas, below = water/oil; transition = blend of the two.
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
    try:
        half = max(0.0, float(contact.transition_m)) * 0.5
    except (TypeError, ValueError):
        half = 0.0

    def _band(
        top_l_d: float,
        top_r_d: float,
        bot_r_d: float,
        bot_l_d: float,
        color: str,
    ) -> TieQuad2D:
        return TieQuad2D(
            corners=np.array(
                [
                    [x_l, top_l_d],
                    [x_r, top_r_d],
                    [x_r, bot_r_d],
                    [x_l, bot_l_d],
                ],
                dtype=np.float64,
            ),
            fill_color=color,
            label=quad.label,
            pattern_id=quad.pattern_id,
        )

    if half <= 0.0:
        above = _band(top_l, top_r, d_r, d_l, above_color)
        below = _band(d_l, d_r, bot_r, bot_l, below_color)
        return above, below

    # Transition band centred on contact; clamp so it stays inside the quad.
    up_l = max(lo_l + 1e-9, d_l - half)
    up_r = max(lo_r + 1e-9, d_r - half)
    dn_l = min(hi_l - 1e-9, d_l + half)
    dn_r = min(hi_r - 1e-9, d_r + half)
    # Degenerate transition (too thin / clamped shut) → sharp split.
    if not (up_l < dn_l and up_r < dn_r):
        above = _band(top_l, top_r, d_r, d_l, above_color)
        below = _band(d_l, d_r, bot_r, bot_l, below_color)
        return above, below

    mid_color = _blend_hex(above_color, below_color)
    above = _band(top_l, top_r, up_r, up_l, above_color)
    mid = _band(up_l, up_r, dn_r, dn_l, mid_color)
    below = _band(dn_l, dn_r, bot_r, bot_l, below_color)
    return above, mid, below


# ---------------------------------------------------------------------------
# Serialization
# ---------------------------------------------------------------------------


def contact_to_json(contact: FluidContact2D) -> dict[str, Any]:
    payload: dict[str, Any] = {
        "fluid_type": str(contact.fluid_type),
        "depths": [
            [int(idx), float(d)]
            for idx, d in sorted(contact.depths.items())
            if d is not None
        ],
        "color": contact.color,
    }
    try:
        t = float(contact.transition_m)
    except (TypeError, ValueError):
        t = 0.0
    if t > 0.0:
        payload["transition_m"] = t
    return payload


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
    try:
        transition_m = max(0.0, float(raw.get("transition_m") or 0.0))
    except (TypeError, ValueError):
        transition_m = 0.0
    return FluidContact2D(
        fluid_type=fluid,
        depths=depths,
        color=color,
        transition_m=transition_m,
    )


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


def split_quad_composite(
    quad: TieQuad2D,
    faults: Iterable[Any],
    contacts: Iterable[FluidContact2D],
    well_count: int,
    surfaces: Iterable[Any] | None = None,
) -> list[TieQuad2D]:
    """Split a tie-quad by surfaces, then **every** fault, then **every** contact.

    Order:
    1. Structural surfaces (erosion/onlap) — applied sequentially; each
       successful truncate replaces the working quad (first-wins was too
       weak when multiple surfaces stack).
    2. Fault throws via ``apply_faults_to_quad`` (all faults), then
       **each** fault plane splits every current piece left/right
       (sequential full cut — not first-hit only).
    3. **Each** fluid contact splits every current piece above/below when
       it crosses that piece (GOC then OWC → up to three stacked fills).

    Because contact depth is per well, lines offset across a fault plane:
    left and right halves split independently.

    Returns 1..N pieces in paint order (fault left before right; contact
    above before below). Empty faults/contacts/surfaces → ``[quad]``.
    """
    from well_log_workstation.section_geometry.erosion_surface import (
        truncate_quad_by_surface,
    )
    from well_log_workstation.section_geometry.fault_section import (
        apply_faults_to_quad,
        split_quad_by_fault,
    )

    surfaces_list = list(surfaces or [])
    faults_list = list(faults or [])
    contacts_list = list(contacts or [])
    base = quad
    # Surfaces: apply every successful truncate in order (stacking).
    for surface in surfaces_list:
        kept = truncate_quad_by_surface(base, surface, well_count)
        if kept is not None:
            base = kept
    eff = (
        apply_faults_to_quad(base, faults_list, well_count)
        if faults_list
        else base
    )
    pieces: list[TieQuad2D] = [eff]
    # Faults: sequential full cut — each fault splits every current piece.
    for fault in faults_list:
        next_pieces: list[TieQuad2D] = []
        for piece in pieces:
            candidate = split_quad_by_fault(piece, fault, well_count)
            if candidate is not None:
                next_pieces.extend((candidate[0], candidate[1]))
            else:
                next_pieces.append(piece)
        pieces = next_pieces
    # Contacts: sequential full cut — each contact on every piece.
    for contact in contacts_list:
        next_pieces = []
        for piece in pieces:
            candidate = split_quad_by_contact(piece, contact, well_count)
            if candidate is not None:
                next_pieces.extend(candidate)
            else:
                next_pieces.append(piece)
        pieces = next_pieces
    return pieces
