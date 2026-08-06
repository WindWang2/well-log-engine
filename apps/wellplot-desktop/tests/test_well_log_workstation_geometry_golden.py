"""Single-well geometry golden subset (T14 / #302).

CI: picked up by ``tests/test_well_log_workstation_*.py`` in the workstation host job.
"""

from __future__ import annotations

import os
import re
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.export_dispatch import PageSpec, export_plot
from well_log_workstation.geometry_golden import (
    GOLDEN_DEPTH_BOTTOM,
    GOLDEN_DEPTH_TOP,
    GOLDEN_PAGE_HEIGHT_MM,
    GOLDEN_PAGE_WIDTH_MM,
    GOLDEN_TEMPLATE_ID,
    GOLDEN_TRACK_FRACTIONS,
    GOLDEN_WELL_NAME,
    GeometryGoldenError,
    TOL_MM,
    TOL_MM_CGM,
    assert_cgm_track_left_vdc,
    assert_depth_mapping,
    assert_layout_matches_golden,
    assert_within_tol,
    fixture_las_path,
    golden_export_layout,
    layout_export_tracks_mm,
    scene_mm_to_cgm_vdc,
)
from well_log_workstation.plot_document import load_plot_document
from well_log_workstation.shell import WellLogWorkstationWindow
from well_log_workstation.workspace import create_workspace


def test_fixture_las_in_repo() -> None:
    path = fixture_las_path()
    assert path.is_file(), f"golden LAS missing: {path}"
    text = path.read_text(encoding="utf-8")
    assert GOLDEN_WELL_NAME in text
    assert "GR" in text and "RT" in text and "RHOB" in text


def test_golden_layout_self_consistent() -> None:
    layout = golden_export_layout()
    assert_layout_matches_golden(layout)
    assert_depth_mapping(layout)


def test_layout_diagnostic_is_readable() -> None:
    layout = golden_export_layout()
    # Force a mismatch by swapping expected left for depth
    with pytest.raises(GeometryGoldenError) as ei:
        assert_layout_matches_golden(
            layout,
            expected_left={"depth": 0.0, "gr": 47.8, "rt": 122.0, "den": 196.2},
        )
    msg = str(ei.value)
    assert "T14 geometry golden mismatch" in msg
    assert "depth" in msg
    assert "tol=" in msg


def test_presentation_fractions_match_golden(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws-g", name="T14")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(fixture_las_path())
    win.create_single_well_plot_document(well_id, GOLDEN_TEMPLATE_ID)
    pres = win.active_presentation
    assert pres is not None
    assert pres.track_count == len(GOLDEN_TRACK_FRACTIONS)

    for track in pres.tracks:
        exp = GOLDEN_TRACK_FRACTIONS.get(track.id)
        assert exp is not None, f"unexpected track id {track.id!r}"
        assert track.width_fraction == pytest.approx(exp, abs=1e-9), (
            f"track {track.id}: width_fraction {track.width_fraction} != {exp}"
        )

    # Depth span from presentation must match fixture
    import numpy as np

    depth = np.asarray(pres.depth, dtype=float)
    assert float(np.nanmin(depth)) == pytest.approx(GOLDEN_DEPTH_TOP)
    assert float(np.nanmax(depth)) == pytest.approx(GOLDEN_DEPTH_BOTTOM)


def test_export_layout_from_presentation_within_tol(qtbot, tmp_path: Path) -> None:
    """Live presentation → export mm layout matches frozen golden (0.1 mm)."""
    ws = create_workspace(tmp_path / "ws-g2", name="T14")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(fixture_las_path())
    win.create_single_well_plot_document(well_id, GOLDEN_TEMPLATE_ID)
    pres = win.active_presentation
    assert pres is not None

    tracks = [(t.id, float(t.width_fraction)) for t in pres.visible_tracks]
    n_lines = max(1, len(pres.header.header_lines(
        well_name=pres.well_name,
        template_name=pres.template_name,
        depth_unit=pres.depth_unit or "m",
        scale_summary=pres.scale_summary(),
    ))) if getattr(pres, "header", None) is not None else 2

    layout = layout_export_tracks_mm(
        tracks,
        page_width_mm=GOLDEN_PAGE_WIDTH_MM,
        page_height_mm=GOLDEN_PAGE_HEIGHT_MM,
        n_header_lines=n_lines,
        depth_top=GOLDEN_DEPTH_TOP,
        depth_bottom=GOLDEN_DEPTH_BOTTOM,
    )
    assert_layout_matches_golden(layout)
    assert_depth_mapping(layout)


def test_qt_paint_svg_page_box_mm(qtbot, tmp_path: Path, monkeypatch) -> None:
    """Qt-paint SVG viewBox page box matches PageSpec mm (export geometry seam)."""
    monkeypatch.setenv("WLWS_DISABLE_ENGINE", "1")
    from well_log_workstation.engine_bridge import reset_engine_capability_cache

    reset_engine_capability_cache()

    ws = create_workspace(tmp_path / "ws-svg", name="T14")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(fixture_las_path())
    win.create_single_well_plot_document(well_id, GOLDEN_TEMPLATE_ID)
    plot_doc = load_plot_document(ws, win.active_plot_id)
    assert plot_doc is not None

    out = export_plot(
        plot_doc,
        "svg",
        backend="qt",
        page_spec=PageSpec(page_size="A4", orientation="landscape"),
        paint_fn=win._paint_active_plot,
        path=str(tmp_path / "golden.svg"),
    )
    assert out.is_file() and out.stat().st_size >= 50
    text = out.read_text(encoding="utf-8", errors="replace")

    # QSvgGenerator viewBox is set to page mm in export_dispatch._qt_paint_export
    # viewBox="0 0 297 210" or with decimals / viewBox attribute variants
    m = re.search(
        r'viewBox\s*=\s*"([0-9.+\-eE]+)\s+([0-9.+\-eE]+)\s+'
        r'([0-9.+\-eE]+)\s+([0-9.+\-eE]+)"',
        text,
    )
    assert m is not None, (
        "SVG missing viewBox; cannot verify page box mm.\n"
        f"head={text[:400]!r}"
    )
    x, y, vw, vh = (float(m.group(i)) for i in range(1, 5))
    assert_within_tol(x, 0.0, label="svg viewBox x")
    assert_within_tol(y, 0.0, label="svg viewBox y")
    assert_within_tol(vw, GOLDEN_PAGE_WIDTH_MM, label="svg viewBox width_mm")
    assert_within_tol(vh, GOLDEN_PAGE_HEIGHT_MM, label="svg viewBox height_mm")


def test_tol_constant_is_section_16_target() -> None:
    assert TOL_MM == pytest.approx(0.1)


def test_cgm_format_dimension_vdc_golden() -> None:
    """B1.CGM.3 / ADR 0054: CGM VDC track edges within 0.5 mm entry tol."""
    assert TOL_MM_CGM == pytest.approx(0.5)
    layout = golden_export_layout()
    assert_cgm_track_left_vdc(layout)
    # Spot-check transform: top of page → high VDC y
    _vx, vy = scene_mm_to_cgm_vdc(0.0, 0.0, window_height_mm=210.0)
    assert vy == 21000
