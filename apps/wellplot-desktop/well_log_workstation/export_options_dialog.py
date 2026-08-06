"""PDF export options dialog for single-well engine exports (FRS §5).

Replaces the bare text-mode QMessageBox with a small options dialog: text
mode (engine outline vs searchable) plus the crop-marks and layered-PDF
toggles the engine binding accepts. Only the engine PDF path shows this —
the Qt fallback backend has neither OCG layers nor crop marks.
"""

from __future__ import annotations

from PySide6.QtWidgets import (
    QCheckBox,
    QDialog,
    QDialogButtonBox,
    QLabel,
    QRadioButton,
    QVBoxLayout,
)

# Text-mode values mirror export_dispatch's pdf_text_mode vocabulary.
TEXT_MODE_OUTLINE = "outline"
TEXT_MODE_SEARCHABLE = "searchable"


class PdfExportOptionsDialog(QDialog):
    """Choose PDF export options: text mode + crop marks + PDF layers."""

    def __init__(
        self,
        parent=None,
        *,
        text_mode: str = TEXT_MODE_OUTLINE,
        crop_marks: bool = False,
        layered_pdf: bool = False,
    ) -> None:
        super().__init__(parent)
        self.setWindowTitle("PDF 导出选项")
        self.setObjectName("PdfExportOptionsDialog")
        layout = QVBoxLayout(self)

        layout.addWidget(
            QLabel("文本模式（仅引擎图形 PDF 生效；Qt 回退路径无文本层）")
        )
        self.radio_outline = QRadioButton("引擎图形 PDF（不可搜索）")
        self.radio_outline.setObjectName("PdfTextModeOutline")
        self.radio_searchable = QRadioButton("可搜索 PDF（Base-14 拉丁文本层）")
        self.radio_searchable.setObjectName("PdfTextModeSearchable")
        if text_mode == TEXT_MODE_SEARCHABLE:
            self.radio_searchable.setChecked(True)
        else:
            self.radio_outline.setChecked(True)
        layout.addWidget(self.radio_outline)
        layout.addWidget(self.radio_searchable)

        self.chk_crop_marks = QCheckBox("剪切线（四角裁切标记）")
        self.chk_crop_marks.setObjectName("PdfCropMarks")
        self.chk_crop_marks.setChecked(bool(crop_marks))
        layout.addWidget(self.chk_crop_marks)

        self.chk_layered = QCheckBox("PDF 图层（每图道一个 OCG，查看器可开关）")
        self.chk_layered.setObjectName("PdfLayered")
        self.chk_layered.setChecked(bool(layered_pdf))
        layout.addWidget(self.chk_layered)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok
            | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

    def value(self) -> tuple[str, bool, bool]:
        """Return ``(text_mode, crop_marks, layered_pdf)``."""
        mode = (
            TEXT_MODE_SEARCHABLE
            if self.radio_searchable.isChecked()
            else TEXT_MODE_OUTLINE
        )
        return (mode, self.chk_crop_marks.isChecked(), self.chk_layered.isChecked())
