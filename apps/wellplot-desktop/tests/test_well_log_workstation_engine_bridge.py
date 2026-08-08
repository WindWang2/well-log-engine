"""Optional WellLogEngine bridge (#224 / #225)."""

from __future__ import annotations

import os
from pathlib import Path

import numpy as np
import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.engine_bridge import (  # noqa: E402
    EngineUnavailable,
    create_well_log_view,
    engine_available,
    presentation_to_multi_track_payload,
    presentations_to_multi_well_payload,
    primary_curve_from_presentation,
    probe_engine,
    reset_engine_capability_cache,
)
from well_log_workstation.shell import WellLogWorkstationWindow  # noqa: E402
from well_log_workstation.tops_model import FormationTop  # noqa: E402
from well_log_workstation.workspace import create_workspace  # noqa: E402


def _write_las(path: Path, well: str = "ENG-1") -> Path:
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


@pytest.fixture(autouse=True)
def _reset_probe() -> None:
    reset_engine_capability_cache()
    yield
    reset_engine_capability_cache()


def test_probe_respects_disable_env(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("WLWS_DISABLE_ENGINE", "1")
    reset_engine_capability_cache()
    cap = probe_engine()
    assert cap.available is False
    assert "WLWS_DISABLE_ENGINE" in cap.detail
    assert engine_available() is False


def test_create_view_raises_when_disabled(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("WLWS_DISABLE_ENGINE", "1")
    reset_engine_capability_cache()
    with pytest.raises(EngineUnavailable):
        create_well_log_view()


def test_shell_default_is_host_multitrack(qtbot, tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setenv("WLWS_DISABLE_ENGINE", "1")
    reset_engine_capability_cache()
    ws = create_workspace(tmp_path / "ws")
    las = _write_las(tmp_path / "e.las")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(las)
    win.apply_template_to_well(well_id, "std-gr-rt-den")
    assert win.multi_track_canvas.track_count() >= 2
    assert win.primary_surface == "host"
    assert win.single_well_stack.currentIndex() == 0
    with pytest.raises((EngineUnavailable, Exception)):
        win.open_engine_preview()
    assert win.active_presentation is not None
    assert win.primary_surface == "host"


def test_prefer_engine_falls_back_to_host(qtbot, tmp_path: Path, monkeypatch) -> None:
    """With engine disabled, prefer flag still yields host surface."""
    monkeypatch.setenv("WLWS_DISABLE_ENGINE", "1")
    reset_engine_capability_cache()
    ws = create_workspace(tmp_path / "pref")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(_write_las(tmp_path / "p.las"))
    win.set_prefer_engine_canvas(True)
    win.apply_template_to_well(well_id, "std-gr-rt-den")
    assert win.primary_surface == "host"


def test_force_host_canvas_env(qtbot, tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setenv("WLWS_FORCE_HOST_CANVAS", "1")
    monkeypatch.delenv("WLWS_DISABLE_ENGINE", raising=False)
    reset_engine_capability_cache()
    # Reconstruct default prefer from env
    from well_log_workstation.shell import WellLogWorkstationWindow as W

    assert W._default_prefer_engine() is False


def test_primary_curve_from_presentation(qtbot, tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setenv("WLWS_DISABLE_ENGINE", "1")
    reset_engine_capability_cache()
    ws = create_workspace(tmp_path / "ws2")
    las = _write_las(tmp_path / "p.las")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(las)
    pres = win.apply_template_to_well(well_id, "std-gr-rt-den")
    primary = primary_curve_from_presentation(pres)
    assert primary is not None
    depth, values, mnemonic, unit = primary
    assert depth.size == values.size
    assert depth.size >= 2
    assert mnemonic


def test_marker_semantic_reaches_the_engine_payload(
    qtbot, tmp_path: Path, monkeypatch
) -> None:
    """SDK marker symbols: FormationTop.semantic flows into the payload;
    legacy tops (no semantic) keep the historical shape (no key)."""
    monkeypatch.setenv("WLWS_DISABLE_ENGINE", "1")
    reset_engine_capability_cache()
    ws = create_workspace(tmp_path / "sem")
    las = _write_las(tmp_path / "s.las")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(las)
    pres = win.apply_template_to_well(well_id, "std-gr-rt-den")
    tops = [
        FormationTop(
            name="CSG",
            depth=1001.0,
            id="00000000-0000-0000-0000-000000000001",
            semantic="casing_shoe",
        ),
        FormationTop(
            name="T1",
            depth=1002.0,
            id="00000000-0000-0000-0000-000000000002",
        ),
    ]
    payload = presentation_to_multi_track_payload(pres, tops=tops)
    by_label = {m["label"]: m for m in payload["markers"]}
    assert by_label["CSG"]["semantic"] == "casing_shoe"
    assert "semantic" not in by_label["T1"], (
        "legacy tops must not fabricate a semantic"
    )


def test_multi_track_payload_from_presentation(
    qtbot, tmp_path: Path, monkeypatch
) -> None:
    monkeypatch.setenv("WLWS_DISABLE_ENGINE", "1")
    reset_engine_capability_cache()
    ws = create_workspace(tmp_path / "payload")
    las = _write_las(tmp_path / "p2.las")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(las)
    pres = win.apply_template_to_well(well_id, "std-gr-rt-den")
    tops = [
        FormationTop(
            name="T1",
            depth=1001.0,
            id="00000000-0000-0000-0000-000000000001",
        )
    ]
    payload = presentation_to_multi_track_payload(pres, tops=tops)
    assert "document_id" in payload
    assert len(payload["curves"]) >= 1
    assert len(payload["tracks"]) >= 1
    assert payload["tracks"][0]["layers"]
    assert payload["markers"][0]["label"] == "T1"
    n = payload["depth"].size
    for c in payload["curves"]:
        assert c["values"].size == n


def test_multi_well_payload_two_wells(qtbot, tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setenv("WLWS_DISABLE_ENGINE", "1")
    reset_engine_capability_cache()
    ws = create_workspace(tmp_path / "mw")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    id1 = win.import_las_path(_write_las(tmp_path / "a.las", "A"))
    id2 = win.import_las_path(_write_las(tmp_path / "b.las", "B"))
    p1 = win.apply_template_to_well(id1, "std-gr-rt-den")
    p2 = win.apply_template_to_well(id2, "std-gr-rt-den")
    payload = presentations_to_multi_well_payload(
        [p1, p2], shared_depth=(1000.0, 1003.0)
    )
    assert len(payload["wells"]) == 2
    assert payload["shared_top"] == 1000.0
    assert payload["shared_bottom"] == 1003.0
    for w in payload["wells"]:
        assert "depth" in w
        # multi-track (#232) or legacy single curve
        assert ("curves" in w and "tracks" in w) or "values" in w


def test_submit_multi_track_when_engine_present(
    qtbot, tmp_path: Path, monkeypatch
) -> None:
    monkeypatch.delenv("WLWS_DISABLE_ENGINE", raising=False)
    monkeypatch.delenv("WLWS_FORCE_HOST_CANVAS", raising=False)
    reset_engine_capability_cache()
    if not engine_available():
        pytest.skip(probe_engine().detail)
    ws = create_workspace(tmp_path / "eng")
    las = _write_las(tmp_path / "g.las")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    win.set_prefer_engine_canvas(True)
    well_id = win.import_las_path(las)
    win.apply_template_to_well(well_id, "std-gr-rt-den")
    # Primary path should be engine without extra menu
    assert win.primary_surface == "engine"
    assert win.single_well_stack.currentIndex() == 1
    report = win.open_engine_preview()
    assert isinstance(report, dict)
    assert report.get("render_prepared") is True or "depth" in report
    if "track_count" in report:
        assert int(report["track_count"]) >= 1


def test_multi_well_payload_has_multi_track_columns(
    qtbot, tmp_path: Path, monkeypatch
) -> None:
    monkeypatch.setenv("WLWS_DISABLE_ENGINE", "1")
    reset_engine_capability_cache()
    from well_log_workstation.engine_bridge import presentations_to_multi_well_payload

    ws = create_workspace(tmp_path / "mt-mw")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    id1 = win.import_las_path(_write_las(tmp_path / "a.las", "A"))
    id2 = win.import_las_path(_write_las(tmp_path / "b.las", "B"))
    p1 = win.apply_template_to_well(id1, "std-gr-rt-den")
    p2 = win.apply_template_to_well(id2, "std-gr-rt-den")
    assert p1.curve_track_count >= 1
    payload = presentations_to_multi_well_payload([p1, p2], multi_track=True)
    assert len(payload["wells"]) == 2
    for well in payload["wells"]:
        assert "curves" in well and len(well["curves"]) >= 1
        assert "tracks" in well and len(well["tracks"]) >= 1
        assert "depth" in well
        # Legacy-only fields not required
        assert "values" not in well or "curves" in well


def test_correlation_prefers_host_without_engine(
    qtbot, tmp_path: Path, monkeypatch
) -> None:
    monkeypatch.setenv("WLWS_DISABLE_ENGINE", "1")
    reset_engine_capability_cache()
    ws = create_workspace(tmp_path / "corr-host")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    id1 = win.import_las_path(_write_las(tmp_path / "w1.las", "W1"))
    id2 = win.import_las_path(_write_las(tmp_path / "w2.las", "W2"))
    win.set_prefer_engine_canvas(True)
    win.create_correlation_plot_document([id1, id2], "std-gr-rt-den")
    assert win.correlation_canvas.column_count() == 2
    assert win.correlation_stack.currentIndex() == 0
    assert win.primary_surface == "host"


def test_submit_multi_well_when_engine_present(
    qtbot, tmp_path: Path, monkeypatch
) -> None:
    monkeypatch.delenv("WLWS_DISABLE_ENGINE", raising=False)
    monkeypatch.delenv("WLWS_FORCE_HOST_CANVAS", raising=False)
    reset_engine_capability_cache()
    if not engine_available():
        pytest.skip(probe_engine().detail)
    view = create_well_log_view()
    if not hasattr(view, "submit_multi_well_section"):
        pytest.skip("submit_multi_well_section not in this welllog build")
    ws = create_workspace(tmp_path / "eng2")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    win.set_prefer_engine_canvas(True)
    id1 = win.import_las_path(_write_las(tmp_path / "w1.las", "W1"))
    id2 = win.import_las_path(_write_las(tmp_path / "w2.las", "W2"))
    # Primary path: create correlation auto-submits multi-well when preferred
    win.create_correlation_plot_document([id1, id2], "std-gr-rt-den")
    assert win.primary_surface == "engine"
    assert win.correlation_stack.currentIndex() == 1
    report = win.open_engine_correlation_preview()
    assert report.get("render_prepared") is True
    assert int(report.get("well_count", 0)) == 2


def test_multi_track_payload_per_curve_depth(
    qtbot, tmp_path: Path, monkeypatch
) -> None:
    """Multi-rate (Epic A): a layer with its own sampling axis carries its
    depth in the payload; shared-axis curves stay truncated to the shared
    depth (and the shared depth follows the shortest shared curve)."""
    monkeypatch.setenv("WLWS_DISABLE_ENGINE", "1")
    reset_engine_capability_cache()
    ws = create_workspace(tmp_path / "mr-payload")
    las = _write_las(tmp_path / "p3.las")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(las)
    pres = win.apply_template_to_well(well_id, "std-gr-rt-den")
    # Give the RT layer its own (shorter, offset) sampling axis.
    for track in pres.tracks:
        for layer in track.layers:
            if layer.mnemonic.upper() == "RT":
                rt_d = np.asarray(pres.depth[::2], dtype=np.float64).copy()
                layer.depth = rt_d
                layer.values = np.asarray(layer.values[::2], dtype=np.float64).copy()
    payload = presentation_to_multi_track_payload(pres)
    curves = {c["mnemonic"].upper(): c for c in payload["curves"]}
    assert "depth" not in curves["GR"], "shared-axis curve must omit depth"
    rt = curves["RT"]
    assert rt["depth"] is not None, "per-curve-axis curve must carry depth"
    assert rt["values"].size == rt["depth"].size
    assert rt["values"].size != payload["depth"].size
    n = payload["depth"].size
    for mnem, c in curves.items():
        if "depth" not in c:
            assert c["values"].size == n, (
                f"shared-axis curve {mnem} must match the shared depth"
            )


def test_survey_depth_transform_builds_control_points() -> None:
    """TVD/TVDSS 域区间道投影: MD→display control points from a trajectory."""
    from dataclasses import replace  # noqa: PLC0415

    from well_log_workstation.engine_bridge import survey_depth_transform  # noqa: PLC0415
    from well_log_workstation.survey import (  # noqa: PLC0415
        SurveyStation,
        compute_trajectory,
    )

    traj = compute_trajectory(
        [SurveyStation(0, 0, 0), SurveyStation(1000, 45, 0)]
    )
    pts = survey_depth_transform(traj, "tvd")
    assert len(pts) >= 2
    refs = [p["reference"] for p in pts]
    disps = [p["display"] for p in pts]
    assert refs == sorted(refs), "MD control points are sorted"
    assert disps == sorted(disps), "TVD display increases with MD"

    pts_ss = survey_depth_transform(traj, "tvdss")
    d_ss = [p["display"] for p in pts_ss]
    assert d_ss == sorted(d_ss, reverse=True), (
        "TVDSS display decreases with MD (engine accepts either direction)"
    )
    assert survey_depth_transform(traj, "md") == []
    assert survey_depth_transform(traj, "horizon") == []
    single = compute_trajectory([SurveyStation(0, 0, 0)])
    assert survey_depth_transform(single, "tvd") == []


def test_multi_track_payload_carries_depth_transform(
    qtbot, tmp_path: Path, monkeypatch
) -> None:
    monkeypatch.setenv("WLWS_DISABLE_ENGINE", "1")
    reset_engine_capability_cache()
    ws = create_workspace(tmp_path / "xform")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(_write_las(tmp_path / "x.las"))
    pres = win.apply_template_to_well(well_id, "std-gr-rt-den")
    payload = presentation_to_multi_track_payload(pres)
    assert "depth_transform" not in payload, "MD default adds no transform"
    pts = [{"reference": 1000.0, "display": 10.0},
           {"reference": 1003.0, "display": -5.0}]
    payload = presentation_to_multi_track_payload(
        pres, depth_transform=pts
    )
    assert payload["depth_transform"] == pts


def test_multi_well_payload_carries_per_well_depth_transform(
    qtbot, tmp_path: Path, monkeypatch
) -> None:
    monkeypatch.setenv("WLWS_DISABLE_ENGINE", "1")
    reset_engine_capability_cache()
    ws = create_workspace(tmp_path / "mw-xform")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    id1 = win.import_las_path(_write_las(tmp_path / "a.las", "A"))
    id2 = win.import_las_path(_write_las(tmp_path / "b.las", "B"))
    p1 = win.apply_template_to_well(id1, "std-gr-rt-den")
    p2 = win.apply_template_to_well(id2, "std-gr-rt-den")
    pts = [{"reference": 1000.0, "display": 10.0},
           {"reference": 1003.0, "display": -5.0}]
    payload = presentations_to_multi_well_payload(
        [p1, p2],
        shared_depth=(1000.0, 1003.0),
        depth_transform_per_well={p1.well_name: pts},
    )
    by_name = {w["document_id"]: w for w in payload["wells"]}
    # Only the well with a transform carries the key.
    with_transform = [w for w in payload["wells"] if "depth_transform" in w]
    assert len(with_transform) == 1
    assert with_transform[0]["depth_transform"] == pts


def test_tvdss_engine_submission_accepts_decreasing_transform(
    qtbot, tmp_path: Path, monkeypatch
) -> None:
    """End-to-end: TVDSS datum + deviated survey → per-well decreasing
    transform submitted and accepted by the native engine."""
    from dataclasses import replace  # noqa: PLC0415

    from well_log_workstation.survey import SurveyStation  # noqa: PLC0415
    from well_log_workstation.tops_model import save_survey_for_well  # noqa: PLC0415

    monkeypatch.delenv("WLWS_DISABLE_ENGINE", raising=False)
    monkeypatch.delenv("WLWS_FORCE_HOST_CANVAS", raising=False)
    reset_engine_capability_cache()
    if not engine_available():
        pytest.skip(probe_engine().detail)
    ws = create_workspace(tmp_path / "tvdss-eng")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    id1 = win.import_las_path(_write_las(tmp_path / "a.las", "A"))
    id2 = win.import_las_path(_write_las(tmp_path / "b.las", "B"))
    for wid in (id1, id2):
        idx = next(i for i, w in enumerate(ws.wells) if w.id == wid)
        ws.wells[idx] = replace(ws.wells[idx], kb_m=25.0)
    # Deviated survey for A only; B has none (stays MD in the payload).
    save_survey_for_well(
        ws, id1, [SurveyStation(0, 0, 0), SurveyStation(1000, 45, 0)]
    )
    win.set_prefer_engine_canvas(True)
    win.create_correlation_plot_document([id1, id2], "std-gr-rt-den")
    win.corr_datum_mode.setCurrentIndex(
        win.corr_datum_mode.findData("tvdss")
    )
    from well_log_workstation.plot_document import load_plot_document  # noqa: PLC0415

    plot = load_plot_document(ws, win._active_plot_id)
    assert plot.datum_mode == "tvdss", "datum combo applied to the plot"
    report = win.open_engine_correlation_preview()
    assert report.get("render_prepared") is True
    assert int(report.get("well_count", 0)) == 2
