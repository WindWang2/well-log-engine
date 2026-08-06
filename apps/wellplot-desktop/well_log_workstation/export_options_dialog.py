"""PDF export options dialog (single-well engine + Qt correlation paths).

Shared product surface: text mode, crop marks, layered PDF. Callers pass
``engine_options=False`` for plot types that only use the Qt paint backend
(e.g. correlation): layered OCG and engine text-mode radios are disabled so
we do not claim multi-well engine behaviour; crop marks remain available.
"""

from __future__ import annotations

from typing import Literal

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

PlotTypeForPdfOptions = Literal["single_well", "correlation", "section"]


def pdf_options_applicability(plot_type: str) -> dict[str, bool]:
    """Which PDF option controls apply for a plot type.

    Pure helper for tests and shell routing — no Qt required beyond callers.
    """
    if plot_type == "single_well":
        return {
            "text_mode": True,
            "crop_marks": True,
            "layered_pdf": True,
            "engine_options": True,
        }
    if plot_type in ("correlation", "section"):
        # Qt paint paths (no engine multi-well / section PDF backend).
        return {
            "text_mode": False,
            "crop_marks": True,
            "layered_pdf": False,
            "engine_options": False,
        }
    return {
        "text_mode": False,
        "crop_marks": False,
        "layered_pdf": False,
        "engine_options": False,
    }


class PdfExportOptionsDialog(QDialog):
    """Choose PDF export options: text mode + crop marks + PDF layers."""

    def __init__(
        self,
        parent=None,
        *,
        text_mode: str = TEXT_MODE_OUTLINE,
        crop_marks: bool = False,
        layered_pdf: bool = False,
        plot_type: str = "single_well",
        engine_options: bool | None = None,
    ) -> None:
        super().__init__(parent)
        self.setWindowTitle("PDF 导出选项")
        self.setObjectName("PdfExportOptionsDialog")
        layout = QVBoxLayout(self)

        appl = pdf_options_applicability(plot_type)
        if engine_options is not None:
            self._engine_options = bool(engine_options)
        else:
            self._engine_options = bool(appl["engine_options"])
        self._plot_type = plot_type

        if self._engine_options:
            layout.addWidget(
                QLabel("文本模式（仅引擎图形 PDF 生效；Qt 回退路径无文本层）")
            )
        else:
            note = QLabel(
                "对比图/剖面 · Qt 矢量路径：无可搜索文本层与 PDF 图层（OCG）；"
                "剪切线由 Qt 绘制。"
            )
            note.setObjectName("PdfOptionsQtOnlyNote")
            note.setWordWrap(True)
            layout.addWidget(note)

        self.radio_outline = QRadioButton("引擎图形 PDF（不可搜索）")
        self.radio_outline.setObjectName("PdfTextModeOutline")
        self.radio_searchable = QRadioButton("可搜索 PDF（Base-14 拉丁文本层）")
        self.radio_searchable.setObjectName("PdfTextModeSearchable")
        if text_mode == TEXT_MODE_SEARCHABLE:
            self.radio_searchable.setChecked(True)
        else:
            self.radio_outline.setChecked(True)
        self.radio_outline.setEnabled(self._engine_options)
        self.radio_searchable.setEnabled(self._engine_options)
        layout.addWidget(self.radio_outline)
        layout.addWidget(self.radio_searchable)

        self.chk_crop_marks = QCheckBox("剪切线（四角裁切标记）")
        self.chk_crop_marks.setObjectName("PdfCropMarks")
        self.chk_crop_marks.setChecked(bool(crop_marks))
        self.chk_crop_marks.setEnabled(bool(appl.get("crop_marks", True)))
        layout.addWidget(self.chk_crop_marks)

        self.chk_layered = QCheckBox("PDF 图层（每图道一个 OCG，查看器可开关）")
        self.chk_layered.setObjectName("PdfLayered")
        self.chk_layered.setChecked(
            bool(layered_pdf) if self._engine_options else False
        )
        self.chk_layered.setEnabled(self._engine_options)
        if not self._engine_options:
            self.chk_layered.setToolTip(
                "分层 PDF（OCG）仅引擎单井路径支持；对比图为 Qt 导出，不可用。"
            )
            self.radio_outline.setToolTip("仅引擎单井 PDF 支持文本模式选择。")
            self.radio_searchable.setToolTip("仅引擎单井 PDF 支持文本模式选择。")
        layout.addWidget(self.chk_layered)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok
            | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

    def value(self) -> tuple[str, bool, bool]:
        """Return ``(text_mode, crop_marks, layered_pdf)``.

        When engine options are disabled (e.g. correlation), ``text_mode`` is
        forced to outline and ``layered_pdf`` is always False so callers never
        interpret UI state as engine multi-well export.
        """
        if not self._engine_options:
            return (
                TEXT_MODE_OUTLINE,
                self.chk_crop_marks.isChecked()
                if self.chk_crop_marks.isEnabled()
                else False,
                False,
            )
        mode = (
            TEXT_MODE_SEARCHABLE
            if self.radio_searchable.isChecked()
            else TEXT_MODE_OUTLINE
        )
        return (mode, self.chk_crop_marks.isChecked(), self.chk_layered.isChecked())
