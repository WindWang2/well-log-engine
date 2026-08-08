"""Perforation domain model (Epic C / FRS §3.x 射孔).

Covers: interval structure (explicit depth domain/unit, operation date, shot
density, phasing, status, completion ref, formation linkage, source/version),
validation, persistence round-trip, missing/corrupt tolerance, and the stable
data interface shape for future 射孔道 rendering.
"""

from __future__ import annotations

import os
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.perforation_model import (
    PERFORATION_STATUSES,
    PerforationInterval,
    PerforationModel,
    load_perforation_for_well,
    save_perforation_for_well,
)


def _model() -> PerforationModel:
    return PerforationModel(
        well_id="w1",
        intervals=[
            PerforationInterval(
                id="p1",
                top=2100.0,
                bottom=2104.0,
                operation_date="2024-06-01",
                shot_density=16.0,
                phasing="90°",
                status="已射",
                completion_ref="完井管柱-1",
                formation_unit_id="f1",
            ),
        ],
    )


def test_statuses_extensible() -> None:
    assert "已射" in PERFORATION_STATUSES
    PerforationInterval(id="x", top=1.0, bottom=2.0, status="复射")


def test_structure() -> None:
    m = _model()
    itv = m.intervals[0]
    assert itv.display_label.startswith("射孔 2100–2104 m")
    assert itv.depth_domain == "MD" and itv.unit == "m"
    assert itv.shot_density == pytest.approx(16.0)
    assert itv.phasing == "90°"
    assert itv.completion_ref == "完井管柱-1"
    assert itv.formation_unit_id == "f1"


def test_validation() -> None:
    assert _model().validate() == []
    bad = PerforationModel(
        well_id="w1",
        intervals=[
            PerforationInterval(id="a", top=10.0, bottom=5.0),
            PerforationInterval(id="b", top=5.0, bottom=10.0, depth_domain="?"),
        ],
    )
    problems = bad.validate()
    assert any("顶深大于底深" in p for p in problems)
    assert any("深度域" in p for p in problems)


def test_persistence_round_trip(tmp_path: Path) -> None:
    from well_log_workstation.workspace import add_well, create_workspace

    ws = create_workspace(tmp_path / "ws", name="PERF")
    well = add_well(ws, name="W1", path="wells/w1.las")
    save_perforation_for_well(ws, well.id, _model())
    loaded, diags = load_perforation_for_well(ws, well.id)
    assert diags == []
    itv = loaded.intervals[0]
    assert itv.operation_date == "2024-06-01"
    assert itv.shot_density == pytest.approx(16.0)
    assert itv.status == "已射"
    assert itv.completion_ref == "完井管柱-1"


def test_missing_and_corrupt_files(tmp_path: Path) -> None:
    from well_log_workstation.perforation_model import perforation_file_path
    from well_log_workstation.workspace import add_well, create_workspace

    ws = create_workspace(tmp_path / "ws2", name="PERF2")
    well = add_well(ws, name="W1", path="wells/w1.las")
    m, diags = load_perforation_for_well(ws, well.id)
    assert m.intervals == [] and diags == []
    corrupt = perforation_file_path(ws, well.id)
    corrupt.parent.mkdir(parents=True, exist_ok=True)
    corrupt.write_text("{bad", encoding="utf-8")
    m2, diags2 = load_perforation_for_well(ws, well.id)
    assert m2.intervals == [] and diags2 and "损坏" in diags2[0]


def test_stable_track_interface_shape() -> None:
    """The model is the data interface for the future 射孔道: track code
    binds to intervals (id/top/bottom/status/formation), never ad-hoc JSON."""
    m = _model()
    for itv in m.intervals:
        assert itv.id and itv.top <= itv.bottom
        assert itv.depth_domain in ("MD", "TVD", "TVDSS")
        assert itv.status
        assert itv.formation_unit_id  # → stratigraphic dictionary
