"""Workspace catalog model + shell tree wiring (#217)."""

from __future__ import annotations

import json
import os
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.shell import WellLogWorkstationWindow
from well_log_workstation.workspace import (
    WORKSPACE_FILENAME,
    WorkspaceError,
    add_plot,
    add_well,
    create_workspace,
    open_workspace,
    save_workspace,
)


def test_create_workspace_skeleton(tmp_path: Path) -> None:
    root = tmp_path / "field-a"
    ws = create_workspace(root, name="Field A")
    assert ws.root == root.resolve()
    assert ws.name == "Field A"
    assert (root / WORKSPACE_FILENAME).is_file()
    assert (root / "wells").is_dir()
    assert (root / "plots").is_dir()
    assert (root / "templates").is_dir()
    data = json.loads((root / WORKSPACE_FILENAME).read_text(encoding="utf-8"))
    assert data["schemaVersion"] == 2
    assert data["name"] == "Field A"
    assert data["wells"] == []
    assert data["plots"] == []
    # Phase-2 T2 (#246): workspace serializes its CRS trio.
    assert data["coordinate"]["project_crs"] == "EPSG:4326"
    assert data["coordinate"]["display_crs"] == "EPSG:4326"
    # Must not look like an engine Manifest whole-project
    assert "schemaVersion" in data
    assert "document" not in data
    assert "requiredSdkVersion" not in data


def test_open_round_trip_catalog_entries(tmp_path: Path) -> None:
    root = tmp_path / "field-b"
    ws = create_workspace(root)
    well = add_well(ws, name="Well-A", path="wells/Well-A.las")
    plot = add_plot(
        ws,
        name="Well-A 单井分析图",
        plot_type="single_well",
        well_ids=[well.id],
        template_id="std-gr-rt-den",
        path="plots/well-a-single.json",
    )
    add_plot(
        ws,
        name="A–C 对比",
        plot_type="correlation",
        well_ids=[well.id],
    )

    again = open_workspace(root)
    assert again.name == root.name
    assert len(again.wells) == 1
    assert again.wells[0].id == well.id
    assert again.wells[0].name == "Well-A"
    assert again.wells[0].path == "wells/Well-A.las"
    assert len(again.plots) == 2
    names = {p.name for p in again.plots}
    assert "Well-A 单井分析图" in names
    assert "A–C 对比" in names
    single = next(p for p in again.plots if p.id == plot.id)
    assert single.type == "single_well"
    assert single.well_ids == [well.id]
    assert single.template_id == "std-gr-rt-den"


def test_open_workspace_v1_upgrades_to_v2(tmp_path: Path) -> None:
    """A v1 workspace.json opens as v2 with additive coordinate defaults.

    Phase-2 T9 (#253): wells gain lng/lat/crs, the workspace gains a
    ``coordinate`` CRS trio, and unknown plot types fall back to
    ``single_well``.
    """
    root = tmp_path / "legacy"
    (root / "wells").mkdir(parents=True)
    (root / "plots").mkdir()
    (root / "templates").mkdir()
    v1 = {
        "schemaVersion": 1,
        "name": "Legacy Field",
        "defaultTemplateId": None,
        "wells": [{"id": "w1", "name": "Well-1", "path": "wells/w1.las"}],
        "plots": [
            {"id": "p1", "name": "Legacy Plot", "type": "weird_type", "well_ids": ["w1"]},
            {"id": "p2", "name": "A-C", "type": "correlation", "well_ids": ["w1"]},
        ],
    }
    (root / WORKSPACE_FILENAME).write_text(
        json.dumps(v1, ensure_ascii=False), encoding="utf-8"
    )

    ws = open_workspace(root)
    assert ws.schema_version == 2
    # Wells gained coordinate defaults.
    assert ws.wells[0].lng is None
    assert ws.wells[0].lat is None
    assert ws.wells[0].crs == "EPSG:4326"
    # Workspace gained the CRS trio.
    assert ws.coordinate.project_crs == "EPSG:4326"
    assert ws.coordinate.display_crs == "EPSG:4326"
    assert ws.coordinate.target_crs is None
    # Unknown plot type fell back to single_well; correlation preserved.
    by_id = {p.id: p for p in ws.plots}
    assert by_id["p1"].type == "single_well"
    assert by_id["p2"].type == "correlation"
    # Re-saving persists v2.
    save_workspace(ws)
    data = json.loads((root / WORKSPACE_FILENAME).read_text(encoding="utf-8"))
    assert data["schemaVersion"] == 2
    assert data["coordinate"]["project_crs"] == "EPSG:4326"
    assert data["wells"][0]["crs"] == "EPSG:4326"


def test_add_well_with_coordinates_round_trips(tmp_path: Path) -> None:
    """Phase-2 T2 (#246): wellhead coords persist through save/open."""
    root = tmp_path / "coords"
    ws = create_workspace(root)
    well = add_well(
        ws,
        name="Well-CRS",
        path="wells/crs.las",
        lng=116.5,
        lat=30.25,
        crs="EPSG:4326",
    )
    again = open_workspace(root)
    assert again.wells[0].id == well.id
    assert again.wells[0].lng == pytest.approx(116.5)
    assert again.wells[0].lat == pytest.approx(30.25)
    assert again.wells[0].crs == "EPSG:4326"


def test_create_rejects_nonempty_without_clobber(tmp_path: Path) -> None:
    root = tmp_path / "busy"
    root.mkdir()
    (root / "noise.txt").write_text("x", encoding="utf-8")
    with pytest.raises(WorkspaceError, match="not empty"):
        create_workspace(root)


def test_open_missing_catalog(tmp_path: Path) -> None:
    with pytest.raises(WorkspaceError, match="not a workspace"):
        open_workspace(tmp_path / "nope")


def test_shell_tree_shows_catalog(qtbot, tmp_path: Path) -> None:
    root = tmp_path / "ui-field"
    ws = create_workspace(root, name="UI Field")
    add_well(ws, name="Well-X", path="wells/x.las")
    add_plot(ws, name="X 单井", plot_type="single_well", well_ids=[ws.wells[0].id])

    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)

    assert win.workspace is not None
    assert win.workspace.name == "UI Field"
    # Flatten tree labels
    labels: list[str] = []

    def walk(item) -> None:
        labels.append(item.text(0))
        for i in range(item.childCount()):
            walk(item.child(i))

    for i in range(win.workspace_tree.topLevelItemCount()):
        walk(win.workspace_tree.topLevelItem(i))

    # IA: top level is 数据/图件 only — no workspace root node
    tops = [
        win.workspace_tree.topLevelItem(i).text(0)
        for i in range(win.workspace_tree.topLevelItemCount())
    ]
    assert tops == ["数据", "图件"]
    assert all("UI Field" not in x for x in labels)
    assert any("Well-X" in x for x in labels)
    assert any("X 单井" in x for x in labels)
    assert "WellPlot Desktop" in win.windowTitle() or "UI Field" in win.windowTitle()
