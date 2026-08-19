from typing import Any

from PySide6.QtOpenGLWidgets import QOpenGLWidget
from PySide6.QtWidgets import QWidget

from .errors import (
    WellLogCapabilityError,
    WellLogError,
    WellLogExportError,
    WellLogThreadError,
    WellLogValidationError,
    WellLogVersionConflict,
)


class WellLogView(QOpenGLWidget):
    def __init__(self, parent: QWidget | None = ...) -> None: ...
    def submit_curve(
        self,
        depth: Any,
        values: Any,
        document_id: str,
        axis_id: str,
        curve_id: str,
        mnemonic: str,
        depth_unit: str,
        value_unit: str,
    ) -> dict[str, object]: ...
    def submit_multi_track(self, payload: dict[str, Any]) -> dict[str, object]: ...
    def append_curves(self, payload: dict[str, Any]) -> dict[str, object]: ...
    def patch_document(self, payload: dict[str, Any]) -> dict[str, object]: ...
    def document_metrics(self, document_id: str) -> dict[str, object]: ...
    def poll_session(self) -> None: ...
    def submit_multi_well_section(
        self, payload: dict[str, Any]
    ) -> dict[str, object]: ...
    def clear_multi_well_section(self) -> None: ...
    def sample_value(self, curve_id: str, sample_index: int) -> float | None: ...
    def reset_viewport(self) -> None: ...
    def export_scene_svg(
        self,
        document_id: str,
        export_pixel_height: int = 0,
    ) -> bytes: ...
    def export_scene_pdf(
        self,
        document_id: str,
        export_pixel_height: int = 0,
        searchable_text: bool = False,
        crop_marks: bool = False,
        layered_pdf: bool = False,
        show_depth_ruler: bool = False,
    ) -> bytes: ...
    def export_scene_cgm(
        self,
        document_id: str,
        page_height_mm: float = 0.0,
    ) -> bytes: ...
