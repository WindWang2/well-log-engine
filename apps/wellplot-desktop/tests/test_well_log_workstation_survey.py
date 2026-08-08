"""Deviation survey → trajectory + TVD datum (FRS §1.1 / P1-C).

Covers:
* compute_trajectory: vertical well (TVD==MD, zero displacement), deviated
  well (TVD<MD, displacement>0), single station, empty;
* interpolate_tvd: linear interp + clamping + empty;
* serialization round-trip + tolerant parsing;
* WellSectionDatum tvd mode: shift non-zero with survey, 0 without survey,
  md/horizon unaffected;
* WellSectionDatum tvdss mode: true subsea elevation with a deviated survey
  (TVD(ref) - ref - kb), -kb fallback without a survey / with a vertical one;
* per-well survey.json storage round-trip.
"""

from __future__ import annotations

import os
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import numpy as np
import pytest

from well_log_workstation.datum.well_section_datum import WellSectionDatum
from well_log_workstation.survey import (
    SurveyStation,
    SurveyTrajectory,
    compute_trajectory,
    interpolate_tvd,
    survey_from_json,
    survey_to_json,
)


# ---------------------------------------------------------------------------
# compute_trajectory
# ---------------------------------------------------------------------------


def test_vertical_well_tvd_equals_md() -> None:
    stations = [
        SurveyStation(0, 0, 0),
        SurveyStation(100, 0, 0),
        SurveyStation(200, 0, 0),
    ]
    t = compute_trajectory(stations)
    np.testing.assert_allclose(t.tvd, [0, 100, 200])
    np.testing.assert_allclose(t.north, [0, 0, 0])
    np.testing.assert_allclose(t.east, [0, 0, 0])
    np.testing.assert_allclose(t.closure_dist, [0, 0, 0])


def test_deviated_well_tvd_less_than_md() -> None:
    # Build to 45° inclination due north over 100 m.
    stations = [SurveyStation(0, 0, 0), SurveyStation(100, 45, 0)]
    t = compute_trajectory(stations)
    assert t.tvd[-1] < 100.0
    assert t.tvd[-1] > 70.0  # cos(45)=0.707 straight-line floor
    assert t.north[-1] > 0.0
    assert t.east[-1] == pytest.approx(0.0)  # azimuth due north
    assert t.closure_dist[-1] == pytest.approx(t.north[-1])


def test_deviated_well_east_for_azimuth_90() -> None:
    # 45° inclination due east (az 90): east displacement, no north.
    stations = [SurveyStation(0, 0, 0), SurveyStation(100, 45, 90)]
    t = compute_trajectory(stations)
    assert t.north[-1] == pytest.approx(0.0, abs=1e-9)
    assert t.east[-1] > 0.0


def test_tvdss_uses_kb() -> None:
    stations = [SurveyStation(0, 0, 0), SurveyStation(100, 0, 0)]
    t = compute_trajectory(stations, kb_m=25.0)
    # TVD at MD 100 = 100; TVDSS = kb - TVD = 25 - 100 = -75.
    assert t.tvdss[-1] == pytest.approx(25.0 - 100.0)


def test_single_station_vertical() -> None:
    t = compute_trajectory([SurveyStation(150, 0, 0)])
    assert t.tvd[0] == 150.0
    assert t.closure_dist[0] == 0.0


def test_empty_trajectory() -> None:
    t = compute_trajectory([])
    assert len(t) == 0
    assert t.md.size == 0


# ---------------------------------------------------------------------------
# interpolate_tvd
# ---------------------------------------------------------------------------


def test_interpolate_linear() -> None:
    stations = [SurveyStation(0, 0, 0), SurveyStation(100, 45, 0)]
    t = compute_trajectory(stations)
    mid = interpolate_tvd(t, 50.0)
    assert t.tvd[0] < mid < t.tvd[-1]


def test_interpolate_clamps_outside_range() -> None:
    stations = [SurveyStation(0, 0, 0), SurveyStation(100, 45, 0)]
    t = compute_trajectory(stations)
    assert interpolate_tvd(t, -50.0) == pytest.approx(t.tvd[0])
    assert interpolate_tvd(t, 500.0) == pytest.approx(t.tvd[-1])


def test_interpolate_empty_returns_zero() -> None:
    assert interpolate_tvd(compute_trajectory([]), 50.0) == 0.0


# ---------------------------------------------------------------------------
# Serialization
# ---------------------------------------------------------------------------


def test_survey_round_trip() -> None:
    stations = [
        SurveyStation(0, 0, 0),
        SurveyStation(500, 10, 45),
        SurveyStation(1000, 30, 60),
    ]
    back = survey_from_json(survey_to_json(stations))
    assert back == stations


def test_survey_from_json_drops_invalid() -> None:
    raw = [{"md": 0, "inc": 0, "az": 0}, {"md": "x"}, "y", None]
    assert len(survey_from_json(raw)) == 1


def test_survey_from_json_non_list_returns_empty() -> None:
    assert survey_from_json(None) == []
    assert survey_from_json({"not": "list"}) == []


# ---------------------------------------------------------------------------
# WellSectionDatum tvd mode
# ---------------------------------------------------------------------------


def test_datum_tvd_with_survey_nonzero_shift() -> None:
    survey = [SurveyStation(0, 0, 0), SurveyStation(100, 45, 0)]
    d = WellSectionDatum(mode="tvd")
    shifts = d.compute_shifts(
        [{"name": "A", "tops": [{"name": "T", "depth": 100.0}]}],
        surveys={"A": survey},
    )
    # At MD 100, TVD ≈ 90 → shift ≈ -10.
    assert shifts["A"] < 0
    assert shifts["A"] == pytest.approx(-10.0, abs=1.0)


def test_datum_tvd_without_survey_zero_shift() -> None:
    d = WellSectionDatum(mode="tvd")
    shifts = d.compute_shifts([{"name": "A", "tops": [{"name": "T", "depth": 100.0}]}])
    assert shifts == {"A": 0.0}


def test_datum_md_horizon_unaffected_by_surveys_arg() -> None:
    survey = [SurveyStation(0, 0, 0), SurveyStation(100, 45, 0)]
    wells = [{"name": "A", "kb_m": 25.0, "tops": [{"name": "T", "depth": 50.0}]}]
    assert WellSectionDatum("md").compute_shifts(wells, surveys={"A": survey}) == {"A": 0.0}
    assert (
        WellSectionDatum("horizon", target_horizon="T").compute_shifts(
            wells, surveys={"A": survey}
        )
        == {"A": -50.0}
    )


def test_datum_tvdss_without_survey_uses_kb_fallback() -> None:
    # No survey → historic approximation: shift = -kb (well treated as vertical).
    d = WellSectionDatum(mode="tvdss")
    wells = [{"name": "A", "kb_m": 25.0, "tops": [{"name": "T", "depth": 50.0}]}]
    assert d.compute_shifts(wells) == {"A": -25.0}
    # kb_elevations override wins.
    assert d.compute_shifts(wells, kb_elevations={"A": 40.0}) == {"A": -40.0}


def test_datum_tvdss_vertical_survey_keeps_kb_shift() -> None:
    # Vertical well with a survey: TVD == MD → shift = -kb (same as fallback).
    survey = [SurveyStation(0, 0, 0), SurveyStation(100, 0, 0)]
    d = WellSectionDatum(mode="tvdss")
    shifts = d.compute_shifts(
        [{"name": "A", "kb_m": 25.0, "tops": [{"name": "T", "depth": 50.0}]}],
        surveys={"A": survey},
    )
    assert shifts["A"] == pytest.approx(-25.0)


def test_datum_tvdss_deviated_survey_true_subsea_shift() -> None:
    # Well already at 45° (first station included): each 100 m MD adds
    # 100·cos(45°) ≈ 70.71 m TVD, so TVD(200) ≈ 141.42. True placement:
    # TVD(ref) - ref - kb = 141.42 - 200 - 25.
    survey = [
        SurveyStation(0, 45, 0),
        SurveyStation(100, 45, 0),
        SurveyStation(200, 45, 0),
    ]
    d = WellSectionDatum(mode="tvdss")
    shifts = d.compute_shifts(
        [{"name": "A", "kb_m": 25.0, "tops": [{"name": "T", "depth": 200.0}]}],
        surveys={"A": survey},
    )
    assert shifts["A"] == pytest.approx(2 * 100 * np.cos(np.radians(45)) - 200.0 - 25.0)
    # A deviated well must no longer read as vertical (naive would be -25).
    assert shifts["A"] < -25.0


def test_datum_tvdss_deviated_survey_zero_kb() -> None:
    # kb = 0: the shift is the pure TVD-minus-MD deviation at the reference.
    survey = [
        SurveyStation(0, 45, 0),
        SurveyStation(100, 45, 0),
        SurveyStation(200, 45, 0),
    ]
    d = WellSectionDatum(mode="tvdss")
    shifts = d.compute_shifts(
        [{"name": "A", "tops": [{"name": "T", "depth": 200.0}]}],
        surveys={"A": survey},
    )
    assert shifts["A"] == pytest.approx(2 * 100 * np.cos(np.radians(45)) - 200.0)


def test_datum_tvd_mode_accepted() -> None:
    # P1-C: tvd is now a valid mode (previously rejected).
    WellSectionDatum(mode="tvd")  # must not raise


# ---------------------------------------------------------------------------
# Per-well survey.json storage
# ---------------------------------------------------------------------------


def test_survey_storage_round_trip(tmp_path: Path) -> None:
    from well_log_workstation.tops_model import (
        load_survey_for_well,
        save_survey_for_well,
    )
    from well_log_workstation.workspace import add_well, create_workspace

    ws = create_workspace(tmp_path / "ws", name="Survey")
    well = add_well(ws, name="DEV-1", path="wells/dev1.las")
    stations = [
        SurveyStation(0, 0, 0),
        SurveyStation(500, 15, 90),
        SurveyStation(1000, 30, 90),
    ]
    save_survey_for_well(ws, well.id, stations)

    loaded, diags = load_survey_for_well(ws, well.id)
    assert diags == []
    assert loaded == stations


def test_survey_storage_missing_returns_empty(tmp_path: Path) -> None:
    from well_log_workstation.tops_model import load_survey_for_well
    from well_log_workstation.workspace import add_well, create_workspace

    ws = create_workspace(tmp_path / "ws2", name="NoSurvey")
    well = add_well(ws, name="VERT-1", path="wells/vert1.las")
    loaded, diags = load_survey_for_well(ws, well.id)
    assert loaded == []
    assert diags == []


def test_shell_binds_secondary_tvdss_axis(qtbot, tmp_path: Path) -> None:
    """Epic B 多轴: a well with survey + KB gets a TVDSS secondary axis;
    without either the axis stays unbound (explicit unavailability)."""
    from dataclasses import replace

    from well_log_workstation.shell import WellLogWorkstationWindow
    from well_log_workstation.tops_model import save_survey_for_well
    from well_log_workstation.workspace import create_workspace

    ws = create_workspace(tmp_path / "ws-sec", name="SECAXIS")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)

    def _las(path: Path) -> Path:
        path.write_text(
            """~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1003.0
STEP.M 1.0
NULL. -999.25
WELL. A
~CURVE INFORMATION
DEPT.M
GR.GAPI
~ASCII
1000 10
1001 20
1002 30
1003 40
""",
            encoding="utf-8",
        )
        return path

    well_id = win.import_las_path(_las(tmp_path / "a.las"))
    canvas = win.multi_track_canvas
    # No KB yet → unbound (explicit unavailability).
    win.apply_template_to_well(well_id, "std-gr-rt-den")
    assert canvas.secondary_depth_axis() is None

    # Set KB on the catalog entry.
    entry = next(w for w in ws.wells if w.id == well_id)
    idx = ws.wells.index(entry)
    ws.wells[idx] = replace(entry, kb_m=500.0)
    # Vertical survey: tvdss = 500 − md.
    save_survey_for_well(
        ws,
        well_id,
        [SurveyStation(0, 0, 0), SurveyStation(2000, 0, 0)],
    )
    win.apply_template_to_well(well_id, "std-gr-rt-den")
    points = canvas.secondary_depth_axis()
    assert points is not None
    assert len(points) >= 2
    # First station: tvdss = 500 − 0 = 500.
    assert points[0][0] == pytest.approx(500.0)
    assert points[0][1] == pytest.approx(0.0)
