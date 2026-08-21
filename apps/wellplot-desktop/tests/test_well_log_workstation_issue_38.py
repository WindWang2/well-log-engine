"""Regression for issue #38 desktop batch (1/2/5).

Covers workspace-switch state cleanup, multi-axis table tab linkage with
index-axis skip, and export atomic-write tmp cleanup. Engine thread-guard
(#38-3) and error fallback (#38-4) are verified by C++ build + ctest.
"""

from __future__ import annotations

import os
from pathlib import Path

import numpy as np
import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.display_set import leaf_id_for_curve
from well_log_workstation.las_import import ImportedCurve, ImportedWellDocument
from well_log_workstation.shell import WellLogWorkstationWindow
from well_log_workstation.table_projection import build_table_projections
from well_log_workstation.template_model import get_builtin_template
from well_log_workstation.workspace import create_workspace


def _write_min_las(path: Path, *, well: str = "W1") -> Path:
    path.write_text(
        f"""~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1004.0
STEP.M 1.0
NULL. -999.25
WELL. {well}
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


# ---------------------------------------------------------------------------
# #38-1: workspace switch must clear per-workspace state
# ---------------------------------------------------------------------------


def test_workspace_switch_clears_state(qtbot, tmp_path: Path) -> None:
    """Open A (import + settings) -> set_workspace(B) clears session etc.

    Previously only None->ws triggered cleanup; wsA->wsB leaked documents.
    """
    from well_log_workstation.las_import import import_las_into_workspace

    ws_a = create_workspace(tmp_path / "ws-a", name="A")
    ws_b = create_workspace(tmp_path / "ws-b", name="B")

    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws_a)
    r = import_las_into_workspace(ws_a, _write_min_las(tmp_path / "a.las", well="A-1"))
    win.session.put(r.document)
    win._selected_well_id = r.catalog_well_id
    win.apply_template_to_well(r.catalog_well_id, "std-gr-rt-den")
    assert win.session.document_ids() != []
    assert win._selected_well_id is not None
    assert win.active_presentation is not None
    # Populate display set / view mode to ensure they are cleared too
    assert win._display_sets, "display set should be seeded by apply_template"
    assert win._view_modes or win._view_mode == "graphic"

    # Switch to B — must wipe A state
    win.set_workspace(ws_b)

    assert win.session.document_ids() == []
    assert win._selected_well_id is None
    assert win.active_presentation is None
    assert win._display_sets == {}
    # View mode reset to graphic + presentation cleared on canvas
    assert win._view_mode == "graphic"
    # Table model cleared
    if hasattr(win, "_primary_table_model"):
        assert win._primary_table_model.projection() is None


# ---------------------------------------------------------------------------
# #38-2: secondary axis tab selection -> semantic_selection
# ---------------------------------------------------------------------------


def _make_dual_axis_doc(well_id: str) -> ImportedWellDocument:
    """Doc with raw GR on shared MD + resampled GR on its own axis.

    Produces two table projections: md (5 rows) and curve-GR (3 rows, real depth).
    """
    depth = np.arange(1000.0, 1005.0, dtype=np.float64)  # 5
    raw_vals = np.array([10.0, 20.0, 30.0, 40.0, 50.0], dtype=np.float64)
    res_depth = np.array([1000.0, 1002.0, 1004.0], dtype=np.float64)
    res_vals = np.array([1.0, 2.0, 3.0], dtype=np.float64)
    doc = ImportedWellDocument(
        document_id=well_id,
        well_name="Dual",
        source_path=f"wells/{well_id}.las",
        depth=depth,
        depth_unit="m",
        curves=[
            ImportedCurve(
                mnemonic="GR",
                unit="GAPI",
                values=raw_vals,
                null_mask=np.zeros(5, dtype=bool),
                version="raw",
            ),
            ImportedCurve(
                mnemonic="GR",
                unit="GAPI",
                values=res_vals,
                null_mask=np.zeros(3, dtype=bool),
                depth=res_depth,
                version="resample-0.5m",
            ),
        ],
    )
    return doc


def test_secondary_tab_selection_updates_semantic(qtbot, tmp_path: Path) -> None:
    """Second axis tab selectRow updates semantic_selection (no sender inference)."""
    from well_log_workstation.workspace import add_well

    ws = create_workspace(tmp_path / "ws", name="DualWS")
    well = add_well(ws, name="Dual", path="wells/dual.las")
    doc = _make_dual_axis_doc(well.id)

    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    win.session.put(doc)
    win._selected_well_id = well.id
    # Display set = both versions
    display = {
        leaf_id_for_curve(doc.document_id, "GR"),
        leaf_id_for_curve(doc.document_id, "GR", "resample-0.5m"),
    }
    win.set_display_set(well.id, display, template_id="std-gr-rt-den")
    win.set_view_mode("table")
    qtbot.wait(50)

    # Expect 2 tabs: per-axis (curve-GR, 3 rows) is primary (listed first),
    # shared MD (5 rows) is the secondary tab.
    assert win.table_axis_tabs.count() == 2, (
        f"expected 2 axis tabs, got {win.table_axis_tabs.count()}"
    )
    assert len(win._table_models) == 2
    # Per table_projection grouping: per-axis tables come first
    assert win._table_models[0].projection().axis_id == "curve-GR"
    assert win._table_models[1].projection().axis_id == "md"
    sec_view = win.table_axis_tabs.widget(1)
    sec_proj = win._table_models[1].projection()
    assert sec_proj is not None
    assert sec_view is not None

    # Select row 1 on the secondary tab (md axis: depth 1001.0)
    sec_view.selectRow(1)
    # Process queued selectionChanged
    from PySide6.QtWidgets import QApplication

    QApplication.processEvents()
    sel = win.semantic_selection
    assert sel is not None
    assert sel.well_id == well.id
    # Row 1 on md axis (5 rows) -> reference_depth 1001.0, sample_index 1
    assert sel.sample_index == 1
    assert sel.reference_depth == pytest.approx(1001.0)
    assert sel.leaf_id == leaf_id_for_curve(doc.document_id, "GR")


def test_index_axis_apply_skips_depth_mapping(qtbot, tmp_path: Path) -> None:
    """Index axis (len-N, unit idx) must not nearest-map by reference depth.

    If the primary tab were an index axis, applying a semantic selection
    whose sample_index is out of range would previously seek the nearest
    pseudo-depth (arange value) and land on the wrong row. The fix is to
    leave the tab unhighlighted (clearSelection) rather than jump.
    """
    from well_log_workstation.semantic_selection import SemanticSelection

    ws = create_workspace(tmp_path / "ws", name="IdxWS")
    well_id = "w-idx"
    from well_log_workstation.workspace import add_well

    well = add_well(ws, name="Idx", path="wells/idx.las")
    # Use the well catalog id as doc id so ensure_well_loaded returns ours
    well_id = well.id
    depth = np.arange(5, dtype=np.float64)  # device md
    short_vals = np.array([1.0, 2.0, 3.0], dtype=np.float64)
    doc = ImportedWellDocument(
        document_id=well_id,
        well_name="Idx",
        source_path="wells/idx.las",
        depth=depth,
        depth_unit="m",
        curves=[
            ImportedCurve(
                mnemonic="SHORT",
                unit="",
                values=short_vals,
                null_mask=np.zeros(3, dtype=bool),
            )
        ],
    )
    # sanity: projection is len-N idx axis
    tpl = get_builtin_template("std-gr-rt-den")
    assert tpl is not None
    projs = build_table_projections(
        doc, {leaf_id_for_curve(well_id, "SHORT")}, tpl
    )
    assert len(projs) == 1
    assert projs[0].depth_unit == "idx"
    assert np.array_equal(projs[0].depth, np.arange(3, dtype=np.float64))

    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    win.session.put(doc)
    win._selected_well_id = well_id
    win.set_display_set(
        well_id, {leaf_id_for_curve(well_id, "SHORT")}, template_id="std-gr-rt-den"
    )
    win.set_view_mode("table")
    qtbot.wait(50)
    # Primary is the idx table (single tab)
    assert win.table_axis_tabs.count() == 1
    primary_proj = win._primary_table_model.projection()
    assert primary_proj is not None
    assert primary_proj.depth_unit == "idx"

    # A selection whose sample_index is out of range for this idx table
    # (e.g. from the main MD axis). Nearest on arange(3) to 1002.5 would be
    # row 2, but we must NOT highlight — the axis has no depth semantics.
    sel = SemanticSelection(
        well_id=well_id,
        sample_index=10,  # out of range for len-3 table
        reference_depth=1002.5,
    )
    win.apply_semantic_selection(sel)
    from PySide6.QtWidgets import QApplication

    QApplication.processEvents()
    rows = win._primary_table_view.selectionModel().selectedRows()
    assert rows == [], "index-axis tab must stay unhighlighted rather than nearest-map"

    # In-range index should still highlight normally
    sel2 = SemanticSelection(well_id=well_id, sample_index=1, reference_depth=1.0)
    win.apply_semantic_selection(sel2)
    QApplication.processEvents()
    rows2 = win._primary_table_view.selectionModel().selectedRows()
    assert len(rows2) == 1 and rows2[0].row() == 1


# ---------------------------------------------------------------------------
# #38-5: export atomic write — failed replace cleans tmp, no half-write
# ---------------------------------------------------------------------------


def test_engine_export_atomic_tmp_cleaned_on_replace_failure(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """monkeypatched os.replace raises -> target not half-written, tmp removed."""
    from well_log_workstation.export_dispatch import PlotDocument, export_plot

    plot_doc = PlotDocument(
        id="pd-38-5",
        name="Atomic test",
        type="single_well",
        well_ids=["w1"],
        template_id=None,
        path="plots/pd-38-5.json",
    )
    out = tmp_path / "engine.pdf"
    out.write_bytes(b"OLD_CONTENT_" + b"x" * 64)
    old = out.read_bytes()

    class _FakeView:
        def export_scene_pdf(self, document_id, export_pixel_height=0, searchable_text=False, crop_marks=False, layered_pdf=False, show_depth_ruler=True):  # noqa: ANN001
            return b"%PDF-1.7\n" + b"y" * 128

    import well_log_workstation.export_dispatch as ed

    def boom(src, dst):  # noqa: ANN001
        raise OSError("simulated replace failure")

    monkeypatch.setattr(ed.os, "replace", boom)
    with pytest.raises(OSError, match="simulated replace failure"):
        export_plot(
            plot_doc,
            "pdf",
            backend="engine",
            view=_FakeView(),
            document_id="doc-38-5",
            path=str(out),
            pdf_text_mode="outline",
        )
    # Target unchanged (not half-written)
    assert out.read_bytes() == old
    # No orphaned tmp left behind
    tmps = [p for p in tmp_path.iterdir() if p.name.endswith(".tmp")]
    assert tmps == []
