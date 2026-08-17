"""Core-photo image track (FRS §2.x 岩心照片道).

Covers the Qt-free data model (load/save/normalize), template binding
(role == "image" -> core_photo_segments), the shared paint helper
(paint_core_photos draws depth-ranged images), paint smoke for the
canvas and export, and the shell menu wiring. Mirrors the lithology
track tests.
"""

from __future__ import annotations

import os
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import numpy as np
import pytest
from PySide6.QtCore import QRectF
from PySide6.QtGui import QImage, QPainter

from well_log_workstation.core_photo_model import (
    CorePhotoModel,
    CorePhotoSegment,
    core_photo_dir,
    core_photo_file_path,
    load_core_photos_for_well,
    normalize_segments,
    save_core_photos_for_well,
)
from well_log_workstation.las_import import import_las_into_workspace
from well_log_workstation.template_model import (
    BoundTrack,
    HostPresentation,
    ScaleSpec,
    get_builtin_template,
)


def _write_las(path: Path, well: str = "W") -> Path:
    path.write_text(
        f"""~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1003.0
STEP.M 1.0
NULL. -999.25
WELL. {well}
~CURVE INFORMATION
DEPT.M
GR.GAPI
~ASCII
1000 10
1001 20
1002 30
1003 40
""",
        encoding="utf-8",
    )
    return path


def _workspace_with_well(tmp_path: Path):
    from well_log_workstation.workspace import create_workspace

    ws = create_workspace(tmp_path / "ws", name="CorePhoto")
    result = import_las_into_workspace(ws, _write_las(tmp_path / "m.las"))
    return ws, result.catalog_well_id


# -- pure model -----------------------------------------------------


def test_normalize_segments_drops_invalid_and_sorts() -> None:
    segs = [
        CorePhotoSegment(id="a", top=1010.0, bottom=1000.0, image_path="x.png"),
        CorePhotoSegment(id="b", top=1000.0, bottom=1010.0, image_path=""),
        CorePhotoSegment(id="c", top=1000.0, bottom=1010.0, image_path="c.png"),
        CorePhotoSegment(id="d", top=1005.0, bottom=1015.0, image_path="d.png"),
    ]
    out, diags = normalize_segments(segs)
    # a (bottom<=top) and b (empty path) dropped; c, d kept and sorted by top.
    assert [s.id for s in out] == ["c", "d"]
    assert len(diags) == 2


def test_save_load_roundtrip(tmp_path: Path) -> None:
    ws, well_id = _workspace_with_well(tmp_path)
    model = CorePhotoModel(
        well_id=well_id,
        segments=[
            CorePhotoSegment(id="s1", top=1000.0, bottom=1010.0, image_path="a.png"),
            CorePhotoSegment(id="s2", top=1010.0, bottom=1020.0, image_path="b.png", label="层1"),
        ],
    )
    save_core_photos_for_well(ws, model)
    assert core_photo_file_path(ws, well_id).is_file()
    loaded, diags = load_core_photos_for_well(ws, well_id)
    assert diags == []
    assert loaded.well_id == well_id
    assert len(loaded.segments) == 2
    assert loaded.segments[0].image_path == "a.png"
    assert loaded.segments[1].label == "层1"


def test_load_missing_file_returns_empty(tmp_path: Path) -> None:
    ws, well_id = _workspace_with_well(tmp_path)
    model, diags = load_core_photos_for_well(ws, well_id)
    assert model.segments == []
    assert diags == []


def test_core_photo_dir_under_well_data(tmp_path: Path) -> None:
    ws, well_id = _workspace_with_well(tmp_path)
    d = core_photo_dir(ws, well_id)
    assert d.name == "core_photos"
    assert d.parent.name == well_id


# -- template binding ------------------------------------------------


def test_std_core_photo_template_loads() -> None:
    tmpl = get_builtin_template("std-core-photo")
    assert tmpl is not None
    assert tmpl.id == "std-core-photo"
    roles = [str(t.get("role")) for t in tmpl.tracks]
    assert "image" in roles


def test_apply_template_binds_core_photo_segments() -> None:
    from well_log_workstation.las_import import ImportedCurve, ImportedWellDocument
    from well_log_workstation.template_model import apply_template

    doc = ImportedWellDocument(
        document_id="w",
        well_name="W",
        source_path="wells/w/a.las",
        depth=np.array([1000.0, 1010.0]),
        depth_unit="m",
        curves=[
            ImportedCurve(
                mnemonic="GR", unit="GAPI",
                values=np.array([10.0, 20.0]),
                null_mask=np.zeros(2, bool),
            )
        ],
    )
    doc.core_photos = CorePhotoModel(
        well_id="w",
        segments=[
            CorePhotoSegment(id="s1", top=1000.0, bottom=1010.0, image_path="a.png"),
        ],
    )
    tmpl = get_builtin_template("std-core-photo")
    pres = apply_template(tmpl, doc)
    image_tracks = [t for t in pres.tracks if t.role == "image"]
    assert len(image_tracks) == 1
    assert len(image_tracks[0].core_photo_segments) == 1
    assert image_tracks[0].core_photo_segments[0].image_path == "a.png"


# -- paint smoke (canvas + export) ----------------------------------


def _presentation_with_image(img_path: str) -> HostPresentation:
    depth = np.array([1000.0, 1010.0, 1020.0])
    track = BoundTrack(
        id="img",
        role="image",
        title="岩心照片",
        width_fraction=1.0,
        scale=None,
        core_photo_segments=[
            CorePhotoSegment(
                id="s1", top=1000.0, bottom=1020.0, image_path=img_path
            )
        ],
    )
    return HostPresentation(
        template_id="t",
        template_name="t",
        well_document_id="w",
        well_name="W",
        depth=depth,
        depth_unit="m",
        tracks=[track],
    )


def _make_png(path: Path) -> str:
    """Write a tiny valid PNG and return its absolute path."""
    img = QImage(4, 8, QImage.Format.Format_RGB32)
    img.fill(0xFF0000FF)  # red
    img.save(str(path), "PNG")
    return str(path)


def test_canvas_image_track_paint_smoke(qtbot, tmp_path: Path) -> None:
    from well_log_workstation.multi_track_canvas import MultiTrackCanvas

    img = _make_png(tmp_path / "core.png")
    canvas = MultiTrackCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 400)
    canvas.set_core_photo_resolver(lambda rel: rel if rel == img else None)
    canvas.set_presentation(_presentation_with_image(img))
    grabbed = canvas.grab()
    assert grabbed.width() == 600


def test_export_image_track_paint_smoke(qtbot, tmp_path: Path) -> None:
    from well_log_workstation.export_plot import _paint_presentation

    img = _make_png(tmp_path / "core.png")
    target = QImage(600, 400, QImage.Format.Format_ARGB32)
    target.fill(0xFFFFFFFF)
    painter = QPainter(target)
    _paint_presentation(
        painter,
        _presentation_with_image(img),
        QRectF(0, 0, 600, 400),
        core_photo_resolver=lambda rel: rel if rel == img else None,
    )
    painter.end()
    assert target.width() == 600


def test_image_track_paint_missing_image_no_crash(qtbot) -> None:
    """A segment whose image cannot be resolved falls back to gray, no crash."""
    from well_log_workstation.multi_track_canvas import MultiTrackCanvas

    canvas = MultiTrackCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 400)
    canvas.set_core_photo_resolver(lambda rel: None)  # nothing resolves
    canvas.set_presentation(_presentation_with_image("missing.png"))
    grabbed = canvas.grab()
    assert grabbed.width() == 600


# -- shell menu wiring ----------------------------------------------


def test_pick_image_button_writes_path(qtbot, tmp_path: Path, monkeypatch) -> None:
    """#729: 选择图片 must be wired and write the chosen path into COL_IMAGE."""
    from PySide6.QtWidgets import QFileDialog, QPushButton

    from well_log_workstation.core_photo_dialog import (
        COL_IMAGE,
        CorePhotoDialog,
    )

    dlg = CorePhotoDialog()
    qtbot.addWidget(dlg)
    btn = dlg.findChild(QPushButton, "CorePhotoPickImage")
    assert btn is not None
    assert "图片" in btn.text()
    dlg.table.setCurrentCell(0, COL_IMAGE)
    chosen = str(tmp_path / "core.png")
    monkeypatch.setattr(
        QFileDialog, "getOpenFileName", lambda *a, **k: (chosen, "")
    )
    btn.click()
    assert dlg.table.item(0, COL_IMAGE).text() == chosen


def test_shell_core_photo_menu_enabled_with_workspace(
    qtbot, tmp_path: Path
) -> None:
    from well_log_workstation.shell import WellLogWorkstationWindow
    from well_log_workstation.workspace import create_workspace

    ws = create_workspace(tmp_path / "ws", name="CorePhotoShell")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    # No workspace -> disabled.
    assert win._act_core_photo.isEnabled() is False
    win.set_workspace(ws)
    assert win._act_core_photo.isEnabled() is True
