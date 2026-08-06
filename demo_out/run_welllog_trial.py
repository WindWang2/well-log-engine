#!/usr/bin/env python3
"""Interactive WellLogEngine trial window.

Usage (from repo root, with a Python that has welllog + PySide6)::

    DISPLAY=:1 LIBGL_ALWAYS_SOFTWARE=1 \\
      path/to/python well-log-engine/demo_out/run_welllog_trial.py

Controls (engine-owned session)::
  - Mouse drag / wheel: pan & zoom depth (view gestures)
  - Window close: exit
"""

from __future__ import annotations

import math
import sys
from pathlib import Path

import numpy as np
from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QApplication,
    QHBoxLayout,
    QLabel,
    QMainWindow,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from welllog import WellLogView
from welllog._QtWidgets import welllog as wl_qt


def _make_curve(
    n: int,
    top: float,
    bottom: float,
    base: float,
    amp: float,
    phase: float,
    spike_at: float | None = None,
    spike_val: float = 140.0,
) -> tuple[np.ndarray, np.ndarray]:
    depth = np.linspace(top, bottom, n, dtype=np.float64)
    t = np.linspace(0.0, 10.0 * math.pi, n)
    values = (base + amp * np.sin(t + phase) + 0.15 * amp * np.sin(3.0 * t)).astype(
        np.float64
    )
    if spike_at is not None:
        idx = int(np.clip(np.searchsorted(depth, spike_at), 0, n - 1))
        values[idx] = spike_val
    depth.setflags(write=False)
    values.setflags(write=False)
    return depth, values


class TrialWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("WellLogEngine 试用 — 拖拽平移 / 滚轮缩放")
        self.resize(900, 1000)

        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)

        self.status = QLabel()
        self.status.setWordWrap(True)
        root.addWidget(self.status)

        btn_row = QHBoxLayout()
        self.btn_gr = QPushButton("加载 GR（5000 点）")
        self.btn_dense = QPushButton("加载密集曲线（5 万点）")
        self.btn_reset = QPushButton("重置视口")
        self.btn_gr.clicked.connect(lambda: self.load_demo("gr"))
        self.btn_dense.clicked.connect(lambda: self.load_demo("dense"))
        self.btn_reset.clicked.connect(self.reset_viewport)
        btn_row.addWidget(self.btn_gr)
        btn_row.addWidget(self.btn_dense)
        btn_row.addWidget(self.btn_reset)
        btn_row.addStretch(1)
        root.addLayout(btn_row)

        self.view = WellLogView()
        root.addWidget(self.view, 1)

        tip = QLabel(
            "提示：在曲线区域用鼠标拖动平移深度、滚轮缩放；"
            "右上角/手势以引擎实现为准。关闭窗口退出。"
        )
        tip.setStyleSheet("color: #666; font-size: 12px;")
        root.addWidget(tip)

        self._curve_id = "d0000000-0000-4000-8000-000000000003"
        self.load_demo("gr")

    def load_demo(self, kind: str) -> None:
        if kind == "dense":
            n = 50_000
            depth, values = _make_curve(
                n, 1000.0, 2000.0, 50.0, 35.0, 0.4, spike_at=1500.0
            )
            label = f"密集 GR · {n:,} 样点 · 1000–2000 m"
        else:
            n = 5_000
            depth, values = _make_curve(
                n, 1000.0, 1500.0, 40.0, 30.0, 0.0, spike_at=1250.0
            )
            label = f"GR · {n:,} 样点 · 1000–1500 m · 尖峰@1250m"

        doc = "d0000000-0000-4000-8000-000000000001"
        axis = "d0000000-0000-4000-8000-000000000002"
        curve = self._curve_id
        try:
            report = self.view.submit_curve(
                depth, values, doc, axis, curve, "GR", "m", "API"
            )
        except Exception as exc:  # pragma: no cover - UI feedback
            self.status.setText(f"提交失败: {exc}")
            return

        mode = (report or {}).get("curve", {}).get("access_mode", "?")
        prepared = (report or {}).get("render_prepared", False)
        mid = values[n // 2]
        try:
            sample = self.view.sample_value(curve, n // 2)
        except Exception:
            sample = mid
        self.status.setText(
            f"WellLogEngine 已就绪 · {label}\n"
            f"接入: {mode} · render_prepared={prepared} · "
            f"中点样点={sample:.2f}"
        )
        self.view.update()

    def reset_viewport(self) -> None:
        try:
            self.view.reset_viewport()
            self.view.update()
            self.status.setText(self.status.text().split("\n")[0] + "\n视口已重置")
        except Exception as exc:
            self.status.setText(f"重置视口失败: {exc}")


def main() -> int:
    wl_qt.configure_well_log_surface_format()
    app = QApplication(sys.argv)
    app.setApplicationName("WellLogEngine Trial")
    win = TrialWindow()
    win.show()
    win.raise_()
    win.activateWindow()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
