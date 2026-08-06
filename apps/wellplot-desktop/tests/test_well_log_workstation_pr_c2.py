"""PlaneMapView + three_d_bridge + shell dispatch tests (Phase-2 PR-C2)."""

from __future__ import annotations

import os
from pathlib import Path

import pytest

from PySide6.QtCore import Qt

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.plane_map_view import PlaneMapView  # noqa: E402
from well_log_workstation.workspace import (  # noqa: E402
    CoordinateReference,
    WellCatalogEntry,
    create_workspace,
)


def _entry(name: str, lng=None, lat=None, crs="EPSG:4326") -> WellCatalogEntry:
    return WellCatalogEntry(id=name, name=name, path="", lng=lng, lat=lat, crs=crs)


def test_filter_wells_with_coords_keeps_only_complete():
    wells = [
        _entry("A", lng=116.0, lat=30.0),
        _entry("B"),  # no coords -> excluded
        _entry("C", lng=117.0, lat=31.0),
        _entry("D", lng="bad", lat=31.0),  # invalid -> excluded
    ]
    out = PlaneMapView.filter_wells_with_coords(wells)
    assert [w["name"] for w in out] == ["A", "C"]
    assert out[0] == {"name": "A", "lng": 116.0, "lat": 30.0}


def test_filter_wells_with_coords_empty():
    assert PlaneMapView.filter_wells_with_coords([]) == []


def test_plane_map_view_constructs_without_mapping():
    """View degrades to placeholder when the geoviz mapping surface is gone."""
    # Force the capability probe to fail closed.
    os.environ["WLWS_DISABLE_MAPPING"] = "1"
    try:
        from well_log_workstation.engine_bridge import reset_mapping_capability_cache
        reset_mapping_capability_cache()
        view = PlaneMapView()
        assert view.mapping_available() is False
        # set_plot_data is a safe no-op in degraded mode.
        view.set_plot_data([])
    finally:
        os.environ.pop("WLWS_DISABLE_MAPPING", None)
        from well_log_workstation.engine_bridge import reset_mapping_capability_cache
        reset_mapping_capability_cache()


def test_three_d_bridge_probe_returns_capability():
    from well_log_workstation.three_d_bridge import (
        reset_3d_capability_cache,
        probe_3d,
    )
    reset_3d_capability_cache()
    cap = probe_3d()
    # On this (broken-env) machine pyqtgraph may or may not be importable;
    # the contract is: returns a ThreeDCapability with a bool + detail.
    assert isinstance(cap.available, bool)
    assert isinstance(cap.detail, str)


def test_three_d_bridge_disable_env():
    from well_log_workstation.three_d_bridge import (
        reset_3d_capability_cache,
        probe_3d,
    )
    os.environ["WLWS_DISABLE_3D"] = "1"
    try:
        reset_3d_capability_cache()
        cap = probe_3d()
        assert cap.available is False
        assert "WLWS_DISABLE_3D" in cap.detail
    finally:
        os.environ.pop("WLWS_DISABLE_3D", None)
        reset_3d_capability_cache()


def test_shell_tree_labels_six_plot_types(tmp_path: Path):
    """_refresh_tree labels all six plot types (T9 2->6)."""
    from well_log_workstation.shell import WellLogWorkstationWindow
    from well_log_workstation.plot_document import (
        create_composite_plot,
        create_fence_3d_plot,
        create_plane_map_plot,
        create_section_plot,
        PanelRef,
    )

    ws = create_workspace(tmp_path / "tree-ws", name="Tree")
    from well_log_workstation.workspace import add_well
    add_well(ws, name="A", path="wells/a.las", well_id="w-a", lng=116.0, lat=30.0)
    add_well(ws, name="B", path="wells/b.las", well_id="w-b", lng=117.0, lat=31.0)
    create_section_plot(ws, well_ids=["w-a", "w-b"], template_id="t")
    create_plane_map_plot(ws, wells=["w-a"], template_id="t")
    create_fence_3d_plot(ws, well_ids=["w-a", "w-b"], template_id="t")
    create_composite_plot(ws, panels=[PanelRef(plot_id="x")], template_id="t")

    win = WellLogWorkstationWindow()
    win.set_workspace(ws)

    labels: list[str] = []
    tree = win.workspace_tree
    for i in range(tree.topLevelItemCount()):
        item = tree.topLevelItem(i)
        meta = item.data(0, Qt.ItemDataRole.UserRole) or {}
        if meta.get("kind") == "plots_folder":
            for k in range(item.childCount()):
                labels.append(item.child(k).text(0))
    joined = " | ".join(labels)
    assert "[剖面]" in joined
    assert "[平面图]" in joined
    assert "[栅状图]" in joined
    assert "[综合图]" in joined
