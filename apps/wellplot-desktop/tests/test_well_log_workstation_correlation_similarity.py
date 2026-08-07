"""Curve-shape similarity for correlation links (FRS §3.x 曲线形态自动对比).

Covers the pure-numpy ``best_lag`` / ``refine_link_depths`` math (windowed
normalized cross-correlation) and the shell integration that refines
existing name-matched links' right_depth via the primary curves.
"""

from __future__ import annotations

import os
from pathlib import Path

import numpy as np
import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.correlation_links import HorizonLink  # noqa: E402
from well_log_workstation.correlation_similarity import (  # noqa: E402
    CurveSamples,
    best_lag,
    refine_link_depths,
)


# -- pure math: best_lag --------------------------------------------


def _curve(depth: np.ndarray, *, phase: float = 0.0) -> np.ndarray:
    """A smooth morphology (sinusoid) with an optional phase shift."""
    return np.sin((depth - 1000.0) * 0.5 + phase)


def test_best_lag_finds_known_shift() -> None:
    """Right curve morphology sits 3 m deeper; mis-picked top must recover it."""
    d = np.arange(1000.0, 1020.0, 1.0)
    v = _curve(d)
    mask = np.zeros(20, bool)
    dr = d + 3.0  # same morphology, 3 m deeper
    # Left top at 1010 (morphology index 10). Correct right top = 1013.
    # Mis-pick right at 1015 (2 m too deep) -> lag should be positive,
    # nudging right_depth toward 1013 (i.e. left_depth + lag ~ 1013).
    lag, score = best_lag(d, v, mask, dr, v, mask, 1010.0, 1015.0,
                          window=8.0, max_lag=6.0)
    assert lag > 0  # positive: right marker should move shallower (toward 1013)
    assert score > 0.9  # strong positive correlation


def test_best_lag_zero_when_aligned() -> None:
    """Already-aligned tops -> lag near zero."""
    d = np.arange(1000.0, 1020.0, 1.0)
    v = _curve(d)
    mask = np.zeros(20, bool)
    lag, score = best_lag(d, v, mask, d, v, mask, 1010.0, 1010.0,
                          window=8.0, max_lag=6.0)
    assert abs(lag) < 1.0
    assert score > 0.99


def test_best_lag_none_insufficient_data() -> None:
    """Fewer than 5 overlapping samples -> None."""
    d = np.array([1000.0, 1001.0])
    v = _curve(d)
    mask = np.zeros(2, bool)
    assert best_lag(d, v, mask, d, v, mask, 1000.0, 1000.0) is None


def test_best_lag_none_all_null() -> None:
    """All-null window -> None."""
    d = np.arange(1000.0, 1020.0, 1.0)
    v = _curve(d)
    mask = np.ones(20, bool)  # all null
    assert best_lag(d, v, mask, d, v, mask, 1010.0, 1010.0) is None


def test_best_lag_ignores_null() -> None:
    """A null gap in the window still yields a correct lag."""
    d = np.arange(1000.0, 1020.0, 1.0)
    v = _curve(d)
    mask = np.zeros(20, bool)
    mask[5] = True  # one null sample
    dr = d + 3.0
    lag, score = best_lag(d, v, mask, dr, v, mask, 1010.0, 1015.0,
                          window=8.0, max_lag=6.0)
    assert lag > 0
    assert score > 0.8


def test_best_lag_constant_curve_is_none() -> None:
    """A constant (zero-variance) curve has undefined correlation -> None."""
    d = np.arange(1000.0, 1020.0, 1.0)
    v = np.full(20, 5.0)
    mask = np.zeros(20, bool)
    assert best_lag(d, v, mask, d, v, mask, 1010.0, 1010.0) is None


# -- pure math: refine_link_depths ----------------------------------


def test_refine_link_depths_adjusts_right_depth() -> None:
    """A mis-picked right depth is nudged toward the morphology-aligned depth."""
    d = np.arange(1000.0, 1020.0, 1.0)
    v = _curve(d)
    mask = np.zeros(20, bool)
    dr = d + 3.0  # morphology 3 m deeper
    curves = {
        "A": CurveSamples(d, v, mask, "GR"),
        "B": CurveSamples(dr, v, mask, "GR"),
    }
    # Left top 1010 (correct). Right top mis-picked at 1015 (should be 1013).
    link = HorizonLink(
        id="lk1", left_well_id="A", right_well_id="B", name="T1",
        left_depth=1010.0, right_depth=1015.0,
    )
    refined = refine_link_depths([link], curves, window=8.0, max_lag=6.0)
    assert len(refined) == 1
    # right_depth moved from 1015 toward 1013 (shallower).
    assert refined[0].right_depth < 1015.0
    assert refined[0].right_depth > 1011.0  # within max_lag of 1013
    # Other fields preserved.
    assert refined[0].id == "lk1"
    assert refined[0].name == "T1"
    assert refined[0].left_depth == 1010.0


def test_refine_link_depths_preserves_unmatched() -> None:
    """A link whose well has no curve is returned unchanged."""
    d = np.arange(1000.0, 1020.0, 1.0)
    v = _curve(d)
    mask = np.zeros(20, bool)
    curves = {"A": CurveSamples(d, v, mask, "GR")}  # B missing
    link = HorizonLink(
        id="lk1", left_well_id="A", right_well_id="B", name="T1",
        left_depth=1010.0, right_depth=1010.0,
    )
    refined = refine_link_depths([link], curves)
    assert refined[0] is link  # unchanged (same object)


def test_refine_link_depths_is_non_destructive() -> None:
    """The input list and its links are not mutated."""
    d = np.arange(1000.0, 1020.0, 1.0)
    v = _curve(d)
    mask = np.zeros(20, bool)
    dr = d + 3.0
    curves = {
        "A": CurveSamples(d, v, mask, "GR"),
        "B": CurveSamples(dr, v, mask, "GR"),
    }
    link = HorizonLink(
        id="lk1", left_well_id="A", right_well_id="B", name="T1",
        left_depth=1010.0, right_depth=1015.0,
    )
    original_right = link.right_depth
    refined = refine_link_depths([link], curves, window=8.0, max_lag=6.0)
    assert link.right_depth == original_right  # input unchanged
    assert refined is not [link]  # new list


def test_refine_link_depths_below_score_threshold_unchanged() -> None:
    """A weak/noise correlation (below min_score) leaves the link unchanged."""
    d = np.arange(1000.0, 1020.0, 1.0)
    rng = np.random.default_rng(42)
    v_a = rng.normal(size=20)  # noise
    v_b = rng.normal(size=20)  # uncorrelated noise
    mask = np.zeros(20, bool)
    curves = {
        "A": CurveSamples(d, v_a, mask, "GR"),
        "B": CurveSamples(d, v_b, mask, "GR"),
    }
    link = HorizonLink(
        id="lk1", left_well_id="A", right_well_id="B", name="T1",
        left_depth=1010.0, right_depth=1010.0,
    )
    refined = refine_link_depths([link], curves, window=8.0, max_lag=6.0,
                                 min_score=0.9)
    assert refined[0].right_depth == 1010.0  # unchanged


# -- shell integration ----------------------------------------------


def _write_las(path: Path, well: str, values: str, start: float = 1000.0,
               stop: float = 1019.0) -> Path:
    path.write_text(
        f"""~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M {start:.1f}
STOP.M {stop:.1f}
STEP.M 1.0
NULL. -999.25
WELL. {well}
~CURVE INFORMATION
DEPT.M
GR.GAPI
~ASCII
{values}
""",
        encoding="utf-8",
    )
    return path


def test_shell_refine_action_adjusts_links(qtbot, tmp_path: Path) -> None:
    """Auto-link by name then refine via curve shape; depths adjust + persist."""
    from well_log_workstation.plot_document import load_plot_document
    from well_log_workstation.shell import WellLogWorkstationWindow
    from well_log_workstation.tops_model import (
        FormationTop,
        save_tops_for_well,
    )
    from well_log_workstation.workspace import create_workspace

    ws = create_workspace(tmp_path / "ws", name="CorrSim")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    # Two wells with the SAME GR morphology, but well B's depth axis is
    # shifted 3 m deeper (so the feature at A-depth 1010 sits at B-depth 1013).
    d = np.arange(1000.0, 1020.0, 1.0)
    v = np.sin((d - 1000.0) * 0.5)
    vals_a = "\n".join(f"{int(di)} {vi:.4f}" for di, vi in zip(d, v))
    # Well B: depth 1003..1022, same morphology values -> feature 3 m deeper.
    db = d + 3.0
    vals_b = "\n".join(f"{int(di)} {vi:.4f}" for di, vi in zip(db, v))
    id_a = win.import_las_path(_write_las(tmp_path / "a.las", "A", vals_a))
    id_b = win.import_las_path(
        _write_las(tmp_path / "b.las", "B", vals_b, start=1003.0, stop=1022.0)
    )
    # Tops: both named T1; A at 1010, B MIS-picked at 1010 (should be 1013).
    save_tops_for_well(ws, id_a, [FormationTop(id="ta", name="T1", depth=1010.0)])
    save_tops_for_well(ws, id_b, [FormationTop(id="tb", name="T1", depth=1010.0)])
    plot = win.create_correlation_plot_document([id_a, id_b], "std-gr-rt-den")
    # Auto-link by name -> one link T1 at (1010, 1010).
    win.auto_link_correlation_tops()
    links = win._correlation_links
    assert len(links) == 1
    assert links[0].right_depth == 1010.0
    # Refine via curve shape -> right_depth should move (toward 1013).
    changed, total = win.refine_correlation_link_depths()
    assert total == 1
    assert changed == 1
    refined = win._correlation_links[0]
    assert refined.right_depth != 1010.0
    # Persisted.
    loaded = load_plot_document(ws, plot.id)
    assert loaded.links[0].right_depth == refined.right_depth
    # Undo restores the name-matched depth.
    assert win.undo_correlation_layout() is True
    assert win._correlation_links[0].right_depth == 1010.0
