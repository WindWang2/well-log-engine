"""PySide6 bindings for the native WellLogEngine widget."""

from .errors import (
    WellLogCapabilityError,
    WellLogError,
    WellLogExportError,
    WellLogThreadError,
    WellLogValidationError,
    WellLogVersionConflict,
)
import os
import sys
from pathlib import Path

if sys.platform == "win32" and hasattr(os, "add_dll_directory"):
    bin_dir = Path(__file__).resolve().parent.parent.parent / "bin"
    if bin_dir.exists():
        try:
            os.add_dll_directory(str(bin_dir))
        except OSError:
            pass

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
