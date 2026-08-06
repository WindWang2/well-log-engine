"""Trajectory-spaced section wells (FRS §3.1 / P1-C).

Covers:
* project_closure_to_section / normalize_offsets / section_trajectory_polyline;
* SectionCanvas unit_to_pixel (equal = legacy mapping; offsets interpolate
  between wells) + offsets/trajectories state + render smoke;
* plot-document well_spacing round-trip + invalid fallback + legacy default.
"""

from __future__ import annotations

import os
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import numpy as np
import pytest

from well_log_workstation.section_geometry import (
    normalize_offsets,
    project_closure_to_section,
    section_trajectory_polyline,
)
from well_log_workstation.survey import SurveyStation, compute_trajectory


def _deviated_north() -> np.ndarray:
    """45° build due north over 100 m (closure north > 0, east == 0)."""
    return compute_trajectory(
        [SurveyStation(0, 0, 0), SurveyStation(100, 45, 0)]
    )


# ---------------------------------------------------------------------------
# Geometry helpers
# ---------------------------------------------------------------------------


def test_project_closure_north_section() -> None:
    traj = _deviated_north()
    offsets = project_closure_to_section(traj, azimuth_deg=0.0)
    assert offsets[-1] == pytest.approx(traj.north[-1])
    assert offsets[-1] > 0.0


def test_project_closure_east_section() -> None:
    traj = _deviated_north()  # north closure, no east
    offsets = project_closure_to_section(traj, azimuth_deg=90.0)
    assert offsets[-1] == pytest.approx(0.0, abs=1e-9)


def test_project_closure_sign_flips_at_180() -> None:
    traj = _deviated_north()
    offsets = project_closure_to_section(traj, azimuth_deg=180.0)
    assert offsets[-1] == pytest.approx(-traj.north[-1])


def test_normalize_offsets() -> None:
    assert normalize_offsets(np.array([100.0, 50.0]), 200.0).tolist() == [0.5, 0.25]


def test_normalize_degenerate_spacing_zeros() -> None:
    assert normalize_offsets(np.array([10.0]), 0.0).tolist() == [0.0]
    assert normalize_offsets(np.array([10.0]), -5.0).tolist() == [0.0]


def test_section_trajectory_polyline_shape_and_shift() -> None:
    traj = _deviated_north()
    pl = section_trajectory_polyline(traj, 0.0, 200.0, shift=-10.0)
    assert pl.shape == (2, 2)
    # x = offset/spacing; y = md + shift.
    assert pl[-1, 0] == pytest.approx(traj.north[-1] / 200.0)
    assert pl[-1, 1] == pytest.approx(100.0 - 10.0)


def test_section_trajectory_polyline_empty() -> None:
    pl = section_trajectory_polyline(compute_trajectory([]), 0.0, 200.0)
    assert pl.shape == (0, 2)


# ---------------------------------------------------------------------------
# SectionCanvas unit_to_pixel + trajectory state
# ---------------------------------------------------------------------------


def test_unit_to_pixel_equal_spacing_is_legacy(qtbot) -> None:
    from well_log_workstation.section_canvas import SectionCanvas
    from well_log_workstation.template_model import HostPresentation

    canvas = SectionCanvas()
    qtbot.addWidget(canvas)
    depth = np.array([0.0, 1.0])
    pres = HostPresentation(
        template_id="t", template_name="T", well_document_id="w1",
        well_name="W1", depth=depth, depth_unit="m", tracks=[],
    )
    canvas.set_section([pres, pres, pres])
    col_w, gap = 80, 6
    # Legacy mapping: 8 + u*(col_w+gap) + col_w/2.
    assert canvas.unit_to_pixel(0, col_w, gap) == pytest.approx(8 + 0 + 40)
    assert canvas.unit_to_pixel(1, col_w, gap) == pytest.approx(8 + 86 + 40)
    assert canvas.unit_to_pixel(0.5, col_w, gap) == pytest.approx(8 + 43 + 40)


def test_unit_to_pixel_with_offsets_interpolates(qtbot) -> None:
    from well_log_workstation.section_canvas import SectionCanvas
    from well_log_workstation.template_model import HostPresentation

    canvas = SectionCanvas()
    qtbot.addWidget(canvas)
    depth = np.array([0.0, 1.0])
    pres = HostPresentation(
        template_id="t", template_name="T", well_document_id="w1",
        well_name="W1", depth=depth, depth_unit="m", tracks=[],
    )
    canvas.set_section([pres, pres, pres])
    canvas.set_well_x_offsets([0.0, 0.5, -0.25])
    col_w, gap = 80, 6
    stride = col_w + gap
    # Well 0: base(0u) + 0*stride. Well 1: base(1u) + 0.5*stride.
    assert canvas.unit_to_pixel(0, col_w, gap) == pytest.approx(8 + 0 * stride + 40)
    assert canvas.unit_to_pixel(1, col_w, gap) == pytest.approx(
        8 + 1 * stride + 40 + 0.5 * stride
    )
    # Midway 0→1: offset = 0.25 → base(0.5u) + 0.25*stride.
    assert canvas.unit_to_pixel(0.5, col_w, gap) == pytest.approx(
        8 + 0.5 * stride + 40 + 0.25 * stride
    )


def test_canvas_offsets_and_trajectories_state(qtbot) -> None:
    from well_log_workstation.section_canvas import SectionCanvas
    from well_log_workstation.template_model import HostPresentation

    canvas = SectionCanvas()
    qtbot.addWidget(canvas)
    depth = np.array([0.0, 1.0])
    pres = HostPresentation(
        template_id="t", template_name="T", well_document_id="w1",
        well_name="W1", depth=depth, depth_unit="m", tracks=[],
    )
    canvas.set_section([pres, pres])
    assert canvas.well_x_offsets() is None
    assert canvas.well_trajectories() is None

    traj = np.array([[0.0, 0.0], [0.4, 100.0]])
    canvas.set_well_x_offsets([0.0, 0.4])
    canvas.set_well_trajectories([traj, None])
    assert canvas.well_x_offsets() == [0.0, 0.4]
    assert canvas.well_trajectories()[0] is traj
    assert canvas.well_trajectories()[1] is None

    canvas.set_well_x_offsets(None)
    canvas.set_well_trajectories(None)
    assert canvas.well_x_offsets() is None


def test_canvas_paints_with_trajectories_no_crash(qtbot) -> None:
    from PySide6.QtGui import QImage

    from well_log_workstation.section_canvas import SectionCanvas
    from well_log_workstation.template_model import HostPresentation

    canvas = SectionCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 480)
    depth = np.array([0.0, 100.0, 200.0])

    class _Layer:
        color = "#1f77b4"
        values = np.array([10.0, 20.0, 30.0])
        null_mask = np.array([False, False, False])

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
    canvas.set_section([pres, pres])
    # Deviated well 1: offsets + trajectory polyline.
    traj = np.array([[0.0, 0.0], [0.3, 50.0], [0.6, 200.0]])
    canvas.set_well_x_offsets([0.0, 0.6])
    canvas.set_well_trajectories([None, traj])
    canvas.set_depth_range(0.0, 200.0)

    img = QImage(canvas.size(), QImage.Format.Format_ARGB32)
    img.fill(0)
    canvas.render(img)  # crash smoke


# ---------------------------------------------------------------------------
# Plot-document persistence
# ---------------------------------------------------------------------------


def test_well_spacing_round_trip(tmp_path: Path) -> None:
    from well_log_workstation.plot_document import (
        create_section_plot,
        load_plot_document,
        save_plot_document,
    )
    from well_log_workstation.workspace import add_well, create_workspace

    ws = create_workspace(tmp_path / "ws", name="Spacing")
    add_well(ws, name="W1", path="wells/w1.las")
    add_well(ws, name="W2", path="wells/w2.las")
    plot = create_section_plot(
        ws, well_ids=[ws.wells[0].id, ws.wells[1].id],
        template_id="gr-only", name="S",
    )
    assert plot.well_spacing == "equal"  # default

    loaded = load_plot_document(ws, plot.id)
    loaded.well_spacing = "geographic"
    save_plot_document(ws, loaded)

    again = load_plot_document(ws, plot.id)
    assert again.well_spacing == "geographic"


def test_well_spacing_invalid_falls_back(tmp_path: Path) -> None:
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
                "well_spacing": "bogus",
            }
        ),
        encoding="utf-8",
    )
    loaded = load_plot_document(ws, plot.id)
    assert loaded.well_spacing == "equal"


def test_legacy_section_json_defaults_equal(tmp_path: Path) -> None:
    import json

    from well_log_workstation.plot_document import load_plot_document
    from well_log_workstation.workspace import add_plot, create_workspace

    ws = create_workspace(tmp_path / "legacy2", name="Legacy2")
    plot = add_plot(
        ws, name="Legacy Section 2", plot_type="section",
        well_ids=["w1", "w2"], template_id="gr-only", path="plots/legacy2.json",
    )
    (ws.root / plot.path).write_text(
        json.dumps(
            {
                "schemaVersion": 7,
                "id": plot.id, "name": "Legacy Section 2", "type": "section",
                "well_ids": ["w1", "w2"], "template_id": "gr-only",
            }
        ),
        encoding="utf-8",
    )
    loaded = load_plot_document(ws, plot.id)
    assert loaded.well_spacing == "equal"
