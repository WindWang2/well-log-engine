"""CoordinateReference editor dialog (Phase-2, T2 / #246).

Lets the user set the workspace project/target/display CRS trio. The picker
is populated from ``geoviz.list_known_crs()`` when the mapping surface is
available, else falls back to a small builtin list (bare-clone friendly).
"""

from __future__ import annotations

from PySide6.QtWidgets import (
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QFormLayout,
    QVBoxLayout,
)

from well_log_workstation.workspace import CoordinateReference

_FALLBACK_CRS = [
    "EPSG:4326",  # WGS 84 (lng/lat)
    "EPSG:4490",  # CGCS2000 (lng/lat)
    "EPSG:4610",  # Beijing 1954 (lng/lat)
    "EPSG:4612",  # Xian 1980 (lng/lat)
    "EPSG:3857",  # Web Mercator (m)
]


def _known_crs_list() -> list[str]:
    try:
        from geoviz import list_known_crs
        codes = list_known_crs()
        if codes:
            return codes
    except Exception:
        pass
    return list(_FALLBACK_CRS)


class CoordinateReferenceDialog(QDialog):
    """Modal editor for the workspace CRS trio."""

    def __init__(self, coordinate: CoordinateReference | None = None, parent=None) -> None:
        super().__init__(parent)
        self.setWindowTitle("坐标系设置（CRS）")
        self.setMinimumWidth(420)
        coord = coordinate or CoordinateReference()

        form = QFormLayout()
        self._project = QComboBox()
        self._target = QComboBox()
        self._display = QComboBox()
        known = _known_crs_list()
        for combo in (self._project, self._target, self._display):
            combo.addItems(known)
        self._project.setEditable(True)
        self._target.setEditable(True)
        self._display.setEditable(True)
        self._project.setCurrentText(coord.project_crs)
        self._target.setCurrentText(coord.target_crs or "")
        self._display.setCurrentText(coord.display_crs)
        form.addRow("项目坐标系 (project_crs):", self._project)
        form.addRow("目标坐标系 (target_crs):", self._target)
        form.addRow("显示坐标系 (display_crs):", self._display)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)

        layout = QVBoxLayout(self)
        layout.addLayout(form)
        layout.addWidget(buttons)

    def result_coordinate(self) -> CoordinateReference:
        target = self._target.currentText().strip()
        return CoordinateReference(
            project_crs=self._project.currentText().strip() or "EPSG:4326",
            target_crs=target or None,
            display_crs=self._display.currentText().strip() or "EPSG:4326",
        )
