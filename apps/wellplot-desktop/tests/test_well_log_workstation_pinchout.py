"""Pinchout wedge fills for the correlation canvas (FRS §3.3 / P0-C).

Covers:
* pure geometry (interwell_fill builder) — wedge direction, apex placement,
  min-thickness filter, factor clamp, smooth flag, quad/wedge coexistence;
* canvas state (set_pinchout + getters);
* plot-document persistence + reopen round-trip;
* offscreen paint smoke (no crash with wedges on).
"""

from __future__ import annotations

import os
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.interwell_fill import (
    PINCH_LEFT,
    PINCH_OFF,
    PINCH_RIGHT,
    build_interwell_fill_bands,
)
from well_log_workstation.plot_document import load_plot_document
from well_log_workstation.shell import WellLogWorkstationWindow
from well_log_workstation.tops_model import FormationTop, save_tops_for_well
from well_log_workstation.workspace import create_workspace


def _top(name: str, depth: float, i: int = 0) -> FormationTop:
    return FormationTop(name=name, depth=depth, id=f"{name}-{i}")


# ---------------------------------------------------------------------------
# Pure geometry
# ---------------------------------------------------------------------------


def test_left_only_interval_wedges_right() -> None:
    """Interval present only on the left column → wedge points right."""
    cols = [
        [_top("A", 1000.0, 0), _top("B", 1010.0, 0)],
        [],  # right column empty
    ]
    bands = build_interwell_fill_bands(cols, pinchout_mode="linear")
    assert len(bands) == 1
    b = bands[0]
    assert b.pinch == PINCH_RIGHT
    assert b.left_top_depth == pytest.approx(1000.0)
    assert b.left_bottom_depth == pytest.approx(1010.0)
    # Right side collapses to the interval midpoint.
    mid = 0.5 * (1000.0 + 1010.0)
    assert b.right_top_depth == pytest.approx(mid)
    assert b.right_bottom_depth == pytest.approx(mid)
    assert b.apex_depth == pytest.approx(mid)
    assert b.apex_frac == pytest.approx(0.5)


def test_right_only_interval_wedges_left() -> None:
    """Interval present only on the right column → wedge points left."""
    cols = [
        [],
        [_top("A", 1002.0, 1), _top("B", 1008.0, 1)],
    ]
    bands = build_interwell_fill_bands(cols, pinchout_mode="linear")
    assert len(bands) == 1
    b = bands[0]
    assert b.pinch == PINCH_LEFT
    assert b.right_top_depth == pytest.approx(1002.0)
    assert b.right_bottom_depth == pytest.approx(1008.0)
    mid = 0.5 * (1002.0 + 1008.0)
    assert b.apex_depth == pytest.approx(mid)


def test_shared_tops_stay_quad_bands() -> None:
    """Fully shared consecutive tops produce a normal quad (pinch off)."""
    cols = [
        [_top("T1", 1000.0, 0), _top("T2", 1010.0, 0)],
        [_top("T1", 1001.0, 1), _top("T2", 1011.0, 1)],
    ]
    bands = build_interwell_fill_bands(cols, pinchout_mode="linear")
    assert len(bands) == 1
    assert bands[0].pinch == PINCH_OFF


def test_pinchout_off_drops_unilateral_intervals() -> None:
    """Default mode keeps legacy behaviour: unilateral intervals are dropped."""
    cols = [
        [_top("A", 1000.0, 0), _top("B", 1010.0, 0)],
        [],
    ]
    assert build_interwell_fill_bands(cols) == []
    assert build_interwell_fill_bands(cols, pinchout_mode="off") == []


def test_quad_and_wedge_coexist() -> None:
    """A shared pair + a unilateral interval → one quad + one wedge."""
    cols = [
        # Left: T1/T2 shared, T3/T4 unilateral
        [_top("T1", 1000.0, 0), _top("T2", 1010.0, 0), _top("T3", 1020.0, 0), _top("T4", 1030.0, 0)],
        [_top("T1", 1001.0, 1), _top("T2", 1011.0, 1)],  # missing T3/T4
    ]
    bands = build_interwell_fill_bands(cols, pinchout_mode="linear")
    quads = [b for b in bands if b.pinch == PINCH_OFF]
    wedges = [b for b in bands if b.pinch != PINCH_OFF]
    assert len(quads) == 1  # T1/T2
    assert len(wedges) == 1  # T3/T4 on left → wedge right
    assert wedges[0].pinch == PINCH_RIGHT
    assert wedges[0].top_name == "T3" and wedges[0].bottom_name == "T4"


def test_min_thickness_filters_thin_wedge() -> None:
    """A sub-min_thickness unilateral interval yields no wedge."""
    cols = [
        [_top("A", 1000.0, 0), _top("B", 1000.0005, 0)],  # ~0 thickness
        [],
    ]
    bands = build_interwell_fill_bands(
        cols, pinchout_mode="linear", min_thickness=1e-3
    )
    assert bands == []


def test_factor_is_clamped() -> None:
    """Out-of-range factors clamp to [0.05, 1.0]."""
    cols = [[_top("A", 1000.0, 0), _top("B", 1010.0, 0)], []]
    b_lo = build_interwell_fill_bands(cols, pinchout_mode="linear", pinchout_factor=0.0)
    b_hi = build_interwell_fill_bands(cols, pinchout_mode="linear", pinchout_factor=5.0)
    assert b_lo[0].apex_frac == pytest.approx(0.05)
    assert b_hi[0].apex_frac == pytest.approx(1.0)


def test_smooth_flag_propagates() -> None:
    cols = [[_top("A", 1000.0, 0), _top("B", 1010.0, 0)], []]
    bands = build_interwell_fill_bands(
        cols, pinchout_mode="linear", pinchout_smooth=True
    )
    assert bands[0].smooth is True


def test_invalid_mode_falls_back_to_off() -> None:
    cols = [[_top("A", 1000.0, 0), _top("B", 1010.0, 0)], []]
    bands = build_interwell_fill_bands(cols, pinchout_mode="nonsense")
    assert bands == []  # treated as off → unilateral dropped


def test_needs_two_columns() -> None:
    cols = [[_top("A", 1000.0, 0), _top("B", 1010.0, 0)]]
    assert build_interwell_fill_bands(cols, pinchout_mode="linear") == []


# ---------------------------------------------------------------------------
# Canvas state
# ---------------------------------------------------------------------------


def test_canvas_set_pinchout_state(qtbot) -> None:
    from well_log_workstation.correlation_canvas import CorrelationCanvas

    canvas = CorrelationCanvas()
    qtbot.addWidget(canvas)
    assert canvas.pinchout_mode() == "off"
    canvas.set_pinchout("linear", 0.3, True)
    assert canvas.pinchout_mode() == "linear"
    assert canvas.pinchout_factor() == pytest.approx(0.3)
    assert canvas.pinchout_smooth() is True
    # Invalid mode falls back to off; factor clamps.
    canvas.set_pinchout("bogus", 99.0, False)
    assert canvas.pinchout_mode() == "off"
    assert canvas.pinchout_factor() == pytest.approx(1.0)


def test_canvas_paints_with_wedges_no_crash(qtbot) -> None:
    """Offscreen paint with wedges enabled must not raise."""
    from PySide6.QtGui import QImage

    from well_log_workstation.correlation_canvas import CorrelationCanvas
    from well_log_workstation.template_model import HostPresentation
    import numpy as np

    canvas = CorrelationCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 480)
    depth = np.array([1000.0, 1005.0, 1010.0])
    vals = np.array([10.0, 20.0, 30.0])
    nulls = np.array([False, False, False])

    class _Layer:
        color = "#1f77b4"
        values = vals
        null_mask = nulls

    class _Scale:
        mode = "linear"
        min = 0.0
        max = 100.0

    class _Track:
        role = "curve"
        layers = [_Layer()]
        scale = _Scale()

    pres = HostPresentation(
        template_id="std-gr-rt-den",
        template_name="GR/RT/DEN",
        well_document_id="w1",
        well_name="W1",
        depth=depth,
        depth_unit="m",
        tracks=[_Track()],  # type: ignore[arg-type]
    )
    canvas.set_columns([pres, pres])
    canvas.set_tops_per_column(
        [
            [_top("A", 1000.0), _top("B", 1010.0)],
            [_top("A", 1001.0)],  # B missing → wedge
        ]
    )
    canvas.set_show_interwell_fill(True)
    canvas.set_pinchout("linear", 0.5, smooth=True)
    canvas.set_depth_range(999.0, 1011.0)

    img = QImage(canvas.size(), QImage.Format.Format_ARGB32)
    img.fill(0)
    # render(QPaintDevice) single-arg form triggers paintEvent.
    canvas.render(img)
    # No assertion on pixels — this is a crash smoke test.


# ---------------------------------------------------------------------------
# Plot-document persistence
# ---------------------------------------------------------------------------


def _write_las(path: Path, well: str) -> Path:
    path.write_text(
        f"""~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1005.0
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
1005 60 6
""",
        encoding="utf-8",
    )
    return path


def test_pinchout_persists_and_reopens(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws", name="Pinch")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    id1 = win.import_las_path(_write_las(tmp_path / "a.las", "A"))
    id2 = win.import_las_path(_write_las(tmp_path / "b.las", "B"))
    save_tops_for_well(
        ws,
        id1,
        [
            FormationTop(name="T1", depth=1001.0, id="x1"),
            FormationTop(name="T2", depth=1003.0, id="x2"),
            FormationTop(name="T3", depth=1004.5, id="x3"),
        ],
    )
    save_tops_for_well(
        ws,
        id2,
        [
            FormationTop(name="T1", depth=1001.2, id="y1"),
            FormationTop(name="T2", depth=1003.2, id="y2"),
            # T3 absent on well B → wedge
        ],
    )
    plot = win.create_correlation_plot_document([id1, id2], "std-gr-rt-den")

    # Defaults: fill off, pinchout off.
    assert win.correlation_canvas.pinchout_mode() == "off"

    # Turn fill on first (pinchout is gated behind fill), then pinchout on.
    win.corr_fill_check.setChecked(True)
    assert win.corr_pinch_check.isEnabled()
    win.corr_pinch_check.setChecked(True)
    win.corr_pinch_factor.setValue(0.3)
    win.corr_pinch_smooth.setChecked(True)

    assert win.correlation_canvas.pinchout_mode() == "linear"
    assert win.correlation_canvas.pinchout_factor() == pytest.approx(0.3)
    assert win.correlation_canvas.pinchout_smooth() is True

    loaded = load_plot_document(ws, plot.id)
    assert loaded.pinchout_mode == "linear"
    assert loaded.pinchout_factor == pytest.approx(0.3)
    assert loaded.pinchout_smooth is True

    # Reopen in a fresh window restores the three pinchout fields.
    win2 = WellLogWorkstationWindow()
    qtbot.addWidget(win2)
    win2.set_workspace(ws)
    win2.open_plot_document(plot.id)
    assert win2.correlation_canvas.pinchout_mode() == "linear"
    assert win2.correlation_canvas.pinchout_factor() == pytest.approx(0.3)
    assert win2.correlation_canvas.pinchout_smooth() is True


def test_legacy_plot_json_without_pinchout_defaults_off(tmp_path: Path) -> None:
    """A v7 plot JSON written before pinchout fields loads with defaults."""
    import json

    from well_log_workstation.workspace import add_plot, create_workspace

    ws = create_workspace(tmp_path / "legacy", name="Legacy")
    plot = add_plot(
        ws,
        name="Legacy Corr",
        plot_type="correlation",
        well_ids=["w1", "w2"],
        template_id="std-gr-rt-den",
        path="plots/legacy.json",
    )
    # Hand-write a minimal v7 correlation doc with NO pinchout fields.
    (ws.root / plot.path).write_text(
        json.dumps(
            {
                "schemaVersion": 7,
                "id": plot.id,
                "name": "Legacy Corr",
                "type": "correlation",
                "well_ids": ["w1", "w2"],
                "template_id": "std-gr-rt-den",
                "links": [],
                "column_gap_px": 6,
                "datum_mode": "md",
                "show_interwell_fill": False,
            }
        ),
        encoding="utf-8",
    )
    loaded = load_plot_document(ws, plot.id)
    assert loaded.pinchout_mode == "off"
    assert loaded.pinchout_factor == pytest.approx(0.5)
    assert loaded.pinchout_smooth is False
