"""Correlation real well-distance + vertical exaggeration (FRS §3.x).

Covers the pure-Python geodesy helpers (haversine / bearing / offsets), the
canvas offset + VE state and unified ``_x_well`` mapping, the PlotDocument
persistence roundtrip (incl. legacy files), and the shell wiring of the
spacing combo + VE spinbox to the canvas.
"""

from __future__ import annotations

import os
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import numpy as np
import pytest

from well_log_workstation.well_spacing import (
    bearing_deg,
    haversine_m,
    wellhead_offsets,
)


# -- geodesy helpers (pure Python) -----------------------------------


def test_haversine_known_distance() -> None:
    # Beijing (116.4, 39.9) -> Shanghai (121.5, 31.2): ~1067 km.
    d = haversine_m(116.4, 39.9, 121.5, 31.2)
    assert 1_000_000 < d < 1_130_000


def test_haversine_zero_distance() -> None:
    assert haversine_m(100.0, 20.0, 100.0, 20.0) == 0.0


def test_bearing_cardinal() -> None:
    assert abs(bearing_deg(0.0, 0.0, 0.0, 10.0)) < 1e-6  # due north → 0°
    assert abs(bearing_deg(0.0, 0.0, 10.0, 0.0) - 90.0) < 1e-6  # due east → 90°


def test_wellhead_offsets_equal_spacing() -> None:
    # 3 wells along a meridian, equal 1° gaps → offsets [0, 1, 2].
    offs, spacing_m = wellhead_offsets([(0.0, 0.0), (0.0, 1.0), (0.0, 2.0)])
    assert offs == [0.0, 1.0, 2.0]
    assert 100_000 < spacing_m < 120_000  # ~111 km per degree


def test_wellhead_offsets_unequal() -> None:
    # gaps 1°, 3° → mean 2° → offsets [0, 0.5, 2.0].
    offs, _ = wellhead_offsets([(0.0, 0.0), (0.0, 1.0), (0.0, 4.0)])
    assert offs == [0.0, 0.5, 2.0]


def test_wellhead_offsets_degrades_on_missing_coord() -> None:
    offs, spacing_m = wellhead_offsets([(None, None), (1.0, 2.0)])
    assert offs is None
    assert spacing_m == 0.0


def test_wellhead_offsets_single_and_empty() -> None:
    assert wellhead_offsets([(5.0, 5.0)]) == ([0.0], 0.0)
    assert wellhead_offsets([]) == ([], 0.0)


# -- correlation canvas offset + VE ----------------------------------


def _make_canvas():
    from well_log_workstation.correlation_canvas import CorrelationCanvas
    from well_log_workstation.template_model import (
        BoundCurveLayer,
        BoundTrack,
        HostPresentation,
        ScaleSpec,
    )

    canvas = CorrelationCanvas()
    canvas.resize(600, 400)
    depth = np.array([1000.0, 1100.0, 1200.0])
    vals = np.array([20.0, 50.0, 80.0])

    def mk(i: int) -> HostPresentation:
        track = BoundTrack(
            id="c",
            role="curve",
            title="GR",
            width_fraction=1.0,
            scale=ScaleSpec(min=0.0, max=100.0),
            layers=[
                BoundCurveLayer(
                    mnemonic="GR",
                    color="#1f77b4",
                    unit="gapi",
                    values=vals,
                    null_mask=np.zeros(3, bool),
                )
            ],
        )
        return HostPresentation(
            template_id="t",
            template_name="t",
            well_document_id=str(i),
            well_name=f"W{i}",
            depth=depth,
            depth_unit="m",
            tracks=[track],
        )

    canvas.set_columns([mk(i) for i in range(3)])
    return canvas


def test_canvas_x_well_equal_default(qtbot) -> None:
    canvas = _make_canvas()
    qtbot.addWidget(canvas)
    # Equal spacing: well 1 centre = 8 + 1*(col_w+gap) + col_w/2.
    col_w = max(40, (600 - 16 - 6 * 2) // 3)
    assert canvas._x_well(1, col_w, 6) == pytest.approx(8 + (col_w + 6) + col_w / 2)


def test_canvas_x_well_with_offsets(qtbot) -> None:
    canvas = _make_canvas()
    qtbot.addWidget(canvas)
    col_w = max(40, (600 - 16 - 6 * 2) // 3)
    base1 = canvas._x_well(1, col_w, 6)
    canvas.set_well_x_offsets([0.0, 0.5, 2.0])
    # offset 0.5 stride added to well 1.
    assert canvas._x_well(1, col_w, 6) == pytest.approx(base1 + 0.5 * (col_w + 6))
    assert canvas.well_x_offsets() == [0.0, 0.5, 2.0]


def test_canvas_vertical_exaggeration_clamp_and_getter(qtbot) -> None:
    canvas = _make_canvas()
    qtbot.addWidget(canvas)
    canvas.set_vertical_exaggeration(3.0)
    assert canvas.vertical_exaggeration() == 3.0
    canvas.set_vertical_exaggeration(999.0)
    assert canvas.vertical_exaggeration() == 20.0
    canvas.set_vertical_exaggeration(0.0)
    assert canvas.vertical_exaggeration() == 0.1


def test_canvas_paint_smoke_with_offsets_and_ve(qtbot) -> None:
    canvas = _make_canvas()
    qtbot.addWidget(canvas)
    canvas.set_well_x_offsets([0.0, 0.5, 2.0])
    canvas.set_vertical_exaggeration(3.0)
    img = canvas.grab()
    assert img.width() == 600


# -- PlotDocument persistence ----------------------------------------


def test_plot_doc_spacing_and_ve_roundtrip(tmp_path: Path) -> None:
    from well_log_workstation.plot_document import (
        _from_json,
        _to_json,
        PlotDocument,
    )

    doc = PlotDocument(
        id="p1",
        name="c",
        type="correlation",
        well_ids=["a", "b"],
        path="plots/p1.json",
        template_id="t",
        correlation_spacing="real",
        vertical_exaggeration=2.5,
    )
    back = _from_json(_to_json(doc), path="plots/p1.json")
    assert back.correlation_spacing == "real"
    assert back.vertical_exaggeration == 2.5


def test_plot_doc_legacy_defaults(tmp_path: Path) -> None:
    from well_log_workstation.plot_document import _from_json

    legacy = {
        "id": "p2",
        "type": "correlation",
        "well_ids": ["a"],
        "path": "plots/p2.json",
        "schemaVersion": 10,
    }
    back = _from_json(legacy, path="plots/p2.json")
    assert back.correlation_spacing == "equal"
    assert back.vertical_exaggeration == 1.0


def test_plot_doc_clamps_invalid_values() -> None:
    from well_log_workstation.plot_document import _from_json

    bad = {
        "id": "p3",
        "type": "correlation",
        "well_ids": ["a"],
        "path": "plots/p3.json",
        "schemaVersion": 10,
        "vertical_exaggeration": 999.0,
        "correlation_spacing": "bogus",
    }
    back = _from_json(bad, path="plots/p3.json")
    assert back.vertical_exaggeration == 20.0
    assert back.correlation_spacing == "equal"


# -- shell wiring ----------------------------------------------------


def test_shell_correlation_spacing_and_ve_wires_canvas(
    qtbot, tmp_path: Path
) -> None:
    from well_log_workstation.shell import WellLogWorkstationWindow
    from well_log_workstation.workspace import create_workspace

    def _las(path: Path, well: str, lng: str, lat: str) -> Path:
        path.write_text(
            f"""~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1002.0
STEP.M 1.0
NULL. -999.25
WELL. {well}
LAT.  {lat}
LONG. {lng}
~CURVE INFORMATION
DEPT.M
GR.GAPI
~ASCII
1000 20
1001 30
1002 40
""",
            encoding="utf-8",
        )
        return path

    ws = create_workspace(tmp_path / "ws")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    id1 = win.import_las_path(_las(tmp_path / "a.las", "A", "0.0", "0.0"))
    id2 = win.import_las_path(_las(tmp_path / "b.las", "B", "0.0", "1.0"))

    plot = win.create_correlation_plot_document([id1, id2], "std-gr-rt-den")
    win.open_plot_document(plot.id)

    # Toggles enabled on a correlation plot.
    assert win.corr_spacing_combo.isEnabled()
    assert win.corr_ve_spin.isEnabled()

    # Switch to real spacing → canvas gets non-None offsets.
    idx_real = win.corr_spacing_combo.findData("real")
    win.corr_spacing_combo.setCurrentIndex(idx_real)
    assert win.correlation_canvas.well_x_offsets() is not None

    # VE spinbox updates the canvas.
    win.corr_ve_spin.setValue(4.0)
    assert win.correlation_canvas.vertical_exaggeration() == 4.0
