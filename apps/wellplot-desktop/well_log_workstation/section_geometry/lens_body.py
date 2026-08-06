"""Freehand lens bodies on the reservoir section (FRS §3.x 透镜体手绘).

A lens is a closed polygon in section space: x = well-index units (0..n-1),
y = measured depth. Pure numpy / dataclasses — no Qt.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Iterable, Sequence

import numpy as np

_DEFAULT_FILL = "#a78bfa"  # soft purple — distinct from oil/gas/water fills
_DEFAULT_STROKE = "#5b21b6"


@dataclass
class LensBody2D:
    """One freehand sand-lens / body polygon on the section canvas.

    ``points`` is an (N, 2) array of ``[x_unit, depth]`` with N ≥ 3. The
    polygon is treated as closed for paint (last→first edge implied).
    """

    points: np.ndarray
    label: str = ""
    fill_color: str = _DEFAULT_FILL
    stroke_color: str = _DEFAULT_STROKE
    pattern_id: str = ""

    def __post_init__(self) -> None:
        arr = np.asarray(self.points, dtype=np.float64)
        if arr.ndim != 2 or arr.shape[1] != 2:
            raise ValueError("lens points must be shape (N, 2)")
        self.points = arr

    @property
    def n_vertices(self) -> int:
        return int(self.points.shape[0])

    def is_valid(self) -> bool:
        return self.n_vertices >= 3 and np.all(np.isfinite(self.points))

    def closed_ring(self) -> np.ndarray:
        """Return points with first vertex appended so the ring closes."""
        if self.n_vertices < 1:
            return self.points.copy()
        return np.vstack([self.points, self.points[0:1]])


def make_ellipse_lens(
    cx: float,
    cy: float,
    rx: float,
    ry: float,
    *,
    n: int = 24,
    label: str = "",
    fill_color: str = _DEFAULT_FILL,
) -> LensBody2D:
    """Helper: elliptical lens in section units (for tests / default seed)."""
    n = max(6, int(n))
    t = np.linspace(0.0, 2.0 * np.pi, n, endpoint=False)
    pts = np.column_stack(
        [cx + rx * np.cos(t), cy + ry * np.sin(t)]
    )
    return LensBody2D(points=pts, label=label or "透镜体", fill_color=fill_color)


def lens_to_json(lens: LensBody2D) -> dict[str, Any]:
    return {
        "label": str(lens.label or ""),
        "fill_color": str(lens.fill_color or _DEFAULT_FILL),
        "stroke_color": str(lens.stroke_color or _DEFAULT_STROKE),
        "pattern_id": str(lens.pattern_id or ""),
        "points": [
            [float(x), float(y)] for x, y in np.asarray(lens.points, dtype=np.float64)
        ],
    }


def lens_from_json(raw: Any) -> LensBody2D | None:
    if not isinstance(raw, dict):
        return None
    pts_raw = raw.get("points")
    if not isinstance(pts_raw, (list, tuple)) or len(pts_raw) < 3:
        return None
    rows: list[list[float]] = []
    for item in pts_raw:
        try:
            if isinstance(item, (list, tuple)) and len(item) >= 2:
                rows.append([float(item[0]), float(item[1])])
        except (TypeError, ValueError):
            continue
    if len(rows) < 3:
        return None
    try:
        return LensBody2D(
            points=np.asarray(rows, dtype=np.float64),
            label=str(raw.get("label") or ""),
            fill_color=str(raw.get("fill_color") or _DEFAULT_FILL),
            stroke_color=str(raw.get("stroke_color") or _DEFAULT_STROKE),
            pattern_id=str(raw.get("pattern_id") or ""),
        )
    except ValueError:
        return None


def lenses_to_json(lenses: Iterable[LensBody2D]) -> list[dict[str, Any]]:
    return [lens_to_json(L) for L in lenses if L.is_valid()]


def lenses_from_json(raw: Any) -> list[LensBody2D]:
    if not isinstance(raw, list):
        return []
    out: list[LensBody2D] = []
    for item in raw:
        L = lens_from_json(item)
        if L is not None and L.is_valid():
            out.append(L)
    return out


def append_vertex(
    draft: Sequence[tuple[float, float]] | np.ndarray,
    x: float,
    y: float,
    *,
    min_dist: float = 1e-6,
) -> list[tuple[float, float]]:
    """Append a draft vertex unless it coincides with the last point."""
    pts = [(float(a), float(b)) for a, b in np.asarray(draft, dtype=np.float64).reshape(-1, 2)]
    if pts:
        lx, ly = pts[-1]
        if abs(lx - x) < min_dist and abs(ly - y) < min_dist:
            return pts
    pts.append((float(x), float(y)))
    return pts


def finalize_draft(
    draft: Sequence[tuple[float, float]] | np.ndarray,
    *,
    label: str = "",
    fill_color: str = _DEFAULT_FILL,
) -> LensBody2D | None:
    """Close a freehand draft into a :class:`LensBody2D` if ≥3 vertices."""
    pts = np.asarray(list(draft), dtype=np.float64).reshape(-1, 2)
    if pts.shape[0] < 3:
        return None
    # Drop trailing duplicate of first vertex if user closed explicitly.
    if pts.shape[0] >= 4:
        if np.allclose(pts[0], pts[-1]):
            pts = pts[:-1]
    if pts.shape[0] < 3:
        return None
    return LensBody2D(
        points=pts,
        label=label or f"透镜体{pts.shape[0]}点",
        fill_color=fill_color,
    )
