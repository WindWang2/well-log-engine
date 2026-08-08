"""TST computation + bedding editor dialog (Epic D / FRS §1.x 真地层厚度).

Table rows are bedding layers (top/bottom MD, dip, dip azimuth, optional
stratigraphic unit reference). The computed columns update live:

* 表观厚度 = MD interval (bottom − top);
* TVD 厚度  = interpolated TVD difference along the deviation survey;
* TST       = tst_along_path over a single-layer book (SDK-authoritative
  mirror, ``well_log_workstation.tst``) — per-layer contribution of the
  polyline well path.

Surface bedding (Epic D high-order, bedding.json v2): a layer bounded by
top/bottom surface grids computes its TST with ``tst_along_surface_path``
over the single unit the two surfaces bound; the 形态 column marks such
rows 曲面 (planar rows stay 平面). Surface rows' dip/azimuth and top/bottom
MD remain declared metadata.

Without a survey the TVD/TST columns show 「—」 (explicit unavailability —
never a fake zero, matching the SDK's "empty = unavailable" discipline).
Dip/azimuth inputs follow the ``normal_from_dip_azimuth`` convention
(azimuth 0 = dip toward north, φ=0 degenerates to the C++ fixture form).
"""

from __future__ import annotations

import math

import numpy as np
from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QMessageBox,
    QPushButton,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
)

from well_log_workstation.stratigraphy import StratigraphicDictionary
from well_log_workstation.survey import SurveyTrajectory, interpolate_tvd
from well_log_workstation.tst import (
    BeddingLayer,
    BeddingLayerSpec,
    PathPoint3D,
    SurfaceGridSpec,
    normal_from_dip_azimuth,
    path_from_trajectory,
    tst_along_path,
    tst_along_surface_path,
)
from well_log_workstation.unit_combo import make_unit_combo

(C_TOP, C_BOTTOM, C_DIP, C_AZIMUTH, C_UNIT,
 C_APPARENT, C_TVD, C_TST, C_SHAPE) = range(9)

HEADERS = ["顶深MD", "底深MD", "倾角°", "方位°", "层位单元",
           "表观厚度", "TVD厚度", "TST", "形态"]

NA = "—"  # explicit unavailability marker
SHAPE_PLANAR = "平面"
SHAPE_SURFACE = "曲面"


class TstDialog(QDialog):
    """Compute per-layer TST for one well (editable bedding table)."""

    def __init__(
        self,
        well_name: str,
        specs: list[BeddingLayerSpec] | None = None,
        *,
        trajectory: SurveyTrajectory | None = None,
        dictionary: StratigraphicDictionary | None = None,
        parent=None,
    ) -> None:
        super().__init__(parent)
        self.setWindowTitle(f"真地层厚度 TST · {well_name}")
        self.setObjectName("TstDialog")
        self.resize(820, 420)
        self._trajectory = trajectory if trajectory is not None else SurveyTrajectory(
            md=np.empty(0),
            tvd=np.empty(0),
            tvdss=np.empty(0),
            north=np.empty(0),
            east=np.empty(0),
            closure_dist=np.empty(0),
        )
        self._path: list[PathPoint3D] = (
            path_from_trajectory(self._trajectory)
            if self._trajectory.md.size >= 2
            else []
        )
        self._dictionary = dictionary
        self._updating = False
        # Per-row declared surfaces (top, bottom) — either may be None; a
        # planar row has both None, an inconsistent surface row exactly one.
        # Kept in sync by _add_row/_add_empty_row/_swap_rows/
        # _delete_selected_rows.
        self._row_surfaces: list[
            tuple[SurfaceGridSpec | None, SurfaceGridSpec | None]
        ] = []

        layout = QVBoxLayout(self)
        if self._path:
            note = (
                "每行一个产状层：顶/底 MD、倾角（0=水平，90=垂直）、倾角方位（0=北，指向最大下倾方向）。\n"
                "表观厚度 = MD 区间；TVD 厚度与 TST 按测斜轨迹计算（TST 为 SDK 分段平面模型逐层贡献；"
                "曲面层按顶/底曲面网格求交计算，见「形态」列）。"
            )
        else:
            note = (
                "该井暂无测斜轨迹（wells/<id>/survey.json）：只能给出表观厚度（MD 区间），\n"
                "TVD 厚度与 TST 显示「—」（显式不可用，绝不冒充 0）。"
            )
        layout.addWidget(QLabel(note))

        self.table = QTableWidget(0, len(HEADERS))
        self.table.setObjectName("TstTable")
        self.table.setHorizontalHeaderLabels(HEADERS)
        header = self.table.horizontalHeader()
        header.setSectionResizeMode(C_TOP, QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(C_BOTTOM, QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(C_DIP, QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(C_AZIMUTH, QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(C_UNIT, QHeaderView.ResizeMode.Stretch)
        for col in (C_APPARENT, C_TVD, C_TST, C_SHAPE):
            header.setSectionResizeMode(col, QHeaderView.ResizeMode.ResizeToContents)
        self.table.itemChanged.connect(self._on_item_changed)
        layout.addWidget(self.table)

        row_buttons = QHBoxLayout()
        add_btn = QPushButton("添加层")
        add_btn.setObjectName("TstAddLayer")
        add_btn.clicked.connect(self._add_empty_row)
        row_buttons.addWidget(add_btn)
        del_btn = QPushButton("删除层")
        del_btn.setObjectName("TstDeleteLayer")
        del_btn.clicked.connect(self._delete_selected_rows)
        row_buttons.addWidget(del_btn)
        up_btn = QPushButton("上移")
        up_btn.setObjectName("TstMoveUp")
        up_btn.clicked.connect(lambda: self._move_selected(-1))
        row_buttons.addWidget(up_btn)
        down_btn = QPushButton("下移")
        down_btn.setObjectName("TstMoveDown")
        down_btn.clicked.connect(lambda: self._move_selected(1))
        row_buttons.addWidget(down_btn)
        row_buttons.addStretch(1)
        layout.addLayout(row_buttons)

        self.total_label = QLabel("")
        self.total_label.setObjectName("TstTotal")
        layout.addWidget(self.total_label)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok
            | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(self._on_accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

        for spec in specs or []:
            self._add_row(spec)
        if self.table.rowCount() == 0:
            self._add_empty_row()
        self._recompute_all()

    # ------------------------------------------------------------------
    # Rows
    # ------------------------------------------------------------------
    def _add_empty_row(self) -> None:
        r = self.table.rowCount()
        self.table.insertRow(r)
        for col in (C_TOP, C_BOTTOM, C_DIP, C_AZIMUTH):
            item = QTableWidgetItem("" if col in (C_TOP, C_BOTTOM) else "0")
            self.table.setItem(r, col, item)
        self.table.setCellWidget(r, C_UNIT, make_unit_combo(self._dictionary))
        for col in (C_APPARENT, C_TVD, C_TST):
            item = QTableWidgetItem(NA)
            item.setFlags(Qt.ItemFlag.ItemIsEnabled)  # read-only computed cell
            self.table.setItem(r, col, item)
        shape = QTableWidgetItem(SHAPE_PLANAR)
        shape.setFlags(Qt.ItemFlag.ItemIsEnabled)  # read-only
        self.table.setItem(r, C_SHAPE, shape)
        self._row_surfaces.append((None, None))

    def _add_row(self, spec: BeddingLayerSpec) -> None:
        r = self.table.rowCount()
        self.table.insertRow(r)
        self.table.setItem(r, C_TOP, QTableWidgetItem(f"{spec.top_md:g}"))
        self.table.setItem(r, C_BOTTOM, QTableWidgetItem(f"{spec.bottom_md:g}"))
        self.table.setItem(r, C_DIP, QTableWidgetItem(f"{spec.dip_deg:g}"))
        self.table.setItem(r, C_AZIMUTH, QTableWidgetItem(f"{spec.dip_azimuth_deg:g}"))
        self.table.setCellWidget(
            r, C_UNIT, make_unit_combo(self._dictionary, spec.unit_id)
        )
        for col in (C_APPARENT, C_TVD, C_TST):
            self.table.setItem(r, col, QTableWidgetItem(NA))
            self.table.item(r, col).setFlags(Qt.ItemFlag.ItemIsEnabled)
        # Any declared surface marks the row 曲面; a row with exactly one
        # surface is inconsistent — its TST stays 「—」 (explicit
        # unavailability, never a silent planar fallback).
        is_surface = spec.top_surface is not None or spec.bottom_surface is not None
        shape = QTableWidgetItem(SHAPE_SURFACE if is_surface else SHAPE_PLANAR)
        shape.setFlags(Qt.ItemFlag.ItemIsEnabled)
        self.table.setItem(r, C_SHAPE, shape)
        self._row_surfaces.append((spec.top_surface, spec.bottom_surface))

    def _delete_selected_rows(self) -> None:
        rows = sorted({i.row() for i in self.table.selectedIndexes()}, reverse=True)
        for r in rows:
            self.table.removeRow(r)
            if 0 <= r < len(self._row_surfaces):
                del self._row_surfaces[r]
        self._recompute_all()

    def _move_selected(self, delta: int) -> None:
        rows = sorted({i.row() for i in self.table.selectedIndexes()})
        if not rows:
            return
        for r in rows:
            target = r + delta
            if target < 0 or target >= self.table.rowCount():
                continue
            self._swap_rows(r, target)
        self._recompute_all()

    def _swap_rows(self, a: int, b: int) -> None:
        for col in range(len(HEADERS)):
            item_a = self.table.takeItem(a, col)
            item_b = self.table.takeItem(b, col)
            if item_a is not None:
                self.table.setItem(b, col, item_a)
            if item_b is not None:
                self.table.setItem(a, col, item_b)
        widget_a = self.table.cellWidget(a, C_UNIT)
        widget_b = self.table.cellWidget(b, C_UNIT)
        if widget_a is not None:
            self.table.setCellWidget(b, C_UNIT, widget_a)
        if widget_b is not None:
            self.table.setCellWidget(a, C_UNIT, widget_b)
        if 0 <= a < len(self._row_surfaces) and 0 <= b < len(self._row_surfaces):
            self._row_surfaces[a], self._row_surfaces[b] = (
                self._row_surfaces[b],
                self._row_surfaces[a],
            )

    # ------------------------------------------------------------------
    # Live computation
    # ------------------------------------------------------------------
    def _on_item_changed(self, item: QTableWidgetItem) -> None:
        if self._updating:
            return
        if item.column() in (C_TOP, C_BOTTOM, C_DIP, C_AZIMUTH):
            self._recompute_all()

    def _cell_text(self, row: int, col: int) -> str:
        item = self.table.item(row, col)
        return item.text().strip() if item is not None else ""

    def _combo_data(self, row: int, col: int) -> object:
        widget = self.table.cellWidget(row, col)
        if isinstance(widget, QComboBox):
            return widget.currentData()
        return None

    def _recompute_all(self) -> None:
        self._updating = True
        try:
            total_tst = 0.0
            has_tst = False
            for r in range(self.table.rowCount()):
                self._recompute_row(r)
                tst = self._row_tst_value(r)
                if tst is not None:
                    total_tst += tst
                    has_tst = True
            if has_tst:
                self.total_label.setText(
                    f"合计 TST：{total_tst:g} m（{self.table.rowCount()} 层，"
                    f"未声明产状的井段不计入）"
                )
            else:
                self.total_label.setText("合计 TST：—（需测斜轨迹与有效产状层）")
        finally:
            self._updating = False

    def _recompute_row(self, r: int) -> None:
        top = self._cell_text(r, C_TOP)
        bottom = self._cell_text(r, C_BOTTOM)
        try:
            t = float(top)
            b = float(bottom)
        except (TypeError, ValueError):
            self._set_computed(r, NA, NA, NA)
            return
        if not (math.isfinite(t) and math.isfinite(b)) or not (b > t):
            self._set_computed(r, NA, NA, NA)
            return
        apparent = b - t
        tvd_value: float | None = None
        if self._trajectory.md.size >= 2:
            tvd_value = interpolate_tvd(self._trajectory, b) - interpolate_tvd(
                self._trajectory, t
            )
        tst_value: float | None = None
        if self._path:
            try:
                top_spec, bottom_spec = (
                    self._row_surfaces[r]
                    if 0 <= r < len(self._row_surfaces)
                    else (None, None)
                )
                if top_spec is not None and bottom_spec is not None:
                    # Surface-typed row: TST of the single unit bounded by
                    # the top/bottom grids (crossings computed
                    # geometrically). Invalid grids raise ValueError → 「—」.
                    tst_value = tst_along_surface_path(
                        self._path,
                        [top_spec.to_grid(), bottom_spec.to_grid()],
                    ).value
                elif top_spec is not None or bottom_spec is not None:
                    tst_value = None  # inconsistent surface spec
                else:
                    dip = self._optional_float(r, C_DIP) or 0.0
                    az = self._optional_float(r, C_AZIMUTH) or 0.0
                    layer = BeddingLayer(
                        top_md=t,
                        bottom_md=b,
                        normal=normal_from_dip_azimuth(dip, az),
                    )
                    tst_value = tst_along_path(self._path, [layer]).value
            except ValueError:
                tst_value = None
        self._set_computed(
            r,
            f"{apparent:g}",
            NA if tvd_value is None else f"{tvd_value:g}",
            NA if tst_value is None else f"{tst_value:g}",
        )

    def _set_computed(self, r: int, apparent: str, tvd: str, tst: str) -> None:
        for col, text in ((C_APPARENT, apparent), (C_TVD, tvd), (C_TST, tst)):
            item = self.table.item(r, col)
            if item is not None and item.text() != text:
                item.setText(text)

    def _row_tst_value(self, r: int) -> float | None:
        text = self._cell_text(r, C_TST)
        if text in (NA, ""):
            return None
        try:
            return float(text)
        except ValueError:
            return None

    def _optional_float(self, row: int, col: int) -> float | None:
        text = self._cell_text(row, col)
        if not text:
            return None
        try:
            return float(text)
        except ValueError:
            return None

    # ------------------------------------------------------------------
    # Accept / value
    # ------------------------------------------------------------------
    def _on_accept(self) -> None:
        for r in range(self.table.rowCount()):
            top = self._cell_text(r, C_TOP)
            bottom = self._cell_text(r, C_BOTTOM)
            if not top and not bottom:
                continue  # fully empty rows are dropped
            try:
                t = float(top)
                b = float(bottom)
            except (TypeError, ValueError):
                QMessageBox.warning(self, "产状层不完整", f"第 {r + 1} 行：顶深/底深必须是数字。")
                return
            if not (b > t):
                QMessageBox.warning(self, "产状层无效", f"第 {r + 1} 行：底深必须大于顶深。")
                return
            dip = self._optional_float(r, C_DIP)
            if dip is not None and not (0.0 <= dip <= 90.0):
                QMessageBox.warning(self, "产状层无效", f"第 {r + 1} 行：倾角须在 0–90° 之间。")
                return
        self.accept()

    def value(self) -> list[BeddingLayerSpec]:
        """Validated specs (empty rows dropped, sorted by top). Surface rows
        keep their top/bottom surface grids."""
        specs: list[BeddingLayerSpec] = []
        for r in range(self.table.rowCount()):
            top = self._cell_text(r, C_TOP)
            bottom = self._cell_text(r, C_BOTTOM)
            if not top or not bottom:
                continue
            try:
                t = float(top)
                b = float(bottom)
            except (TypeError, ValueError):
                continue
            if not (b > t):
                continue
            row_surfaces = (
                self._row_surfaces[r]
                if 0 <= r < len(self._row_surfaces)
                else (None, None)
            )
            specs.append(
                BeddingLayerSpec(
                    top_md=t,
                    bottom_md=b,
                    dip_deg=self._optional_float(r, C_DIP) or 0.0,
                    dip_azimuth_deg=self._optional_float(r, C_AZIMUTH) or 0.0,
                    unit_id=str(self._combo_data(r, C_UNIT) or ""),
                    top_surface=row_surfaces[0],
                    bottom_surface=row_surfaces[1],
                )
            )
        specs.sort(key=lambda s: (s.top_md, s.bottom_md))
        return specs
