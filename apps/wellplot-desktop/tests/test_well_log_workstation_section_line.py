"""Section-line well picking (FRS §4.2 / P2-B, workflow 1).

Covers:
* point_to_segment_distance (perpendicular, endpoint, outside);
* project_onto_segment (midpoint, clamped);
* pick_wells_along_line (buffer in/out, ordering, no-coord skip, cap);
* dialog endpoints parsing + live preview count;
* shell integration: correlation plot created with line-ordered wells.
"""

from __future__ import annotations

import os

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import pytest

from well_log_workstation.section_line import (
    pick_wells_along_line,
    point_to_segment_distance,
    project_onto_segment,
)
from well_log_workstation.workspace import WellCatalogEntry


def _well(well_id: str, name: str, lng=None, lat=None) -> WellCatalogEntry:
    return WellCatalogEntry(id=well_id, name=name, path=f"{well_id}.las",
                            lng=lng, lat=lat)


# ---------------------------------------------------------------------------
# Distance
# ---------------------------------------------------------------------------


def test_distance_perpendicular_to_segment() -> None:
    # Segment (0,0)-(10,0), point (5,3): perpendicular distance 3.
    assert point_to_segment_distance(5, 3, (0, 0), (10, 0)) == pytest.approx(3.0)


def test_distance_to_endpoint_when_outside() -> None:
    # Point beyond endpoint A: distance to A.
    assert point_to_segment_distance(-2, 1, (0, 0), (10, 0)) == pytest.approx(
        (2**2 + 1**2) ** 0.5
    )


def test_distance_zero_length_segment() -> None:
    assert point_to_segment_distance(3, 4, (1, 1), (1, 1)) == pytest.approx(
        (2**2 + 3**2) ** 0.5
    )


# ---------------------------------------------------------------------------
# Projection
# ---------------------------------------------------------------------------


def test_projection_midpoint() -> None:
    assert project_onto_segment(5, 3, (0, 0), (10, 0)) == pytest.approx(0.5)


def test_projection_clamped() -> None:
    assert project_onto_segment(20, 0, (0, 0), (10, 0)) == pytest.approx(1.0)
    assert project_onto_segment(-5, 0, (0, 0), (10, 0)) == pytest.approx(0.0)


# ---------------------------------------------------------------------------
# pick_wells_along_line
# ---------------------------------------------------------------------------


def test_pick_filters_by_buffer() -> None:
    wells = [
        _well("w1", "W1", lng=1.0, lat=0.0),
        _well("w2", "W2", lng=4.0, lat=0.0),
        _well("w3", "W3", lng=2.0, lat=1.0),  # 1° off the line
        _well("w4", "W4", lng=9.0, lat=5.0),  # far away
    ]
    picked = pick_wells_along_line(wells, (0, 0), (10, 0), buffer_deg=0.5)
    assert [w.name for w in picked] == ["W1", "W2"]
    picked2 = pick_wells_along_line(wells, (0, 0), (10, 0), buffer_deg=1.2)
    assert [w.name for w in picked2] == ["W1", "W3", "W2"]


def test_pick_orders_along_line() -> None:
    # Wells scattered along the line, out of order in the catalog.
    wells = [
        _well("w3", "W3", lng=8.0, lat=0.0),
        _well("w1", "W1", lng=1.0, lat=0.0),
        _well("w2", "W2", lng=5.0, lat=0.0),
    ]
    picked = pick_wells_along_line(wells, (0, 0), (10, 0), buffer_deg=0.5)
    assert [w.name for w in picked] == ["W1", "W2", "W3"]


def test_pick_skips_wells_without_coords() -> None:
    wells = [
        _well("w1", "W1", lng=1.0, lat=0.0),
        _well("n1", "NOCOORD"),  # no lng/lat
        _well("n2", "NOLAT", lng=2.0),
    ]
    picked = pick_wells_along_line(wells, (0, 0), (10, 0), buffer_deg=0.5)
    assert [w.name for w in picked] == ["W1"]


def test_pick_max_wells_cap() -> None:
    wells = [
        _well("w1", "W1", lng=1.0, lat=0.0),
        _well("w2", "W2", lng=2.0, lat=0.0),
        _well("w3", "W3", lng=3.0, lat=0.0),
    ]
    picked = pick_wells_along_line(wells, (0, 0), (10, 0), buffer_deg=0.5,
                                   max_wells=2)
    assert [w.name for w in picked] == ["W1", "W2"]


def test_pick_empty_and_zero_buffer() -> None:
    wells = [_well("w1", "W1", lng=1.0, lat=0.0)]
    # A single well inside the buffer is picked (the ≥2 rule lives in the UI).
    assert [w.name for w in pick_wells_along_line(
        wells, (0, 0), (10, 0), buffer_deg=0.5)] == ["W1"]
    picked = pick_wells_along_line(wells, (0, 0), (10, 0), buffer_deg=0.0)
    assert picked == []
    assert pick_wells_along_line([], (0, 0), (10, 0), buffer_deg=0.5) == []


# ---------------------------------------------------------------------------
# Dialog endpoint parsing
# ---------------------------------------------------------------------------


def test_dialog_parse_point_and_preview(qtbot) -> None:
    from well_log_workstation.section_line_dialog import SectionLineDialog

    wells = [
        _well("w1", "W1", lng=1.0, lat=0.0),
        _well("w2", "W2", lng=4.0, lat=0.0),
    ]
    dlg = SectionLineDialog(wells, parent=None)
    qtbot.addWidget(dlg)
    # Invalid endpoints → no value.
    assert dlg.value() is None
    # Valid endpoints → (a, b, buffer_deg).
    dlg._end_a.setText("0, 0")
    dlg._end_b.setText("10, 0")
    value = dlg.value()
    assert value is not None
    assert value[0] == (0.0, 0.0) and value[1] == (10.0, 0.0)
    assert value[2] > 0.0
    # Live preview count shows both wells.
    assert "2 口井" in dlg._preview.text()


def test_dialog_fill_from_well(qtbot) -> None:
    from well_log_workstation.section_line_dialog import SectionLineDialog

    wells = [_well("w1", "W1", lng=1.5, lat=2.5)]
    dlg = SectionLineDialog(wells, parent=None)
    qtbot.addWidget(dlg)
    idx = dlg._well_combo.findData("w1")
    assert idx >= 0
    dlg._well_combo.setCurrentIndex(idx)
    dlg._fill_from_well(dlg._end_a)
    lng, lat = dlg._parse_point(dlg._end_a.text())
    assert lng == pytest.approx(1.5)
    assert lat == pytest.approx(2.5)


# ---------------------------------------------------------------------------
# Shell integration
# ---------------------------------------------------------------------------


def test_section_from_line_creates_ordered_correlation(qtbot, tmp_path) -> None:
    from well_log_workstation.shell import WellLogWorkstationWindow
    from well_log_workstation.workspace import create_workspace

    ws = create_workspace(tmp_path / "ws", name="Line")
    # Three wells along an east-west line with real LAS data, catalog order
    # scrambled.
    las_template = """~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1002.0
STEP.M 1.0
NULL. -999.25
WELL. {name}
~CURVE INFORMATION
DEPT.M
GR.GAPI
~ASCII
1000 10
1001 20
1002 30
"""
    for well_id, name, lng in (("w3", "W3", 3.0), ("w1", "W1", 1.0), ("w2", "W2", 2.0)):
        rel = f"wells/{well_id}.las"
        p = ws.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(las_template.format(name=name), encoding="utf-8")
        ws.wells.append(
            WellCatalogEntry(
                id=well_id, name=name, path=rel, lng=lng, lat=30.0
            )
        )

    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    assert win._act_section_from_line.isEnabled()

    # Exercise the pick + create path directly (dialog is UI-interactive).
    from well_log_workstation.section_line import pick_wells_along_line

    picked = pick_wells_along_line(
        ws.wells, (0.0, 30.0), (4.0, 30.0), buffer_deg=0.5
    )
    assert [w.name for w in picked] == ["W1", "W2", "W3"]
    plot = win.create_correlation_plot_document(
        [w.id for w in picked], "std-gr-rt-den",
        name="沿线剖面 3井",
    )
    assert [plot.well_ids[0], plot.well_ids[-1]] == [picked[0].id, picked[-1].id]
    assert len(plot.well_ids) == 3
