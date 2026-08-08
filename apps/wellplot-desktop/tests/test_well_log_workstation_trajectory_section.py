"""Trajectory-spaced section wells (FRS §3.1 / P1-C) + Unfolded (沿 MD 展布).

Covers:
* project_closure_to_section / normalize_offsets / section_trajectory_polyline;
* path_segment_frame (Unfolded warp: position + segment direction at depth);
* SectionCanvas unit_to_pixel (equal = legacy mapping; offsets interpolate
  between wells) + offsets/trajectories state + render smoke; Unfolded state,
  datum-shift window fit and the pixel-diff warp render;
* plot-document well_spacing round-trip + invalid fallback + legacy default
  (equal / geographic / unfolded);
* shell wiring: section opens in unfolded / geographic with surveys under an
  md datum (surveys now load for spacing regardless of datum mode).
"""

from __future__ import annotations

import os
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import numpy as np
import pytest

from well_log_workstation.section_geometry import (
    normalize_offsets,
    path_segment_frame,
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
# path_segment_frame (Unfolded warp)
# ---------------------------------------------------------------------------


def test_path_segment_frame_interior_position_and_direction() -> None:
    path = np.array([[0.0, 0.0], [0.3, 100.0], [0.6, 200.0]])
    u, y, du, dy = path_segment_frame(path, np.array([50.0, 150.0]))
    np.testing.assert_allclose(u, [0.15, 0.45])
    np.testing.assert_allclose(y, [50.0, 150.0])
    # Directions are the containing-segment endpoint deltas.
    np.testing.assert_allclose(du, [0.3, 0.3])
    np.testing.assert_allclose(dy, [100.0, 100.0])


def test_path_segment_frame_nonuniform_segments() -> None:
    path = np.array([[0.0, 0.0], [0.3, 50.0], [0.6, 200.0]])
    u, y, du, dy = path_segment_frame(path, np.array([100.0]))
    # In the 50→200 segment: x = 0.3 + (100-50)/150 * 0.3 = 0.4.
    np.testing.assert_allclose(u, [0.4])
    np.testing.assert_allclose(y, [100.0])
    np.testing.assert_allclose(du, [0.3])
    np.testing.assert_allclose(dy, [150.0])


def test_path_segment_frame_clamps_endpoints() -> None:
    path = np.array([[0.0, 0.0], [0.3, 100.0]])
    u, y, du, dy = path_segment_frame(path, np.array([-50.0, 250.0]))
    np.testing.assert_allclose(u, [0.0, 0.3])
    np.testing.assert_allclose(y, [0.0, 100.0])
    # Both carry the single segment direction.
    np.testing.assert_allclose(du, [0.3, 0.3])
    np.testing.assert_allclose(dy, [100.0, 100.0])


def test_path_segment_frame_single_row_is_vertical() -> None:
    path = np.array([[0.2, 150.0]])
    u, y, du, dy = path_segment_frame(path, np.array([100.0, 200.0]))
    np.testing.assert_allclose(u, [0.2, 0.2])
    np.testing.assert_allclose(y, [150.0, 150.0])
    # Degenerate path: vertical direction → pure lateral normal.
    np.testing.assert_allclose(du, [0.0, 0.0])
    np.testing.assert_allclose(dy, [1.0, 1.0])


def test_path_segment_frame_empty() -> None:
    u, y, du, dy = path_segment_frame(np.empty((0, 2)), np.array([10.0]))
    assert u.size == 0
    assert y.size == 0


# ---------------------------------------------------------------------------
# SectionCanvas unit_to_pixel + trajectory state
# ---------------------------------------------------------------------------


def test_unit_to_pixel_equal_spacing_is_legacy(qtbot) -> None:
    from well_log_workstation.depth_ruler import RULER_WIDTH
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
    # Legacy mapping: 8 + RULER_WIDTH + u*(col_w+gap) + col_w/2 (ruler margin).
    assert canvas.unit_to_pixel(0, col_w, gap) == pytest.approx(8 + RULER_WIDTH + 0 + 40)
    assert canvas.unit_to_pixel(1, col_w, gap) == pytest.approx(8 + RULER_WIDTH + 86 + 40)
    assert canvas.unit_to_pixel(0.5, col_w, gap) == pytest.approx(8 + RULER_WIDTH + 43 + 40)


def test_unit_to_pixel_with_offsets_interpolates(qtbot) -> None:
    from well_log_workstation.depth_ruler import RULER_WIDTH
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
    assert canvas.unit_to_pixel(0, col_w, gap) == pytest.approx(8 + RULER_WIDTH + 0 * stride + 40)
    assert canvas.unit_to_pixel(1, col_w, gap) == pytest.approx(
        8 + RULER_WIDTH + 1 * stride + 40 + 0.5 * stride
    )
    # Midway 0→1: offset = 0.25 → base(0.5u) + 0.25*stride.
    assert canvas.unit_to_pixel(0.5, col_w, gap) == pytest.approx(
        8 + RULER_WIDTH + 0.5 * stride + 40 + 0.25 * stride
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


def test_canvas_unfolded_state_and_depth_shift_fit(qtbot) -> None:
    from well_log_workstation.section_canvas import SectionCanvas
    from well_log_workstation.template_model import HostPresentation

    canvas = SectionCanvas()
    qtbot.addWidget(canvas)
    depth = np.array([0.0, 100.0])
    pres = HostPresentation(
        template_id="t", template_name="T", well_document_id="w1",
        well_name="W1", depth=depth, depth_unit="m", tracks=[],
    )
    pres2 = HostPresentation(
        template_id="t", template_name="T", well_document_id="w2",
        well_name="W2", depth=depth, depth_unit="m", tracks=[],
    )
    canvas.set_section([pres, pres2])
    assert canvas.unfolded() is False
    assert canvas.depth_shifts() == {}

    canvas.set_unfolded(True)
    assert canvas.unfolded() is True
    canvas.set_depth_shifts({"w1": 50.0, "w2": -25.0})
    assert canvas.depth_shifts() == {"w1": 50.0, "w2": -25.0}
    # Unfolded fit includes the per-well datum shifts.
    assert canvas.depth_range() == pytest.approx((-25.0, 150.0))

    # Leaving unfolded mode refits to raw depths (shifts ignored).
    canvas.set_unfolded(False)
    assert canvas.depth_range() == pytest.approx((0.0, 100.0))
    canvas.set_depth_shifts(None)
    assert canvas.depth_shifts() == {}


def test_canvas_paints_unfolded_no_crash(qtbot) -> None:
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
    traj = np.array([[0.0, 0.0], [0.3, 50.0], [0.6, 200.0]])
    canvas.set_well_x_offsets([0.0, 0.6])
    canvas.set_well_trajectories([traj, None])
    canvas.set_depth_range(0.0, 200.0)
    canvas.set_unfolded(True)
    canvas.set_depth_shifts({"w1": 0.0, "w2": 0.0})

    img = QImage(canvas.size(), QImage.Format.Format_ARGB32)
    img.fill(0)
    canvas.render(img)  # crash smoke (warp path exercised)


def _render_canvas(canvas) -> "QImage":
    from PySide6.QtGui import QImage

    img = QImage(canvas.size(), QImage.Format.Format_ARGB32)
    img.fill(0xFFFFFFFF)
    canvas.render(img)
    return img


def _blueish_at(img, cx: int, cy: int, radius: int = 4) -> bool:
    """True when a curve-blue (#1f77b4-ish) pixel sits near (cx, cy).

    The strip borders (#ccc), trajectory dashes (#94a3b8) and footer text
    (#555) are all neutral enough that a blue-dominance test cannot misfire.
    """
    from PySide6.QtGui import QColor

    w, h = img.width(), img.height()
    for dy in range(-radius, radius + 1):
        for dx in range(-radius, radius + 1):
            x, y = int(cx) + dx, int(cy) + dy
            if 0 <= x < w and 0 <= y < h:
                c = QColor(img.pixel(x, y))
                if c.blue() > 120 and c.blue() > c.red() + 40:
                    return True
    return False


def test_unfolded_warps_curve_along_path(qtbot) -> None:
    """The curve bottom must follow the deviated wellbore, not the column.

    Well 1 has a straight 0.15-column-stride deviated path (its own offset
    interp adds +0.0225 stride at u=0.15). At the bottom depth (200 m) the
    value maps to the strip edge (+123 px lateral, col_w = 263): in equal
    mode the sample sits at the well-1 column (315, 456); in Unfolded mode
    it moves along the path normal to ≈(115, 470) — the differential proves
    the warp (a plain vertical strip would keep both samples in the column).
    """
    from well_log_workstation.section_canvas import SectionCanvas
    from well_log_workstation.template_model import HostPresentation

    canvas = SectionCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 480)
    depth = np.linspace(0.0, 200.0, 201)

    class _Layer:
        color = "#1f77b4"
        values = depth / 2.0  # t = v/100 runs 0 → 1 across the window
        null_mask = np.zeros_like(depth, dtype=bool)

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
    canvas.set_depth_range(0.0, 200.0)

    # Equal mode: curve bottom at the well-1 column (315, 456), not at the
    # unfolded warp point. Probe at (115, 456) — inside the strip, above the
    # footer text band (y >= 464), whose glyph rendering is font-environment
    # dependent and can carry colored (subpixel) fringes on some CI hosts.
    img_equal = _render_canvas(canvas)
    assert _blueish_at(img_equal, 315, 456)
    assert not _blueish_at(img_equal, 115, 456)

    # Unfolded: deviated path + lateral offsets; the bottom sample warps to
    # (115, 470) and the column bottom is left empty.
    traj = np.array([[0.0, 0.0], [0.15, 200.0]])
    canvas.set_well_x_offsets([0.0, 0.15])
    canvas.set_well_trajectories([traj, None])
    canvas.set_unfolded(True)
    canvas.set_depth_shifts({"w1": 0.0, "w2": 0.0})
    img_unfolded = _render_canvas(canvas)
    assert _blueish_at(img_unfolded, 115, 470)
    assert not _blueish_at(img_unfolded, 315, 456)


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
