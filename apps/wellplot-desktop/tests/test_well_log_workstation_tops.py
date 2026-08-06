"""Formation tops on single-well and correlation plots (#223)."""

from __future__ import annotations

import json
import os
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.shell import WellLogWorkstationWindow  # noqa: E402
from well_log_workstation.tops_model import (  # noqa: E402
    FormationTop,
    TopsError,
    import_tops_from_json_file,
    load_tops_for_well,
    make_stub_tops,
    save_tops_for_well,
    tops_file_path,
)
from well_log_workstation.workspace import create_workspace  # noqa: E402


def _write_las(path: Path, well: str) -> Path:
    path.write_text(
        f"""~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1004.0
STEP.M 1.0
NULL. -999.25
WELL. {well}
~CURVE INFORMATION
DEPT.M
GR.GAPI
RT.OHMM
~ASCII
1000 10 1
1001 20 2
1002 30 3
1003 40 4
1004 50 5
""",
        encoding="utf-8",
    )
    return path


def test_save_load_tops_for_well(tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws")
    from well_log_workstation.workspace import add_well

    well = add_well(ws, name="W1", path="wells/w1/data.las", well_id="well-1")
    (ws.root / "wells" / "w1").mkdir(parents=True)
    tops = [
        FormationTop(name="T1", depth=1001.0, unit="m", color="#c0392b", id="a"),
        FormationTop(name="T2", depth=1003.0, unit="m", color="#2980b9", id="b"),
    ]
    path = save_tops_for_well(ws, well.id, tops)
    assert path.is_file()
    assert path.name == "tops.json"

    loaded, diags = load_tops_for_well(ws, well.id)
    assert diags == []
    assert len(loaded) == 2
    assert loaded[0].name == "T1"
    assert loaded[0].depth == pytest.approx(1001.0)
    assert loaded[1].name == "T2"


def test_missing_tops_file_is_empty_no_error(tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws")
    from well_log_workstation.workspace import add_well

    well = add_well(ws, name="W1", path="wells/w1/x.las", well_id="w1")
    tops, diags = load_tops_for_well(ws, well.id)
    assert tops == []
    assert diags == []


def test_corrupt_tops_degrades_with_diagnostics(tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws")
    from well_log_workstation.workspace import add_well

    well = add_well(ws, name="W1", path="wells/w1/x.las", well_id="w1")
    path = tops_file_path(ws, well.id)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("{not json", encoding="utf-8")
    tops, diags = load_tops_for_well(ws, well.id)
    assert tops == []
    assert diags
    assert "无法读取" in diags[0] or "json" in diags[0].lower() or "层位" in diags[0]


def test_import_external_json(tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws")
    from well_log_workstation.workspace import add_well

    well = add_well(ws, name="W1", path="wells/w1/x.las", well_id="w1")
    src = tmp_path / "external.json"
    src.write_text(
        json.dumps(
            {
                "tops": [
                    {"name": "TopA", "depth": 1001.5},
                    {"name": "TopB", "md": 1002.5, "color": "#00ff00"},
                ]
            }
        ),
        encoding="utf-8",
    )
    tops, diags = import_tops_from_json_file(ws, well.id, src)
    assert len(tops) == 2
    assert tops[0].name == "TopA"
    assert tops_file_path(ws, well.id).is_file()


def test_import_empty_raises(tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws")
    from well_log_workstation.workspace import add_well

    well = add_well(ws, name="W1", path="wells/w1/x.las", well_id="w1")
    src = tmp_path / "empty.json"
    src.write_text('{"tops": []}', encoding="utf-8")
    with pytest.raises(TopsError):
        import_tops_from_json_file(ws, well.id, src)


def test_make_stub_tops_span() -> None:
    tops = make_stub_tops(1000.0, 1100.0, names=("A", "B", "C"))
    assert len(tops) == 3
    assert tops[0].depth == pytest.approx(1025.0)
    assert tops[-1].depth == pytest.approx(1075.0)


def test_shell_stub_tops_on_single_well_canvas(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ui", name="Tops")
    las = _write_las(tmp_path / "a.las", "TOP-A")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(las)
    win.apply_template_to_well(well_id, "std-gr-rt-den")
    assert win.multi_track_canvas.tops() == []

    tops = win.generate_stub_tops_for_well(well_id)
    assert len(tops) >= 2
    assert len(win.multi_track_canvas.tops()) == len(tops)
    # Right pane lists tops
    labels = [
        win.tops_list.item(i).text() for i in range(win.tops_list.count())
    ]
    assert any("T1" in t or tops[0].name in t for t in labels)
    assert tops_file_path(ws, well_id).is_file()


def test_shell_tops_on_correlation(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "corr", name="CorrTops")
    las1 = _write_las(tmp_path / "a.las", "A")
    las2 = _write_las(tmp_path / "b.las", "B")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    id1 = win.import_las_path(las1)
    id2 = win.import_las_path(las2)
    win.generate_stub_tops_for_well(id1)
    win.generate_stub_tops_for_well(id2)

    plot = win.create_correlation_plot_document([id1, id2], "std-gr-rt-den")
    assert plot.type == "correlation"
    per = win.correlation_canvas.tops_per_column()
    assert len(per) == 2
    assert len(per[0]) >= 2
    assert len(per[1]) >= 2


def test_add_top_at_depth_persists_and_lists(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "pick")
    las = _write_las(tmp_path / "pick.las", "PICK")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(las)
    win.apply_template_to_well(well_id, "std-gr-rt-den")

    top = win.add_top_at_depth(well_id, "PickedTop", 1002.0)
    assert top.name == "PickedTop"
    assert top.depth == pytest.approx(1002.0)
    assert tops_file_path(ws, well_id).is_file()

    labels = [
        win.tops_list.item(i).text() for i in range(win.tops_list.count())
    ]
    assert any("PickedTop" in t for t in labels)
    canvas_names = [t.name for t in win.multi_track_canvas.tops()]
    assert "PickedTop" in canvas_names

    # Reload from disk
    loaded, diags = load_tops_for_well(ws, well_id)
    assert diags == []
    assert any(t.name == "PickedTop" for t in loaded)


def test_add_top_rejects_empty_name(qtbot, tmp_path: Path) -> None:
    from well_log_workstation.tops_model import TopsError

    ws = create_workspace(tmp_path / "empty")
    las = _write_las(tmp_path / "e.las", "E")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(las)
    win.apply_template_to_well(well_id, "std-gr-rt-den")
    with pytest.raises(TopsError):
        win.add_top_at_depth(well_id, "   ", 1001.0)


def test_depth_at_y_and_pick_mode(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "y")
    las = _write_las(tmp_path / "y.las", "Y")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(las)
    win.apply_template_to_well(well_id, "std-gr-rt-den")
    canvas = win.multi_track_canvas
    canvas.resize(600, 500)
    canvas.show()
    qtbot.waitExposed(canvas)
    canvas.set_depth_range(1000.0, 1004.0)
    d0, d1 = canvas.depth_range()  # type: ignore[misc]
    top_band, bottom_band = 36, canvas.height() - 16  # matches _plot_band header
    mid_y = 0.5 * (top_band + bottom_band)
    depth = canvas.depth_at_y(mid_y)
    assert depth is not None
    assert d0 <= depth <= d1
    assert depth == pytest.approx(0.5 * (d0 + d1), abs=0.5)

    canvas.set_pick_mode(True)
    assert canvas.pick_mode() is True
    canvas.set_pick_mode(False)
    assert canvas.pick_mode() is False


def test_inspector_lists_tops_on_well_select(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "sel")
    las = _write_las(tmp_path / "s.las", "SEL")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(las)
    save_tops_for_well(
        ws,
        well_id,
        [FormationTop(name="HorizonX", depth=1002.0, id="hx")],
    )
    win._selected_well_id = well_id
    win.load_tops_for_selected_well()
    texts = [
        win.tops_list.item(i).text() for i in range(win.tops_list.count())
    ]
    assert any("HorizonX" in t for t in texts)
