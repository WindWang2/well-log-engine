"""WellSectionDatum — per-well depth shift for section flattening (Phase-2, T5).

T5 (#249): the Workstation holds its own ``WellSectionDatum`` subset (same
shape as the Workbench type, independent type per T1). Modes:

- ``md``: no shift (raw measured depth)
- ``tvd``: shift = TVD(MD) - MD at a reference depth (P1-C; needs a deviation
  survey — without one the shift degrades to 0)
- ``tvdss``: true subsea placement — shift = TVD(MD) - MD - kb at a reference
  depth when a survey is present, falling back to the historic ``-kb``
  approximation (well treated as vertical) without one
- ``horizon``: shift = -depth of the named top (flatten on that horizon)

The ``tvd`` mode was added in P1-C (FRS §1.1): the host computes TVD via the
minimum-curvature method (``well_log_workstation.survey``) and uses the
TVD-minus-MD delta at a reference MD as a scalar per-well shift. The
``tvdss`` mode gained the same survey hook in a later pass: with a survey the
reference depth lands at its true elevation (kb - TVD), so deviated wells no
longer read as vertical.
"""

from __future__ import annotations

from typing import Any, Sequence


class WellSectionDatum:
    """Computes per-well scalar shifts for section flatten modes."""

    VALID_MODES = ("md", "tvd", "tvdss", "horizon")

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
        surveys: dict[str, Sequence[Any]] | None = None,
    ) -> dict[str, float]:
        """Return per-well shift ``{well_name: float}``.

        Well dict shape (catalog-aligned): ``{"name", "tops": [{"name",
        "depth"}], "kb_m"?: float}``. ``kb_elevations`` may override the
        per-well kb (keyed by well name). ``surveys`` (P1-C) maps well name →
        survey stations for the ``tvd`` and ``tvdss`` modes; a well without a
        survey degrades to a 0 shift (``tvd``) or the ``-kb`` approximation
        (``tvdss``).
        """
        horizon = target_horizon or self.target_horizon
        surveys = surveys or {}
        shifts: dict[str, float] = {}
        for well in wells:
            name = str(well.get("name") or "")
            if self.mode == "md":
                shifts[name] = 0.0
            elif self.mode == "tvd":
                shifts[name] = self._tvd_shift(name, well, surveys.get(name))
            elif self.mode == "tvdss":
                kb = float(well.get("kb_m") or 0.0)
                if kb_elevations and name in kb_elevations:
                    kb = float(kb_elevations[name])
                shifts[name] = self._tvdss_shift(
                    name, well, surveys.get(name), kb
                )
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

    @staticmethod
    def _reference_md(well: dict[str, Any]) -> float:
        """Reference MD: mean of the well's tops (or 0 when there are none).

        The reference depth only sets an absolute offset; relative structural
        shape between wells is preserved regardless of the chosen reference.
        """
        depths = []
        for top in well.get("tops") or []:
            try:
                depths.append(float(top.get("depth", 0.0)))
            except (TypeError, ValueError):
                continue
        return sum(depths) / len(depths) if depths else 0.0

    @staticmethod
    def _tvd_shift(
        name: str,
        well: dict[str, Any],
        survey: Sequence[Any] | None,
    ) -> float:
        """TVD-minus-MD at a reference depth (mean of the well's tops).

        Without a survey, the well is treated as vertical → TVD == MD → shift 0.
        """
        if not survey:
            return 0.0
        from well_log_workstation.survey import (
            SurveyStation,
            compute_trajectory,
            interpolate_tvd,
        )

        stations = [
            s if isinstance(s, SurveyStation) else SurveyStation(*s)
            for s in survey
        ]
        if not stations:
            return 0.0
        traj = compute_trajectory(stations)
        ref_md = WellSectionDatum._reference_md(well)
        tvd = interpolate_tvd(traj, ref_md)
        return float(tvd - ref_md)

    @staticmethod
    def _tvdss_shift(
        name: str,
        well: dict[str, Any],
        survey: Sequence[Any] | None,
        kb: float,
    ) -> float:
        """TVDSS-minus-MD at a reference depth (mean of the well's tops).

        True subsea placement: the reference depth lands at TVD(ref) - kb
        (its elevation kb - TVD(ref)). Without a survey the well is treated
        as vertical (TVD == MD) → shift = -kb, the historic approximation.
        """
        if not survey:
            return -kb
        from well_log_workstation.survey import (
            SurveyStation,
            compute_trajectory,
            interpolate_tvd,
        )

        stations = [
            s if isinstance(s, SurveyStation) else SurveyStation(*s)
            for s in survey
        ]
        if not stations:
            return -kb
        traj = compute_trajectory(stations)
        ref_md = WellSectionDatum._reference_md(well)
        tvd = interpolate_tvd(traj, ref_md)
        return float(tvd - ref_md - kb)

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
