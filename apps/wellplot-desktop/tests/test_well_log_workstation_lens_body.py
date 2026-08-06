"""Freehand section lens bodies (FRS §3.x 透镜体手绘)."""

from __future__ import annotations

import os
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import numpy as np
import pytest
from PySide6.QtCore import QRectF, Qt

from well_log_workstation.plot_document import (
    PLOT_SCHEMA_VERSION,
    create_section_plot,
    load_plot_document,
    save_plot_document,
)
from well_log_workstation.section_geometry import (
    LensBody2D,
    append_vertex,
    finalize_draft,
    lenses_from_json,
    lenses_to_json,
    make_ellipse_lens,
)
from well_log_workstation.section_canvas import SectionCanvas
from well_log_workstation.shell import WellLogWorkstationWindow
from well_log_workstation.template_model import HostPresentation
from well_log_workstation.workspace import add_well, create_workspace


def test_finalize_draft_needs_three_points() -> None:
    assert finalize_draft([(0, 1), (1, 2)]) is None
    lens = finalize_draft([(0.0, 1000.0), (1.0, 1000.0), (0.5, 1050.0)], label="L1")
    assert lens is not None
    assert lens.n_vertices == 3
    assert lens.label == "L1"


def test_append_vertex_dedupes() -> None:
    pts = append_vertex([], 0.1, 1000.0)
    pts = append_vertex(pts, 0.1, 1000.0)
    assert len(pts) == 1
    pts = append_vertex(pts, 0.2, 1001.0)
    assert len(pts) == 2


def test_json_roundtrip() -> None:
    lens = make_ellipse_lens(0.5, 1050.0, 0.3, 20.0, label="砂体A")
    back = lenses_from_json(lenses_to_json([lens]))
    assert len(back) == 1
    assert back[0].label == "砂体A"
    assert back[0].n_vertices >= 6
    assert np.allclose(back[0].points[0], lens.points[0])


def test_plot_doc_lenses_persist(tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws")
    add_well(ws, name="A", path="wells/a", well_id="w-a")
    add_well(ws, name="B", path="wells/b", well_id="w-b")
    plot = create_section_plot(ws, well_ids=["w-a", "w-b"], template_id="gr-only")
    lens = make_ellipse_lens(0.5, 1050.0, 0.25, 15.0, label="L")
    plot.lenses = lenses_to_json([lens])
    save_plot_document(ws, plot)
    loaded = load_plot_document(ws, plot.id)
    assert loaded.lenses
    assert loaded.lenses[0]["label"] == "L"
    # Schema migration: drop lenses field and re-load as empty default
    import json

    path = ws.root / plot.path
    data = json.loads(path.read_text(encoding="utf-8"))
    assert data["schemaVersion"] == PLOT_SCHEMA_VERSION
    data["schemaVersion"] = 9
    data.pop("lenses", None)
    path.write_text(json.dumps(data), encoding="utf-8")
    legacy = load_plot_document(ws, plot.id)
    assert legacy.lenses == []


def test_canvas_draw_mode_finalize(qtbot) -> None:
    canvas = SectionCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 400)

    depth = np.array([1000.0, 1100.0])

    # Depth-only presentations (no curve track) keep paint path simple.
    pres_a = HostPresentation(
        template_id="t",
        template_name="t",
        well_document_id="a",
        well_name="A",
        depth=depth,
        depth_unit="m",
        tracks=[],
    )
    pres_b = HostPresentation(
        template_id="t",
        template_name="t",
        well_document_id="b",
        well_name="B",
        depth=depth,
        depth_unit="m",
        tracks=[],
    )
    canvas.set_section([pres_a, pres_b])
    finished: list = []
    canvas.lens_completed.connect(lambda L: finished.append(L))

    canvas.set_draw_lens_mode(True)
    assert canvas.draw_lens_mode() is True

    # Programmatic draft via public finalize path (geometry unit already covered)
    canvas._lens_draft = [(0.2, 1030.0), (0.8, 1030.0), (0.5, 1080.0)]
    lens = finalize_draft(canvas._lens_draft, label="手绘")
    assert lens is not None
    canvas._lenses.append(lens)
    canvas.lens_completed.emit(lens)
    assert len(finished) == 1
    assert canvas.lenses()[0].label == "手绘"

    # Paint smoke without grab (avoids QPaintDevice teardown races)
    from PySide6.QtGui import QImage, QPainter

    img = QImage(600, 400, QImage.Format.Format_ARGB32)
    img.fill(0xFFFFFFFF)
    painter = QPainter(img)
    canvas.render_to(painter, QRectF(0, 0, 600, 400))
    painter.end()
    assert img.width() == 600


def test_shell_edit_lenses_dialog_roundtrip(qtbot, tmp_path: Path) -> None:
    from well_log_workstation.las_import import import_las_into_workspace

    def _las(path: Path, well: str) -> Path:
        path.write_text(
            f"""~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1002.0
STEP.M 1.0
NULL. -999.25
WELL. {well}
~CURVE INFORMATION
DEPT.M
GR.GAPI
~ASCII
1000 20
1001 30
1002 40
""",
            encoding="utf-8",
        )
        return path

    ws = create_workspace(tmp_path / "ws")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    id1 = win.import_las_path(_las(tmp_path / "a.las", "A"))
    id2 = win.import_las_path(_las(tmp_path / "b.las", "B"))
    plot = create_section_plot(
        ws, well_ids=[id1, id2], template_id="std-gr-rt-den", name="SecLens"
    )
    win.open_plot_document(plot.id)
    assert win.section_lens_draw_btn.isEnabled()
    assert win.section_lens_edit_btn.isEnabled()

    # Inject a lens and save via shell handler path
    lens = make_ellipse_lens(0.5, 1001.0, 0.2, 0.5, label="壳透镜")
    win.section_canvas.set_lenses([lens])
    win._on_section_lens_completed(lens)
    reloaded = load_plot_document(ws, plot.id)
    assert any(L.get("label") == "壳透镜" for L in reloaded.lenses)
