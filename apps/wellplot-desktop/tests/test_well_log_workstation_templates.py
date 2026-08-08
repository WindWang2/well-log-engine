"""Multi-track template library + apply (#219)."""

from __future__ import annotations

import os
from pathlib import Path

import numpy as np
import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.las_import import import_las_into_workspace
from well_log_workstation.shell import WellLogWorkstationWindow
from well_log_workstation.template_model import (
    HostPresentation,
    apply_template,
    get_builtin_template,
    list_builtin_templates,
)
from well_log_workstation.workspace import create_workspace


def _write_las(path: Path) -> Path:
    path.write_text(
        """~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1005.0
STEP.M 1.0
NULL. -999.25
WELL. MULTI-1
~CURVE INFORMATION
DEPT.M
GR.GAPI
RT.OHMM
RHOB.G/C3
~ASCII
1000 20 2 2.2
1001 30 5 2.3
1002 40 10 2.4
1003 50 20 2.5
1004 60 50 2.6
1005 70 100 2.7
""",
        encoding="utf-8",
    )
    return path


def test_builtin_library_has_multi_track() -> None:
    templates = list_builtin_templates()
    assert len(templates) >= 1
    std = get_builtin_template("std-gr-rt-den")
    assert std is not None
    assert len(std.tracks) >= 2
    roles = [t.get("role") for t in std.tracks]
    assert "depth" in roles
    assert roles.count("curve") >= 1


def test_apply_template_binds_multiple_tracks(tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws")
    las = _write_las(tmp_path / "m.las")
    result = import_las_into_workspace(ws, las)
    template = get_builtin_template("std-gr-rt-den")
    assert template is not None
    pres = apply_template(template, result.document)
    assert pres.track_count >= 2
    assert pres.curve_track_count >= 1
    # depth + at least one bound curve track
    assert any(t.role == "depth" for t in pres.tracks)
    bound_layers = sum(len(t.layers) for t in pres.tracks if t.role == "curve")
    assert bound_layers >= 1


def test_default_template_is_std_multi_track(qtbot) -> None:
    """UI must default to GR-RT-DEN, not alphabetically first gr-only."""
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    tid = win._current_template_id()
    assert tid == "std-gr-rt-den", (
        f"default template should be std-gr-rt-den, got {tid!r} "
        "(gr-only only shows GR and confuses multi-curve LAS users)"
    )


def test_apply_std_template_binds_gr_rt_den_from_rich_las(tmp_path: Path) -> None:
    """Realistic multi-curve LAS (like data/井曲线) binds three tracks."""
    las = tmp_path / "A2like.las"
    las.write_text(
        """~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1004.0
STEP.M 1.0
NULL. -999.25
WELL. A2
~CURVE INFORMATION
DEPT.M
AC.
GR.GAPI
RT.OHMM
DEN.G/C3
CNL.
~ASCII
1000 200 30 5 2.2 20
1001 210 40 10 2.3 22
1002 220 50 20 2.4 24
1003 230 60 50 2.5 26
1004 240 70 100 2.6 28
""",
        encoding="utf-8",
    )
    from well_log_workstation.las_import import parse_las_file

    doc = parse_las_file(las)
    assert len(doc.curves) >= 4
    template = get_builtin_template("std-gr-rt-den")
    assert template is not None
    pres = apply_template(template, doc)
    bound = {
        tr.id: [ly.mnemonic for ly in tr.layers]
        for tr in pres.tracks
        if tr.role == "curve"
    }
    assert bound.get("gr") == ["GR"]
    assert bound.get("rt") == ["RT"]
    assert bound.get("den") == ["DEN"]
    assert pres.curve_track_count >= 3


def test_shell_apply_shows_multi_track_canvas(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ui", name="Tpl")
    las = _write_las(tmp_path / "u.las")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(las)
    pres = win.apply_template_to_well(well_id, "std-gr-rt-den")
    assert pres.track_count >= 2
    assert win.multi_track_canvas.track_count() >= 2
    assert win.active_presentation is not None
    assert win.multi_track_canvas.presentation() is not None


def test_apply_template_preserves_per_curve_depth() -> None:
    """Multi-rate (Epic A): a curve with its own sampling axis binds the
    layer's depth; shared-axis curves keep depth=None."""
    from well_log_workstation.las_import import (
        ImportedCurve,
        ImportedWellDocument,
    )

    doc = ImportedWellDocument(
        document_id="aaaaaaaa-0000-4000-8000-000000000001",
        well_name="MR",
        source_path="",
        depth=np.array([1000.0, 1000.5, 1001.0]),
        depth_unit="m",
        curves=[
            ImportedCurve(
                mnemonic="GR", unit="API",
                values=np.array([1.0, 2.0, 3.0]),
                null_mask=np.zeros(3, dtype=bool),
            ),
            ImportedCurve(
                mnemonic="RT", unit="OHMM",
                values=np.array([10.0, 20.0]),
                null_mask=np.zeros(2, dtype=bool),
                depth=np.array([1000.1, 1000.6]),
            ),
        ],
    )
    template = get_builtin_template("std-gr-rt-den")
    assert template is not None
    pres = apply_template(template, doc)
    by_mnemonic: dict[str, object] = {}
    for t in pres.tracks:
        for layer in t.layers:
            by_mnemonic[layer.mnemonic] = layer
    gr_layer = by_mnemonic["GR"]
    rt_layer = by_mnemonic["RT"]
    assert gr_layer.depth is None, "shared-axis curve must bind no depth"
    assert rt_layer.depth is not None, "per-curve-axis curve must bind depth"
    np.testing.assert_allclose(rt_layer.depth, [1000.1, 1000.6])


def test_canvas_paints_per_curve_depth_layer(qtbot) -> None:
    """Multi-rate (Epic A): the shared window covers per-layer axes and the
    layer paints against its own depth (render smoke)."""
    from PySide6.QtGui import QImage

    from well_log_workstation.multi_track_canvas import MultiTrackCanvas
    from well_log_workstation.template_model import BoundCurveLayer, BoundTrack

    canvas = MultiTrackCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 480)
    depth = np.linspace(1000.0, 2000.0, 101)
    per_curve_depth = np.linspace(1000.5, 1999.5, 50)
    pres = HostPresentation(
        template_id="t", template_name="T", well_document_id="w1",
        well_name="W1", depth=depth, depth_unit="m",
        tracks=[
            BoundTrack(
                id="d", role="depth", title="深度",
                width_fraction=0.12, scale=None, layers=[],
            ),
            BoundTrack(
                id="c", role="curve", title="GR",
                width_fraction=0.25, scale=None,
                layers=[
                    BoundCurveLayer(
                        mnemonic="GR", color="#1a6fb5", unit="API",
                        values=np.linspace(10.0, 90.0, 50),
                        null_mask=np.zeros(50, dtype=bool),
                        depth=per_curve_depth,
                    ),
                ],
            ),
        ],
    )
    canvas.set_presentation(pres)
    # The shared window covers the union of both axes.
    d0, d1 = canvas.depth_range()
    assert d0 == pytest.approx(1000.0)
    assert d1 == pytest.approx(2000.0)
    img = QImage(canvas.size(), QImage.Format.Format_ARGB32)
    img.fill(0xFFFFFFFF)
    canvas.render(img)  # crash smoke with a per-layer axis


def test_canvas_secondary_axis_ruler(qtbot) -> None:
    """Epic B 多轴: a bound secondary axis shrinks the plot width and paints
    a right-margin ruler; unbound keeps the historic full-width layout."""
    from PySide6.QtGui import QImage

    from well_log_workstation.depth_ruler import RULER_WIDTH
    from well_log_workstation.multi_track_canvas import MultiTrackCanvas
    from well_log_workstation.template_model import BoundCurveLayer, BoundTrack

    depth = np.linspace(1000.0, 2000.0, 101)
    pres = HostPresentation(
        template_id="t", template_name="T", well_document_id="w1",
        well_name="W1", depth=depth, depth_unit="m",
        tracks=[
            BoundTrack(id="c", role="curve", title="GR",
                       width_fraction=0.5, scale=None,
                       layers=[BoundCurveLayer(
                           mnemonic="GR", color="#1a6fb5", unit="API",
                           values=np.linspace(10.0, 90.0, 101),
                           null_mask=np.zeros(101, dtype=bool))]),
        ],
    )
    canvas = MultiTrackCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 480)
    canvas.set_presentation(pres)
    assert canvas.secondary_depth_axis() is None
    assert canvas._plot_width() == 600  # historic full width

    # kb=500 vertical well: tvdss = 500 − md.
    points = [(500.0 - 1000.0, 1000.0), (500.0 - 1500.0, 1500.0),
              (500.0 - 2000.0, 2000.0)]
    canvas.set_secondary_depth_axis(points, "TVDSS (m)")
    assert canvas._plot_width() == 600 - RULER_WIDTH
    assert canvas.secondary_depth_axis() is not None

    img = QImage(canvas.size(), QImage.Format.Format_ARGB32)
    img.fill(0xFFFFFFFF)
    canvas.render(img)  # crash smoke with the secondary ruler
    # The right margin strip carries ruler content (ticks/labels).
    from PySide6.QtGui import QColor

    dark = False
    for y in range(60, 420, 8):
        for x in range(600 - RULER_WIDTH + 2, 600 - 2):
            c = QColor(img.pixel(x, y))
            if c.red() < 120 and c.green() < 120 and c.blue() < 120:
                dark = True
                break
        if dark:
            break
    assert dark, "the secondary ruler must paint ticks in the right margin"
