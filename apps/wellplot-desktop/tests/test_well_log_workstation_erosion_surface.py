"""Erosion/onlap surface truncation (FRS §3.x 尖灭行 P1).

Covers:
* truncate_quad_by_surface — erosion keeps below / onlap keeps above,
  precise corner assertions, fill/pattern preserved, rejection cases;
* split_quad_composite ordering — surface truncates first, then fault,
  then contact;
* serialization round-trip + v8→v9 migration (old files load surfaces=[]);
* dialog value() round-trip (first section-dialog test);
* section canvas renders the surface line + truncated quad (pixel-diff).
"""

from __future__ import annotations

import os
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import numpy as np
import pytest
from PySide6.QtWidgets import QTableWidgetItem

from well_log_workstation.plot_document import load_plot_document
from well_log_workstation.section_geometry import (
    ErosionSurface2D,
    FluidContact2D,
    SectionFault2D,
    TieQuad2D,
    split_quad_composite,
    surfaces_from_json,
    surfaces_to_json,
    truncate_quad_by_surface,
)
from well_log_workstation.workspace import create_workspace


def _quad(x0: float, d_top: float, d_bot: float) -> TieQuad2D:
    """Quad spanning columns x0..x0+1, depths d_top..d_bot."""
    return TieQuad2D(
        corners=np.array(
            [[x0, d_top], [x0 + 1, d_top], [x0 + 1, d_bot], [x0, d_bot]]
        ),
        fill_color="#aaa",
        pattern_id="syt-sandstone",
    )


def _surface(mode: str = "erosion", depths: dict | None = None) -> ErosionSurface2D:
    return ErosionSurface2D(
        name="SB",
        mode=mode,
        depths=depths or {0: 1050.0, 1: 1050.0},
    )


# ---------------------------------------------------------------------------
# truncate_quad_by_surface
# ---------------------------------------------------------------------------


def test_truncate_erosion_keeps_below() -> None:
    q = _quad(0.0, 1005.0, 1095.0)
    kept = truncate_quad_by_surface(q, _surface("erosion"), 3)
    assert kept is not None
    # Below polygon: (x_l, d_l) → (x_r, d_r) → right_bottom → left_bottom
    assert kept.corners[0, 0] == 0.0 and kept.corners[0, 1] == 1050.0
    assert kept.corners[1, 0] == 1.0 and kept.corners[1, 1] == 1050.0
    assert kept.corners[2, 1] == 1095.0
    assert kept.corners[3, 1] == 1095.0
    # Structural truncation keeps fill + pattern (no recolouring).
    assert kept.fill_color == "#aaa"
    assert kept.pattern_id == "syt-sandstone"


def test_truncate_onlap_keeps_above() -> None:
    q = _quad(0.0, 1005.0, 1095.0)
    kept = truncate_quad_by_surface(q, _surface("onlap"), 3)
    assert kept is not None
    # Above polygon: left_top → right_top → (x_r, d_r) → (x_l, d_l)
    assert kept.corners[0, 1] == 1005.0
    assert kept.corners[1, 1] == 1005.0
    assert kept.corners[2, 0] == 1.0 and kept.corners[2, 1] == 1050.0
    assert kept.corners[3, 0] == 0.0 and kept.corners[3, 1] == 1050.0


def test_truncate_interpolates_sloped_surface() -> None:
    q = _quad(0.0, 1005.0, 1095.0)
    s = _surface("erosion", {0: 1050.0, 1: 1070.0})
    kept = truncate_quad_by_surface(q, s, 3)
    assert kept is not None
    assert kept.corners[0, 1] == pytest.approx(1050.0)
    assert kept.corners[1, 1] == pytest.approx(1070.0)


def test_truncate_surface_outside_returns_none() -> None:
    q = _quad(0.0, 1005.0, 1095.0)
    # Surface above the quad top → nothing truncated.
    above = _surface("erosion", {0: 1000.0, 1: 1000.0})
    assert truncate_quad_by_surface(q, above, 3) is None
    # Surface below the quad bottom → nothing truncated.
    below = _surface("erosion", {0: 1100.0, 1: 1100.0})
    assert truncate_quad_by_surface(q, below, 3) is None
    # Missing well depth → no interpolation → None.
    missing = _surface("erosion", {0: 1050.0})
    assert truncate_quad_by_surface(q, missing, 3) is None
    # Zero-width quad → None.
    degenerate = TieQuad2D(
        corners=np.array(
            [[0.0, 1000.0], [0.0, 1000.0], [0.0, 1100.0], [0.0, 1100.0]]
        ),
        fill_color="#aaa",
    )
    assert truncate_quad_by_surface(degenerate, _surface(), 3) is None


# ---------------------------------------------------------------------------
# Composite ordering: surface → fault → contact
# ---------------------------------------------------------------------------


def test_composite_surface_truncates_first() -> None:
    q = _quad(0.0, 1005.0, 1095.0)
    surface = _surface("erosion", {0: 1080.0, 1: 1080.0})
    fault = SectionFault2D(
        name="F", between=(0, 1), x_frac=0.5, top_depth=1000, bottom_depth=1100
    )
    contact = FluidContact2D(fluid_type="owc", depths={0: 1060.0, 1: 1060.0})
    # Without the surface: fault → contact → 4 pieces. With the surface the
    # quad is truncated to 1080..1095 first — the contact at 1060 falls
    # outside the kept band and no longer splits; the fault still cuts.
    pieces = split_quad_composite(q, [fault], [contact], 3)
    assert len(pieces) == 4
    truncated = split_quad_composite(q, [fault], [contact], 3, surfaces=[surface])
    assert len(truncated) == 2  # fault split only — contact excluded
    assert truncated[0].corners[0, 1] == pytest.approx(1080.0)
    assert truncated[0].fill_color == "#aaa"


def test_composite_surface_then_fault() -> None:
    q = _quad(0.0, 1005.0, 1095.0)
    surface = _surface("erosion", {0: 1030.0, 1: 1030.0})  # thin band kept
    fault = SectionFault2D(
        name="F", between=(0, 1), x_frac=0.5, top_depth=1000, bottom_depth=1100
    )
    pieces = split_quad_composite(q, [fault], [], 3, surfaces=[surface])
    assert len(pieces) == 2  # fault splits the truncated band
    assert pieces[0].corners[0, 1] == pytest.approx(1030.0)


# ---------------------------------------------------------------------------
# Serialization + schema migration
# ---------------------------------------------------------------------------


def test_surface_json_roundtrip() -> None:
    s = _surface("onlap", {0: 1050.0, 2: 1070.0})
    out = surfaces_from_json(surfaces_to_json([s]))
    assert len(out) == 1
    assert out[0].name == "SB"
    assert out[0].mode == "onlap"
    assert out[0].depths == {0: 1050.0, 2: 1070.0}


def test_surface_from_json_invalid_mode_defaults_erosion() -> None:
    raw = [{"name": "X", "mode": "bogus", "depths": [[0, 100.0], [1, 110.0]]}]
    out = surfaces_from_json(raw)
    assert len(out) == 1
    assert out[0].mode == "erosion"


def test_plot_document_v9_migration(tmp_path: Path) -> None:
    """A v8 file (no surfaces) migrates additively to v9 with []."""
    ws = create_workspace(tmp_path / "ws-v9")
    path = ws.plots_dir / "pd-v9.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        '{"schemaVersion": 8, "id": "pd-v9", "name": "V9", "type": "section",'
        ' "well_ids": ["w1", "w2"], "template_id": null,'
        ' "path": "plots/pd-v9.json", "datum_mode": "md"}',
        encoding="utf-8",
    )
    loaded = load_plot_document(ws, "pd-v9")
    assert loaded.surfaces == []


def test_plot_document_surfaces_roundtrip(tmp_path: Path) -> None:
    from well_log_workstation.plot_document import (
        PlotDocument,
        save_plot_document,
    )

    ws = create_workspace(tmp_path / "ws-surf")
    plot = PlotDocument(
        id="pd-surf",
        name="Surf",
        type="section",
        well_ids=["w1", "w2"],
        template_id=None,
        path="plots/pd-surf.json",
        surfaces=surfaces_to_json([_surface("erosion", {0: 1050.0, 1: 1060.0})]),
    )
    save_plot_document(ws, plot)
    loaded = load_plot_document(ws, "pd-surf")
    assert len(loaded.surfaces) == 1
    assert loaded.surfaces[0]["mode"] == "erosion"


# ---------------------------------------------------------------------------
# Dialog
# ---------------------------------------------------------------------------


def test_erosion_surface_dialog_value(qtbot) -> None:
    from well_log_workstation.section_geometry.erosion_surface_dialog import (
        SectionErosionSurfaceDialog,
    )

    dlg = SectionErosionSurfaceDialog(
        current=[_surface("onlap", {0: 1050.0, 1: 1070.0})],
        well_count=2,
        well_names=["W1", "W2"],
    )
    qtbot.addWidget(dlg)
    out = dlg.value()
    assert len(out) == 1
    assert out[0].name == "SB"
    assert out[0].mode == "onlap"
    assert out[0].depths == {0: 1050.0, 1: 1070.0}


def test_erosion_surface_dialog_drops_short_rows(qtbot) -> None:
    from well_log_workstation.section_geometry.erosion_surface_dialog import (
        SectionErosionSurfaceDialog,
    )

    dlg = SectionErosionSurfaceDialog(well_count=2)
    qtbot.addWidget(dlg)
    # Fill only one well depth → row dropped (<2 depths).
    dlg.table.item(0, 0).setText("Solo")
    dlg.table.setItem(0, 2, QTableWidgetItem("1050.0"))
    assert dlg.value() == []


# ---------------------------------------------------------------------------
# Canvas render
# ---------------------------------------------------------------------------


def test_section_canvas_surface_truncation_changes_render(qtbot, pixel_bytes) -> None:
    from PySide6.QtGui import QImage

    from well_log_workstation.section_canvas import SectionCanvas
    from well_log_workstation.template_model import HostPresentation

    canvas = SectionCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 480)

    depth = np.array([1000.0, 1100.0])

    class _Layer:
        color = "#1f77b4"
        values = np.array([10.0, 30.0])
        null_mask = np.array([False, False])

    class _Scale:
        mode = "linear"
        min = 0.0
        max = 100.0

    class _Track:
        role = "curve"
        layers = [_Layer()]
        scale = _Scale()

    pres = HostPresentation(
        template_id="t", template_name="T", well_document_id="w1",
        well_name="W1", depth=depth, depth_unit="m",
        tracks=[_Track()],  # type: ignore[arg-type]
    )
    quad = _quad(0.0, 1005.0, 1095.0)

    def grab(surfaces) -> QImage:
        img = QImage(canvas.size(), QImage.Format.Format_ARGB32)
        img.fill(0)
        canvas.set_section(
            [pres, pres], [[], []], surfaces=surfaces, tie_quads=[quad]
        )
        canvas.render(img)
        return img

    plain = grab([])
    truncated = grab([_surface("erosion", {0: 1050.0, 1: 1050.0})])
    assert truncated.size() == plain.size()
    assert pixel_bytes(truncated) != pixel_bytes(plain)


def test_section_canvas_surface_line_renders(qtbot) -> None:
    """The surface dash-dot line alone (no quad) still paints without crash."""
    from PySide6.QtGui import QImage

    from well_log_workstation.section_canvas import SectionCanvas
    from well_log_workstation.template_model import HostPresentation

    canvas = SectionCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 480)
    depth = np.array([1000.0, 1100.0])

    class _Layer:
        color = "#1f77b4"
        values = np.array([10.0, 30.0])
        null_mask = np.array([False, False])

    class _Scale:
        mode = "linear"
        min = 0.0
        max = 100.0

    class _Track:
        role = "curve"
        layers = [_Layer()]
        scale = _Scale()

    pres = HostPresentation(
        template_id="t", template_name="T", well_document_id="w1",
        well_name="W1", depth=depth, depth_unit="m",
        tracks=[_Track()],  # type: ignore[arg-type]
    )
    canvas.set_section(
        [pres, pres],
        [[], []],
        surfaces=[_surface("erosion", {0: 1050.0, 1: 1050.0})],
    )
    img = QImage(canvas.size(), QImage.Format.Format_ARGB32)
    img.fill(0)
    canvas.render(img)
    assert not img.isNull()
