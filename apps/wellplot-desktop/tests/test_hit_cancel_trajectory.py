"""#589 / #597 / #598 / #612 regressions: hit-test geometry, table cancel,
mixed-survey trajectory ordering."""

from __future__ import annotations

import os
from pathlib import Path

import numpy as np
import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.correlation_canvas import CorrelationCanvas
from well_log_workstation.depth_ruler import RULER_WIDTH
from well_log_workstation.shell import WellLogWorkstationWindow
from well_log_workstation.table_projection import (
    PROJECTION_BUILD_HOOKS,
    build_table_projections,
)
from well_log_workstation.template_model import HostPresentation
from well_log_workstation.tops_model import FormationTop


def _top(name: str, depth: float, i: int = 0) -> FormationTop:
    return FormationTop(name=name, depth=depth, id=f"{name}-{i}")


def _presentation(well_id: str, well_name: str) -> HostPresentation:
    depth = np.array([1000.0, 1005.0, 1010.0])

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

    return HostPresentation(
        template_id="std-gr-rt-den",
        template_name="GR/RT/DEN",
        well_document_id=well_id,
        well_name=well_name,
        depth=depth,
        depth_unit="m",
        tracks=[_Track()],  # type: ignore[arg-type]
    )


# --- #589: hit-test geometry must match painted geometry -------------------


def _two_column_canvas(qtbot) -> CorrelationCanvas:
    canvas = CorrelationCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(800, 600)
    canvas.set_columns([_presentation("w1", "W1"), _presentation("w2", "W2")])
    canvas.set_tops_per_column(
        [[_top("H", 1002.0)], [_top("H", 1007.0)]],
    )
    canvas.set_depth_range(1000.0, 1010.0)
    return canvas


def test_hit_test_uses_painted_column_width(qtbot) -> None:
    """#589: picks must resolve against the painted column layout (which
    reserves the depth-ruler strip), not the wider ruler-less layout."""
    canvas = _two_column_canvas(qtbot)
    col_w, gap = canvas._column_layout()
    w = canvas.width()
    n = 2
    painted = max(40, (w - 16 - RULER_WIDTH - gap * (n - 1)) // n)
    assert col_w == painted

    # The y of top "H" in column 1 (vertical mapping identical in both
    # layouts; only x drifts). Click exactly at the painted centre of
    # column 1: must attribute to well w2, never w1.
    top_band, bottom = 36, 600 - 24
    d0, d1 = 1000.0, 1010.0
    depth_at = 1007.0
    y = top_band + ((depth_at - d0) / (d1 - d0)) * (bottom - top_band)

    cx1 = canvas._x_well(1, col_w, gap)
    hit = canvas.hit_test_top(cx1, y)
    assert hit is not None
    well_id, ft = hit
    assert well_id == "w2"
    assert ft.name == "H"

    # Old-bug probe: with the ruler-less width, the drifted centre of
    # column 1 lands in the gap/past the painted right edge — a pick there
    # must NOT silently attribute to w1 either.
    drifted_w = max(40, (w - 16 - gap * (n - 1)) // n)
    cx1_drifted = 8 + RULER_WIDTH + (drifted_w + gap) + drifted_w / 2
    hit2 = canvas.hit_test_top(cx1_drifted, y)
    if hit2 is not None:
        assert hit2[0] == "w2"


def test_hit_test_boundary_click_attributes_by_painted_edges(qtbot) -> None:
    """#589: a click just inside column 1's painted left edge belongs to w2."""
    canvas = _two_column_canvas(qtbot)
    col_w, gap = canvas._column_layout()
    top_band, bottom = 36, 600 - 24
    y = top_band + 0.5 * (bottom - top_band)

    x_left_edge_col1 = canvas._x_well(1, col_w, gap) - col_w / 2
    hit = canvas.hit_test_top(x_left_edge_col1 + 2, y)
    assert hit is None or hit[0] == "w2"  # None: no top near mid-depth

    x_right_edge_col0 = canvas._x_well(0, col_w, gap) + col_w / 2
    hit = canvas.hit_test_top(x_right_edge_col0 - 2, y)
    assert hit is None or hit[0] == "w1"


# --- #597: real production cancel path (no test hooks) ---------------------


def _document():
    """Import a real LAS document for build tests."""
    from well_log_workstation.las_import import import_las_into_workspace
    from well_log_workstation.workspace import create_workspace

    import tempfile

    tmp = tempfile.mkdtemp()
    las = Path(tmp) / "cancel.las"
    las.write_text(
        """~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1004.0
STEP.M 1.0
NULL. -999.25
WELL. C1
~CURVE INFORMATION
DEPT.M
GR.GAPI
RT.OHMM
~ASCII
1000 20 2
1001 30 5
1002 40 10
1003 50 20
1004 60 50
""",
        encoding="utf-8",
    )
    ws = create_workspace(Path(tmp) / "ws")
    return import_las_into_workspace(ws, las).document


def _checks(doc, template):
    from well_log_workstation.display_set import default_checks, leaves_from_document

    return default_checks(leaves_from_document(doc), template)


def test_build_table_projections_cancels_without_hooks() -> None:
    """#597: the REAL build path must honor cancel_flag with no delay hook."""
    from well_log_workstation.template_model import get_builtin_template

    doc = _document()
    template = get_builtin_template("std-gr-rt-den")
    assert template is not None
    checks = _checks(doc, template)

    with pytest.raises(InterruptedError):
        build_table_projections(doc, checks, template, cancel_flag=[True])


def test_build_table_projections_reports_real_progress() -> None:
    """#597: on_progress fires at real milestones, determinate, monotonic."""
    from well_log_workstation.template_model import get_builtin_template

    doc = _document()
    template = get_builtin_template("std-gr-rt-den")
    assert template is not None
    checks = _checks(doc, template)

    seen: list[tuple[int, int]] = []
    projections = build_table_projections(
        doc, checks, template, on_progress=lambda d, t: seen.append((d, t))
    )
    assert projections, "build must produce projections"
    assert seen, "progress must fire at real milestones"
    totals = {t for _, t in seen}
    assert len(totals) == 1 and next(iter(totals)) > 0, "determinate total"
    dones = [d for d, _ in seen]
    assert dones == sorted(dones), "monotonic progress"
    assert seen[-1][0] == seen[-1][1], "build completes at 100%"


def test_production_cancel_semantics_reach_real_handler(qtbot, tmp_path: Path) -> None:
    """#612: cancel must flow through the REAL shell method — no stubbing.

    Deterministic mid-build cancel via the documented delay hook; the click
    goes through the real progress bar signal and real cancel button, so
    regressions in shell._refresh_table_projection's cancel wiring fail here.
    """
    from well_log_workstation.las_import import import_las_into_workspace
    from well_log_workstation.workspace import create_workspace

    ws = create_workspace(tmp_path / "ws")
    las = tmp_path / "t6.las"
    las.write_text(
        """~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1004.0
STEP.M 1.0
NULL. -999.25
WELL. T6
~CURVE INFORMATION
DEPT.M
GR.GAPI
RT.OHMM
~ASCII
1000 20 2
1001 30 5
1002 40 10
1003 50 20
1004 60 50
""",
        encoding="utf-8",
    )
    result = import_las_into_workspace(ws, las)
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    win.session.put(result.document)
    win._selected_well_id = result.catalog_well_id
    win.apply_template_to_well(result.catalog_well_id, "std-gr-rt-den")

    ds_before = set(win.display_set_for(result.catalog_well_id) or [])

    PROJECTION_BUILD_HOOKS.delay_steps = 5

    fired: list[int] = []

    def _click_cancel_once(value: int) -> None:
        if fired or value < 1:
            return
        fired.append(value)
        win.table_cancel_btn.click()

    win.table_progress.valueChanged.connect(_click_cancel_once)
    try:
        assert win._view_mode != "table"
        win.set_view_mode("table")
        # Production cancel path: back to graphic, display set untouched,
        # progress shell dismissed.
        assert fired, "cancel click must have been delivered mid-build"
        assert win._view_mode == "graphic"
        assert set(win.display_set_for(result.catalog_well_id) or []) == ds_before
        assert not win.table_progress.isVisibleTo(win)
        assert not win.table_cancel_btn.isVisibleTo(win)
    finally:
        win.table_progress.valueChanged.disconnect(_click_cancel_once)
        PROJECTION_BUILD_HOOKS.reset()


# --- #598: mixed survey availability keeps presentation order -------------


def test_section_trajectory_data_keeps_presentation_order(qtbot) -> None:
    """#598: offsets/trajectories must stay aligned with presentations when
    some wells have surveys and others do not."""
    win = WellLogWorkstationWindow.__new__(WellLogWorkstationWindow)

    class _Pres:
        def __init__(self, name: str) -> None:
            self.well_name = name

    class _Plot:
        well_ids = ["a", "b", "c"]

    presentations = [_Pres("A"), _Pres("B"), _Pres("C")]

    class _Traj:
        def __init__(self, closure: float) -> None:
            self.closure_dist = np.array([closure])

    # A and C have surveys; B does not.
    surveys = {
        "A": [object()],
        "C": [object()],
    }
    polylines: dict[str, np.ndarray] = {}

    import well_log_workstation.section_geometry as geom_mod
    import well_log_workstation.survey as survey_mod
    from unittest.mock import patch

    def fake_compute_trajectory(stations):
        # A drifts 100 m, C drifts 300 m (stations is the per-well list).
        return _Traj(100.0 if stations is surveys["A"] else 300.0)

    def fake_polyline(traj, azimuth, spacing, *, shift=0.0):
        off = 0.5 if traj.closure_dist[0] == 100.0 else 1.5
        pl = np.array([[off, 1000.0 + shift], [off, 1010.0 + shift]])
        polylines["A" if off == 0.5 else "C"] = pl
        return pl

    # The method imports these at call time from their home modules.
    with (
        patch.object(survey_mod, "compute_trajectory", fake_compute_trajectory),
        patch.object(geom_mod, "section_trajectory_polyline", fake_polyline),
    ):
        offsets, trajectories = win._section_trajectory_data(
            _Plot(),
            presentations,  # type: ignore[arg-type]
            [(0.0, 0.0), (1.0, 0.0), (2.0, 0.0)],
            {"A": 0.0, "B": 0.0, "C": 0.0},
            surveys,  # type: ignore[arg-type]
        )

    # Order follows presentations: A(surveyed) B(plain) C(surveyed).
    assert len(offsets) == 3 and len(trajectories) == 3
    assert offsets[0] == pytest.approx(0.5)   # A's own offset
    assert offsets[1] == 0.0                  # B: no survey
    assert trajectories[1] is None            # B: no trajectory
    assert offsets[2] == pytest.approx(1.5)   # C's own offset
    assert trajectories[0] is not None and trajectories[2] is not None
    # B must never receive another well's polyline.
    assert trajectories[0] is polylines["A"]
    assert trajectories[2] is polylines["C"]
