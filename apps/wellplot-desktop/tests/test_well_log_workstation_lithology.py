"""Single-well lithology track — model, template binding, canvas, export, editor.

Covers the FRS §2.x「岩性描述道」gap and roadmap P0-B acceptance「单井岩性道可见」:
* lithology_model: normalize (sort / id / drop invalid), save-load roundtrip,
  missing/corrupt file degradation, demo stub generator;
* template binding: ``apply_template`` and the Display Set path both emit
  ``role == "litho"`` tracks (with or without data);
* canvas: MultiTrackCanvas paints litho bands (segments and empty track);
* export: SVG export runs the shared ``paint_litho_bands`` path;
* editor: LithologyDialog value() roundtrip, demo fill, empty-row drop;
* shell: applying「标准岩性图」shows a litho track bound to ``lithology.json``,
  and re-applying after an edit picks up fresh segments.
"""

from __future__ import annotations

import os
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtWidgets import QPushButton, QTableWidgetItem

from well_log_workstation.display_set import presentation_from_display_set
from well_log_workstation.export_plot import export_presentation_svg
from well_log_workstation.las_import import import_las_into_workspace
from well_log_workstation.lithology_dialog import LithologyDialog
from well_log_workstation.lithology_model import (
    LithologyModel,
    LithologySegment,
    lithology_file_path,
    load_lithology_for_well,
    make_stub_lithology,
    normalize_segments,
    save_lithology_for_well,
)
from well_log_workstation.multi_track_canvas import MultiTrackCanvas
from well_log_workstation.shell import WellLogWorkstationWindow
from well_log_workstation.template_model import (
    apply_template,
    get_builtin_template,
)
from well_log_workstation.workspace import create_workspace


def _write_las(path: Path) -> Path:
    path.write_text(
        """~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1005.0
STEP.M 1.0
NULL. -999.25
WELL. LITHO-1
~CURVE INFORMATION
DEPT.M
GR.GAPI
~ASCII
1000 20
1001 30
1002 40
1003 50
1004 60
1005 70
""",
        encoding="utf-8",
    )
    return path


def _doc_with_lithology(tmp_path: Path, segments) -> tuple:
    ws = create_workspace(tmp_path / "ws")
    result = import_las_into_workspace(ws, _write_las(tmp_path / "m.las"))
    result.document.lithology = LithologyModel(
        well_id=result.catalog_well_id, segments=segments
    )
    return ws, result


# ---------------------------------------------------------------------------
# Model (Qt-free)
# ---------------------------------------------------------------------------


def test_normalize_sorts_and_assigns_ids() -> None:
    segs, diags = normalize_segments(
        [
            LithologySegment(id="", top=50.0, bottom=60.0, pattern_id="syt-sandstone"),
            LithologySegment(id="", top=10.0, bottom=20.0, pattern_id="syt-mudstone"),
            LithologySegment(id="keep", top=30.0, bottom=40.0, pattern_id="syt-shale"),
        ]
    )
    assert not diags
    assert [s.top for s in segs] == [10.0, 30.0, 50.0]
    assert all(s.id for s in segs)
    assert segs[1].id == "keep"  # sorted position 2 keeps its id


def test_normalize_drops_invalid_segments() -> None:
    segs, diags = normalize_segments(
        [
            LithologySegment(id="a", top=20.0, bottom=20.0, pattern_id="syt-shale"),
            LithologySegment(id="b", top=float("nan"), bottom=30.0, pattern_id="syt-shale"),
            LithologySegment(id="c", top=10.0, bottom=30.0, pattern_id="  "),
            LithologySegment(id="d", top=10.0, bottom=30.0, pattern_id="syt-shale"),
        ]
    )
    assert [s.id for s in segs] == ["d"]
    assert len(diags) == 3


def test_save_load_roundtrip(tmp_path: Path) -> None:
    ws, result = _doc_with_lithology(
        tmp_path,
        [
            LithologySegment(
                id="s1", top=20.0, bottom=40.0, pattern_id="syt-sandstone", label="砂岩"
            ),
            LithologySegment(id="s2", top=10.0, bottom=20.0, pattern_id="syt-mudstone"),
        ],
    )
    path = save_lithology_for_well(
        ws, LithologyModel(well_id=result.catalog_well_id, segments=result.document.lithology.segments)
    )
    assert path.name == "lithology.json"
    assert path.is_file()
    loaded, diags = load_lithology_for_well(ws, result.catalog_well_id)
    assert not diags
    assert loaded.well_id == result.catalog_well_id
    assert [s.top for s in loaded.segments] == [10.0, 20.0]  # sorted on load
    assert loaded.segments[0].pattern_id == "syt-mudstone"
    assert loaded.segments[1].label == "砂岩"


def test_load_missing_file_returns_empty_model(tmp_path: Path) -> None:
    ws, result = _doc_with_lithology(tmp_path, [])
    model, diags = load_lithology_for_well(ws, result.catalog_well_id)
    assert model.segments == []
    assert not diags


def test_load_corrupt_file_reports_diagnostics(tmp_path: Path) -> None:
    ws, result = _doc_with_lithology(tmp_path, [])
    path = lithology_file_path(ws, result.catalog_well_id)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("{ not json", encoding="utf-8")
    model, diags = load_lithology_for_well(ws, result.catalog_well_id)
    assert model.segments == []
    assert any("无法读取" in d for d in diags)


def test_make_stub_lithology_covers_range() -> None:
    segs = make_stub_lithology(1000.0, 1100.0)
    assert len(segs) == 5
    assert segs[0].top == 1000.0
    assert segs[-1].bottom == 1100.0
    for a, b in zip(segs, segs[1:]):
        assert a.bottom == b.top  # contiguous bands
    assert all(s.pattern_id.startswith("syt-") for s in segs)
    assert all(s.label for s in segs)


# ---------------------------------------------------------------------------
# Template binding (Qt-free)
# ---------------------------------------------------------------------------


def test_apply_template_binds_litho_track(tmp_path: Path) -> None:
    ws, result = _doc_with_lithology(
        tmp_path,
        [LithologySegment(id="x", top=1000.0, bottom=1003.0, pattern_id="syt-sandstone")],
    )
    template = get_builtin_template("std-litho")
    assert template is not None
    pres = apply_template(template, result.document)
    litho = [t for t in pres.tracks if t.role == "litho"]
    assert len(litho) == 1
    assert litho[0].title == "岩性"
    assert [s.pattern_id for s in litho[0].litho_segments] == ["syt-sandstone"]
    assert any(t.role == "depth" for t in pres.tracks)
    # GR curve still binds from the same template
    assert pres.curve_track_count >= 1


def test_apply_template_litho_without_data(tmp_path: Path) -> None:
    ws, result = _doc_with_lithology(tmp_path, [])
    result.document.lithology = None
    template = get_builtin_template("std-litho")
    pres = apply_template(template, result.document)
    litho = [t for t in pres.tracks if t.role == "litho"]
    assert len(litho) == 1
    assert litho[0].litho_segments == []  # empty band, not dropped


def test_display_set_presentation_includes_litho_track(tmp_path: Path) -> None:
    ws, result = _doc_with_lithology(
        tmp_path,
        [LithologySegment(id="x", top=1000.0, bottom=1002.0, pattern_id="syt-shale")],
    )
    template = get_builtin_template("std-litho")
    pres = presentation_from_display_set(template, result.document, frozenset())
    roles = [t.role for t in pres.tracks]
    assert "litho" in roles
    litho = next(t for t in pres.tracks if t.role == "litho")
    assert len(litho.litho_segments) == 1
    # Empty Display Set → no curve tracks, but litho stays bound (like depth).
    assert pres.curve_track_count == 0


# ---------------------------------------------------------------------------
# Canvas + export (offscreen Qt)
# ---------------------------------------------------------------------------


def test_canvas_paints_litho_track(qtbot, tmp_path: Path) -> None:
    ws, result = _doc_with_lithology(tmp_path, make_stub_lithology(1000.0, 1005.0))
    template = get_builtin_template("std-litho")
    pres = apply_template(template, result.document)
    canvas = MultiTrackCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(900, 600)
    canvas.set_presentation(pres)
    pix = canvas.grab()  # forces paintEvent
    assert not pix.isNull()


def test_canvas_paints_empty_litho_track(qtbot, tmp_path: Path) -> None:
    ws, result = _doc_with_lithology(tmp_path, [])
    template = get_builtin_template("std-litho")
    pres = apply_template(template, result.document)
    canvas = MultiTrackCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(900, 600)
    canvas.set_presentation(pres)
    pix = canvas.grab()
    assert not pix.isNull()


def test_svg_export_includes_litho_bands(qtbot, tmp_path: Path) -> None:
    ws, result = _doc_with_lithology(tmp_path, make_stub_lithology(1000.0, 1005.0))
    template = get_builtin_template("std-litho")
    pres = apply_template(template, result.document)
    out = export_presentation_svg(pres, tmp_path / "litho.svg", width=900, height=1200)
    text = out.read_text(encoding="utf-8")
    assert "砂岩" in text  # litho band label rendered by the shared paint path


# ---------------------------------------------------------------------------
# Editor dialog
# ---------------------------------------------------------------------------


def test_dialog_value_roundtrip(qtbot) -> None:
    model = LithologyModel(
        well_id="w1",
        segments=[
            LithologySegment(
                id="s1", top=100.0, bottom=110.0, pattern_id="syt-sandstone", label="砂岩"
            ),
        ],
    )
    dlg = LithologyDialog(model, depth_range=(100.0, 200.0))
    qtbot.addWidget(dlg)
    assert dlg.table.rowCount() == 1
    out = dlg.value()
    assert out.well_id == "w1"
    assert len(out.segments) == 1
    assert out.segments[0].pattern_id == "syt-sandstone"
    assert out.segments[0].top == 100.0


def test_dialog_demo_fill_replaces_rows(qtbot) -> None:
    dlg = LithologyDialog(None, depth_range=(1000.0, 1100.0))
    qtbot.addWidget(dlg)
    stub_btn = dlg.findChild(QPushButton, "LithologyFillStub")
    assert stub_btn is not None
    stub_btn.click()
    assert dlg.table.rowCount() == 5
    out = dlg.value()
    assert len(out.segments) == 5
    assert out.segments[0].top == 1000.0
    assert out.segments[-1].bottom == 1100.0


def test_dialog_drops_empty_rows(qtbot) -> None:
    dlg = LithologyDialog(None)
    qtbot.addWidget(dlg)
    assert dlg.table.rowCount() == 1  # seeded empty row
    dlg._add_empty_row()
    # Fill top/bottom on the second row; pattern combo defaults to first item.
    dlg.table.setItem(1, 0, QTableWidgetItem("10"))
    dlg.table.setItem(1, 1, QTableWidgetItem("20"))
    out = dlg.value()
    assert len(out.segments) == 1
    assert out.segments[0].top == 10.0
    assert out.segments[0].bottom == 20.0


# ---------------------------------------------------------------------------
# Shell integration
# ---------------------------------------------------------------------------


def test_shell_std_litho_template_shows_track(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws", name="Litho")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(_write_las(tmp_path / "m.las"))
    save_lithology_for_well(
        ws,
        LithologyModel(well_id=well_id, segments=make_stub_lithology(1000.0, 1005.0)),
    )
    pres = win.apply_template_to_well(well_id, "std-litho")
    litho = [t for t in pres.tracks if t.role == "litho"]
    assert len(litho) == 1
    assert len(litho[0].litho_segments) == 5
    assert win.multi_track_canvas.track_count() >= 3
    win.multi_track_canvas.grab()  # paint smoke


def test_shell_reapply_after_edit_refreshes_segments(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws", name="Litho")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(_write_las(tmp_path / "m.las"))
    pres = win.apply_template_to_well(well_id, "std-litho")
    litho = next(t for t in pres.tracks if t.role == "litho")
    assert litho.litho_segments == []
    # What the editor would write after OK (lithology.json on disk)…
    save_lithology_for_well(
        ws,
        LithologyModel(
            well_id=well_id,
            segments=[
                LithologySegment(id="n", top=1000.0, bottom=1003.0, pattern_id="syt-shale")
            ],
        ),
    )
    pres2 = win.apply_template_to_well(well_id, "std-litho")
    litho2 = next(t for t in pres2.tracks if t.role == "litho")
    assert [s.pattern_id for s in litho2.litho_segments] == ["syt-shale"]


def test_shell_lithology_file_persists_across_reopen(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws", name="Litho")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(_write_las(tmp_path / "m.las"))
    save_lithology_for_well(
        ws,
        LithologyModel(
            well_id=well_id,
            segments=[
                LithologySegment(id="p", top=1001.0, bottom=1004.0, pattern_id="syt-limestone")
            ],
        ),
    )
    # Simulate a fresh window / reopened workspace
    win2 = WellLogWorkstationWindow()
    qtbot.addWidget(win2)
    win2.set_workspace(ws)
    pres = win2.apply_template_to_well(well_id, "std-litho")
    litho = next(t for t in pres.tracks if t.role == "litho")
    assert [s.pattern_id for s in litho.litho_segments] == ["syt-limestone"]


def test_shell_has_lithology_menu_action(qtbot) -> None:
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    assert win._act_litho is not None
    assert not win._act_litho.isEnabled()  # disabled until a workspace opens
