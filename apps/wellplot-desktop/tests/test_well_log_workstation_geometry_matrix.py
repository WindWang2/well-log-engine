"""B1.GEOM multi-format geometry matrix (#304 / export B1).

Expands T14 single-well golden across format dimensions toward §16 0.1 mm.
CGM full-scene clip may still use the 0.5 mm entry tol (ADR 0054).
"""

from __future__ import annotations

import os
import re
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.export_dispatch import PageSpec, export_plot
from well_log_workstation.geometry_golden import (
    B1_ASSERTED_RUN_MAP,
    B1_FORMAT_MATRIX,
    GOLDEN_PAGE_HEIGHT_MM,
    GOLDEN_PAGE_WIDTH_MM,
    GOLDEN_TEMPLATE_ID,
    TOL_MM,
    TOL_MM_CGM,
    assert_page_box_mm,
    asserted_matrix_rows,
    deferred_matrix_rows,
    expected_fixed_page_count,
    fixture_las_path,
    golden_export_layout,
    run_b1_geometry_matrix,
    scene_mm_to_cgm_vdc,
    cgm_vdc_to_scene_mm,
)
from well_log_workstation.plot_document import load_plot_document
from well_log_workstation.shell import WellLogWorkstationWindow
from well_log_workstation.workspace import create_workspace


def test_b1_format_matrix_is_documented() -> None:
    assert len(B1_FORMAT_MATRIX) >= 6
    statuses = {r.status for r in B1_FORMAT_MATRIX}
    assert "asserted" in statuses
    # At least one deferred row remains for honesty (full §16 not claimed).
    assert "deferred" in statuses
    assert statuses <= {"asserted", "entry", "deferred"}


def test_asserted_and_deferred_helpers() -> None:
    asserted = asserted_matrix_rows()
    deferred = deferred_matrix_rows()
    assert len(asserted) >= 1
    assert len(deferred) >= 1
    assert all(r.status == "asserted" for r in asserted)
    assert all(r.status == "deferred" for r in deferred)
    # Partition: helpers are disjoint subsets of the full matrix.
    assert {id(r) for r in asserted} | {id(r) for r in deferred} <= {
        id(r) for r in B1_FORMAT_MATRIX
    }
    assert not ({(r.format_id, r.metric) for r in asserted} & {
        (r.format_id, r.metric) for r in deferred
    })


def test_deferred_matrix_rows_non_empty() -> None:
    """B1 closeout must keep at least one honest deferred §16 gap."""
    rows = deferred_matrix_rows()
    assert len(rows) >= 1
    format_ids = {r.format_id for r in rows}
    assert "engine_pdf" in format_ids or "cgm_full_scene" in format_ids


def test_asserted_rows_map_to_runner_or_documented_live() -> None:
    """Every asserted matrix row has a pure-runner token or a documented None map.

    None means covered by a dedicated live/integration test (see
    ``B1_ASSERTED_RUN_MAP`` comments), not by ``run_b1_geometry_matrix``.
    """
    ran = set(run_b1_geometry_matrix())
    asserted = asserted_matrix_rows()
    assert asserted, "expected at least one asserted matrix row"

    map_keys = set(B1_ASSERTED_RUN_MAP)
    row_keys = {(r.format_id, r.metric) for r in asserted}
    assert row_keys == map_keys, (
        f"B1_ASSERTED_RUN_MAP keys must match asserted_matrix_rows exactly.\n"
        f"  only in matrix: {sorted(row_keys - map_keys)}\n"
        f"  only in map: {sorted(map_keys - row_keys)}"
    )

    for row in asserted:
        key = (row.format_id, row.metric)
        token = B1_ASSERTED_RUN_MAP[key]
        if token is None:
            # Documented external coverage (e.g. svg_viewbox live export).
            assert row.format_id == "svg_viewbox", (
                f"{key}: only svg_viewbox may use None mapping today"
            )
            continue
        assert token in ran, (
            f"asserted row {key} maps to run token {token!r}, "
            f"but run_b1_geometry_matrix returned {sorted(ran)}"
        )


def test_run_b1_geometry_matrix() -> None:
    ran = run_b1_geometry_matrix()
    assert "qt_paint_layout" in ran
    assert "qt_paint_layout_depth" in ran
    assert "cross_format" in ran
    assert "cgm_pagination" in ran
    assert "cgm_vdc_roundtrip" in ran
    assert "cgm_vdc_entry" in ran


def test_cgm_vdc_roundtrip_at_01mm() -> None:
    vx, vy = scene_mm_to_cgm_vdc(47.8, 68.0, window_height_mm=210.0)
    rx, ry = cgm_vdc_to_scene_mm(vx, vy, window_height_mm=210.0)
    assert abs(rx - 47.8) <= TOL_MM
    assert abs(ry - 68.0) <= TOL_MM


def test_pagination_page_count_formula() -> None:
    assert expected_fixed_page_count(100.0, 50.0) == 2
    assert expected_fixed_page_count(100.0, 40.0) == 3
    assert expected_fixed_page_count(100.0, 0.0) == 1
    assert expected_fixed_page_count(100.0, 50.0, page_overlap=0.2) == 3


def test_svg_page_box_in_matrix(qtbot, tmp_path: Path, monkeypatch) -> None:
    """SVG viewBox participates in the format matrix at 0.1 mm."""
    monkeypatch.setenv("WLWS_DISABLE_ENGINE", "1")
    from well_log_workstation.engine_bridge import reset_engine_capability_cache

    reset_engine_capability_cache()
    ws = create_workspace(tmp_path / "ws-m", name="Matrix")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(fixture_las_path())
    win.create_single_well_plot_document(well_id, GOLDEN_TEMPLATE_ID)
    plot_doc = load_plot_document(ws, win.active_plot_id)
    out = export_plot(
        plot_doc,
        "svg",
        backend="qt",
        page_spec=PageSpec(page_size="A4", orientation="landscape"),
        paint_fn=win._paint_active_plot,
        path=str(tmp_path / "m.svg"),
    )
    text = out.read_text(encoding="utf-8", errors="replace")
    m = re.search(
        r'viewBox\s*=\s*"([0-9.+\-eE]+)\s+([0-9.+\-eE]+)\s+'
        r'([0-9.+\-eE]+)\s+([0-9.+\-eE]+)"',
        text,
    )
    assert m is not None, text[:300]
    w, h = float(m.group(3)), float(m.group(4))
    assert_page_box_mm(w, h)
    assert w == pytest.approx(GOLDEN_PAGE_WIDTH_MM, abs=TOL_MM)
    assert h == pytest.approx(GOLDEN_PAGE_HEIGHT_MM, abs=TOL_MM)


def test_cgm_entry_tol_not_looser_than_half_mm() -> None:
    assert TOL_MM_CGM <= 0.5 + 1e-9
    assert TOL_MM <= TOL_MM_CGM
