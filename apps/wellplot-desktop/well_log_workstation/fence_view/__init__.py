"""FenceView — 3D curtain/fence mesh surface (Phase-2, T6 / #250).

Wraps a pyqtgraph ``GLViewWidget`` fed by ``geoviz.generate_fence_mesh``
(the promoted headless numpy mesh from T10 PR-A). Orbit camera defaults come
from ``Renderer3D`` (``distance=500, elevation=30, azimuth=45``). Vector
export is NOT supported (T8: PNG only via ``grabFramebuffer()``) — this
module only exposes the PNG grab.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
from PySide6.QtCore import Qt
from PySide6.QtWidgets import QLabel, QVBoxLayout, QWidget

from well_log_workstation.three_d_bridge import probe_3d


class FenceView(QWidget):
    """3D fence surface; safe to construct when 3D is unavailable."""

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self.setObjectName("FenceView")
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        self._view = None
        self._placeholder = QLabel(
            "三维栅状图需要 pyqtgraph + OpenGL。请安装依赖后重试。"
        )
        self._placeholder.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._placeholder.setWordWrap(True)
        layout.addWidget(self._placeholder)

    def three_d_available(self) -> bool:
        return probe_3d().available

    def _ensure_view(self):
        if self._view is not None:
            return self._view
        cap = probe_3d()
        if not cap.available or cap.gl_view_cls is None:
            return None
        view = cap.gl_view_cls()
        view.setBackgroundColor("#1e1e2e")
        # Renderer3D default camera (geoviz_seismic/renderer_3d.py:1033-1037)
        view.setCameraPosition(distance=500, elevation=30, azimuth=45)
        self._view = view
        self.layout().removeWidget(self._placeholder)
        self._placeholder.deleteLater()
        self.layout().addWidget(self._view, 1)
        return view

    def set_wells(self, wells: list[dict], nz_samples: int = 20) -> None:
        """Build the fence mesh from well dicts and add it to the view.

        Well dict shape: ``{"name", "x", "y", "depth"}`` (same as
        ``geoviz.generate_fence_mesh``). Existing mesh items are replaced on
        re-call so the view reflects updated data.
        """
        view = self._ensure_view()
        if view is None:
            return
        from geoviz import generate_fence_mesh

        verts, faces, colors = generate_fence_mesh(wells, nz_samples=nz_samples)
        # Remove previous mesh items
        from pyqtgraph.opengl import GLMeshItem

        for item in list(view.items):
            if isinstance(item, GLMeshItem):
                view.removeItem(item)
        if verts.size == 0:
            return
        mesh = GLMeshItem(
            vertexes=verts,
            faces=faces,
            faceColors=colors,
            smooth=False,
            drawEdges=False,
        )
        view.addItem(mesh)

    def grab_fence_png(self, path: Path | str) -> Path:
        """Save the current frame as PNG via grabFramebuffer (T8: PNG only)."""
        if self._view is None:
            raise RuntimeError("栅状图视图未构建（3D 不可用）")
        out = Path(path)
        out.parent.mkdir(parents=True, exist_ok=True)
        if not self._view.grabFramebuffer().save(str(out)):
            raise RuntimeError(f"无法保存栅状图 PNG: {out}")
        return out
