"""WellSectionDatum — per-well depth shift for section flattening (Phase-2, T5).

T5 (#249): the Workstation holds its own ``WellSectionDatum`` subset (same
shape as the Workbench type, independent type per T1). Modes:

- ``md``: no shift (raw measured depth)
- ``tvdss``: shift = -kb elevation (true vertical depth subsea)
- ``horizon``: shift = -depth of the named top (flatten on that horizon)

``tvd`` is deliberately NOT included (no inclination/azimuth survey data;
G12: full survey trajectory math is out of render core by design).
"""

from __future__ import annotations

from typing import Any


class WellSectionDatum:
    """Computes per-well scalar shifts for section flatten modes."""

    VALID_MODES = ("md", "tvdss", "horizon")

    def __init__(self, mode: str = "md", target_horizon: str | None = None):
        if mode not in self.VALID_MODES:
            raise ValueError(
                f"未知剖面基准模式: {mode}（可选 {', '.join(self.VALID_MODES)}）"
            )
        self.mode = mode
        self.target_horizon = target_horizon

    def compute_shifts(
        self,
        wells: list[dict[str, Any]],
        *,
        target_horizon: str | None = None,
        kb_elevations: dict[str, float] | None = None,
    ) -> dict[str, float]:
        """Return per-well shift ``{well_name: float}``.

        Well dict shape (catalog-aligned): ``{"name", "tops": [{"name",
        "depth"}], "kb_m"?: float}``. ``kb_elevations`` may override the
        per-well kb (keyed by well name).
        """
        horizon = target_horizon or self.target_horizon
        shifts: dict[str, float] = {}
        for well in wells:
            name = str(well.get("name") or "")
            if self.mode == "md":
                shifts[name] = 0.0
            elif self.mode == "tvdss":
                kb = float(well.get("kb_m") or 0.0)
                if kb_elevations and name in kb_elevations:
                    kb = float(kb_elevations[name])
                shifts[name] = -kb
            else:  # horizon
                shift = 0.0
                if horizon:
                    for top in well.get("tops") or []:
                        if str(top.get("name")) == horizon:
                            try:
                                shift = -float(top.get("depth", 0.0))
                            except (TypeError, ValueError):
                                shift = 0.0
                            break
                shifts[name] = shift
        return shifts

    def align_depths(
        self,
        depths: dict[str, list[float]],
        shifts: dict[str, float],
    ) -> dict[str, list[float]]:
        """Apply shifts to per-well depth arrays: ``depths[name] + shift``."""
        out: dict[str, list[float]] = {}
        for name, arr in depths.items():
            s = shifts.get(name, 0.0)
            out[name] = [float(d) + s for d in arr]
        return out
