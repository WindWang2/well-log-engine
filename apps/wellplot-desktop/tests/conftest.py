"""Qt/pytest defaults for WellPlot Desktop tests."""

from __future__ import annotations

import os

# Must be set before QApplication is created.
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")


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
