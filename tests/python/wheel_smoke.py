from PySide6.QtWidgets import QApplication, QVBoxLayout, QWidget
import numpy as np

from welllog import WellLogView


app = QApplication.instance() or QApplication([])
host = QWidget()
layout = QVBoxLayout(host)
view = WellLogView()
layout.addWidget(view)
depth = np.arange(4, dtype=np.float64)
values = np.arange(4, dtype=np.float32)
depth.flags.writeable = False
values.flags.writeable = False
report = view.submit_curve(
    depth,
    values,
    "30000000-0000-4000-8000-000000000001",
    "30000000-0000-4000-8000-000000000002",
    "30000000-0000-4000-8000-000000000003",
    "GR",
    "m",
    "API",
)
assert report["depth"]["access_mode"] == "zero_copy"
assert report["curve"]["access_mode"] == "zero_copy"
assert view.parentWidget() is host
