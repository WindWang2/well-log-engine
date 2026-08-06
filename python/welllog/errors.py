"""Stable Python exception categories for synchronous WellLogEngine failures."""

from __future__ import annotations


class WellLogError(RuntimeError):
    """Base class carrying the stable native error code."""

    def __init__(self, message: str, code: str) -> None:
        super().__init__(message)
        self.code = code


class WellLogValidationError(WellLogError):
    """The submitted document, buffer, or identifier is invalid."""


class WellLogVersionConflict(WellLogError):
    """A command targeted an obsolete document revision."""


class WellLogCapabilityError(WellLogError):
    """The requested rendering capability is unavailable."""


class WellLogThreadError(WellLogError):
    """A GUI-thread-only operation was called from another thread."""


class WellLogExportError(WellLogError):
    """A synchronous export operation failed."""
