"""Well-header editor dialog (FRS §1.x 井口坐标 / KB / GL / MaxMD).

Edits the per-well header fields on ``WellCatalogEntry``: KB (kelly-bushing
elevation / 补心海拔), GL (ground level / 地面海拔), MaxMD, plus the
wellhead coordinates (lng/lat/crs). KB feeds the tvdss section datum
(shift = -kb). Mirrors the QFormLayout pattern of ``crs_dialog``.
"""

from __future__ import annotations

from PySide6.QtWidgets import (
    QDialog,
    QDialogButtonBox,
    QDoubleSpinBox,
    QFormLayout,
    QLabel,
    QLineEdit,
    QVBoxLayout,
)

from well_log_workstation.workspace import WellCatalogEntry

# Sentinel range for elevation/depth spin boxes (metres).
_ELEV_MIN, _ELEV_MAX = -2000.0, 10000.0


def _elev_spin(value: float | None) -> QDoubleSpinBox:
    sb = QDoubleSpinBox()
    sb.setRange(_ELEV_MIN, _ELEV_MAX)
    sb.setDecimals(2)
    sb.setSuffix(" m")
    # None / NaN -> 0 (the spin box has no "no value" state; 0 is the
    # neutral shift and the caller treats 0 as "no KB").
    try:
        sb.setValue(float(value) if value is not None else 0.0)
    except (TypeError, ValueError):
        sb.setValue(0.0)
    return sb


class WellHeaderDialog(QDialog):
    """Modal editor for a well's header fields (KB/GL/MaxMD + coordinates)."""

    def __init__(self, entry: WellCatalogEntry, parent=None) -> None:
        super().__init__(parent)
        self.setWindowTitle(f"井头数据编辑 — {entry.name}")
        self.setMinimumWidth(380)
        self._entry = entry

        form = QFormLayout()
        form.addRow(QLabel(f"井名：{entry.name}"))

        self._kb = _elev_spin(entry.kb_m)
        self._kb.setToolTip("补心海拔 KB（m）；用于 TVDSS 基准（shift = -KB）")
        form.addRow("补心海拔 KB:", self._kb)

        self._gl = _elev_spin(entry.gl_m)
        self._gl.setToolTip("地面海拔 GL（m）；元数据，留作后续真 TVDSS")
        form.addRow("地面海拔 GL:", self._gl)

        self._max_md = _elev_spin(entry.max_md)
        self._max_md.setToolTip("最大井深 MaxMD（m）")
        form.addRow("最大井深 MaxMD:", self._max_md)

        self._lng = QLineEdit()
        self._lng.setText("" if entry.lng is None else f"{entry.lng:.6f}")
        form.addRow("经度 / X:", self._lng)

        self._lat = QLineEdit()
        self._lat.setText("" if entry.lat is None else f"{entry.lat:.6f}")
        form.addRow("纬度 / Y:", self._lat)

        self._crs = QLineEdit()
        self._crs.setText(str(entry.crs or "EPSG:4326"))
        form.addRow("坐标系 (CRS):", self._crs)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok
            | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)

        layout = QVBoxLayout(self)
        layout.addLayout(form)
        layout.addWidget(buttons)

    @staticmethod
    def _parse_float(text: str) -> float | None:
        s = text.strip()
        if not s:
            return None
        try:
            return float(s)
        except ValueError:
            return None

    def result_entry(self) -> WellCatalogEntry:
        """Return a new ``WellCatalogEntry`` with the edited fields."""
        kb = self._kb.value()
        gl = self._gl.value()
        max_md = self._max_md.value()
        return WellCatalogEntry(
            id=self._entry.id,
            name=self._entry.name,
            path=self._entry.path,
            lng=self._parse_float(self._lng.text()),
            lat=self._parse_float(self._lat.text()),
            crs=self._crs.text().strip() or "EPSG:4326",
            # A 0 KB is treated as "no KB" by the datum (shift stays 0),
            # so store None when the user left it at the neutral default.
            kb_m=kb if abs(kb) > 1e-9 else None,
            gl_m=gl if abs(gl) > 1e-9 else None,
            max_md=max_md if abs(max_md) > 1e-9 else None,
        )
