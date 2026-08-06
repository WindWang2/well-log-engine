"""erosion_surface — 2D unconformity / onlap surface model for the section.

An erosion surface (不整合/剥蚀面) is defined directly in section 2D space:
a per-well MD depth, plus a mode that selects which side of the surface is
kept when truncating tie quads — ``erosion`` keeps the part BELOW the
surface (the unconformity cuts the top of the older strata), ``onlap`` keeps
the part ABOVE (strata onlap onto the surface). A missing well depth breaks
the surface, exactly like a fluid contact.

Pure numpy + dataclasses, headless (no Qt). Outputs feed SectionCanvas:
``surface_segment_2d`` for the surface line and ``truncate_quad_by_surface``
for the vertical truncation of a tie-quad (FRS §3.x 尖灭行 P1).
"""

from __future__ import annotations

from dataclasses import dataclass, field, replace
from typing import Any, Iterable, Mapping

import numpy as np

from well_log_workstation.section_geometry.contact_section import (
    interpolate_depth_at,
    segment_by_depths,
)
from well_log_workstation.section_geometry.tie_polygons import TieQuad2D

# Surface-line colour (unconformity convention: dark brown) and dash pattern.
_SURFACE_COLOR = "#92400e"
_SURFACE_DASH: tuple[float, ...] = (3.0, 1.5, 0.5, 1.5)  # dash-dot

# Which side of the surface is kept when truncating a quad.
MODE_EROSION = "erosion"  # keep below (unconformity cuts the top)
MODE_ONLAP = "onlap"  # keep above (strata onlap onto the surface)
_VALID_MODES = (MODE_EROSION, MODE_ONLAP)


@dataclass(frozen=True)
class ErosionSurface2D:
    """One unconformity / onlap surface in section 2D space (x, y = MD)."""

    name: str
    mode: str = MODE_EROSION
    depths: dict[int, float] = field(default_factory=dict)
    color: str = _SURFACE_COLOR


def surface_segment_2d(
    surface: ErosionSurface2D, well_count: int
) -> list[np.ndarray]:
    """Return surface line segments in section 2D space (x = well index, y = MD).

    Only contiguous runs of adjacent wells that both define the surface are
    connected (mirrors contact_segment_2d).
    """
    return segment_by_depths(surface.depths, well_count)


def truncate_quad_by_surface(
    quad: TieQuad2D, surface: ErosionSurface2D, well_count: int
) -> TieQuad2D | None:
    """Truncate a tie-quad against an erosion/onlap surface.

    ``erosion`` keeps the part below the surface (returns the lower polygon);
    ``onlap`` keeps the part above. The surface's interpolated depth at the
    quad's left and right x must both lie strictly inside the quad's depth
    extent on that side, and both bounding wells must define the surface
    (mirrors split_quad_by_contact's rejection conditions). Returns the kept
    polygon (fill/pattern/label preserved — a structural truncation, no
    recolouring) or None when the surface does not strictly cross the quad.

    Quad corner order is ``[left_top, right_top, right_bottom, left_bottom]``.
    """
    corners = np.asarray(quad.corners, dtype=np.float64)
    if corners.shape != (4, 2):
        return None
    left_top, right_top, right_bottom, left_bottom = corners
    x_l = float(left_top[0])
    x_r = float(right_top[0])
    if x_l == x_r:
        return None  # zero-width quad; nothing to truncate

    d_l = interpolate_depth_at(surface.depths, x_l)
    d_r = interpolate_depth_at(surface.depths, x_r)
    if d_l is None or d_r is None:
        return None

    top_l, bot_l = float(left_top[1]), float(left_bottom[1])
    top_r, bot_r = float(right_top[1]), float(right_bottom[1])
    lo_l, hi_l = min(top_l, bot_l), max(top_l, bot_l)
    lo_r, hi_r = min(top_r, bot_r), max(top_r, bot_r)
    if not (lo_l < d_l < hi_l) or not (lo_r < d_r < hi_r):
        return None

    if surface.mode == MODE_ONLAP:
        # Keep above: left_top → right_top → (x_r, d_r) → (x_l, d_l)
        kept_corners = np.array(
            [
                [x_l, top_l],
                [x_r, top_r],
                [x_r, d_r],
                [x_l, d_l],
            ],
            dtype=np.float64,
        )
    else:
        # Erosion (default): keep below → (x_l, d_l) → (x_r, d_r) → rb → lb
        kept_corners = np.array(
            [
                [x_l, d_l],
                [x_r, d_r],
                [x_r, bot_r],
                [x_l, bot_l],
            ],
            dtype=np.float64,
        )
    return replace(
        quad,
        corners=kept_corners,
    )


# ---------------------------------------------------------------------------
# Serialization (mirrors contact_to_json / contacts_from_json)
# ---------------------------------------------------------------------------


def surface_to_json(surface: ErosionSurface2D) -> dict[str, Any]:
    depths = [
        [int(idx), float(d)]
        for idx, d in sorted(surface.depths.items())
        if d is not None
    ]
    return {
        "name": surface.name,
        "mode": surface.mode,
        "depths": depths,
        "color": surface.color,
    }


def surface_from_json(raw: Any) -> ErosionSurface2D | None:
    if not isinstance(raw, dict):
        return None
    name = str(raw.get("name") or "").strip()
    if not name:
        return None
    mode = str(raw.get("mode") or MODE_EROSION).strip().lower()
    if mode not in _VALID_MODES:
        mode = MODE_EROSION
    depths: dict[int, float] = {}
    raw_depths = raw.get("depths")
    if isinstance(raw_depths, dict):
        for k, v in raw_depths.items():
            try:
                depths[int(k)] = float(v)
            except (TypeError, ValueError):
                continue
    elif isinstance(raw_depths, list):
        for item in raw_depths:
            if isinstance(item, dict) and "idx" in item and "depth" in item:
                try:
                    depths[int(item["idx"])] = float(item["depth"])
                except (TypeError, ValueError):
                    continue
            elif isinstance(item, list) and len(item) == 2:
                try:
                    depths[int(item[0])] = float(item[1])
                except (TypeError, ValueError):
                    continue
    return ErosionSurface2D(
        name=name,
        mode=mode,  # type: ignore[arg-type]
        depths=depths,
        color=str(raw.get("color") or _SURFACE_COLOR),
    )


def surfaces_to_json(surfaces: Iterable[ErosionSurface2D]) -> list[dict[str, Any]]:
    return [surface_to_json(s) for s in surfaces]


def surfaces_from_json(raw: Any) -> list[ErosionSurface2D]:
    out: list[ErosionSurface2D] = []
    if not isinstance(raw, list):
        return out
    for item in raw:
        s = surface_from_json(item)
        if s is not None:
            out.append(s)
    return out
