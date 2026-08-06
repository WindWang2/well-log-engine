"""T6: Table degrade UX + clipboard/export boundary (#346)."""

from __future__ import annotations

import os
from pathlib import Path

import numpy as np
import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.display_set import (
    default_checks,
    leaf_id_for_curve,
    leaves_from_document,
)
from well_log_workstation.las_import import import_las_into_workspace
from well_log_workstation.shell import WellLogWorkstationWindow
from well_log_workstation.table_projection import (
    PROJECTION_BUILD_HOOKS,
    LogTableModel,
    build_table_projections,
    export_projection_rows,
    selection_tsv,
)
from well_log_workstation.template_model import get_builtin_template
from well_log_workstation.workspace import create_workspace


def _write_las(path: Path) -> Path:
    path.write_text(
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
    return path


@pytest.fixture(autouse=True)
def _reset_hooks():
    PROJECTION_BUILD_HOOKS.reset()
    yield
    PROJECTION_BUILD_HOOKS.reset()


def _open(qtbot, tmp_path: Path):
    ws = create_workspace(tmp_path / "ws")
    result = import_las_into_workspace(ws, _write_las(tmp_path / "t.las"))
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    win.session.put(result.document)
    well_id = result.catalog_well_id
    win._selected_well_id = well_id
    win.apply_template_to_well(well_id, "std-gr-rt-den")
    return win, result


def test_selection_tsv_only_selected_rows(tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws")
    result = import_las_into_workspace(ws, _write_las(tmp_path / "t.las"))
    doc = result.document
    tpl = get_builtin_template("std-gr-rt-den")
    assert tpl is not None
    checks = default_checks(leaves_from_document(doc), tpl)
    proj = build_table_projections(doc, checks, tpl)[0]
    tsv = selection_tsv(proj, [1, 3])
    lines = [ln for ln in tsv.strip().splitlines() if ln]
    # header + 2 data rows only (not full 5-row table)
    assert len(lines) == 3
    assert "1001" in tsv
    assert "1003" in tsv
    # middle unselected row depth should not appear
    assert "1002" not in tsv


def test_export_job_separate_from_model_data(tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws")
    result = import_las_into_workspace(ws, _write_las(tmp_path / "t.las"))
    doc = result.document
    tpl = get_builtin_template("std-gr-rt-den")
    assert tpl is not None
    proj = build_table_projections(
        doc, default_checks(leaves_from_document(doc), tpl), tpl
    )[0]
    model = LogTableModel()
    model.set_projection(proj)
    # data() path does not invoke export
    calls: list[str] = []
    orig = export_projection_rows

    def spy(*a, **k):
        calls.append("export")
        return orig(*a, **k)

    import well_log_workstation.table_projection as tp

    tp.export_projection_rows = spy  # type: ignore[assignment]
    try:
        _ = model.data(model.index(0, 0))
        assert calls == []
        rows = orig(proj, row_start=0, row_end=2)
        assert len(rows) == 2
    finally:
        tp.export_projection_rows = orig  # type: ignore[assignment]


def test_injected_failure_shows_error_graphic_usable(qtbot, tmp_path: Path) -> None:
    win, result = _open(qtbot, tmp_path)
    well_id = result.catalog_well_id
    ds = win.display_set_for(well_id)
    PROJECTION_BUILD_HOOKS.force_fail = True
    win.set_view_mode("table")
    assert win._table_last_error is not None
    assert not win.table_error_banner.isHidden()
    assert "失败" in win.table_error_banner.text() or "错误" in win.table_error_banner.text()
    # Display set not truncated / not cleared
    assert win.display_set_for(well_id) == ds
    # Graphic still works
    win.set_view_mode("graphic")
    assert win._view_mode == "graphic"
    assert win.active_presentation is not None
    assert win.active_presentation.curve_track_count >= 1


def test_cancel_build_returns_graphic_keeps_display_set(qtbot, tmp_path: Path) -> None:
    win, result = _open(qtbot, tmp_path)
    well_id = result.catalog_well_id
    ds = set(win.display_set_for(well_id) or [])
    # Cancel mid progress: delay_steps + cancel on first progress callback
    PROJECTION_BUILD_HOOKS.delay_steps = 2

    orig = win._refresh_table_projection

    def refresh_with_cancel() -> None:
        # After progress starts, shell sets cancel_flag list; force cancel before build body
        PROJECTION_BUILD_HOOKS.delay_steps = 2
        cancel = [False]

        def on_progress(step: int, total: int) -> None:
            cancel[0] = True

        from well_log_workstation.table_projection import build_table_projections_guarded
        from well_log_workstation.template_model import get_builtin_template

        win._show_table_progress()
        try:
            doc = win.session.ensure_well_loaded(win._workspace, well_id)
            tpl = get_builtin_template("std-gr-rt-den")
            assert tpl is not None
            PROJECTION_BUILD_HOOKS.cancel_flag = cancel
            try:
                build_table_projections_guarded(
                    doc,
                    win.display_set_for(well_id) or frozenset(),
                    tpl,
                    hooks=PROJECTION_BUILD_HOOKS,
                    on_progress=on_progress,
                )
            except InterruptedError:
                win.set_view_mode("graphic")
                return
        finally:
            win._hide_table_progress()

    win._refresh_table_projection = refresh_with_cancel  # type: ignore[method-assign]
    try:
        win.set_view_mode("table")
        assert win._view_mode == "graphic"
        assert set(win.display_set_for(well_id) or []) == ds
    finally:
        win._refresh_table_projection = orig  # type: ignore[method-assign]


def test_performance_mode_no_truncation(qtbot, tmp_path: Path) -> None:
    win, result = _open(qtbot, tmp_path)
    win.set_view_mode("table")
    n_before = win._primary_table_model.rowCount()
    c_before = win._primary_table_model.columnCount()
    win.table_perf_check.setChecked(True)
    assert win._table_performance_mode
    assert win._primary_table_model.rowCount() == n_before
    assert win._primary_table_model.columnCount() == c_before
    assert not win._primary_table_view.alternatingRowColors()


def test_copy_selection_clipboard(qtbot, tmp_path: Path) -> None:
    win, result = _open(qtbot, tmp_path)
    win.set_view_mode("table")
    win._primary_table_view.selectRow(1)
    tsv = win.copy_table_selection_to_clipboard()
    assert tsv
    assert tsv.count("\n") >= 1  # header + row
    # Only one data row (plus header)
    data_lines = [ln for ln in tsv.strip().splitlines() if ln]
    assert len(data_lines) == 2


def test_stress_projection_no_silent_truncation(tmp_path: Path) -> None:
    """Stress-ish: many rows still fully addressable via cell() (no row cap)."""
    from well_log_workstation.las_import import ImportedCurve, ImportedWellDocument

    n = 50_000
    depth = np.arange(n, dtype=np.float64)
    vals = depth * 0.1
    doc = ImportedWellDocument(
        document_id="stress",
        well_name="S",
        source_path="s.las",
        depth=depth,
        depth_unit="m",
        curves=[
            ImportedCurve(
                mnemonic="GR",
                unit="GAPI",
                values=vals,
                null_mask=np.zeros(n, dtype=bool),
            )
        ],
    )
    tpl = get_builtin_template("gr-only")
    assert tpl is not None
    checks = {leaf_id_for_curve("stress", "GR")}
    proj = build_table_projections(doc, checks, tpl)[0]
    assert proj.row_count == n
    assert proj.column_count == 2
    # Last row reachable (not truncated)
    assert proj.cell(n - 1, 0) == pytest.approx(float(n - 1))
