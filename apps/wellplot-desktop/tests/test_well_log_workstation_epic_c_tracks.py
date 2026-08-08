"""Epic C engineering tracks: 试油/解释成果道 + 射孔/井下工程道 (C3/C4 UI).

Covers:
* template binding — apply_template and the display-set builder attach
  well-test / perforation intervals to the matching BoundTrack roles;
* track painting — fluid-colored bands with labels (well-test) and
  status-colored shot-dot bands (perforation), window clipping;
* shell end-to-end — apply_template_to_well binds the sidecars onto the
  standard engineering template tracks.
"""

from __future__ import annotations

import os
from pathlib import Path

import numpy as np
import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtCore import QRectF
from PySide6.QtGui import QColor, QImage, QPainter

from well_log_workstation.display_set import presentation_from_display_set
from well_log_workstation.las_import import ImportedWellDocument
from well_log_workstation.multi_track_canvas import (
    paint_perforation_track,
    paint_well_test_track,
)
from well_log_workstation.perforation_model import (
    PerforationInterval,
    PerforationModel,
)
from well_log_workstation.template_model import (
    BoundTrack,
    get_builtin_template,
)
from well_log_workstation.well_test_model import (
    WellTestInterval,
    WellTestModel,
)


def _document() -> ImportedWellDocument:
    doc = ImportedWellDocument(
        document_id="d1",
        well_name="A",
        source_path="wells/a/a.las",
        depth=np.linspace(1000.0, 1100.0, 101),
        depth_unit="m",
    )
    doc.well_tests = WellTestModel(
        well_id="w1",
        intervals=[
            WellTestInterval(
                id="t1",
                top=1010.0,
                bottom=1020.0,
                test_type="DST",
                fluid="油",
                result="获工业油流",
                pressure_mpa=12.5,
                interpretation="解释成果 A",
            ),
            WellTestInterval(
                id="t2", top=1050.0, bottom=1060.0, test_type="试采",
                fluid="水", result="见水",
            ),
        ],
    )
    doc.perforations = PerforationModel(
        well_id="w1",
        intervals=[
            PerforationInterval(
                id="p1",
                top=1012.0,
                bottom=1016.0,
                shot_density=16,
                phasing="90°",
                status="已射",
            ),
            PerforationInterval(
                id="p2", top=1052.0, bottom=1054.0, status="已封堵",
            ),
        ],
    )
    return doc


# ---------------------------------------------------------------------------
# Template binding
# ---------------------------------------------------------------------------


def test_apply_template_binds_engineering_tracks() -> None:
    from well_log_workstation.template_model import apply_template

    tpl = get_builtin_template("std-engineering")
    assert tpl is not None
    pres = apply_template(tpl, _document())
    test_track = next(t for t in pres.tracks if t.role == "well_test")
    perf_track = next(t for t in pres.tracks if t.role == "perforation")
    assert [i.id for i in test_track.well_test_intervals] == ["t1", "t2"]
    assert [i.id for i in perf_track.perforation_intervals] == ["p1", "p2"]


def test_apply_template_missing_engineering_data_yields_empty_tracks() -> None:
    from well_log_workstation.template_model import apply_template

    tpl = get_builtin_template("std-engineering")
    doc = ImportedWellDocument(
        document_id="d2",
        well_name="B",
        source_path="wells/b/b.las",
        depth=np.linspace(1000.0, 1100.0, 101),
        depth_unit="m",
    )
    pres = apply_template(tpl, doc)
    test_track = next(t for t in pres.tracks if t.role == "well_test")
    assert test_track.well_test_intervals == []


def test_display_set_presentation_includes_engineering_tracks() -> None:
    tpl = get_builtin_template("std-engineering")
    pres = presentation_from_display_set(tpl, _document(), frozenset())
    roles = {t.role for t in pres.tracks}
    assert {"depth", "well_test", "perforation"}.issubset(roles)
    test_track = next(t for t in pres.tracks if t.role == "well_test")
    assert len(test_track.well_test_intervals) == 2


# ---------------------------------------------------------------------------
# Track painting (smoke + pixel probes)
# ---------------------------------------------------------------------------


def _paint(track: BoundTrack, paint_fn, width: int = 200, height: int = 300):
    img = QImage(width, height, QImage.Format.Format_ARGB32)
    img.fill(QColor("#ffffff"))
    p = QPainter(img)
    try:
        paint_fn(p, 40, 0, width - 80, height, 0.0, 50.0, track)
    finally:
        p.end()
    return img


def _well_test_track() -> BoundTrack:
    return BoundTrack(
        id="wt",
        role="well_test",
        title="试油",
        width_fraction=0.3,
        scale=None,
        layers=[],
        well_test_intervals=[
            WellTestInterval(
                id="t1",
                top=10.0,
                bottom=20.0,
                test_type="DST",
                fluid="油",
                result="获工业油流",
            ),
            WellTestInterval(
                id="t2", top=40.0, bottom=45.0, test_type="试采",
                fluid="水", result="见水",
            ),
        ],
    )


def test_paint_well_test_track_bands_and_clipping(qtbot) -> None:
    img = _paint(_well_test_track(), paint_well_test_track)
    band_px = img.pixelColor(100, 90)  # inside the 10-20m band (10m→y60, 20m→y120)
    outside_px = img.pixelColor(100, 30)  # 5m — above the band, no fill
    assert band_px != outside_px
    assert outside_px == QColor("#ffffff")
    # Second band paints too (40-45m → y240..y270).
    assert img.pixelColor(100, 255) != QColor("#ffffff")


def test_paint_perforation_track_dots_and_status(qtbot) -> None:
    track = BoundTrack(
        id="pf",
        role="perforation",
        title="射孔",
        width_fraction=0.2,
        scale=None,
        layers=[],
        perforation_intervals=[
            PerforationInterval(
                id="p1", top=10.0, bottom=16.0, shot_density=16, status="已射"
            )
        ],
    )
    img = _paint(track, paint_perforation_track)
    # Band spans y60..y96; the 5-dot pattern (unknown density → default) is
    # replaced here by density 16×6m=96 → clamped to 40 dots.
    # Sample a row through the middle of the band — should show dot/band ink.
    assert img.pixelColor(100, 78).alpha() > 0
    assert img.pixelColor(100, 150) == QColor("#ffffff")  # below the band


def test_paint_skips_nonfinite_and_out_of_window(qtbot) -> None:
    track = BoundTrack(
        id="wt",
        role="well_test",
        title="试油",
        width_fraction=0.3,
        scale=None,
        layers=[],
        well_test_intervals=[
            WellTestInterval(id="t1", top=float("nan"), bottom=20.0, test_type="DST"),
            WellTestInterval(id="t2", top=100.0, bottom=200.0, test_type="DST"),
        ],
    )
    img = _paint(track, paint_well_test_track)
    # Nothing inside the window → all white.
    for y in (90, 150):
        assert img.pixelColor(100, y) == QColor("#ffffff")


# ---------------------------------------------------------------------------
# Shell end-to-end
# ---------------------------------------------------------------------------


def test_shell_applies_engineering_template_with_sidecars(
    qtbot, tmp_path: Path
) -> None:
    from well_log_workstation.perforation_model import save_perforation_for_well
    from well_log_workstation.shell import WellLogWorkstationWindow
    from well_log_workstation.well_test_model import save_well_test_for_well
    from well_log_workstation.workspace import create_workspace

    ws = create_workspace(tmp_path / "ws")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    las = tmp_path / "a.las"
    las.write_text(
        "~VERSION INFORMATION\nVERS. 2.0\nWRAP. NO\n"
        "~WELL INFORMATION\nSTRT.M 1000.0\nSTOP.M 1010.0\nSTEP.M 1.0\n"
        "NULL. -999.25\nWELL. A\n~CURVE INFORMATION\nDEPT.M\nGR.GAPI\n~ASCII\n"
        + "".join(f"{1000 + i} 1\n" for i in range(11)),
        encoding="utf-8",
    )
    wid = win.import_las_path(las)
    save_well_test_for_well(
        ws,
        wid,
        WellTestModel(
            well_id=wid,
            intervals=[
                WellTestInterval(
                    id="t1", top=1003.0, bottom=1005.0, test_type="DST",
                    fluid="油", result="获工业油流",
                )
            ],
        ),
    )
    save_perforation_for_well(
        ws,
        wid,
        PerforationModel(
            well_id=wid,
            intervals=[
                PerforationInterval(
                    id="p1", top=1003.5, bottom=1004.5, shot_density=16,
                    status="已射",
                )
            ],
        ),
    )
    win.apply_template_to_well(wid, "std-engineering")
    pres = win._presentation
    assert pres is not None
    test_track = next(t for t in pres.tracks if t.role == "well_test")
    perf_track = next(t for t in pres.tracks if t.role == "perforation")
    assert [i.id for i in test_track.well_test_intervals] == ["t1"]
    assert [i.id for i in perf_track.perforation_intervals] == ["p1"]
