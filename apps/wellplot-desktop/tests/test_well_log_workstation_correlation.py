"""Correlation-lite multi-well plot document (#222)."""

from __future__ import annotations

import os
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.plot_document import (  # noqa: E402
    create_correlation_plot,
    load_plot_document,
)
from well_log_workstation.shell import WellLogWorkstationWindow  # noqa: E402
from well_log_workstation.workspace import (  # noqa: E402
    WorkspaceError,
    add_well,
    create_workspace,
    open_workspace,
)


def _write_las(path: Path, well: str) -> Path:
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
RT.OHMM
~ASCII
1000 10 1
1001 20 2
1002 30 3
1003 40 4
""",
        encoding="utf-8",
    )
    return path


def test_create_correlation_requires_two_wells(tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws")
    w1 = add_well(ws, name="A", path="wells/a.las", well_id="w1")
    with pytest.raises(WorkspaceError, match="2"):
        create_correlation_plot(
            ws, well_ids=[w1.id], template_id="std-gr-rt-den"
        )


def test_create_persist_reopen_correlation_metadata(tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws")
    w1 = add_well(ws, name="A", path="wells/a.las", well_id="w1")
    w2 = add_well(ws, name="B", path="wells/b.las", well_id="w2")
    plot = create_correlation_plot(
        ws,
        well_ids=[w1.id, w2.id],
        template_id="std-gr-rt-den",
        name="A–B 对比",
    )
    assert plot.type == "correlation"
    assert plot.well_ids == [w1.id, w2.id]
    assert (ws.root / plot.path).is_file()
    assert any(p.id == plot.id and p.type == "correlation" for p in ws.plots)

    again = open_workspace(ws.root)
    loaded = load_plot_document(again, plot.id)
    assert loaded.type == "correlation"
    assert loaded.well_ids == [w1.id, w2.id]
    assert loaded.template_id == "std-gr-rt-den"


def test_shell_two_wells_correlation_shared_depth(qtbot, tmp_path: Path) -> None:
    ws_root = tmp_path / "ui-ws"
    ws = create_workspace(ws_root, name="Corr")
    las1 = _write_las(tmp_path / "a.las", "WELL-A")
    las2 = _write_las(tmp_path / "b.las", "WELL-B")

    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    id1 = win.import_las_path(las1)
    id2 = win.import_las_path(las2)

    plot = win.create_correlation_plot_document(
        [id1, id2], "std-gr-rt-den", name="双井对比"
    )
    assert plot.type == "correlation"
    assert win.active_plot_id == plot.id
    assert win.active_plot_type == "correlation"
    assert win.correlation_canvas.column_count() == 2
    depth = win.correlation_canvas.depth_range()
    assert depth is not None
    d0, d1 = depth
    assert d1 > d0

    # Shared depth pan/zoom stays consistent (set_depth_range)
    win.correlation_canvas.set_depth_range(1000.5, 1002.5)
    again = win.correlation_canvas.depth_range()
    assert again == pytest.approx((1000.5, 1002.5))
    assert win.correlation_canvas.column_count() == 2

    # Reopen in fresh window
    win2 = WellLogWorkstationWindow()
    qtbot.addWidget(win2)
    win2.set_workspace(open_workspace(ws_root))
    opened = win2.open_plot_document(plot.id)
    assert opened.type == "correlation"
    assert win2.correlation_canvas.column_count() == 2
    assert id1 in win2.session.document_ids()
    assert id2 in win2.session.document_ids()
