"""Section fault throw model (FRS §3.3 / P1-A).

Covers:
* fault_polyline / fault_x placement in section space;
* apply_fault_throw_to_quad — normal fault (down-dip drops), reverse fault
  (down-dip lifts), quad not crossing fault unchanged, depth-out-of-range
  unchanged;
* serialization round-trip + tolerant parsing;
* section canvas render smoke (fault line + displaced quad);
* plot-document faults round-trip + legacy default [].
"""

from __future__ import annotations

import os
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import numpy as np
import pytest

from well_log_workstation.section_geometry import (
    SectionFault2D,
    TieQuad2D,
    apply_fault_throw_to_quad,
    fault_polyline,
    fault_x,
    faults_from_json,
    faults_to_json,
)


# ---------------------------------------------------------------------------
# Placement
# ---------------------------------------------------------------------------


def test_fault_x_midpoint_default() -> None:
    f = SectionFault2D(name="F", between=(0, 1))
    assert fault_x(f, 3) == pytest.approx(0.5)


def test_fault_x_clamps_frac() -> None:
    f = SectionFault2D(name="F", between=(0, 1), x_frac=2.0)
    assert fault_x(f, 3) == pytest.approx(1.0)
    f2 = SectionFault2D(name="F", between=(0, 1), x_frac=-1.0)
    assert fault_x(f2, 3) == pytest.approx(0.0)


def test_fault_polyline_two_points() -> None:
    f = SectionFault2D(
        name="F", between=(0, 1), x_frac=0.5, top_depth=1000, bottom_depth=1100
    )
    pts = fault_polyline(f, 3)
    assert pts.shape == (2, 2)
    assert pts[0, 0] == pytest.approx(0.5)
    assert pts[0, 1] == 1000 and pts[1, 1] == 1100


def test_fault_polyline_invalid_well_count_empty() -> None:
    f = SectionFault2D(name="F", between=(0, 1))
    assert fault_polyline(f, 1).shape == (0, 2)
    assert fault_polyline(f, 0).shape == (0, 2)


def test_fault_polyline_same_indices_empty() -> None:
    f = SectionFault2D(name="F", between=(1, 1))
    assert fault_polyline(f, 3).shape == (0, 2)


# ---------------------------------------------------------------------------
# Throw application to tie-quads
# ---------------------------------------------------------------------------


def _quad(x0: float, d_top: float, d_bot: float) -> TieQuad2D:
    """A quad spanning columns x0..x0+1, depths d_top..d_bot."""
    return TieQuad2D(
        corners=np.array(
            [[x0, d_top], [x0 + 1, d_top], [x0 + 1, d_bot], [x0, d_bot]]
        ),
        fill_color="#aaa",
    )


def test_normal_fault_drops_down_dip_side() -> None:
    """throw > 0 → corners with x > fault_x get +throw on depth."""
    q = _quad(0.0, 1005.0, 1095.0)
    f = SectionFault2D(
        name="F", between=(0, 1), x_frac=0.5, top_depth=1000,
        bottom_depth=1100, throw=20.0,
    )
    out = apply_fault_throw_to_quad(q, f, 3)
    # down-dip corners (x=1.0) shifted; up-dip (x=0.0) unchanged
    assert out.corners[1, 1] == pytest.approx(1025.0)
    assert out.corners[2, 1] == pytest.approx(1115.0)
    assert out.corners[0, 1] == pytest.approx(1005.0)
    assert out.corners[3, 1] == pytest.approx(1095.0)


def test_reverse_fault_lifts_down_dip_side() -> None:
    """throw < 0 → down-dip corners get +throw (negative) → depth decreases."""
    q = _quad(0.0, 1005.0, 1095.0)
    f = SectionFault2D(
        name="F", between=(0, 1), x_frac=0.5, top_depth=1000,
        bottom_depth=1100, throw=-15.0,
    )
    out = apply_fault_throw_to_quad(q, f, 3)
    assert out.corners[1, 1] == pytest.approx(990.0)
    assert out.corners[2, 1] == pytest.approx(1080.0)


def test_quad_not_crossing_fault_unchanged() -> None:
    """A quad entirely on one side of the fault is not displaced."""
    q = _quad(0.0, 1005.0, 1095.0)
    # Fault at x=1.5 (between wells 1 and 2); quad spans x=0..1, no straddle.
    f = SectionFault2D(
        name="F", between=(1, 2), x_frac=0.5, top_depth=1000,
        bottom_depth=1100, throw=20.0,
    )
    out = apply_fault_throw_to_quad(q, f, 3)
    np.testing.assert_array_equal(out.corners, q.corners)


def test_fault_depth_out_of_range_unchanged() -> None:
    """Quad depths outside the fault's depth extent → no displacement."""
    q = _quad(0.0, 1005.0, 1095.0)
    f = SectionFault2D(
        name="F", between=(0, 1), x_frac=0.5, top_depth=2000,
        bottom_depth=2100, throw=20.0,
    )
    out = apply_fault_throw_to_quad(q, f, 3)
    np.testing.assert_array_equal(out.corners, q.corners)


def test_zero_throw_unchanged() -> None:
    q = _quad(0.0, 1005.0, 1095.0)
    f = SectionFault2D(
        name="F", between=(0, 1), x_frac=0.5, top_depth=1000,
        bottom_depth=1100, throw=0.0,
    )
    out = apply_fault_throw_to_quad(q, f, 3)
    np.testing.assert_array_equal(out.corners, q.corners)


# ---------------------------------------------------------------------------
# Serialization
# ---------------------------------------------------------------------------


def test_faults_round_trip() -> None:
    fs = [
        SectionFault2D(name="F1", between=(0, 1), x_frac=0.3, top_depth=1000,
                       bottom_depth=1100, throw=20.0),
        SectionFault2D(name="F2", between=(1, 2), x_frac=0.7, top_depth=900,
                       bottom_depth=1050, throw=-10.0),
    ]
    back = faults_from_json(faults_to_json(fs))
    assert len(back) == 2
    assert back[0].name == "F1" and back[0].throw == 20.0
    assert back[1].between == (1, 2) and back[1].throw == -10.0


def test_faults_from_json_drops_invalid() -> None:
    raw = [
        {"name": "F", "between": [0, 1], "x_frac": 0.5, "throw": 5},
        {"between": [0]},  # missing fields
        "not a dict",
        None,
    ]
    assert len(faults_from_json(raw)) == 1


def test_faults_from_json_non_list_returns_empty() -> None:
    assert faults_from_json(None) == []
    assert faults_from_json({"not": "list"}) == []


# ---------------------------------------------------------------------------
# Section canvas render smoke
# ---------------------------------------------------------------------------


def test_section_canvas_renders_with_fault(qtbot) -> None:
    from PySide6.QtGui import QImage

    from well_log_workstation.section_canvas import SectionCanvas
    from well_log_workstation.template_model import HostPresentation

    canvas = SectionCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 480)

    depth = np.array([1000.0, 1010.0])

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
    fault = SectionFault2D(
        name="F", between=(0, 1), x_frac=0.5, top_depth=1000,
        bottom_depth=1100, throw=20.0,
    )
    canvas.set_section([pres, pres], [[], []], faults=[fault], tie_quads=[quad])
    canvas.set_depth_range(999.0, 1101.0)

    img = QImage(canvas.size(), QImage.Format.Format_ARGB32)
    img.fill(0)
    canvas.render(img)  # crash smoke; no pixel assertion.


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
STOP.M 1010.0
STEP.M 1.0
NULL. -999.25
WELL. {well}
~CURVE INFORMATION
DEPT.M
GR.GAPI
~ASCII
1000 10
1010 30
""",
        encoding="utf-8",
    )
    return path


def test_section_faults_persist_and_reopen(qtbot, tmp_path: Path) -> None:
    from well_log_workstation.plot_document import (
        create_section_plot,
        load_plot_document,
        save_plot_document,
    )
    from well_log_workstation.section_geometry import faults_to_json
    from well_log_workstation.workspace import add_well, create_workspace

    ws = create_workspace(tmp_path / "ws", name="Fault")
    add_well(ws, name="W1", path="wells/w1.las")
    add_well(ws, name="W2", path="wells/w2.las")
    plot = create_section_plot(
        ws, well_ids=[ws.wells[0].id, ws.wells[1].id],
        template_id="gr-only", name="S",
    )

    # Edit faults via the document model + save.
    loaded = load_plot_document(ws, plot.id)
    loaded.faults = faults_to_json(
        [SectionFault2D(name="F1", between=(0, 1), x_frac=0.5,
                        top_depth=1000, bottom_depth=1010, throw=15.0)]
    )
    save_plot_document(ws, loaded)

    again = load_plot_document(ws, plot.id)
    assert len(again.faults) == 1
    parsed = faults_from_json(again.faults)
    assert parsed[0].name == "F1" and parsed[0].throw == 15.0


def test_legacy_section_json_without_faults(tmp_path: Path) -> None:
    import json

    from well_log_workstation.plot_document import load_plot_document
    from well_log_workstation.workspace import add_plot, create_workspace

    ws = create_workspace(tmp_path / "legacy", name="Legacy")
    plot = add_plot(
        ws, name="Legacy Section", plot_type="section",
        well_ids=["w1", "w2"], template_id="gr-only", path="plots/legacy.json",
    )
    (ws.root / plot.path).write_text(
        json.dumps(
            {
                "schemaVersion": 7,
                "id": plot.id, "name": "Legacy Section", "type": "section",
                "well_ids": ["w1", "w2"], "template_id": "gr-only",
            }
        ),
        encoding="utf-8",
    )
    loaded = load_plot_document(ws, plot.id)
    assert loaded.faults == []
