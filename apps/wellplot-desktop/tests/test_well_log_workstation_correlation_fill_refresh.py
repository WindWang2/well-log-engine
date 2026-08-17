"""T9 interwell fill + T10 single-well→correlation tops refresh."""

from __future__ import annotations

import os
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.interwell_fill import build_interwell_fill_bands
from well_log_workstation.plot_document import load_plot_document
from well_log_workstation.shell import WellLogWorkstationWindow
from well_log_workstation.tops_model import FormationTop, load_tops_for_well, save_tops_for_well
from well_log_workstation.workspace import create_workspace


def _write_las(path: Path, well: str) -> Path:
    path.write_text(
        f"""~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1005.0
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
1004 50 5
1005 60 6
""",
        encoding="utf-8",
    )
    return path


def test_build_interwell_fill_bands_between_shared_tops() -> None:
    cols = [
        [
            FormationTop(name="T1", depth=1001.0, id="a"),
            FormationTop(name="T2", depth=1003.0, id="b"),
        ],
        [
            FormationTop(name="T1", depth=1001.5, id="c"),
            FormationTop(name="T2", depth=1003.5, id="d"),
        ],
    ]
    bands = build_interwell_fill_bands(cols)
    assert len(bands) == 1
    assert bands[0].top_name == "T1"
    assert bands[0].bottom_name == "T2"
    assert bands[0].left_col == 0
    assert bands[0].right_col == 1


def test_t10_auto_refresh_correlation_after_tops_edit(qtbot, tmp_path: Path) -> None:
    """Single-well tops edit auto-updates open correlation tops (#298)."""
    ws = create_workspace(tmp_path / "ws", name="Refresh")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    id1 = win.import_las_path(_write_las(tmp_path / "a.las", "A"))
    id2 = win.import_las_path(_write_las(tmp_path / "b.las", "B"))
    save_tops_for_well(
        ws,
        id1,
        [
            FormationTop(name="T1", depth=1001.0, id="t1a"),
            FormationTop(name="T2", depth=1003.0, id="t2a"),
        ],
    )
    save_tops_for_well(
        ws,
        id2,
        [
            FormationTop(name="T1", depth=1001.2, id="t1b"),
            FormationTop(name="T2", depth=1003.2, id="t2b"),
        ],
    )
    win.create_correlation_plot_document([id1, id2], "std-gr-rt-den")
    tops0 = win.correlation_canvas.tops_per_column()
    assert any(t.depth == pytest.approx(1001.0) for t in tops0[0])

    # Edit well 1 tops while correlation is open
    win.set_top_depth(id1, "t1a", 1001.8)
    tops1 = win.correlation_canvas.tops_per_column()
    assert any(t.depth == pytest.approx(1001.8) for t in tops1[0])
    # Link depths should follow name T1
    for lk in win.correlation_canvas.links():
        if lk.name == "T1" and lk.left_well_id == id1:
            assert lk.left_depth == pytest.approx(1001.8)


def test_set_top_depth_dirty_well_gates_correlation_refresh(
    qtbot, tmp_path: Path, monkeypatch
) -> None:
    """#601: one set_top_depth must not reload every correlation well."""
    import well_log_workstation.shell as shell_mod
    from well_log_workstation.tops_model import load_tops_for_well as _real_load

    ws = create_workspace(tmp_path / "ws10", name="Dirty")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    corr_ids: list[str] = []
    for i in range(10):
        wid = win.import_las_path(_write_las(tmp_path / f"w{i}.las", f"W{i}"))
        corr_ids.append(wid)
        save_tops_for_well(
            ws,
            wid,
            [
                FormationTop(name="T1", depth=1001.0 + i * 0.01, id=f"t1-{i}"),
                FormationTop(name="T2", depth=1003.0 + i * 0.01, id=f"t2-{i}"),
            ],
        )
    outside = win.import_las_path(_write_las(tmp_path / "out.las", "OUT"))
    save_tops_for_well(
        ws,
        outside,
        [FormationTop(name="T1", depth=1001.0, id="t1-out")],
    )
    win.create_correlation_plot_document(corr_ids, "std-gr-rt-den")
    edited = corr_ids[0]

    loaded: list[str] = []
    refresh_loads: list[str] = []
    submits: list[int] = []

    def _spy_load(workspace, well_id, *args, **kwargs):
        loaded.append(str(well_id))
        return _real_load(workspace, well_id, *args, **kwargs)

    real_refresh = win.refresh_correlation_from_sources

    def _spy_refresh(*, reason: str = "manual", **kwargs):
        before = len(loaded)
        result = real_refresh(reason=reason, **kwargs)
        refresh_loads.extend(loaded[before:])
        return result

    def _spy_submit(*_a, **_k):
        submits.append(1)
        return {"well_count": len(corr_ids)}

    monkeypatch.setattr(shell_mod, "load_tops_for_well", _spy_load)
    monkeypatch.setattr(win, "refresh_correlation_from_sources", _spy_refresh)
    monkeypatch.setattr(shell_mod, "submit_multi_well_presentations", _spy_submit)

    loaded.clear()
    refresh_loads.clear()
    submits.clear()
    win.set_top_depth(edited, "t1-0", 1001.8)
    qtbot.wait(20)

    assert set(loaded) <= {edited}, f"loaded other wells: {loaded}"
    assert len(refresh_loads) <= 1
    assert all(wid == edited for wid in refresh_loads)
    assert len(submits) <= 1
    tops1 = win.correlation_canvas.tops_per_column()
    assert any(t.depth == pytest.approx(1001.8) for t in tops1[0])
    assert any(t.depth == pytest.approx(1001.01) for t in tops1[1])

    loaded.clear()
    refresh_loads.clear()
    submits.clear()
    win.set_top_depth(outside, "t1-out", 1002.0)
    qtbot.wait(20)
    assert set(loaded) <= {outside}
    assert refresh_loads == []
    assert submits == []


def test_paint_builds_interwell_bands_once(qtbot, monkeypatch) -> None:
    """#733: one paintEvent must call build_interwell_fill_bands exactly once."""
    import numpy as np
    from PySide6.QtGui import QImage

    import well_log_workstation.correlation_canvas as cc_mod
    from well_log_workstation.correlation_canvas import CorrelationCanvas
    from well_log_workstation.template_model import (
        BoundCurveLayer,
        BoundTrack,
        HostPresentation,
        ScaleSpec,
    )
    from well_log_workstation.tops_model import FormationTop

    depth = np.array([1000.0, 1005.0, 1010.0])
    vals = np.array([10.0, 20.0, 30.0])
    pres = HostPresentation(
        template_id="t",
        template_name="T",
        well_document_id="w1",
        well_name="W1",
        depth=depth,
        depth_unit="m",
        tracks=[
            BoundTrack(
                id="c",
                role="curve",
                title="GR",
                width_fraction=1.0,
                scale=ScaleSpec(min=0.0, max=100.0, mode="linear", unit="API"),
                layers=[
                    BoundCurveLayer(
                        mnemonic="GR",
                        color="#1f77b4",
                        unit="API",
                        values=vals,
                        null_mask=np.zeros(3, dtype=bool),
                    )
                ],
            )
        ],
    )
    canvas = CorrelationCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 480)
    canvas.set_columns([pres, pres])
    canvas.set_tops_per_column(
        [
            [
                FormationTop(name="T1", depth=1001.0, id="a"),
                FormationTop(name="T2", depth=1003.0, id="b"),
            ],
            [
                FormationTop(name="T1", depth=1001.5, id="c"),
                FormationTop(name="T2", depth=1003.5, id="d"),
            ],
        ]
    )
    canvas.set_show_interwell_fill(True)
    canvas.set_depth_range(999.0, 1011.0)

    real = cc_mod.build_interwell_fill_bands
    calls: list[int] = []

    def _spy(*args, **kwargs):
        calls.append(1)
        return real(*args, **kwargs)

    monkeypatch.setattr(cc_mod, "build_interwell_fill_bands", _spy)
    img = QImage(canvas.size(), QImage.Format.Format_ARGB32)
    img.fill(0)
    canvas.render(img)
    assert len(calls) == 1


def test_t9_fill_toggle_persists_and_paints(qtbot, tmp_path: Path) -> None:
    """Interwell fill checkbox persists and enables canvas fill (#297)."""
    ws = create_workspace(tmp_path / "ws2", name="Fill")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    id1 = win.import_las_path(_write_las(tmp_path / "c.las", "C"))
    id2 = win.import_las_path(_write_las(tmp_path / "d.las", "D"))
    save_tops_for_well(
        ws,
        id1,
        [
            FormationTop(name="T1", depth=1001.0, id="x1"),
            FormationTop(name="T2", depth=1003.0, id="x2"),
        ],
    )
    save_tops_for_well(
        ws,
        id2,
        [
            FormationTop(name="T1", depth=1001.5, id="y1"),
            FormationTop(name="T2", depth=1003.5, id="y2"),
        ],
    )
    plot = win.create_correlation_plot_document([id1, id2], "std-gr-rt-den")
    assert win.correlation_canvas.show_interwell_fill() is False

    win.corr_fill_check.setChecked(True)
    assert win.correlation_canvas.show_interwell_fill() is True
    loaded = load_plot_document(ws, plot.id)
    assert loaded.show_interwell_fill is True

    # Manual refresh still works
    win.refresh_correlation_from_sources(reason="manual")
    assert len(win.correlation_canvas.tops_per_column()) == 2

    # Reopen restores fill flag
    win2 = WellLogWorkstationWindow()
    qtbot.addWidget(win2)
    win2.set_workspace(ws)
    win2.open_plot_document(plot.id)
    assert win2.correlation_canvas.show_interwell_fill() is True
