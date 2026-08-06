"""Section-line picker dialog (FRS §4.2 / P2-B, workflow 1).

Lets the user define a section line by two endpoints (lng/lat, optionally
picked from wells) and a buffer radius, live-previews how many wells will be
picked, and returns the line definition.
"""

from __future__ import annotations

from PySide6.QtWidgets import (
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QDoubleSpinBox,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QPushButton,
    QVBoxLayout,
)

from well_log_workstation.section_line import pick_wells_along_line
from well_log_workstation.workspace import WellCatalogEntry

# 1° of lng/lat ≈ 111 km at the equator — a crude but usable metre→degree
# conversion for field-scale buffers.
_M_PER_DEG = 111_000.0


class SectionLineDialog(QDialog):
    """Define a section line + buffer and preview the picked wells."""

    def __init__(
        self,
        wells: list[WellCatalogEntry],
        *,
        default_buffer_m: float = 500.0,
        parent=None,
    ) -> None:
        super().__init__(parent)
        self.setWindowTitle("平面画线生成剖面")
        self.setObjectName("SectionLineDialog")
        self._wells = wells
        layout = QVBoxLayout(self)

        layout.addWidget(
            QLabel(
                "剖面切线：输入两端点坐标（lng, lat），或从井位选取端点。\n"
                "落在缓冲带内的井将按沿线投影顺序生成地层对比图。"
            )
        )

        form = QFormLayout()
        self._end_a = QLineEdit()
        self._end_a.setObjectName("SectionLineEndA")
        self._end_a.setPlaceholderText("116.50, 30.20")
        form.addRow("端点 A (lng, lat)", self._end_a)
        self._end_b = QLineEdit()
        self._end_b.setObjectName("SectionLineEndB")
        self._end_b.setPlaceholderText("116.90, 30.20")
        form.addRow("端点 B (lng, lat)", self._end_b)
        layout.addLayout(form)

        # Endpoint-from-well helpers.
        well_row = QHBoxLayout()
        self._well_combo = QComboBox()
        self._well_combo.setObjectName("SectionLineWellCombo")
        for w in wells:
            label = w.name
            if w.lng is not None and w.lat is not None:
                label += f" ({w.lng:.4f}, {w.lat:.4f})"
            self._well_combo.addItem(label, w.id)
        well_row.addWidget(self._well_combo, 1)
        for target, name in ((self._end_a, "设为端点 A"), (self._end_b, "设为端点 B")):
            btn = QPushButton(name)
            btn.clicked.connect(
                lambda _=False, t=target: self._fill_from_well(t)
            )
            well_row.addWidget(btn)
        layout.addLayout(well_row)

        self._buffer = QDoubleSpinBox()
        self._buffer.setObjectName("SectionLineBuffer")
        self._buffer.setRange(10.0, 5000.0)
        self._buffer.setSingleStep(50.0)
        self._buffer.setValue(default_buffer_m)
        self._buffer.setSuffix(" m")
        form.addRow("缓冲带半径", self._buffer)

        self._preview = QLabel("")
        self._preview.setObjectName("SectionLinePreview")
        layout.addWidget(self._preview)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok
            | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(self._on_accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

        for widget in (self._end_a, self._end_b, self._buffer):
            if isinstance(widget, QLineEdit):
                widget.textChanged.connect(self._update_preview)
            else:
                widget.valueChanged.connect(self._update_preview)
        self._update_preview()

    # -- helpers ---------------------------------------------------------

    def _fill_from_well(self, target: QLineEdit) -> None:
        well_id = self._well_combo.currentData()
        for w in self._wells:
            if w.id == well_id and w.lng is not None and w.lat is not None:
                target.setText(f"{w.lng:.6f}, {w.lat:.6f}")
                break

    def _parse_point(self, text: str) -> tuple[float, float] | None:
        parts = [p.strip() for p in text.replace("，", ",").split(",")]
        if len(parts) < 2:
            return None
        try:
            return float(parts[0]), float(parts[1])
        except ValueError:
            return None

    def _endpoints(self) -> tuple[tuple[float, float], tuple[float, float]] | None:
        a = self._parse_point(self._end_a.text())
        b = self._parse_point(self._end_b.text())
        if a is None or b is None or (a[0] == b[0] and a[1] == b[1]):
            return None
        return a, b

    def _update_preview(self, *_args) -> None:
        pts = self._endpoints()
        if pts is None:
            self._preview.setText("端点无效（需要两个不同的 lng, lat 点）。")
            return
        n = len(
            pick_wells_along_line(
                self._wells,
                pts[0],
                pts[1],
                buffer_deg=self._buffer.value() / _M_PER_DEG,
            )
        )
        self._preview.setText(f"沿线将选中 {n} 口井（≥2 口才能生成剖面）。")

    def _on_accept(self) -> None:
        pts = self._endpoints()
        if pts is None:
            self._preview.setText("端点无效（需要两个不同的 lng, lat 点）。")
            return
        self.accept()

    def value(self) -> tuple[tuple[float, float], tuple[float, float], float] | None:
        """Return (endpoint_a, endpoint_b, buffer_deg) or None."""
        pts = self._endpoints()
        if pts is None:
            return None
        return pts[0], pts[1], self._buffer.value() / _M_PER_DEG
