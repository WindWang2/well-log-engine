"""Qt/pytest defaults for WellPlot Desktop tests."""

from __future__ import annotations

import os

# Must be set before QApplication is created.
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
