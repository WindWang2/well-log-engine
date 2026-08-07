"""Correlation-layout undo/redo alignment (FRS §3.x 全局撤销/重做).

Covers the redo stack added to the correlation-layout undo system
(mirroring the tops/curve books), the plot_id isolation fix (no
cross-plot stale state), the workspace-close clearing fix, and button
enable/disable. The three undo systems (tops / curve-edit /
correlation-layout) stay independent with their existing shortcuts;
this slice makes the third one symmetric with the first two.
"""

from __future__ import annotations

import os
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.plot_document import load_plot_document
from well_log_workstation.shell import WellLogWorkstationWindow
from well_log_workstation.workspace import create_workspace


def _write_las(path: Path, well: str) -> Path:
    path.write_text(
        f"""~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1003.0
STEP.M 1.0
NULL. -999.25
WELL. {well}
~CURVE INFORMATION
DEPT.M
GR.GAPI
RT.OHMM
~ASCII
1000 10 1
1001 20 2
1002 30 3
1003 40 4
""",
        encoding="utf-8",
    )
    return path


def _three_well_plot(qtbot, tmp_path: Path):
    ws = create_workspace(tmp_path / "ws", name="CorrUndoRedo")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    id1 = win.import_las_path(_write_las(tmp_path / "a.las", "A"))
    id2 = win.import_las_path(_write_las(tmp_path / "b.las", "B"))
    id3 = win.import_las_path(_write_las(tmp_path / "c.las", "C"))
    plot = win.create_correlation_plot_document([id1, id2, id3], "std-gr-rt-den")
    return win, ws, plot, (id1, id2, id3)


def _reload(ws, plot):
    return load_plot_document(ws, plot.id)


# -- undo + redo roundtrip ------------------------------------------


def test_layout_undo_then_redo_restores(qtbot, tmp_path: Path) -> None:
    """Undo a mirror, then redo restores the mirrored order."""
    win, ws, plot, (id1, id2, id3) = _three_well_plot(qtbot, tmp_path)
    original = list(plot.well_ids)
    # Mirror -> reversed order.
    win.corr_mirror_btn.click()
    mirrored = _reload(ws, plot).well_ids
    assert mirrored == list(reversed(original))
    # Undo -> back to original.
    assert win.undo_correlation_layout() is True
    assert _reload(ws, plot).well_ids == original
    # Redo -> mirrored again.
    assert win.redo_correlation_layout() is True
    assert _reload(ws, plot).well_ids == list(reversed(original))


def test_redo_button_enabled_after_undo(qtbot, tmp_path: Path) -> None:
    win, ws, plot, _ = _three_well_plot(qtbot, tmp_path)
    assert win.corr_redo_btn.isEnabled() is False
    win.corr_mirror_btn.click()
    assert win.corr_redo_btn.isEnabled() is False  # commit clears redo
    assert win.undo_correlation_layout() is True
    assert win.corr_redo_btn.isEnabled() is True
    assert win.corr_undo_btn.isEnabled() is True  # redo pushed current back


def test_new_commit_clears_redo(qtbot, tmp_path: Path) -> None:
    """A new layout commit after an undo must clear the redo stack."""
    win, ws, plot, _ = _three_well_plot(qtbot, tmp_path)
    win.corr_mirror_btn.click()
    assert win.undo_correlation_layout() is True
    assert win.corr_redo_btn.isEnabled() is True
    # A new commit (another mirror) invalidates redo.
    win.corr_mirror_btn.click()
    assert win.corr_redo_btn.isEnabled() is False
    assert win.redo_correlation_layout() is False  # nothing to redo


def test_undo_redo_idempotent_at_empty(qtbot, tmp_path: Path) -> None:
    """Redo with an empty redo stack returns False and changes nothing."""
    win, ws, plot, _ = _three_well_plot(qtbot, tmp_path)
    before = _reload(ws, plot).well_ids
    assert win.redo_correlation_layout() is False
    assert _reload(ws, plot).well_ids == before


# -- plot_id isolation (cross-plot bug fix) -------------------------


def test_plot_id_isolation(qtbot, tmp_path: Path) -> None:
    """Undo on plot B must not apply plot A's snapshot."""
    win, ws, plot_a, (id1, id2, id3) = _three_well_plot(qtbot, tmp_path)
    # Create a second correlation plot with a different well order.
    plot_b = win.create_correlation_plot_document([id3, id2, id1], "std-gr-rt-den")
    # Edit plot A (mirror) -> pushes an A-tagged snapshot.
    win.open_plot_document(plot_a.id)
    win.corr_mirror_btn.click()
    assert len(win._corr_layout_undo) >= 1
    # Switch to plot B and try undo -> must not apply A's snapshot.
    win.open_plot_document(plot_b.id)
    before_b = _reload(ws, plot_b).well_ids
    # B has no undo entries of its own; undo should be a no-op.
    result = win.undo_correlation_layout()
    # Either False (no B entry) or True but not changing B's wells to A's.
    if result:
        assert _reload(ws, plot_b).well_ids == before_b


# -- workspace-close clearing fix -----------------------------------


def test_workspace_close_clears_corr_stacks(qtbot, tmp_path: Path) -> None:
    win, ws, plot, _ = _three_well_plot(qtbot, tmp_path)
    win.corr_mirror_btn.click()
    assert len(win._corr_layout_undo) >= 1
    assert len(win._corr_layout_redo) == 0
    win.undo_correlation_layout()
    assert len(win._corr_layout_redo) >= 1
    # Close the workspace -> both stacks cleared.
    win.set_workspace(None)
    assert win._corr_layout_undo == []
    assert win._corr_layout_redo == []


# -- link set undo/redo ---------------------------------------------


def test_link_set_undo_redo(qtbot, tmp_path: Path) -> None:
    """Auto-link -> undo clears links -> redo restores them."""
    from well_log_workstation.tops_model import (
        FormationTop,
        save_tops_for_well,
    )

    win, ws, plot, (id1, id2, id3) = _three_well_plot(qtbot, tmp_path)
    # Add a same-named top on two wells so auto-link creates a link.
    save_tops_for_well(ws, id1, [FormationTop(id="t1", name="T1", depth=1001.0)])
    save_tops_for_well(ws, id2, [FormationTop(id="t2", name="T1", depth=1001.0)])
    # Reopen so the canvas loads the freshly-saved tops into its columns.
    win.open_plot_document(plot.id)
    win.auto_link_correlation_tops()
    n_links = len(_reload(ws, plot).links)
    assert n_links >= 1
    # Undo -> links removed.
    assert win.undo_correlation_layout() is True
    assert len(_reload(ws, plot).links) < n_links
    # Redo -> links restored.
    assert win.redo_correlation_layout() is True
    assert len(_reload(ws, plot).links) == n_links


# -- gap change undo/redo -------------------------------------------


def test_gap_change_undo_redo(qtbot, tmp_path: Path) -> None:
    """A column-gap change undoes and redoes."""
    win, ws, plot, _ = _three_well_plot(qtbot, tmp_path)
    assert win.correlation_canvas.column_gap() == 6
    win.corr_gap_spin.setValue(24)
    assert _reload(ws, plot).column_gap_px == 24
    # Undo -> gap back to 6.
    assert win.undo_correlation_layout() is True
    assert _reload(ws, plot).column_gap_px == 6
    # Redo -> gap 24 again.
    assert win.redo_correlation_layout() is True
    assert _reload(ws, plot).column_gap_px == 24


# -- redo button click handler --------------------------------------


def test_redo_button_click_handler(qtbot, tmp_path: Path) -> None:
    """Clicking the redo button drives redo_correlation_layout."""
    win, ws, plot, _ = _three_well_plot(qtbot, tmp_path)
    win.corr_mirror_btn.click()
    win.undo_correlation_layout()
    original = _reload(ws, plot).well_ids
    assert win.corr_redo_btn.isEnabled() is True
    win.corr_redo_btn.click()
    # Redo applied: order no longer equals the undone (original) state.
    assert _reload(ws, plot).well_ids != original
