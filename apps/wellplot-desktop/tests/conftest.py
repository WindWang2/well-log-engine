"""Qt/pytest defaults for WellPlot Desktop tests."""

from __future__ import annotations

import os

import pytest

# Must be set before QApplication is created.
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")


def qimage_pixel_bytes(image) -> bytes:
    """Copy ARGB32 pixels so tests compare content, not ``constBits()`` pointers.

    ``QImage.constBits()`` is a live buffer view. Comparing two views with
    ``!=`` is heap-pointer inequality and is true for any two distinct
    images (#613). ``bytes(...)`` snapshots the pixels.
    """
    from PySide6.QtGui import QImage

    if image is None or image.isNull():
        return b""
    converted = image.convertToFormat(QImage.Format.Format_ARGB32)
    data = bytes(converted.constBits())
    nbytes = int(converted.sizeInBytes())
    return data[:nbytes]


@pytest.fixture
def pixel_bytes():
    return qimage_pixel_bytes


# Round-2 review (K-F3): port the monorepo's deferred-delete cleanup so the
# workstation suite does not segfault at interpreter exit under offscreen.
def pytest_runtest_teardown(item, nextitem):  # noqa: ANN001
    try:
        from PySide6.QtCore import QCoreApplication

        app = QCoreApplication.instance()
        if app is not None:
            app.sendPostedEvents(None, 0)
            app.processEvents()
    except Exception:
        pass


# A QMimeData left on the clipboard is owned by Qt's process-global clipboard,
# whose destructor runs in the C-level atexit chain AFTER Python finalized;
# Shiboken::Object::destroy then touches a NULL thread state and segfaults at
# exit (exit 139 after a fully green run, reproducible via the table clipboard
# test — seen again in the wheel-job workstation leg). Drop clipboard payloads
# while the interpreter is still alive.
def pytest_sessionfinish(session, exitstatus):  # noqa: ANN001
    try:
        from PySide6.QtWidgets import QApplication

        app = QApplication.instance()
        if app is not None:
            QApplication.clipboard().clear()
            app.processEvents()
    except Exception:
        pass
