"""PlaneMapView — 平面图 host surface wrapping PaleoMapCanvas (Phase-2, T2).

T2 (#246) / T5 (#249): the plane map draws well positions on a paleogeographic
canvas. The adapter:
- filters ``Workspace.wells`` to entries with ``lng``/``lat``/``crs`` all set
  (wells without coordinates are listed but NOT drawn)
- reprojects to ``Workspace.coordinate.project_crs`` at draw-time via
  ``geoviz.coerce_to_project_crs`` (paleo_map is Plate Carrée identity on
  the project CRS)
- feeds ``PaleoMapCanvas.load_features(..., wells=...)``

Degraded mode: when the geoviz mapping surface is unavailable
(``probe_mapping().available is False``), the view shows a placeholder label
instead of crashing.
"""

from __future__ import annotations

from collections import defaultdict

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QLabel, QVBoxLayout, QWidget

from well_log_workstation.workspace import CoordinateReference, WellCatalogEntry


class PlaneMapView(QWidget):
    """Wraps a PaleoMapCanvas; safe to construct when mapping is unavailable."""

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self.setObjectName("PlaneMapView")
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        self._canvas = None
        self._placeholder = QLabel(
            "平面图需要 geoviz mapping 表面（geoviz_paleo_map + CRS helpers）。"
            "请确认 geo-viz-engine 已安装。"
        )
        self._placeholder.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._placeholder.setWordWrap(True)
        layout.addWidget(self._placeholder)
        self._coordinate: CoordinateReference | None = None
        self._plot_diagnostics: list[str] = []

    # -- capability -----------------------------------------------------

    def set_coordinate(self, coordinate: CoordinateReference | None) -> None:
        """Bind the workspace CRS trio (reprojection target)."""
        self._coordinate = coordinate

    def mapping_available(self) -> bool:
        try:
            from well_log_workstation.engine_bridge import mapping_available
            return mapping_available()
        except Exception:
            return False

    def _ensure_canvas(self):
        if self._canvas is not None:
            return self._canvas
        if not self.mapping_available():
            return None
        from geoviz import PaleoMapCanvas

        self._canvas = PaleoMapCanvas()
        self.layout().removeWidget(self._placeholder)
        self._placeholder.deleteLater()
        self.layout().addWidget(self._canvas, 1)
        return self._canvas

    # -- data -----------------------------------------------------------

    @staticmethod
    def filter_wells_with_coords(
        wells: list[WellCatalogEntry],
    ) -> list[dict]:
        """Filter catalog wells to entries with complete lng/lat/crs.

        Wells missing coordinates or CRS are deliberately excluded from the
        drawn set (T2: listed but not drawn). Returns
        ``{"name", "lng", "lat", "crs"}`` dicts in catalog order.
        """
        out: list[dict] = []
        for w in wells:
            if w.lng is None or w.lat is None:
                continue
            crs = (w.crs or "").strip()
            if not crs:
                continue
            try:
                lng = float(w.lng)
                lat = float(w.lat)
            except (TypeError, ValueError):
                continue
            out.append({"name": w.name, "lng": lng, "lat": lat, "crs": crs})
        return out

    def set_plot_data(
        self,
        wells: list[WellCatalogEntry],
        features: list[dict] | None = None,
    ) -> None:
        """Reproject + load well positions (and optional features) on canvas."""
        canvas = self._ensure_canvas()
        if canvas is None:
            return
        drawn = self.filter_wells_with_coords(wells)
        self._plot_diagnostics = []
        # Draw-time reprojection (T2): coerce each source CRS group to the
        # project CRS the canvas renders in (Plate Carrée identity).
        if drawn:
            try:
                from geoviz import coerce_to_project_crs
            except ImportError:
                coerce_to_project_crs = None  # type: ignore[assignment]
            if coerce_to_project_crs is not None:
                groups: dict[str, list[int]] = defaultdict(list)
                for i, w in enumerate(drawn):
                    groups[str(w["crs"])].append(i)
                failed: set[int] = set()
                for src_crs, idxs in groups.items():
                    try:
                        pts = coerce_to_project_crs(
                            [[drawn[i]["lng"], drawn[i]["lat"]] for i in idxs],
                            src_crs,
                        )
                        for j, i in enumerate(idxs):
                            drawn[i]["lng"] = float(pts[j][0])
                            drawn[i]["lat"] = float(pts[j][1])
                    except (TypeError, ValueError, RuntimeError) as exc:
                        self._plot_diagnostics.append(
                            f"reproject {src_crs}: {exc}"
                        )
                        failed.update(idxs)
                if failed:
                    drawn = [w for i, w in enumerate(drawn) if i not in failed]
        canvas.load_features(features or [], wells=drawn)
