"""Track drag-reorder: order snapshot/apply, plot schema v8, canvas drag, reopen.

Covers the FRS §2.x「图道增删/隐藏/顺序拖拽/宽度」canvas-drag gap:
* template_model order snapshot + apply (pure functions);
* plot document schema v8 migration + round-trip;
* MultiTrackCanvas header drag via QTest mouse simulation reorders and emits;
* shell persists the order and a fresh window restores it.
"""

from __future__ import annotations

import os
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtCore import QPoint, Qt
from PySide6.QtTest import QTest

from well_log_workstation.las_import import import_las_into_workspace
from well_log_workstation.multi_track_canvas import (
    MultiTrackCanvas,
    track_header_rects,
)
from well_log_workstation.plot_document import (
    PlotDocument,
    load_plot_document,
    save_plot_document,
)
from well_log_workstation.shell import WellLogWorkstationWindow
from well_log_workstation.template_model import (
    HostPresentation,
    apply_template,
    apply_track_order,
    get_builtin_template,
    track_order_from_presentation,
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
WELL. ORD-1
~CURVE INFORMATION
DEPT.M
GR.GAPI
RT.OHMM
RHOB.G/C3
~ASCII
1000 20 2 2.2
1001 30 5 2.3
1002 40 10 2.4
1003 50 20 2.5
1004 60 50 2.6
1005 70 100 2.7
""",
        encoding="utf-8",
    )
    return path


def _presentation(tmp_path: Path) -> HostPresentation:
    ws = create_workspace(tmp_path / "ws")
    result = import_las_into_workspace(ws, _write_las(tmp_path / "a.las"))
    template = get_builtin_template("std-gr-rt-den")
    assert template is not None
    return apply_template(template, result.document)


# ---------------------------------------------------------------------------
# Order snapshot / apply (pure)
# ---------------------------------------------------------------------------


def test_track_order_snapshot_is_id_list(tmp_path: Path) -> None:
    pres = _presentation(tmp_path)
    order = track_order_from_presentation(pres)
    assert order == [t.id for t in pres.tracks]
    assert len(order) >= 3  # depth + gr + rt + den


def test_apply_track_order_reorders_and_trails_unknown(tmp_path: Path) -> None:
    pres = _presentation(tmp_path)
    ids = [t.id for t in pres.tracks]
    # Reversed order of the first three; unknown ids trail in relative order.
    new_order = list(reversed(ids[:3])) + ["does-not-exist"]
    apply_track_order(pres, new_order)
    assert [t.id for t in pres.tracks] == list(reversed(ids[:3])) + ids[3:]


def test_apply_track_order_noop_for_none_or_empty(tmp_path: Path) -> None:
    pres = _presentation(tmp_path)
    before = [t.id for t in pres.tracks]
    apply_track_order(pres, None)
    apply_track_order(pres, [])
    assert [t.id for t in pres.tracks] == before


# ---------------------------------------------------------------------------
# Plot document schema v8
# ---------------------------------------------------------------------------


def test_plot_document_v8_roundtrip(tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws-doc")
    result = import_las_into_workspace(ws, _write_las(tmp_path / "d.las"))
    pres = apply_template(get_builtin_template("std-gr-rt-den"), result.document)
    plot = PlotDocument(
        id="pd-order",
        name="Order",
        type="single_well",
        well_ids=[result.catalog_well_id],
        template_id="std-gr-rt-den",
        path="plots/pd-order.json",
        track_order=track_order_from_presentation(pres),
    )
    save_plot_document(ws, plot)
    loaded = load_plot_document(ws, "pd-order")
    assert loaded.track_order == track_order_from_presentation(pres)


def test_plot_document_v7_loads_with_empty_order(tmp_path: Path) -> None:
    """A v7 file (no track_order) migrates additively to v8 with []."""
    ws = create_workspace(tmp_path / "ws-v7")
    path = ws.plots_dir / "pd-v7.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        '{"schemaVersion": 7, "id": "pd-v7", "name": "V7", "type": "single_well",'
        ' "well_ids": ["w1"], "template_id": "std-gr-rt-den",'
        ' "path": "plots/pd-v7.json"}',
        encoding="utf-8",
    )
    loaded = load_plot_document(ws, "pd-v7")
    assert loaded.track_order == []


# ---------------------------------------------------------------------------
# Canvas header drag (QTest mouse simulation)
# ---------------------------------------------------------------------------


def test_canvas_header_drag_reorders_tracks(qtbot, tmp_path: Path) -> None:
    pres = _presentation(tmp_path)
    canvas = MultiTrackCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(900, 600)
    canvas.set_presentation(pres)
    canvas.grab()  # force a paint so the band geometry is settled

    emitted: list[list[str]] = []
    canvas.track_order_changed.connect(lambda order: emitted.append(order))

    before = [t.id for t in pres.tracks]
    entries, _band = track_header_rects(pres, 900, 600)
    assert len(entries) >= 3
    src_header = entries[0][1]
    last_header = entries[-1][1]
    y = src_header.center().y()
    # Drag past the last header centre → insertion at the end.
    target_x = last_header.x() + last_header.width() + 10

    QTest.mousePress(
        canvas, Qt.MouseButton.LeftButton, pos=src_header.center()
    )
    QTest.mouseMove(canvas, QPoint(int(target_x), int(y)))
    QTest.mouseRelease(
        canvas, Qt.MouseButton.LeftButton, pos=QPoint(int(target_x), int(y))
    )

    after = [t.id for t in pres.tracks]
    assert after[0] == before[1]  # second track moved into first place
    assert after[-1] == before[0]  # dragged track now last
    assert set(after) == set(before)
    assert emitted == [after]
    assert canvas._drag_track_index is None  # drag state cleared


def test_canvas_drag_to_same_spot_does_not_emit(qtbot, tmp_path: Path) -> None:
    pres = _presentation(tmp_path)
    canvas = MultiTrackCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(900, 600)
    canvas.set_presentation(pres)

    emitted: list[list[str]] = []
    canvas.track_order_changed.connect(lambda order: emitted.append(order))

    entries, _band = track_header_rects(pres, 900, 600)
    header = entries[1][1]
    # Press + release in the same header without crossing a centre boundary.
    QTest.mousePress(canvas, Qt.MouseButton.LeftButton, pos=header.center())
    QTest.mouseRelease(canvas, Qt.MouseButton.LeftButton, pos=header.center())
    assert emitted == []


# ---------------------------------------------------------------------------
# Shell persistence + reopen restore
# ---------------------------------------------------------------------------


def test_shell_track_order_persists_across_reopen(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws-sh", name="Order")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(_write_las(tmp_path / "s.las"))
    win.create_single_well_plot_document(well_id, "std-gr-rt-den")
    pres = win.active_presentation
    assert pres is not None
    original = [t.id for t in pres.tracks]

    # What a canvas drag leaves behind: first track moved to the end.
    tracks = pres.tracks
    tracks.append(tracks.pop(0))
    expected = [t.id for t in tracks]
    win._persist_track_overrides()

    # Reopen the plot in a fresh window — order restored.
    win2 = WellLogWorkstationWindow()
    qtbot.addWidget(win2)
    win2.set_workspace(ws)
    win2.open_plot_document(win.active_plot_id)
    restored = win2.active_presentation
    assert restored is not None
    assert [t.id for t in restored.tracks] == expected
