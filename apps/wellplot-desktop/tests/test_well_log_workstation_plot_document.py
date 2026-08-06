"""Single-well plot document persistence (#220)."""

from __future__ import annotations

import os
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.plot_document import (
    PanelRef,
    create_composite_plot,
    create_fence_3d_plot,
    create_plane_map_plot,
    create_section_plot,
    create_single_well_plot,
    load_plot_document,
)
from well_log_workstation.shell import WellLogWorkstationWindow
from well_log_workstation.workspace import (
    WorkspaceError,
    create_workspace,
    open_workspace,
)


def _write_las(path: Path) -> Path:
    path.write_text(
        """~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1003.0
STEP.M 1.0
NULL. -999.25
WELL. PLOT-1
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


def test_create_persist_reopen_plot_metadata(tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws")
    # Catalog well without going through full import path for metadata test
    from well_log_workstation.workspace import add_well

    well = add_well(ws, name="W1", path="wells/w1.las", well_id="well-fixed")
    plot = create_single_well_plot(
        ws,
        well_id=well.id,
        well_name=well.name,
        template_id="std-gr-rt-den",
    )
    assert plot.path.startswith("plots/")
    assert (ws.root / plot.path).is_file()
    assert any(p.id == plot.id for p in ws.plots)

    again = open_workspace(ws.root)
    loaded = load_plot_document(again, plot.id)
    assert loaded.name == plot.name
    assert loaded.well_ids == [well.id]
    assert loaded.template_id == "std-gr-rt-den"
    assert loaded.type == "single_well"


def test_shell_create_and_reopen_plot_restores_tracks(qtbot, tmp_path: Path) -> None:
    ws_root = tmp_path / "ui-ws"
    ws = create_workspace(ws_root, name="Plots")
    las = _write_las(tmp_path / "p.las")

    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(las)
    plot = win.create_single_well_plot_document(well_id, "std-gr-rt-den")
    assert win.active_plot_id == plot.id
    assert win.multi_track_canvas.track_count() >= 2
    assert (ws.root / plot.path).is_file()

    # Simulate reopen: new window, open workspace, open plot
    win2 = WellLogWorkstationWindow()
    qtbot.addWidget(win2)
    win2.set_workspace(open_workspace(ws_root))
    # The tree refresh loads data units on set_workspace, so the session is
    # populated from disk — but the plot document itself must still restore
    # its tracks when opened.
    assert win2.session.get(well_id) is not None
    opened = win2.open_plot_document(plot.id)
    assert opened.id == plot.id
    assert win2.session.get(well_id) is not None
    assert win2.active_presentation is not None
    assert win2.multi_track_canvas.track_count() >= 2
    # Switching selection does not drop session docs
    assert well_id in win2.session.document_ids()


def _wells(tmp_path: Path):
    """Workspace with 2 wells for the Phase-2 T9 create_* helper tests."""
    ws = create_workspace(tmp_path / "ws-t9")
    from well_log_workstation.workspace import add_well

    a = add_well(ws, name="A", path="wells/a.las", well_id="w-a")
    b = add_well(ws, name="B", path="wells/b.las", well_id="w-b")
    return ws, a, b


def test_create_section_plot_requires_two_wells(tmp_path: Path) -> None:
    ws, a, b = _wells(tmp_path)
    with pytest.raises(WorkspaceError, match="至少需要 2 口井"):
        create_section_plot(ws, well_ids=["w-a"], template_id="t")
    plot = create_section_plot(ws, well_ids=["w-a", "w-b"], template_id="t")
    assert plot.type == "section"
    loaded = load_plot_document(ws, plot.id)
    assert loaded.type == "section"
    assert loaded.well_ids == ["w-a", "w-b"]


def test_create_plane_map_plot_requires_coordinate(tmp_path: Path) -> None:
    ws, a, b = _wells(tmp_path)
    plot = create_plane_map_plot(ws, wells=["w-a"], template_id="t")
    assert plot.type == "plane_map"
    # Workspace coordinate defaults to WGS84 so the map has CRS context.
    assert ws.coordinate.project_crs == "EPSG:4326"


def test_create_fence_3d_plot_requires_two_wells(tmp_path: Path) -> None:
    ws, a, b = _wells(tmp_path)
    with pytest.raises(WorkspaceError, match="至少需要 2 口井"):
        create_fence_3d_plot(ws, well_ids=["w-a"], template_id="t")
    plot = create_fence_3d_plot(ws, well_ids=["w-a", "w-b"], template_id="t")
    assert plot.type == "fence_3d"


def test_create_composite_plot_requires_panel(tmp_path: Path) -> None:
    ws, a, b = _wells(tmp_path)
    with pytest.raises(WorkspaceError, match="至少需要 1 个面板"):
        create_composite_plot(ws, panels=[], template_id="t")
    plot = create_composite_plot(
        ws,
        panels=[PanelRef(plot_id="p1", slot="main"), PanelRef(plot_id="p2", slot="side")],
        template_id="t",
    )
    assert plot.type == "composite"
    loaded = load_plot_document(ws, plot.id)
    assert loaded.type == "composite"
    assert [p.plot_id for p in loaded.panels] == ["p1", "p2"]


def test_plot_document_v1_upgrades_to_v2(tmp_path: Path) -> None:
    """A v1 plot JSON opens as v2 with panels defaulting to empty."""
    import json

    ws = create_workspace(tmp_path / "ws-v1")
    pid = "legacy-plot"
    rel = f"plots/{pid}.json"
    (ws.root / "plots").mkdir(exist_ok=True)
    (ws.root / rel).write_text(
        json.dumps(
            {
                "schemaVersion": 1,
                "id": pid,
                "name": "Legacy",
                "type": "single_well",
                "well_ids": ["w-a"],
                "template_id": "t",
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    from well_log_workstation.workspace import add_plot

    add_plot(ws, name="Legacy", plot_type="single_well", well_ids=["w-a"], path=rel, plot_id=pid)
    loaded = load_plot_document(ws, pid)
    assert loaded.type == "single_well"
    assert loaded.panels == []
