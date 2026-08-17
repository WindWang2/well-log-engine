"""Core-photo segment editor dialog (FRS §2.x).

A table of depth bands (top / bottom / image / label). Each segment
references an image file (PNG/JPEG) that is copied into the well's
``core_photos/`` directory. On accept rows are validated (finite depths,
top < bottom, image chosen).
"""

from __future__ import annotations

import shutil
import uuid
from pathlib import Path

from PySide6.QtWidgets import (
    QDialog,
    QDialogButtonBox,
    QFileDialog,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QMessageBox,
    QPushButton,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
)

from well_log_workstation.core_photo_model import (
    CorePhotoModel,
    CorePhotoSegment,
)

COL_TOP, COL_BOTTOM, COL_IMAGE, COL_LABEL = 0, 1, 2, 3


class CorePhotoDialog(QDialog):
    """Edit the core-photo segments for the selected well."""

    def __init__(
        self,
        current: CorePhotoModel | None = None,
        parent=None,
        *,
        image_dir: Path | None = None,
    ) -> None:
        super().__init__(parent)
        self.setWindowTitle("岩心照片道编辑")
        self.setObjectName("CorePhotoDialog")
        self._unit = (current.unit if current is not None else "m") or "m"
        self._well_id = current.well_id if current is not None else ""
        self._image_dir = Path(image_dir) if image_dir is not None else None
        layout = QVBoxLayout(self)

        layout.addWidget(
            QLabel(
                "每行一段：顶深 / 底深 / 图片文件 / 备注；保存时按顶深排序。\n"
                "图片会复制到工区的 wells/<井>/core_photos/ 目录。"
            )
        )

        self.table = QTableWidget(0, 4)
        self.table.setObjectName("CorePhotoTable")
        self.table.setHorizontalHeaderLabels(["顶深", "底深", "图片", "备注"])
        header = self.table.horizontalHeader()
        header.setSectionResizeMode(COL_TOP, QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(COL_BOTTOM, QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(COL_IMAGE, QHeaderView.ResizeMode.Stretch)
        header.setSectionResizeMode(COL_LABEL, QHeaderView.ResizeMode.ResizeToContents)
        layout.addWidget(self.table)

        row_buttons = QHBoxLayout()
        add_btn = QPushButton("添加段")
        add_btn.setObjectName("CorePhotoAddRow")
        add_btn.clicked.connect(self._add_empty_row)
        row_buttons.addWidget(add_btn)
        del_btn = QPushButton("删除段")
        del_btn.setObjectName("CorePhotoDeleteRow")
        del_btn.clicked.connect(self._delete_selected_rows)
        row_buttons.addWidget(del_btn)
        pick_btn = QPushButton("选择图片")
        pick_btn.setObjectName("CorePhotoPickImage")
        pick_btn.clicked.connect(self._pick_image_for_selected)
        row_buttons.addWidget(pick_btn)
        row_buttons.addStretch(1)
        layout.addLayout(row_buttons)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok
            | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(self._on_accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

        for seg in (current.segments if current is not None else []):
            self._add_row(seg)
        if self.table.rowCount() == 0:
            self._add_empty_row()

    # ------------------------------------------------------------------
    # Row management
    # ------------------------------------------------------------------
    def _add_empty_row(self) -> None:
        r = self.table.rowCount()
        self.table.insertRow(r)
        self.table.setItem(r, COL_TOP, QTableWidgetItem(""))
        self.table.setItem(r, COL_BOTTOM, QTableWidgetItem(""))
        self.table.setItem(r, COL_IMAGE, QTableWidgetItem(""))
        self.table.setItem(r, COL_LABEL, QTableWidgetItem(""))

    def _add_row(self, seg: CorePhotoSegment) -> None:
        r = self.table.rowCount()
        self.table.insertRow(r)
        self.table.setItem(r, COL_TOP, QTableWidgetItem(f"{seg.top:g}"))
        self.table.setItem(r, COL_BOTTOM, QTableWidgetItem(f"{seg.bottom:g}"))
        self.table.setItem(r, COL_IMAGE, QTableWidgetItem(seg.image_path))
        self.table.setItem(r, COL_LABEL, QTableWidgetItem(seg.label))

    def _delete_selected_rows(self) -> None:
        rows = sorted({i.row() for i in self.table.selectedIndexes()}, reverse=True)
        for r in rows:
            self.table.removeRow(r)

    def _pick_image_for_selected(self) -> None:
        row = self.table.currentRow()
        if row < 0:
            return
        path, _ = QFileDialog.getOpenFileName(
            self, "选择岩心照片", "", "图片 (*.png *.jpg *.jpeg *.bmp);;All (*.*)"
        )
        if path:
            item = self.table.item(row, COL_IMAGE)
            if item is None:
                self.table.setItem(row, COL_IMAGE, QTableWidgetItem(path))
            else:
                item.setText(path)

    # ------------------------------------------------------------------
    # Accept / value
    # ------------------------------------------------------------------
    def _on_accept(self) -> None:
        for r in range(self.table.rowCount()):
            top = self._cell_text(r, COL_TOP)
            bottom = self._cell_text(r, COL_BOTTOM)
            image = self._cell_text(r, COL_IMAGE)
            if not top and not bottom and not image:
                continue  # empty rows are dropped
            try:
                t = float(top)
                b = float(bottom)
            except (TypeError, ValueError):
                QMessageBox.warning(
                    self, "岩心照片段不完整", f"第 {r + 1} 行：顶深/底深必须是数字。"
                )
                return
            if b <= t:
                QMessageBox.warning(
                    self, "岩心照片段无效", f"第 {r + 1} 行：底深必须大于顶深。"
                )
                return
            if not image:
                QMessageBox.warning(
                    self, "岩心照片段不完整", f"第 {r + 1} 行：请选择图片文件。"
                )
                return
        self.accept()

    def value(self) -> tuple[CorePhotoModel, list[str]]:
        """Return ``(model, diagnostics)``.

        Image files referenced by absolute paths are copied into the
        well's ``core_photos/`` directory; the model stores relative
        filenames. Diagnostics list copy failures (the segment keeps its
        original absolute path so it can still render).
        """
        segments: list[CorePhotoSegment] = []
        diagnostics: list[str] = []
        for r in range(self.table.rowCount()):
            top = self._cell_text(r, COL_TOP)
            bottom = self._cell_text(r, COL_BOTTOM)
            image = self._cell_text(r, COL_IMAGE)
            if not top or not bottom or not image:
                continue
            try:
                t = float(top)
                b = float(bottom)
            except (TypeError, ValueError):
                continue
            if b <= t:
                continue
            stored = image
            # If the path is absolute and an image_dir is set, copy it in.
            if self._image_dir is not None and Path(image).is_absolute():
                src = Path(image)
                if src.is_file():
                    self._image_dir.mkdir(parents=True, exist_ok=True)
                    dest = self._image_dir / f"{uuid.uuid4().hex}{src.suffix.lower()}"
                    try:
                        shutil.copy2(src, dest)
                        stored = dest.name
                    except OSError as exc:
                        diagnostics.append(f"复制图片失败 {src.name}: {exc}")
                else:
                    diagnostics.append(f"图片不存在: {src}")
            segments.append(
                CorePhotoSegment(
                    id="",
                    top=t,
                    bottom=b,
                    image_path=stored,
                    label=self._cell_text(r, COL_LABEL).strip(),
                )
            )
        segments.sort(key=lambda s: (s.top, s.bottom))
        return (
            CorePhotoModel(well_id=self._well_id, unit=self._unit, segments=segments),
            diagnostics,
        )

    def _cell_text(self, row: int, col: int) -> str:
        item = self.table.item(row, col)
        return item.text().strip() if item is not None else ""
