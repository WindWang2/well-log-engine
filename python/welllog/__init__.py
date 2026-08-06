"""PySide6 bindings for the native WellLogEngine widget."""

from .errors import (
    WellLogCapabilityError,
    WellLogError,
    WellLogExportError,
    WellLogThreadError,
    WellLogValidationError,
    WellLogVersionConflict,
)
from PySide6 import QtOpenGLWidgets as _PySideOpenGLWidgets
from PySide6 import QtWidgets as _PySideWidgets

from . import _QtWidgets

WellLogView = _QtWidgets.welllog.WellLogView
# TableModel may be absent from partial/extension builds — optional export.
TableModel = getattr(_QtWidgets.welllog, "TableModel", None)

__all__ = [
    "WellLogCapabilityError",
    "WellLogError",
    "WellLogExportError",
    "WellLogThreadError",
    "WellLogValidationError",
    "WellLogVersionConflict",
    "WellLogView",
    "TableModel",
]
