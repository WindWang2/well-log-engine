"""events bus + PanelRef T7 + export_dispatch tests (Phase-2 PR-C4)."""

from __future__ import annotations

import os
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.events import (  # noqa: E402
    bump_plot_revision,
    emit_plot_changed,
    plot_bus,
    plot_revision,
    reset_revisions,
)
from well_log_workstation.export_dispatch import (  # noqa: E402
    PageSpec,
    UnsupportedFormatError,
    export_plot,
)
from well_log_workstation.plot_document import PanelRef  # noqa: E402


# --- events bus (T7) ---

def test_plot_bus_emits_and_tracks_revision(qtbot):
    reset_revisions()
    received: list[tuple[str, int]] = []

    def _on(plot_id: str, rev: int):
        received.append((plot_id, rev))

    plot_bus.plot_changed.connect(_on)
    try:
        rev1 = emit_plot_changed("plot-a")
        rev2 = emit_plot_changed("plot-a")
        assert rev1 == 1 and rev2 == 2
        assert plot_revision("plot-a") == 2
        assert received == [("plot-a", 1), ("plot-a", 2)]
        # Different plot ids have independent counters.
        assert emit_plot_changed("plot-b") == 1
    finally:
        plot_bus.plot_changed.disconnect(_on)
        reset_revisions()


def test_bump_plot_revision_monotonic():
    reset_revisions()
    assert bump_plot_revision("x") == 1
    assert bump_plot_revision("x") == 2
    assert bump_plot_revision("y") == 1
    reset_revisions()


# --- PanelRef T7 extension ---

def test_panel_ref_new_fields_round_trip(tmp_path: Path):
    from well_log_workstation.plot_document import (
        create_composite_plot,
        load_plot_document,
    )
    from well_log_workstation.workspace import create_workspace

    ws = create_workspace(tmp_path / "ws")
    ref = PanelRef(
        plot_id="p1",
        slot="main",
        source_plot_type="fence_3d",
        rect_mm=[10.0, 20.0, 80.0, 60.0],
        render_mode="snapshot",
    )
    plot = create_composite_plot(ws, panels=[ref], template_id="t")
    loaded = load_plot_document(ws, plot.id)
    assert loaded.panels[0].source_plot_type == "fence_3d"
    assert loaded.panels[0].rect_mm == [10.0, 20.0, 80.0, 60.0]
    assert loaded.panels[0].render_mode == "snapshot"
    assert loaded.panels[0].slot == "main"


def test_panel_ref_legacy_compat(tmp_path: Path):
    """Panels persisted as {plot_id, slot} (pre-T7) load with defaults."""
    import json

    from well_log_workstation.plot_document import load_plot_document
    from well_log_workstation.workspace import add_plot, create_workspace

    ws = create_workspace(tmp_path / "ws-legacy")
    pid = "legacy-comp"
    rel = f"plots/{pid}.json"
    (ws.root / "plots").mkdir(exist_ok=True)
    (ws.root / rel).write_text(
        json.dumps(
            {
                "schemaVersion": 2,
                "id": pid,
                "name": "Legacy Comp",
                "type": "composite",
                "well_ids": [],
                "template_id": "t",
                "panels": [{"plot_id": "p9", "slot": "left"}],
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    add_plot(ws, name="Legacy Comp", plot_type="composite", path=rel, plot_id=pid)
    loaded = load_plot_document(ws, pid)
    assert loaded.panels[0].plot_id == "p9"
    assert loaded.panels[0].source_plot_type == "single_well"  # default
    assert loaded.panels[0].render_mode == "live"  # default
    assert loaded.panels[0].rect_mm is None


# --- export_dispatch (T8) ---

class _FakeView:
    """FenceView stand-in exposing grab_fence_png."""

    def __init__(self, tmp_path: Path):
        self._dir = tmp_path

    def grab_fence_png(self, path: Path | str) -> Path:
        out = Path(path)
        out.write_text("png-data", encoding="utf-8")
        return out


def test_fence_3d_export_png_only(tmp_path: Path):
    from well_log_workstation.plot_document import PlotDocument

    fence = PlotDocument(
        id="f1", name="F", type="fence_3d", well_ids=[], template_id="t",
        path="plots/f1.json",
    )
    view = _FakeView(tmp_path)
    out = export_plot(fence, "png", view=view, path=str(tmp_path / "f.png"))
    assert out.is_file()

    with pytest.raises(UnsupportedFormatError):
        export_plot(fence, "svg", view=view, path=str(tmp_path / "f.svg"))
    with pytest.raises(UnsupportedFormatError):
        export_plot(fence, "pdf", view=view, path=str(tmp_path / "f.pdf"))


def test_page_spec_dimensions():
    spec = PageSpec(page_size="A4", orientation="landscape")
    assert spec.width_mm == 297.0
    assert spec.height_mm == 210.0
    portrait = PageSpec(page_size="A4", orientation="portrait")
    assert portrait.width_mm == 210.0
    assert portrait.height_mm == 297.0


def test_export_plot_unknown_type_raises(tmp_path: Path):
    from well_log_workstation.plot_document import PlotDocument

    bogus = PlotDocument(
        id="z", name="Z", type="bogus",  # type: ignore[arg-type]
        well_ids=[], template_id=None, path="plots/z.json",
    )
    with pytest.raises(UnsupportedFormatError):
        export_plot(bogus, "png", path=str(tmp_path / "z.png"))
